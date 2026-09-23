#pragma once

#include <atomic>
#include <cstdint>

// Decides which decode units reach the decoder while the stream changes size.
// The main thread moves it between states; the decoder thread asks it about
// every unit.
class DecodeGate
{
public:
    DecodeGate();

    // Drop every unit until resumeAtKeyframe()
    void block();

    // Let units through again, starting with the next keyframe
    void resumeAtKeyframe();

    // Called for every unit. True if it may be decoded.
    bool admit(bool isKeyframe);

    bool isOpen() const;

private:
    enum State
    {
        Open,
        Blocked,
        AwaitingKeyframe,
    };

    std::atomic<int> m_State;
};

// Keeps the stream the size of the window it is shown in.
//
// When the window's drawable size settles, the host is asked for a stream of
// that size, and it switches without reconnecting. Video is held back from
// the moment a request is sent, since frames of the new size can arrive
// before the host's answer does. Once the answer is in, the decoder is
// rebuilt at the size the host streams, if that changed, and video resumes
// at the next keyframe, which is asked for after the gate is ready for it.
//
// Only a real window is followed. In fullscreen the stream keeps its size.
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

        // Drop every decode unit until resumeVideoAtKeyframe()
        virtual void blockVideo() = 0;

        // Let decode units through again, starting with the next keyframe
        virtual void resumeVideoAtKeyframe() = 0;

        virtual void requestIdrFrame() = 0;

        // The stream is this size now. The decoder is made at it and input is mapped to it.
        virtual void applyStreamSize(int width, int height, int fps) = 0;

        // Rebuild the decoder. onDecoderRecreated() follows once it exists.
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

    // How long an answer may take. The host's slowest resize, one that has to
    // be rolled back, stays well inside this. Without an answer there is no
    // telling what size the stream is, so the session ends.
    static constexpr uint32_t k_AnswerTimeoutMs = 30000;

    StreamResizeController(Host& host, int width, int height, int fps);

    // Whether the window is a real window, as opposed to fullscreen
    void setWindowed(bool windowed, uint64_t nowMs);

    // The window's drawable size in pixels, whenever it may have changed
    void onDrawableSize(int width, int height, uint64_t nowMs);

    void onTick(uint64_t nowMs);

    // An answer from the host. Stale and repeated answers are ignored.
    void onResult(uint32_t requestId, int width, int height, int fps, uint8_t status, uint64_t nowMs);

    // The decoder asked for by recreateDecoder() exists. Also called for
    // rebuilds that had other causes, which it ignores.
    void onDecoderRecreated(uint64_t nowMs);

    bool isEnabled() const;
    bool isWindowed() const;
    bool isRequestInFlight() const;
    bool isResetPending() const;

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

    void resumeVideo();
    void stop(const char* reason);
    void scheduleNext(uint64_t nowMs);
    uint64_t dueMs() const;

    Host& m_Host;
    bool m_Enabled;
    bool m_Windowed;

    Size m_Current;
    int m_Fps;

    bool m_HasDesired;
    Size m_Desired;
    uint64_t m_DesiredSinceMs;

    bool m_HasInFlight;
    uint32_t m_InFlightId;
    Size m_InFlightSize;

    bool m_HasSent;
    uint64_t m_LastSentMs;

    // A size the host could not switch to. It is not asked for again until the window changes.
    bool m_HasFailed;
    Size m_Failed;

    bool m_ResetPending;
};
