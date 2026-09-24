#include "streamresize.h"

#include <algorithm>

namespace {

bool isKnown(StreamSize size)
{
    return size.width > 0 && size.height > 0;
}

StreamSize withinLimits(StreamSize size)
{
    return {std::clamp(size.width, LI_STREAM_RESIZE_MIN_WIDTH, LI_STREAM_RESIZE_MAX_WIDTH) & ~1,
            std::clamp(size.height, LI_STREAM_RESIZE_MIN_HEIGHT, LI_STREAM_RESIZE_MAX_HEIGHT) & ~1};
}

}

StreamSize chooseAutoStreamSize(bool fullScreen, StreamSize desktop, StreamSize usable, StreamSize last)
{
    if (fullScreen && isKnown(desktop)) {
        return withinLimits(desktop);
    }

    if (isKnown(last) && (!isKnown(usable) || (last.width <= usable.width && last.height <= usable.height))) {
        return withinLimits(last);
    }

    // The same share of the screen a window gets for a stream too big for it
    StreamSize screen = isKnown(usable) ? usable : desktop;
    if (isKnown(screen)) {
        return withinLimits({screen.width * 4 / 5, screen.height * 4 / 5});
    }

    return {0, 0};
}

StreamResizeController::StreamResizeController(Host& host, int width, int height, int fps)
    : m_Host(host),
      m_Enabled(true),
      m_Ended(false),
      m_Windowed(true),
      m_Current({width, height}),
      m_Fps(fps),
      m_HasDesired(false),
      m_Desired({0, 0}),
      m_DesiredSinceMs(0),
      m_Phase(Phase::Idle),
      m_PhaseSinceMs(0),
      m_LastIdrRequestMs(0),
      m_InFlightId(0),
      m_InFlightSize({0, 0}),
      m_ResumeFromFrame(false),
      m_ResumeFirstFrame(0),
      m_HasSent(false),
      m_LastSentMs(0),
      m_HasTick(false),
      m_TickAtMs(0),
      m_HasFailed(false),
      m_Failed({0, 0})
{
}

void StreamResizeController::setWindowed(bool windowed, uint64_t)
{
    if (windowed == m_Windowed) {
        return;
    }

    m_Windowed = windowed;
    if (!windowed) {
        // A size the window had before going fullscreen is not wanted any
        // more. A request already sent still gets its answer applied, since
        // the host acts on it either way.
        m_HasDesired = false;
    }
}

void StreamResizeController::onDrawableSize(int width, int height, uint64_t nowMs)
{
    if (!m_Enabled || !m_Windowed) {
        return;
    }

    // The host takes even sizes within its limits. A window smaller than the
    // smallest stream keeps the stream it has.
    Size size = {std::min(width, LI_STREAM_RESIZE_MAX_WIDTH) & ~1,
                 std::min(height, LI_STREAM_RESIZE_MAX_HEIGHT) & ~1};
    if (size.width < LI_STREAM_RESIZE_MIN_WIDTH || size.height < LI_STREAM_RESIZE_MIN_HEIGHT) {
        m_HasDesired = false;
        return;
    }

    if (m_HasDesired && m_Desired == size) {
        // Nothing changed, so the size keeps settling
        return;
    }

    if (m_HasFailed && !(m_Failed == size)) {
        // The window moved on from the size the host could not do
        m_HasFailed = false;
    }

    m_HasDesired = true;
    m_Desired = size;
    m_DesiredSinceMs = nowMs;
    scheduleNext(nowMs);
}

