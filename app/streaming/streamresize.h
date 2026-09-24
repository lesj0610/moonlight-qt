#pragma once

#include <Limelight.h>

#include <cstdint>
#include <vector>

// A size in pixels. Zero is unknown.
struct StreamSize
{
    int width;
    int height;
};

// Where a stream that takes the size of its window starts, before there is a
// window. In fullscreen it is the desktop's size. In a window it is the size
// the window had last time, if that still fits the usable part of the
// screen, and otherwise most of that part. The result is even and within the
// limits of LiRequestStreamResize(), or zero if nothing is known.
StreamSize chooseAutoStreamSize(bool fullScreen, StreamSize desktop, StreamSize usable, StreamSize last);

// One display, in pixels: all of it, and the part windows can use
struct DisplaySize
{
    StreamSize desktop;
    StreamSize usable;
};

// How a stream that takes the size of its window starts on the display it
// will be shown on, which is displays[target] (the first display if target
// is not one of them). bitrateFor is the size its default bitrate goes by,
// the whole display, since that is the largest the stream gets. It is zero
// when the bitrate is kept as it was set.
struct AutoStreamStart
{
    StreamSize size;
    StreamSize bitrateFor;
};
AutoStreamStart chooseAutoStreamStart(const std::vector<DisplaySize>& displays, int target,
                                      bool fullScreen, bool autoBitrate, StreamSize lastWindow);

// Keeps the stream the size of the window it is shown in, when the
// resolution chosen is Auto. With any other resolution nothing is asked for.
//
// When the window's drawable size settles, the host is asked for a stream of
// that size, and it switches without reconnecting. In fullscreen the stream
// is asked for once at the size of the screen. Video is held back in
// moonlight-common-c from the moment a request is sent, since frames of the
// new size can arrive before the host's answer does. Once the answer is in,
// the decoder is rebuilt at the size the host streams, if that changed, and
// video resumes at the first keyframe of the stream the answer describes.
// Keyframes the host encoded before that, however late they arrive, never
// reach the decoder.
//
// Every wait has an end. An answer that does not come, a rebuild that does
// not happen, or video that does not come back ends the session, since there
// is no stream to show without them.
//
// Only the size of the current mode counts. Whatever was waiting to be asked
// for when the window goes fullscreen or back is dropped, so a size the
// window had before cannot replace the screen's, or the other way round. A
// request already sent still gets its answer applied, since the host acts on
// it either way, and the newest size is asked for after it.
//
// Everything here runs on the main thread. Session implements Host.
class StreamResizeController
{
public:
    class Host
    {
    public:
        virtual ~Host() = default;

        // Ask the host for a new stream size (LiRequestStreamResize()).
        virtual int sendRequest(int width, int height, int fps, uint32_t* requestId) = 0;

        // Keep decode units from the decoder (LiHoldVideo())
        virtual void holdVideo() = 0;

        // Let them through again from the first keyframe at or after
        // firstFrame, or from the next keyframe (LiResumeVideo())
        virtual void resumeVideo(bool fromFrame, uint32_t firstFrame) = 0;

        // Whether the keyframe resumeVideo() waits for got through (LiIsVideoFlowing())
        virtual bool isVideoFlowing() = 0;

        virtual void requestIdrFrame() = 0;

        // The stream is this size now. The decoder is made at it and input is mapped to it.
        virtual void applyStreamSize(int width, int height, int fps) = 0;

        // Rebuild the decoder soon. onDecoderRecreated() follows once it exists.
        virtual void recreateDecoder() = 0;

        // Call onTick() after this many milliseconds.
        virtual void scheduleTick(uint32_t delayMs) = 0;

        // Following the window is over for this session, and why
        virtual void stopFollowing(const char* reason) = 0;

        // The session cannot go on, and why
        virtual void endSession(const char* reason) = 0;
    };

    // How long the window has to keep a size before it is asked for
    static constexpr uint32_t k_SettleMs = 600;

