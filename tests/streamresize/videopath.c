// The depacketizer, its queue, and the platform code are moonlight-common-c's
// own. The rest of the library, which they call out to, is stood in for here.

#include "Limelight-internal.h"
#include "videopath.h"

int AppVersionQuad[4];
STREAM_CONFIGURATION StreamConfig;
CONNECTION_LISTENER_CALLBACKS ListenerCallbacks;
DECODER_RENDERER_CALLBACKS VideoCallbacks;
int NegotiatedVideoFormat;

static int idrRequests;

bool isReferenceFrameInvalidationEnabled(void) { return false; }
bool LiGetCurrentHostDisplayHdrMode(void) { return false; }
void LiRequestIdrFrame(void) { idrRequests++; }
void connectionDetectedFrameLoss(uint32_t startFrame, uint32_t endFrame) { (void)startFrame; (void)endFrame; }
void connectionReceivedCompleteFrame(uint32_t frameIndex, bool frameIsLTR) { (void)frameIndex; (void)frameIsLTR; }
void notifyKeyFrameReceived(void) {}

// Platform.c needs these only to set the platform up, which is not done here
int initializePlatformSockets(void) { return 0; }
void cleanupPlatformSockets(void) {}
void enterLowLatencyMode(void) {}
void exitLowLatencyMode(void) {}
int enet_initialize(void) { return 0; }
void enet_deinitialize(void) {}

static uint32_t streamPacketIndex;
static uint64_t receiveTimeUs;

void videoPathStart(void)
{
    memset(&StreamConfig, 0, sizeof(StreamConfig));
    memset(&VideoCallbacks, 0, sizeof(VideoCallbacks));
    memset(&ListenerCallbacks, 0, sizeof(ListenerCallbacks));

    // Sunshine reports itself as GFE 7.1.431 with a negative last component
    AppVersionQuad[0] = 7;
    AppVersionQuad[1] = 1;
    AppVersionQuad[2] = 431;
    AppVersionQuad[3] = -1;

    // AV1 frames are passed through without parsing NAL units, so a test frame can be any bytes
    NegotiatedVideoFormat = VIDEO_FORMAT_AV1_MAIN8;
    StreamConfig.packetSize = 1024;
    VideoCallbacks.capabilities = CAPABILITY_PULL_RENDERER;

    streamPacketIndex = 0;
    receiveTimeUs = 1000000;
    idrRequests = 0;

    initializeVideoDepacketizer(StreamConfig.packetSize);
}

void videoPathStop(void)
{
    stopVideoDepacketizer();
    destroyVideoDepacketizer();
}

void videoPathSendFrame(uint32_t frameIndex, bool idr, uint16_t width, uint16_t height)
{
    const size_t pictureLength = 4;
    const size_t payloadLength = 8 + pictureLength;
    const size_t packetLength = sizeof(RTP_PACKET) + sizeof(NV_VIDEO_PACKET) + payloadLength;
    const size_t entryOffset = (packetLength + 15) & ~(size_t)15;

    // The queue entry for a packet sits in the same allocation, after it, as
    // the receive thread lays packets out
    char* buffer = (char*)calloc(1, entryOffset + sizeof(RTPV_QUEUE_ENTRY));
    if (buffer == NULL) {
        return;
    }

    PRTP_PACKET rtp = (PRTP_PACKET)buffer;
    PNV_VIDEO_PACKET video = (PNV_VIDEO_PACKET)(rtp + 1);
    uint8_t* payload = (uint8_t*)(video + 1);
    PRTPV_QUEUE_ENTRY entry = (PRTPV_QUEUE_ENTRY)(buffer + entryOffset);

    rtp->header = 0x80;
    video->streamPacketIndex = (streamPacketIndex++ & 0xFFFFFF) << 8;
    video->frameIndex = frameIndex;
    video->flags = FLAG_SOF | FLAG_EOF | FLAG_CONTAINS_PIC_DATA;

    // Sunshine's 8 byte frame header: 0x01, host processing latency, frame
    // type (1 P-frame, 2 IDR frame), and the payload length including it
    payload[0] = 0x01;
    payload[3] = idr ? 2 : 1;
    payload[4] = (uint8_t)payloadLength;
    payload[5] = (uint8_t)(payloadLength >> 8);
    payload[8] = (uint8_t)width;
    payload[9] = (uint8_t)(width >> 8);
    payload[10] = (uint8_t)height;
    payload[11] = (uint8_t)(height >> 8);

    entry->packet = rtp;
    entry->length = (int)packetLength;
    entry->receiveTimeUs = receiveTimeUs;
    entry->presentationTimeUs = receiveTimeUs;
    entry->rtpTimestamp = frameIndex * 1500;
    entry->isParity = false;
    receiveTimeUs += 16667;

    queueRtpPacket(entry);
}

bool videoPathDecodeNext(uint32_t* frameIndex, bool* idr, uint16_t* width, uint16_t* height)
{
    VIDEO_FRAME_HANDLE handle;
    PDECODE_UNIT du;

    if (!LiPollNextVideoFrame(&handle, &du)) {
        return false;
    }

    const uint8_t* picture = (const uint8_t*)du->bufferList->data;
    *frameIndex = (uint32_t)du->frameNumber;
    *idr = du->frameType == FRAME_TYPE_IDR;
    *width = (uint16_t)(picture[0] | (picture[1] << 8));
    *height = (uint16_t)(picture[2] | (picture[3] << 8));

    LiCompleteVideoFrame(handle, DR_OK);
    return true;
}

int videoPathIdrRequests(void)
{
    return idrRequests;
}
