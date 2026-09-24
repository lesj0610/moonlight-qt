// Tests for StreamResizeController: when the window is asked for, which
// answers are taken, where video resumes, and that every wait ends. The last
// tests put it in front of moonlight-common-c's own video path, taking frames
// the way the FFmpeg decoder does.

#include "streamresize.h"
#include "videopath.h"

#include <Limelight.h>

#include <cstdio>
#include <string>
#include <vector>

static int failures;

#define CHECK(cond)                                                          \
    do {                                                                     \
        if (!(cond)) {                                                       \
            std::fprintf(stderr, "%s:%d: CHECK(%s)\n", __FILE__, __LINE__, #cond); \
            failures++;                                                      \
        }                                                                    \
    } while (0)

namespace {

STREAM_RESIZE_RESULT answer(uint32_t id, int width, int height, int fps, uint8_t status)
{
    STREAM_RESIZE_RESULT result = {};
    result.requestId = id;
    result.width = (uint16_t)width;
    result.height = (uint16_t)height;
    result.fps = (uint16_t)fps;
    result.status = status;
    return result;
}

STREAM_RESIZE_RESULT answerFrom(uint32_t id, int width, int height, int fps, uint8_t status, uint32_t firstFrame)
{
    STREAM_RESIZE_RESULT result = answer(id, width, height, fps, status);
    result.hasFirstFrame = true;
    result.firstFrame = firstFrame;
    return result;
}

// A host that records what it is asked to do
struct FakeHost : StreamResizeController::Host
{
    std::vector<std::string> calls;
    std::vector<uint32_t> ticks;
    int sendResult = 0;
    uint32_t nextId = 1;
    bool flowing = true;

    struct Request { int width, height, fps; uint32_t id; };
    std::vector<Request> requests;

    struct Resume { bool fromFrame; uint32_t firstFrame; };
    std::vector<Resume> resumes;

    int sessionsEnded = 0;
    std::string endReason;
    std::string stopReason;

    int sendRequest(int width, int height, int fps, uint32_t* requestId) override
    {
        calls.push_back("send " + std::to_string(width) + "x" + std::to_string(height));
        if (sendResult != 0) {
            return sendResult;
        }
        *requestId = nextId++;
        requests.push_back({width, height, fps, *requestId});
        return 0;
    }

    void holdVideo() override
    {
        flowing = false;
        calls.push_back("hold");
    }

    void resumeVideo(bool fromFrame, uint32_t firstFrame) override
    {
        resumes.push_back({fromFrame, firstFrame});
        calls.push_back("resume");
    }

    bool isVideoFlowing() override
    {
        return flowing;
    }

    void requestIdrFrame() override
    {
        calls.push_back("idr");
    }

    void applyStreamSize(int width, int height, int fps) override
    {
        calls.push_back("apply " + std::to_string(width) + "x" + std::to_string(height) + "@" + std::to_string(fps));
    }

    void recreateDecoder() override
    {
        calls.push_back("recreate");
    }

    void scheduleTick(uint32_t delayMs) override
    {
        ticks.push_back(delayMs);
    }

    void stopFollowing(const char* reason) override
    {
        stopReason = reason;
        calls.push_back("stop");
    }

    void endSession(const char* reason) override
    {
        sessionsEnded++;
        endReason = reason;
        calls.push_back("end");
    }

    int sends() const
    {
        return (int)requests.size();
    }

    int count(const std::string& call) const
    {
        int n = 0;
        for (const std::string& c : calls) {
            n += c == call;
        }
        return n;
    }
};

}

static void testWaitsForTheWindowToSettle()
{
    FakeHost host;
    StreamResizeController controller(host, 1920, 1080, 60);

    controller.onDrawableSize(1280, 720, 0);
    CHECK(!host.ticks.empty() && host.ticks.back() == StreamResizeController::k_SettleMs);

    controller.onTick(599);
    CHECK(host.sends() == 0);

    controller.onTick(600);
    CHECK(host.sends() == 1);
    CHECK(host.requests[0].width == 1280 && host.requests[0].height == 720 && host.requests[0].fps == 60);
}

static void testADragIsAskedForOnceItStops()
{
    FakeHost host;
    StreamResizeController controller(host, 1920, 1080, 60);

    for (int i = 0; i < 10; i++) {
        controller.onDrawableSize(1200 + i * 10, 700 + i * 10, i * 100);
        controller.onTick(i * 100);
    }
    CHECK(host.sends() == 0);

    controller.onTick(900 + StreamResizeController::k_SettleMs - 1);
    CHECK(host.sends() == 0);
    controller.onTick(900 + StreamResizeController::k_SettleMs);
    CHECK(host.sends() == 1);
    CHECK(host.requests[0].width == 1290 && host.requests[0].height == 790);
}

static void testSizesAreMadeValidOrNotAskedFor()
{
    FakeHost host;
    StreamResizeController controller(host, 1920, 1080, 60);

    // Odd sizes are rounded down to even
    controller.onDrawableSize(1281, 721, 0);
    controller.onTick(600);
    CHECK(host.sends() == 1 && host.requests[0].width == 1280 && host.requests[0].height == 720);
    controller.onResult(answerFrom(1, 1280, 720, 60, LI_STREAM_RESIZE_OK, 10), 700);
    controller.onDecoderRecreated(800);
    host.flowing = true;
    controller.onTick(900);

    // Larger than the host allows is clamped
    controller.onDrawableSize(9000, 5001, 1000);
    controller.onTick(2000);
    CHECK(host.sends() == 2 && host.requests[1].width == LI_STREAM_RESIZE_MAX_WIDTH && host.requests[1].height == LI_STREAM_RESIZE_MAX_HEIGHT);
    controller.onResult(answerFrom(2, LI_STREAM_RESIZE_MAX_WIDTH, LI_STREAM_RESIZE_MAX_HEIGHT, 60, LI_STREAM_RESIZE_OK, 20), 2100);
    controller.onDecoderRecreated(2200);
    host.flowing = true;
    controller.onTick(2300);

    // Smaller than the smallest stream keeps the stream as it is
    controller.onDrawableSize(600, 400, 3000);
    controller.onTick(5000);
    CHECK(host.sends() == 2);
}

static void testTheSizeTheStreamHasIsNotAskedFor()
{
    FakeHost host;
    StreamResizeController controller(host, 1920, 1080, 60);

    controller.onDrawableSize(1920, 1080, 0);
    controller.onTick(1000);
    CHECK(host.sends() == 0);
    CHECK(host.calls.empty());
}

static void testOneResizeAtATimeAndAtMostOneRequestPerInterval()
{
    FakeHost host;
    StreamResizeController controller(host, 1920, 1080, 60);

    controller.onDrawableSize(1280, 720, 0);
    controller.onTick(600);
    CHECK(host.sends() == 1);

    // The window changes while the host is still switching
    controller.onDrawableSize(1600, 900, 650);
    controller.onTick(5000);
    CHECK(host.sends() == 1);

    // Answered and rebuilt, but video is not back yet: still one at a time
    controller.onResult(answerFrom(1, 1280, 720, 60, LI_STREAM_RESIZE_OK, 50), 5100);
    controller.onDecoderRecreated(5150);
    controller.onTick(5200);
    CHECK(host.sends() == 1);
    CHECK(controller.isAwaitingKeyframe());

    // Video is back: the waiting size goes out
    host.flowing = true;
    controller.onTick(5300);
    CHECK(host.sends() == 2);
    CHECK(host.requests[1].width == 1600 && host.requests[1].height == 900);

    // And never sooner than a second after the last request
    FakeHost quick;
    StreamResizeController fast(quick, 1920, 1080, 60);
    fast.onDrawableSize(1280, 720, 0);
    fast.onTick(600);
    fast.onDrawableSize(1600, 900, 650);
    fast.onResult(answerFrom(1, 1280, 720, 60, LI_STREAM_RESIZE_OK, 50), 700);
    fast.onDecoderRecreated(750);
    quick.flowing = true;
    fast.onTick(1599);
    CHECK(quick.sends() == 1);
    fast.onTick(1600);
    CHECK(quick.sends() == 2);
}

static void testVideoIsHeldFromTheRequestAndResumesWhereTheAnswerSays()
{
    FakeHost host;
    StreamResizeController controller(host, 1920, 1080, 60);

    controller.onDrawableSize(2560, 1080, 0);
    controller.onTick(600);
    CHECK(host.calls.size() >= 2 && host.calls[0] == "hold" && host.calls[1] == "send 2560x1080");

    controller.onResult(answerFrom(1, 2560, 1080, 60, LI_STREAM_RESIZE_OK, 4321), 900);
    CHECK(controller.isResetPending());
    CHECK(host.resumes.empty());

    controller.onDecoderRecreated(1000);

    // Video is let through again before a keyframe is asked for, from the
    // first frame of the new stream
    std::vector<std::string> expected = {"hold", "send 2560x1080", "apply 2560x1080@60", "recreate", "resume", "idr"};
    CHECK(host.calls == expected);
    CHECK(host.resumes.size() == 1 && host.resumes[0].fromFrame && host.resumes[0].firstFrame == 4321);
    CHECK(controller.isAwaitingKeyframe());

    host.flowing = true;
    controller.onTick(1100);
    CHECK(!controller.isAwaitingKeyframe());

    // Rebuilds with other causes are not this controller's
    controller.onDecoderRecreated(1200);
    CHECK(host.calls == expected);
}

static void testARollbackResumesAtTheStreamThatWasPutBack()
{
    FakeHost host;
    StreamResizeController controller(host, 1920, 1080, 60);

    controller.onDrawableSize(1280, 720, 0);
    controller.onTick(600);
    host.calls.clear();

    // The host rebuilt its encoder at the old size, and frames of the attempt
    // that failed can still be on their way, so the old decoder resumes at the
    // rebuilt encoder's first frame
    controller.onResult(answerFrom(1, 1920, 1080, 60, LI_STREAM_RESIZE_FAILED_ROLLED_BACK, 777), 700);
    std::vector<std::string> expected = {"resume", "idr"};
    CHECK(host.calls == expected);
    CHECK(host.resumes.size() == 1 && host.resumes[0].fromFrame && host.resumes[0].firstFrame == 777);
    CHECK(!controller.isResetPending());
    CHECK(controller.isEnabled());
}

static void testWithoutAFirstFrameAnyKeyframeWillDo()
{
    for (uint8_t status : {(uint8_t)LI_STREAM_RESIZE_FAILED_ROLLED_BACK, (uint8_t)LI_STREAM_RESIZE_REJECTED_INVALID, (uint8_t)LI_STREAM_RESIZE_SUPERSEDED}) {
        FakeHost host;
        StreamResizeController controller(host, 1920, 1080, 60);

        controller.onDrawableSize(1280, 720, 0);
        controller.onTick(600);

        // The host did not start encoding anew, so the old decoder only needs a keyframe
        controller.onResult(answer(1, 1920, 1080, 60, status), 700);
        CHECK(host.resumes.size() == 1 && !host.resumes[0].fromFrame);
        CHECK(!controller.isResetPending());
        CHECK(controller.isEnabled());
    }
}

static void testStaleAndRepeatedAnswersChangeNothing()
{
    FakeHost host;
    StreamResizeController controller(host, 1920, 1080, 60);

    // Nothing in flight
    controller.onResult(answer(1, 1280, 720, 60, LI_STREAM_RESIZE_OK), 0);
    CHECK(host.calls.empty());

    controller.onDrawableSize(1280, 720, 0);
    controller.onTick(600);
    host.calls.clear();

    // Some other request's answer: video stays held back for the real one
    controller.onResult(answer(5, 1024, 768, 60, LI_STREAM_RESIZE_OK), 650);
    CHECK(host.calls.empty());
    CHECK(controller.isRequestInFlight());

    controller.onResult(answerFrom(1, 1280, 720, 60, LI_STREAM_RESIZE_OK, 9), 700);
    controller.onDecoderRecreated(800);
    host.calls.clear();

    // The same answer again
    controller.onResult(answerFrom(1, 1280, 720, 60, LI_STREAM_RESIZE_OK, 9), 900);
    CHECK(host.calls.empty());
    CHECK(!controller.isResetPending());
}

static void testAFailedSizeIsNotAskedForAgainUntilTheWindowMoves()
{
    FakeHost host;
    StreamResizeController controller(host, 1920, 1080, 60);

    controller.onDrawableSize(1280, 720, 0);
    controller.onTick(600);
    controller.onResult(answer(1, 1920, 1080, 60, LI_STREAM_RESIZE_FAILED_ROLLED_BACK), 700);
    host.flowing = true;

    // The window still has the size that failed
    controller.onDrawableSize(1280, 720, 800);
    controller.onTick(5000);
    CHECK(host.sends() == 1);

    // Once it moves, even briefly, the size that failed may be asked for again
    controller.onDrawableSize(1440, 900, 6000);
    controller.onDrawableSize(1280, 720, 6100);
    controller.onTick(6700);
    CHECK(host.sends() == 2);
    CHECK(host.requests[1].width == 1280 && host.requests[1].height == 720);
}

static void testAHostThatCannotResizeIsNotAskedAgain()
{
    for (uint8_t status : {(uint8_t)LI_STREAM_RESIZE_REJECTED_UNSUPPORTED, (uint8_t)LI_STREAM_RESIZE_FAILED_SESSION_ENDING}) {
        FakeHost host;
        StreamResizeController controller(host, 1920, 1080, 60);

        controller.onDrawableSize(1280, 720, 0);
        controller.onTick(600);
        host.calls.clear();
        controller.onResult(answer(1, 1920, 1080, 60, status), 700);

        // Video still comes back
        std::vector<std::string> expected = {"resume", "idr", "stop"};
        CHECK(host.calls == expected);
        CHECK(!controller.isEnabled());

        controller.onDrawableSize(1600, 900, 800);
        host.flowing = true;
        controller.onTick(5000);
        CHECK(host.sends() == 1);
        CHECK(!controller.isAwaitingKeyframe());
    }
}

static void testAFailureAtAnotherSizeStillRebuilds()
{
    FakeHost host;
    StreamResizeController controller(host, 1920, 1080, 60);

    controller.onDrawableSize(1280, 720, 0);
    controller.onTick(600);
    host.calls.clear();

    // Whatever was asked for, the host says what it streams
    controller.onResult(answerFrom(1, 1600, 900, 60, LI_STREAM_RESIZE_FAILED_ROLLED_BACK, 31), 700);
    std::vector<std::string> expected = {"apply 1600x900@60", "recreate"};
    CHECK(host.calls == expected);
    CHECK(controller.isResetPending());
}

static void testARequestThatCouldNotBeSentGoesBackToTheOldVideoAndStops()
{
    for (int err : {-1, (int)LI_ERR_UNSUPPORTED}) {
        FakeHost host;
        host.sendResult = err;
        StreamResizeController controller(host, 1920, 1080, 60);

        controller.onDrawableSize(1280, 720, 0);
        controller.onTick(600);

        // The host never saw it, so nothing is rebuilt, and nothing is retried behind the user's back
        std::vector<std::string> expected = {"hold", "send 1280x720", "resume", "idr", "stop"};
        CHECK(host.calls == expected);
        CHECK(host.resumes.size() == 1 && !host.resumes[0].fromFrame);
        CHECK(!controller.isEnabled());
        CHECK(!controller.isRequestInFlight());

        host.sendResult = 0;
        host.flowing = true;
        controller.onDrawableSize(1600, 900, 700);
        controller.onTick(5000);
        CHECK(host.sends() == 0);
    }
}

static void testAnAnswerThatNeverComesEndsTheSession()
{
    FakeHost host;
    StreamResizeController controller(host, 1920, 1080, 60);

    // The host may take this long even for a rollback that runs every step to its limit
    CHECK(StreamResizeController::k_AnswerTimeoutMs > LI_STREAM_RESIZE_ANSWER_WITHIN_MS);

    controller.onDrawableSize(1280, 720, 0);
    controller.onTick(600);
    CHECK(!host.ticks.empty() && host.ticks.back() == StreamResizeController::k_AnswerTimeoutMs);

    controller.onTick(600 + StreamResizeController::k_AnswerTimeoutMs - 1);
    CHECK(host.sessionsEnded == 0);

    controller.onTick(600 + StreamResizeController::k_AnswerTimeoutMs);
    CHECK(host.sessionsEnded == 1);
    CHECK(!controller.isEnabled());

    // Once
    controller.onTick(600 + 2 * StreamResizeController::k_AnswerTimeoutMs);
    CHECK(host.sessionsEnded == 1);

    // An answer that turns up afterwards changes nothing
    host.calls.clear();
    controller.onResult(answer(1, 1280, 720, 60, LI_STREAM_RESIZE_OK), 3 * StreamResizeController::k_AnswerTimeoutMs);
    CHECK(host.calls.empty());
}

static void testASlowAnswerIsStillTaken()
{
    FakeHost host;
    StreamResizeController controller(host, 1920, 1080, 60);

    controller.onDrawableSize(1280, 720, 0);
    controller.onTick(600);

    // The host used all the time it has, and its answer took a while to get here
    uint64_t answeredMs = 600 + LI_STREAM_RESIZE_ANSWER_WITHIN_MS + StreamResizeController::k_AnswerTravelMs - 1;
    controller.onTick(answeredMs);
    controller.onResult(answerFrom(1, 1920, 1080, 60, LI_STREAM_RESIZE_FAILED_ROLLED_BACK, 12), answeredMs);
    CHECK(host.sessionsEnded == 0);
    CHECK(controller.isAwaitingKeyframe());
    CHECK(host.resumes.size() == 1 && host.resumes[0].firstFrame == 12);
}

static void testARebuildThatNeverHappensEndsTheSession()
{
    FakeHost host;
    StreamResizeController controller(host, 1920, 1080, 60);

    controller.onDrawableSize(1280, 720, 0);
    controller.onTick(600);
    controller.onResult(answerFrom(1, 1280, 720, 60, LI_STREAM_RESIZE_OK, 5), 700);
    CHECK(controller.isResetPending());
    CHECK(!host.ticks.empty() && host.ticks.back() == StreamResizeController::k_RebuildTimeoutMs);

    controller.onTick(700 + StreamResizeController::k_RebuildTimeoutMs - 1);
    CHECK(host.sessionsEnded == 0);
    controller.onTick(700 + StreamResizeController::k_RebuildTimeoutMs);
    CHECK(host.sessionsEnded == 1);

    // A rebuild that comes too late does not let video through
    controller.onDecoderRecreated(700 + StreamResizeController::k_RebuildTimeoutMs + 1);
    CHECK(host.resumes.empty());
}

static void testVideoThatDoesNotComeBackIsAskedForAgainThenEndsTheSession()
{
    FakeHost host;
    StreamResizeController controller(host, 1920, 1080, 60);

    controller.onDrawableSize(1280, 720, 0);
    controller.onTick(600);
    controller.onResult(answerFrom(1, 1280, 720, 60, LI_STREAM_RESIZE_OK, 5), 700);
    controller.onDecoderRecreated(1000);
    CHECK(host.count("idr") == 1);

    // A keyframe is asked for again every retry interval, however often the loop wakes
    for (uint64_t now = 1000; now < 1000 + StreamResizeController::k_KeyframeTimeoutMs; now += 50) {
        controller.onTick(now);
    }
    CHECK(host.count("idr") == (int)(StreamResizeController::k_KeyframeTimeoutMs / StreamResizeController::k_KeyframeRetryMs));
    CHECK(host.sessionsEnded == 0);

    controller.onTick(1000 + StreamResizeController::k_KeyframeTimeoutMs);
    CHECK(host.sessionsEnded == 1);

    // Video that did come back in time is the end of it
    FakeHost later;
    StreamResizeController back(later, 1920, 1080, 60);
    back.onDrawableSize(1280, 720, 0);
    back.onTick(600);
    back.onResult(answerFrom(1, 1280, 720, 60, LI_STREAM_RESIZE_OK, 5), 700);
    back.onDecoderRecreated(1000);
    back.onTick(2500);
    later.flowing = true;
    back.onTick(2600);
    CHECK(!back.isAwaitingKeyframe());
    back.onTick(1000 + StreamResizeController::k_KeyframeTimeoutMs * 2);
    CHECK(later.sessionsEnded == 0);
}

static void testTheLoopWakingOftenDoesNotReplaceTheTimer()
{
    FakeHost host;
    StreamResizeController controller(host, 1920, 1080, 60);

    controller.onDrawableSize(1280, 720, 0);
    controller.onTick(600);
    size_t ticks = host.ticks.size();

    // The main loop calls onTick() for every event, like mouse motion
    for (uint64_t now = 601; now < 5000; now++) {
        controller.onTick(now);
    }
    CHECK(host.ticks.size() == ticks);
}

static void testFullscreenIsNotFollowed()
{
    FakeHost host;
    StreamResizeController controller(host, 1920, 1080, 60);

    // A size the window had before going fullscreen is dropped
    controller.onDrawableSize(1280, 720, 0);
    controller.setWindowed(false, 100);
    controller.onTick(700);
    CHECK(host.sends() == 0);

    // Fullscreen sizes are not asked for
    controller.onDrawableSize(2560, 1440, 800);
    controller.onTick(5000);
    CHECK(host.sends() == 0);
    CHECK(host.calls.empty());

    // Back in a window, nothing from before or during fullscreen is asked for
    controller.setWindowed(true, 5500);
    controller.onTick(5900);
    CHECK(host.sends() == 0);

    // Following starts again from the window's size
    controller.onDrawableSize(1600, 900, 6000);
    controller.onTick(6599);
    CHECK(host.sends() == 0);
    controller.onTick(6600);
    CHECK(host.sends() == 1);
    CHECK(host.requests[0].width == 1600 && host.requests[0].height == 900);
}

static void testAnAnswerThatArrivesInFullscreenIsStillApplied()
{
    FakeHost host;
    StreamResizeController controller(host, 1920, 1080, 60);

    controller.onDrawableSize(1280, 720, 0);
    controller.onTick(600);

    // The host acts on the request whatever the window does meanwhile
    controller.setWindowed(false, 650);
    controller.onResult(answerFrom(1, 1280, 720, 60, LI_STREAM_RESIZE_OK, 3), 700);
    CHECK(controller.isResetPending());
    controller.onDecoderRecreated(800);
    CHECK(host.calls.back() == "idr");

    // Nothing more is asked for while fullscreen
    host.flowing = true;
    controller.onDrawableSize(2560, 1440, 900);
    controller.onTick(10000);
    CHECK(host.sends() == 1);
}

static void testAnAutoStreamStartsAtItsWindowOrScreen()
{
    const StreamSize desktop = {2560, 1440};
    const StreamSize usable = {2560, 1392};

    // Fullscreen takes the screen
    StreamSize size = chooseAutoStreamSize(true, desktop, usable, {1600, 900});
    CHECK(size.width == 2560 && size.height == 1440);

    // A window starts at the size it was left at
    size = chooseAutoStreamSize(false, desktop, usable, {1600, 900});
    CHECK(size.width == 1600 && size.height == 900);

    // Unless that no longer fits the screen, or there is none yet
    size = chooseAutoStreamSize(false, desktop, usable, {3000, 1000});
    CHECK(size.width == 2048 && size.height == 1112);
    size = chooseAutoStreamSize(false, desktop, usable, {0, 0});
    CHECK(size.width == 2048 && size.height == 1112);

    // Always even, and within what a stream can be resized to
    size = chooseAutoStreamSize(false, desktop, usable, {1601, 901});
    CHECK(size.width == 1600 && size.height == 900);
    size = chooseAutoStreamSize(false, desktop, usable, {320, 200});
    CHECK(size.width == LI_STREAM_RESIZE_MIN_WIDTH && size.height == LI_STREAM_RESIZE_MIN_HEIGHT);
    size = chooseAutoStreamSize(true, {10240, 4320}, usable, {0, 0});
    CHECK(size.width == LI_STREAM_RESIZE_MAX_WIDTH && size.height == 4320);

    // Fullscreen without a known screen size goes by the window, and with
    // nothing known there is no size
    size = chooseAutoStreamSize(true, {0, 0}, usable, {1600, 900});
    CHECK(size.width == 1600 && size.height == 900);
    size = chooseAutoStreamSize(false, {0, 0}, {0, 0}, {0, 0});
    CHECK(size.width == 0 && size.height == 0);
}

namespace {

// The controller in front of moonlight-common-c's video path, with a decoder
// that takes frames the way the FFmpeg decoder does and is made for one size
// at a time
struct LibraryHost : FakeHost
{
    int decoderWidth = 1920;
    int decoderHeight = 1080;

    struct Decoded { uint32_t frame; bool idr; int width; int height; int decoderWidth; };
    std::vector<Decoded> decoded;

    void holdVideo() override
    {
        FakeHost::holdVideo();
        LiHoldVideo();
    }

    void resumeVideo(bool fromFrame, uint32_t firstFrame) override
    {
        FakeHost::resumeVideo(fromFrame, firstFrame);
        LiResumeVideo(fromFrame, firstFrame);
    }

    bool isVideoFlowing() override
    {
        return LiIsVideoFlowing();
    }

    void applyStreamSize(int width, int height, int fps) override
    {
        FakeHost::applyStreamSize(width, height, fps);
        decoderWidth = width;
        decoderHeight = height;
    }

    void decodeAll()
    {
        uint32_t frame;
        bool idr;
        uint16_t width, height;
        while (videoPathDecodeNext(&frame, &idr, &width, &height)) {
            decoded.push_back({frame, idr, width, height, decoderWidth});
        }
    }

    bool everyFrameFitItsDecoder() const
    {
        for (const Decoded& d : decoded) {
            if (d.width != d.decoderWidth) {
                return false;
            }
        }
        return true;
    }
};

}

static void testALateKeyframeOfTheOldSizeNeverReachesTheNewDecoder()
{
    LibraryHost host;
    StreamResizeController controller(host, 1920, 1080, 60);
    videoPathStart();

    videoPathSendFrame(1, true, 1920, 1080);
    videoPathSendFrame(2, false, 1920, 1080);
    host.decodeAll();

    controller.onDrawableSize(2560, 1440, 0);
    controller.onTick(600);
    CHECK(controller.isRequestInFlight());

    // Before the answer: the old stream, a keyframe of it the host sent for
    // an earlier request, and the start of the new one
    videoPathSendFrame(3, false, 1920, 1080);
    videoPathSendFrame(4, true, 1920, 1080);
    videoPathSendFrame(5, false, 1920, 1080);
    host.decodeAll();

    // The host switched at frame 7
    controller.onResult(answerFrom(1, 2560, 1440, 60, LI_STREAM_RESIZE_OK, 7), 700);

    // While the decoder is rebuilt nobody takes frames, so they pile up
    videoPathSendFrame(6, true, 1920, 1080);
    videoPathSendFrame(7, true, 2560, 1440);
    videoPathSendFrame(8, false, 2560, 1440);
    controller.onDecoderRecreated(800);

    // Late, but still from before the switch
    host.decodeAll();
    CHECK(controller.isAwaitingKeyframe());
    controller.onTick(850);
    CHECK(!controller.isAwaitingKeyframe());

    videoPathSendFrame(9, false, 2560, 1440);
    host.decodeAll();

    CHECK(host.everyFrameFitItsDecoder());
    CHECK(host.decoded.size() == 5);
    if (host.decoded.size() == 5) {
        CHECK(host.decoded[0].frame == 1 && host.decoded[1].frame == 2);
        CHECK(host.decoded[2].frame == 7 && host.decoded[2].idr && host.decoded[2].width == 2560);
        CHECK(host.decoded[3].frame == 8 && host.decoded[4].frame == 9);
    }

    videoPathStop();
}

static void testARollbackNeverShowsTheAttemptThatFailed()
{
    LibraryHost host;
    StreamResizeController controller(host, 1920, 1080, 60);
    videoPathStart();

    videoPathSendFrame(1, true, 1920, 1080);
    host.decodeAll();

    controller.onDrawableSize(2560, 1440, 0);
    controller.onTick(600);

    // The host got as far as encoding the new size, then put the old one back from frame 6
    videoPathSendFrame(2, false, 1920, 1080);
    videoPathSendFrame(3, true, 2560, 1440);
    videoPathSendFrame(4, false, 2560, 1440);
    controller.onResult(answerFrom(1, 1920, 1080, 60, LI_STREAM_RESIZE_FAILED_ROLLED_BACK, 6), 700);
    videoPathSendFrame(5, true, 2560, 1440);
    videoPathSendFrame(6, true, 1920, 1080);
    host.decodeAll();
    controller.onTick(800);
    CHECK(!controller.isAwaitingKeyframe());

    CHECK(host.everyFrameFitItsDecoder());
    CHECK(host.decoded.size() == 2 && host.decoded.back().frame == 6);

    videoPathStop();
}

int main()
{
    testWaitsForTheWindowToSettle();
    testADragIsAskedForOnceItStops();
    testSizesAreMadeValidOrNotAskedFor();
    testTheSizeTheStreamHasIsNotAskedFor();
    testOneResizeAtATimeAndAtMostOneRequestPerInterval();
    testVideoIsHeldFromTheRequestAndResumesWhereTheAnswerSays();
    testARollbackResumesAtTheStreamThatWasPutBack();
    testWithoutAFirstFrameAnyKeyframeWillDo();
    testStaleAndRepeatedAnswersChangeNothing();
    testAFailedSizeIsNotAskedForAgainUntilTheWindowMoves();
    testAHostThatCannotResizeIsNotAskedAgain();
    testAFailureAtAnotherSizeStillRebuilds();
    testARequestThatCouldNotBeSentGoesBackToTheOldVideoAndStops();
    testAnAnswerThatNeverComesEndsTheSession();
    testASlowAnswerIsStillTaken();
    testARebuildThatNeverHappensEndsTheSession();
    testVideoThatDoesNotComeBackIsAskedForAgainThenEndsTheSession();
    testTheLoopWakingOftenDoesNotReplaceTheTimer();
    testFullscreenIsNotFollowed();
    testAnAnswerThatArrivesInFullscreenIsStillApplied();
    testAnAutoStreamStartsAtItsWindowOrScreen();
    testALateKeyframeOfTheOldSizeNeverReachesTheNewDecoder();
    testARollbackNeverShowsTheAttemptThatFailed();

    if (failures != 0) {
        std::fprintf(stderr, "%d check(s) failed\n", failures);
        return 1;
    }

    std::printf("stream resize controller tests passed\n");
    return 0;
}