void StreamResizeController::onTick(uint64_t nowMs)
{
    if (m_Ended) {
        return;
    }

    uint64_t waitedMs = nowMs - m_PhaseSinceMs;
    switch (m_Phase) {
    case Phase::AwaitingAnswer:
        if (waitedMs >= k_AnswerTimeoutMs) {
            end("The host did not answer a request to resize the stream.");
        }
        else {
            scheduleAt(m_PhaseSinceMs + k_AnswerTimeoutMs, nowMs);
        }
        return;

    case Phase::Rebuilding:
        if (waitedMs >= k_RebuildTimeoutMs) {
            end("The video decoder was not rebuilt for the new stream size.");
        }
        else {
            scheduleAt(m_PhaseSinceMs + k_RebuildTimeoutMs, nowMs);
        }
        return;

    case Phase::AwaitingKeyframe:
        if (!m_Host.isVideoFlowing()) {
            if (waitedMs >= k_KeyframeTimeoutMs) {
                end("Video did not come back after the stream was resized.");
                return;
            }

            // A keyframe that was asked for can be lost, or be one from
            // before the switch, which is dropped. Asking again is cheap.
            if (nowMs - m_LastIdrRequestMs >= k_KeyframeRetryMs) {
                m_Host.requestIdrFrame();
                m_LastIdrRequestMs = nowMs;
            }
            scheduleAt(std::min(m_LastIdrRequestMs + k_KeyframeRetryMs, m_PhaseSinceMs + k_KeyframeTimeoutMs), nowMs);
            return;
        }

        // Done. A size the window took meanwhile can be asked for now.
        enterPhase(Phase::Idle, nowMs);
        break;

    case Phase::Idle:
        break;
    }

    if (!m_Enabled || !m_Windowed || !m_HasDesired) {
        // A window again, or a new size, picks this up
        return;
    }

    uint64_t due = dueMs();
    if (nowMs < due) {
        scheduleAt(due, nowMs);
        return;
    }

    Size target = m_Desired;
    if (target == m_Current) {
        m_HasDesired = false;
        return;
    }

    if (m_HasFailed && m_Failed == target) {
        return;
    }

    // Held back from before the request goes out: frames of the new size can
    // arrive ahead of the answer, and the decoder must never see them.
    m_Host.holdVideo();

    uint32_t requestId = 0;
    int err = m_Host.sendRequest(target.width, target.height, m_Fps, &requestId);
    m_HasSent = true;
    m_LastSentMs = nowMs;

    if (err == 0) {
        // One request at a time. A window that changes meanwhile is asked
        // for once this one is done.
        m_InFlightId = requestId;
        m_InFlightSize = target;
        m_HasDesired = false;
        enterPhase(Phase::AwaitingAnswer, nowMs);

        // Wakes the controller up to notice an answer that never comes
        scheduleAt(nowMs + k_AnswerTimeoutMs, nowMs);
        return;
    }

    // The host never saw the request, so the stream is as it was
    m_ResumeFromFrame = false;
    resumeVideo(nowMs);
    if (err == LI_ERR_UNSUPPORTED) {
        stop("the host does not support resizing the stream");
    }
    else {
        stop("a resize request could not be sent");
    }
}

void StreamResizeController::onResult(const STREAM_RESIZE_RESULT& result, uint64_t nowMs)
{
    if (m_Phase != Phase::AwaitingAnswer || result.requestId != m_InFlightId) {
        // Not the answer being waited for. moonlight-common-c drops stale and
        // repeated answers already, and this is the second check.
        return;
    }

    Size asked = m_InFlightSize;

    switch (result.status) {
    case LI_STREAM_RESIZE_OK:
    case LI_STREAM_RESIZE_SUPERSEDED:
        m_HasFailed = false;
        break;
    case LI_STREAM_RESIZE_REJECTED_UNSUPPORTED:
    case LI_STREAM_RESIZE_FAILED_SESSION_ENDING:
        break;
    default:
        m_HasFailed = true;
        m_Failed = asked;
        break;
    }

    // Where the stream the answer describes starts. Without a first frame the
    // host did not start encoding anew, so any keyframe from now on belongs
    // to the stream the decoder is made for.
    m_ResumeFromFrame = result.hasFirstFrame;
    m_ResumeFirstFrame = result.firstFrame;

    // Whatever was asked for, the answer says what the host streams now
    Size actual = {result.width, result.height};
    if (!(actual == m_Current) || result.fps != m_Fps) {
        m_Current = actual;
        m_Fps = result.fps;
        m_Host.applyStreamSize(result.width, result.height, result.fps);
        enterPhase(Phase::Rebuilding, nowMs);
        m_Host.recreateDecoder();
        scheduleAt(nowMs + k_RebuildTimeoutMs, nowMs);
    }
    else {
        // The stream kept its size, so the decoder it has is right. It only
        // needs a keyframe, since frames were dropped while waiting.
        resumeVideo(nowMs);
    }

    if (result.status == LI_STREAM_RESIZE_REJECTED_UNSUPPORTED) {
        stop("the host cannot resize the stream in its current configuration");
    }
    else if (result.status == LI_STREAM_RESIZE_FAILED_SESSION_ENDING) {
        stop("a resize failed and the host is ending the session");
    }
}