    // The shortest time between two requests
    static constexpr uint32_t k_MinIntervalMs = 1000;

    // How long an answer may take: the time the host has to answer, even with
    // a rollback that runs every step to its limit, and then time for the
    // answer to get through. Without an answer there is no telling what size
    // the stream is, so the session ends.
    static constexpr uint32_t k_AnswerTravelMs = 10000;
    static constexpr uint32_t k_AnswerTimeoutMs = LI_STREAM_RESIZE_ANSWER_WITHIN_MS + k_AnswerTravelMs;

    // How long the decoder may take to be rebuilt once asked for
    static constexpr uint32_t k_RebuildTimeoutMs = 10000;

    // How long video may take to come back once the decoder can take it, and
    // how often a keyframe is asked for meanwhile
    static constexpr uint32_t k_KeyframeTimeoutMs = 10000;
    static constexpr uint32_t k_KeyframeRetryMs = 1000;

    // follow is whether the resolution chosen is Auto
    StreamResizeController(Host& host, int width, int height, int fps, bool follow);

    // Whether the window is a real window, as opposed to fullscreen
    void setWindowed(bool windowed, uint64_t nowMs);

    // The window's drawable size in pixels, whenever it may have changed.
    // Only used in a real window.
    void onDrawableSize(int width, int height, uint64_t nowMs);

    // The size in pixels of the screen a fullscreen window is on, whenever it
    // may have changed. Only used in fullscreen, where it is asked for without
    // waiting for it to settle.
    void onScreenSize(int width, int height, uint64_t nowMs);

    void onTick(uint64_t nowMs);

    // An answer from the host. Stale and repeated answers are ignored.
    void onResult(const STREAM_RESIZE_RESULT& result, uint64_t nowMs);

    // The decoder asked for by recreateDecoder() exists. Also called for
    // rebuilds that had other causes, which it ignores.
    void onDecoderRecreated(uint64_t nowMs);

    // Whether window sizes are still asked for
    bool isEnabled() const;
    bool isWindowed() const;
    bool isRequestInFlight() const;
    bool isResetPending() const;
    bool isAwaitingKeyframe() const;

private:
    struct Size
    {
        int width;
        int height;

        bool operator==(const Size& other) const
        {
            return width == other.width && height == other.height;
        }
    };

    // Where the resize being carried out is. Video is held back in every
    // phase but Idle.
    enum class Phase
    {
        Idle,
        AwaitingAnswer,
        Rebuilding,
        AwaitingKeyframe,
    };

    void setDesired(int width, int height, bool settle, uint64_t nowMs);
    void enterPhase(Phase phase, uint64_t nowMs);
    void resumeVideo(uint64_t nowMs);
    void stop(const char* reason);
    void end(const char* reason);
    void scheduleNext(uint64_t nowMs);
    void scheduleAt(uint64_t atMs, uint64_t nowMs);
    uint64_t dueMs() const;

    Host& m_Host;
    bool m_Enabled;
    bool m_Ended;
    bool m_Windowed;

    Size m_Current;
    int m_Fps;

    bool m_HasDesired;
    Size m_Desired;
    uint64_t m_DesiredSinceMs;
    bool m_DesiredSettles;

    Phase m_Phase;
    uint64_t m_PhaseSinceMs;
    uint64_t m_LastIdrRequestMs;

    uint32_t m_InFlightId;
    Size m_InFlightSize;

    // Where video resumes, from the answer
    bool m_ResumeFromFrame;
    uint32_t m_ResumeFirstFrame;

    bool m_HasSent;
    uint64_t m_LastSentMs;

    // The tick asked for last. onTick() also runs whenever the main loop
    // wakes, so it is only asked for again when it has to come sooner.
    bool m_HasTick;
    uint64_t m_TickAtMs;

    // A size the host could not switch to. It is not asked for again until the window changes.
    bool m_HasFailed;
    Size m_Failed;
};
