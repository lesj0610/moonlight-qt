#include "streamresize.h"

#include <Limelight.h>

#include <algorithm>

StreamResizeController::StreamResizeController(Host& host, int width, int height, int fps, bool enabled)
    : m_Host(host),
      m_Enabled(enabled),
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

void StreamResizeController::onDrawableSize(int width, int height, uint64_t nowMs)
{
    if (!m_Enabled) {
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
    if (!m_Enabled || !m_HasDesired || m_HasInFlight || m_ResetPending) {
        // An answer or a finished rebuild picks this up again
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
    }
    else if (err == LI_ERR_UNSUPPORTED || err == -2) {
        m_Enabled = false;
        m_HasDesired = false;
    }
    else {
        // It did not go out, so try again after the usual interval
        m_Host.scheduleTick(k_MinIntervalMs);
    }
}

void StreamResizeController::onResult(uint32_t requestId, int width, int height, int fps, uint8_t status, uint64_t nowMs)
{
    if (!m_HasInFlight || requestId != m_InFlightId) {
        // Not the answer being waited for. Frames were held back when it
        // arrived, in case it was, and are let through again unless a
        // rebuild is under way.
        if (!m_ResetPending) {
            m_Host.setDecoderInputBlocked(false);
        }
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
        // Nothing more can be asked of this host in this session
        m_Enabled = false;
        m_HasDesired = false;
        break;
    default:
        m_HasFailed = true;
        m_Failed = asked;
        break;
    }

    // Whatever was asked for, the answer says what the host streams now
    Size actual = {width, height};
    if (!(actual == m_Current) || fps != m_Fps) {
        m_Host.setDecoderInputBlocked(true);
        m_Current = actual;
        m_Fps = fps;
        m_Host.applyStreamSize(width, height, fps);
        m_ResetPending = true;
        m_Host.recreateDecoder();
        return;
    }

    m_Host.setDecoderInputBlocked(false);
    scheduleNext(nowMs);
}

void StreamResizeController::onDecoderRecreated(uint64_t nowMs)
{
    if (!m_ResetPending) {
        return;
    }

    // The keyframe is asked for before frames are let through, so the first
    // frame the new decoder takes is a keyframe of its size.
    m_Host.requestIdrFrame();
    m_Host.setDecoderInputBlocked(false);
    m_ResetPending = false;

    scheduleNext(nowMs);
}

bool StreamResizeController::isEnabled() const
{
    return m_Enabled;
}

bool StreamResizeController::isResetPending() const
{
    return m_ResetPending;
}

void StreamResizeController::scheduleNext(uint64_t nowMs)
{
    if (!m_Enabled || !m_HasDesired || m_HasInFlight || m_ResetPending) {
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