void StreamResizeController::onDecoderRecreated(uint64_t nowMs)
{
    if (m_Phase != Phase::Rebuilding || m_Ended) {
        return;
    }

    resumeVideo(nowMs);
}

bool StreamResizeController::isEnabled() const
{
    return m_Enabled;
}

bool StreamResizeController::isWindowed() const
{
    return m_Windowed;
}

bool StreamResizeController::isRequestInFlight() const
{
    return m_Phase == Phase::AwaitingAnswer;
}

bool StreamResizeController::isResetPending() const
{
    return m_Phase == Phase::Rebuilding;
}

bool StreamResizeController::isAwaitingKeyframe() const
{
    return m_Phase == Phase::AwaitingKeyframe;
}

void StreamResizeController::enterPhase(Phase phase, uint64_t nowMs)
{
    m_Phase = phase;
    m_PhaseSinceMs = nowMs;
}

void StreamResizeController::resumeVideo(uint64_t nowMs)
{
    // The library is ready for the keyframe before the keyframe is asked
    // for, so it cannot arrive while units are still being dropped.
    m_Host.resumeVideo(m_ResumeFromFrame, m_ResumeFirstFrame);
    m_Host.requestIdrFrame();
    m_LastIdrRequestMs = nowMs;
    enterPhase(Phase::AwaitingKeyframe, nowMs);
    scheduleAt(nowMs + k_KeyframeRetryMs, nowMs);
}

void StreamResizeController::stop(const char* reason)
{
    // Only new requests stop. A resize under way still finishes, so video
    // comes back.
    m_Enabled = false;
    m_HasDesired = false;
    m_Host.stopFollowing(reason);
}

void StreamResizeController::end(const char* reason)
{
    m_Ended = true;
    m_Enabled = false;
    m_HasDesired = false;
    m_Phase = Phase::Idle;
    m_Host.endSession(reason);
}

void StreamResizeController::scheduleNext(uint64_t nowMs)
{
    if (!m_Enabled || !m_Windowed || !m_HasDesired || m_Phase != Phase::Idle) {
        return;
    }

    scheduleAt(std::max(dueMs(), nowMs), nowMs);
}

void StreamResizeController::scheduleAt(uint64_t atMs, uint64_t nowMs)
{
    // One tick at a time. A tick still to come that is due no later covers
    // this one, since every tick works out what is needed next.
    if (m_HasTick && m_TickAtMs > nowMs && m_TickAtMs <= atMs) {
        return;
    }

    m_HasTick = true;
    m_TickAtMs = atMs;
    m_Host.scheduleTick(atMs > nowMs ? (uint32_t)(atMs - nowMs) : 0);
}

uint64_t StreamResizeController::dueMs() const
{
    uint64_t due = m_DesiredSinceMs + k_SettleMs;
    if (m_HasSent) {
        due = std::max(due, m_LastSentMs + k_MinIntervalMs);
    }
    return due;
}
