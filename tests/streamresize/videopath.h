#pragma once

// The part of moonlight-common-c that video takes on its way to the decoder,
// run without a connection. Frames go in where the RTP queue hands packets to
// the depacketizer, and come out the way the FFmpeg decoder takes them.

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// Starts a stream for a pull renderer, as the FFmpeg decoder is
void videoPathStart(void);
void videoPathStop(void);

// Sends a frame in one packet. Its picture says what size it was encoded at.
void videoPathSendFrame(uint32_t frameIndex, bool idr, uint16_t width, uint16_t height);

// Takes the next frame with LiPollNextVideoFrame() and completes it, as the
// FFmpeg decoder does. False if there is none.
bool videoPathDecodeNext(uint32_t* frameIndex, bool* idr, uint16_t* width, uint16_t* height);

// How often an IDR frame was asked for
int videoPathIdrRequests(void);

#ifdef __cplusplus
}
#endif
