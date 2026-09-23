#pragma once

#include <cstdint>

// Keeps the stream the size of the window it is shown in.
//
// When the window's drawable size settles, the host is asked for a stream of
// that size. The host switches without reconnecting and answers with the size
// it is actually streaming. Only then is the decoder rebuilt at that size, and
// frames are held back from the moment the answer arrives until the new
// decoder exists and a keyframe has been asked for, so the decoder never sees
// frames of a size it was not made for.
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

        // While blocked, decode units are refused so they are dropped until a keyframe.
        virtual void setDecoderInputBlocked(bool blocked) = 0;

        // The stream is this size now. The decoder is made at it and input is mapped to it.
        virtual void applyStreamSize(int width, int height, int fps) = 0;

        // Rebuild the decoder. onDecoderRecreated() follows once it exists.
        virtual void recreateDecoder() = 0;

        virtual void requestIdrFrame() = 0;

        // Call onTick() after this many milliseconds.
        virtual void scheduleTick(uint32_t delayMs) = 0;
    };

    // How long the window has to keep a size before it is asked for
    static constexpr uint32_t k_SettleMs = 600;

    // The shortest time between two requests
    static constexpr uint32_t k_MinIntervalMs = 1000;

    // enabled is whether the host supports resizing at all
    StreamResizeController(Host& host, int width, int height, int fps, bool enabled);

    // The window's drawable size in pixels, whenever it may have changed
    void onDrawableSize(int width, int height, uint64_t nowMs);

    void onTick(uint64_t nowMs);

    // An answer from the host. Stale and repeated answers are ignored.
    void onResult(uint32_t requestId, int width, int height, int fps, uint8_t status, uint64_t nowMs);

    // The decoder asked for by recreateDecoder() exists. Also called for
    // rebuilds that had other causes, which it ignores.
    void onDecoderRecreated(uint64_t nowMs);

    bool isEnabled() const;
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

    void scheduleNext(uint64_t nowMs);
    uint64_t dueMs() const;

    Host& m_Host;
    bool m_Enabled;

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
