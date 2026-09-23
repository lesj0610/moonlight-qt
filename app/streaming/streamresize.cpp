#include "streamresize.h"

#include <Limelight.h>

#include <algorithm>

DecodeGate::DecodeGate()
    : m_State(Open)
{
}

void DecodeGate::block()
{
    m_State.store(Blocked);
}

void DecodeGate::resumeAtKeyframe()
{
    m_State.store(AwaitingKeyframe);
}

bool DecodeGate::admit(bool isKeyframe)
{
    int state = m_State.load();
    if (state == Open) {
        return true;
    }

    if (state == AwaitingKeyframe && isKeyframe) {
        // A block() racing this wins, and the unit is dropped
        return m_State.compare_exchange_strong(state, Open) || state == Open;
    }

    return false;
}

bool DecodeGate::isOpen() const
{
    return m_State.load() == Open;
}

StreamResizeController::StreamResizeController(Host& host, int width, int height, int fps)
    : m_Host(host),
      m_Enabled(true),
      m_Windowed(true),
      m_Current({width, height}),
      m_Fps(fps),
      m_HasDesired(false),
      m_Desired({0, 0}),
      m_DesiredSinceMs(0),
      m_HasInFlight(false),
      m_InFlightId(0),
      m_InFlightSize({0, 0}),
      m_HasSent(false),
      m_LastSentMs(0),
      m_HasFailed(false),
      m_Failed({0, 0}),
      m_ResetPending(false)
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
    if (!m_Enabled) {
        return;
    }

    if (m_HasInFlight) {
        if (nowMs - m_LastSentMs >= k_AnswerTimeoutMs) {
            // Without the answer there is no telling what size the stream is,
            // and video stays held back, so the session cannot go on.
            m_HasInFlight = false;
            m_Enabled = false;
            m_HasDesired = false;
            m_Host.endSession("The host did not answer a request to resize the stream.");
        }
        return;
    }

    if (!m_Windowed || !m_HasDesired || m_ResetPending) {
        // A finished rebuild or a window again picks this up
        return;
    }

    uint64_t due = dueMs();
    if (nowMs < due) {
        m_Host.scheduleTick((uint32_t)(due - nowMs));
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
    m_Host.blockVideo();

    uint32_t requestId = 0;
    int err = m_Host.sendRequest(target.width, target.height, m_Fps, &requestId);
    m_HasSent = true;
    m_LastSentMs = nowMs;

    if (err == 0) {
        // One request at a time. A window that changes meanwhile is asked
        // for once this one is answered.
        m_HasInFlight = true;
        m_InFlightId = requestId;
        m_InFlightSize = target;
        m_HasDesired = false;

        // Wakes the controller up to notice an answer that never comes
        m_Host.scheduleTick(k_AnswerTimeoutMs);
        return;
    }

    // The host never saw the request, so the stream is as it was
    resumeVideo();
    if (err == LI_ERR_UNSUPPORTED) {
        stop("the host does not support resizing the stream");
    }
    else {
        stop("a resize request could not be sent");
    }
}

void StreamResizeController::onResult(uint32_t requestId, int width, int height, int fps, uint8_t status, uint64_t nowMs)
{
    if (!m_HasInFlight || requestId != m_InFlightId) {
        // Not the answer being waited for. moonlight-common-c drops stale and
        // repeated answers already, and this is the second check.
        return;
    }

    Size asked = m_InFlightSize;
    m_HasInFlight = false;

    switch (status) {
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

    // Whatever was asked for, the answer says what the host streams now
    Size actual = {width, height};
    if (!(actual == m_Current) || fps != m_Fps) {
        m_Current = actual;
        m_Fps = fps;
        m_Host.applyStreamSize(width, height, fps);
        m_ResetPending = true;
        m_Host.recreateDecoder();
    }
    else {
        // The stream kept its size, so the decoder it has is right. It only
        // needs a keyframe, since frames were dropped while waiting.
        resumeVideo();
    }

    if (status == LI_STREAM_RESIZE_REJECTED_UNSUPPORTED) {
        stop("the host cannot resize the stream in its current configuration");
    }
    else if (status == LI_STREAM_RESIZE_FAILED_SESSION_ENDING) {
        stop("a resize failed and the host is ending the session");
    }
    else {
        scheduleNext(nowMs);
    }
}

void StreamResizeController::onDecoderRecreated(uint64_t nowMs)
{
    if (!m_ResetPending) {
        return;
    }

    m_ResetPending = false;
    resumeVideo();
    scheduleNext(nowMs);
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
    return m_HasInFlight;
}

bool StreamResizeController::isResetPending() const
{
    return m_ResetPending;
}

void StreamResizeController::resumeVideo()
{
    // The gate is ready for the keyframe before the keyframe is asked for,
    // so it cannot arrive while units are still being dropped.
    m_Host.resumeVideoAtKeyframe();
    m_Host.requestIdrFrame();
}

void StreamResizeController::stop(const char* reason)
{
    m_Enabled = false;
    m_HasDesired = false;
    m_Host.stopFollowing(reason);
}

void StreamResizeController::scheduleNext(uint64_t nowMs)
{
    if (!m_Enabled || !m_Windowed || !m_HasDesired || m_HasInFlight || m_ResetPending) {
        return;
    }

    uint64_t due = dueMs();
    m_Host.scheduleTick(nowMs < due ? (uint32_t)(due - nowMs) : 0);
}

uint64_t StreamResizeController::dueMs() const
{
    uint64_t due = m_DesiredSinceMs + k_SettleMs;
    if (m_HasSent) {
        due = std::max(due, m_LastSentMs + k_MinIntervalMs);
    }
    return due;
}
