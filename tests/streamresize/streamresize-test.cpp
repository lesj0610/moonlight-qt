// Tests for StreamResizeController and DecodeGate: when the window is asked
// for, which answers are taken, and what reaches the decoder while the stream
// changes size.

#include "streamresize.h"

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

// A host that records what it is asked to do, with a real gate in front of a
// decoder that counts what it is given.
struct FakeHost : StreamResizeController::Host
{
    std::vector<std::string> calls;
    std::vector<uint32_t> ticks;
    int sendResult = 0;
    uint32_t nextId = 1;

    struct Request { int width, height, fps; uint32_t id; };
    std::vector<Request> requests;

    DecodeGate gate;
    int decoder = 1;                // Which decoder is current; bumped by a rebuild
    struct Decoded { int decoder; bool keyframe; int width; };
    std::vector<Decoded> decoded;   // What reached a decoder

    int sessionsEnded = 0;
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

    void blockVideo() override
    {
        gate.block();
        calls.push_back("block");
    }

    void resumeVideoAtKeyframe() override
    {
        gate.resumeAtKeyframe();
        calls.push_back("resume");
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
        decoder++;
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

    void endSession(const char*) override
    {
        sessionsEnded++;
        calls.push_back("end");
    }

    // A decode unit arrives from the network
    void frame(bool keyframe, int width)
    {
        if (gate.admit(keyframe)) {
            decoded.push_back({decoder, keyframe, width});
        }
    }

    int sends() const
    {
        return (int)requests.size();
    }
};

}

static void testTheGateDropsUntilAKeyframeOnceResumed()
{
    DecodeGate gate;
    CHECK(gate.isOpen());
    CHECK(gate.admit(false));

    gate.block();
    CHECK(!gate.admit(false));
    CHECK(!gate.admit(true));

    gate.resumeAtKeyframe();
    CHECK(!gate.admit(false));
    CHECK(gate.admit(true));
    CHECK(gate.isOpen());
    CHECK(gate.admit(false));

    // Blocked again before the keyframe came: the keyframe is dropped too
    gate.block();
    gate.resumeAtKeyframe();
    gate.block();
    CHECK(!gate.admit(true));
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
    controller.onResult(1, 1280, 720, 60, LI_STREAM_RESIZE_OK, 700);
    controller.onDecoderRecreated(800);

    // Larger than the host allows is clamped
    controller.onDrawableSize(9000, 5001, 1000);
    controller.onTick(2000);
    CHECK(host.sends() == 2 && host.requests[1].width == LI_STREAM_RESIZE_MAX_WIDTH && host.requests[1].height == LI_STREAM_RESIZE_MAX_HEIGHT);
    controller.onResult(2, LI_STREAM_RESIZE_MAX_WIDTH, LI_STREAM_RESIZE_MAX_HEIGHT, 60, LI_STREAM_RESIZE_OK, 2100);
    controller.onDecoderRecreated(2200);

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

static void testOneRequestAtATimeAndAtMostOnePerInterval()
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

    // Answered: the waiting size goes out, but not before a second has passed since the last one
    controller.onResult(1, 1280, 720, 60, LI_STREAM_RESIZE_OK, 5100);
    controller.onDecoderRecreated(5150);
    controller.onTick(5150);
    CHECK(host.sends() == 2);
    CHECK(host.requests[1].width == 1600 && host.requests[1].height == 900);

    FakeHost quick;
    StreamResizeController fast(quick, 1920, 1080, 60);
    fast.onDrawableSize(1280, 720, 0);
    fast.onTick(600);
    fast.onDrawableSize(1600, 900, 650);
    fast.onResult(1, 1280, 720, 60, LI_STREAM_RESIZE_OK, 700);
    fast.onDecoderRecreated(750);
    fast.onTick(1599);
    CHECK(quick.sends() == 1);
    fast.onTick(1600);
    CHECK(quick.sends() == 2);
}

static void testVideoIsHeldBackFromTheRequestUntilTheNewDecoderHasAKeyframe()
{
    FakeHost host;
    StreamResizeController controller(host, 1920, 1080, 60);

    // The old stream plays until the request goes out
    host.frame(true, 1920);
    host.frame(false, 1920);

    controller.onDrawableSize(2560, 1080, 0);
    controller.onTick(600);
    CHECK(host.calls.size() >= 2 && host.calls[0] == "block" && host.calls[1] == "send 2560x1080");

    // The host switches, and its new frames arrive before its answer does
    host.frame(false, 1920);
    host.frame(true, 2560);
    host.frame(false, 2560);
    host.frame(false, 2560);

    controller.onResult(1, 2560, 1080, 60, LI_STREAM_RESIZE_OK, 900);

    // Still held back while the decoder is rebuilt
    host.frame(false, 2560);
    host.frame(true, 2560);
    CHECK(controller.isResetPending());

    controller.onDecoderRecreated(1000);

    // The gate waits for a keyframe before one is asked for
    std::vector<std::string> expected = {"block", "send 2560x1080", "apply 2560x1080@60", "recreate", "resume", "idr"};
    CHECK(host.calls == expected);

    host.frame(false, 2560);
    host.frame(true, 2560);
    host.frame(false, 2560);

    // Nothing of the new size reached the old decoder, and the new decoder
    // started with a keyframe of its size.
    CHECK(host.decoded.size() == 4);
    CHECK(host.decoded[0].decoder == 1 && host.decoded[0].width == 1920);
    CHECK(host.decoded[1].decoder == 1 && host.decoded[1].width == 1920);
    CHECK(host.decoded[2].decoder == 2 && host.decoded[2].keyframe && host.decoded[2].width == 2560);
    CHECK(host.decoded[3].decoder == 2 && !host.decoded[3].keyframe);

    // Rebuilds with other causes are not this controller's
    controller.onDecoderRecreated(1100);
    CHECK(host.calls == expected);
}

static void testStaleAndRepeatedAnswersChangeNothing()
{
    FakeHost host;
    StreamResizeController controller(host, 1920, 1080, 60);

    // Nothing in flight
    controller.onResult(1, 1280, 720, 60, LI_STREAM_RESIZE_OK, 0);
    CHECK(host.calls.empty());
    CHECK(host.gate.isOpen());

    controller.onDrawableSize(1280, 720, 0);
    controller.onTick(600);
    host.calls.clear();

    // Some other request's answer: video stays held back for the real one
    controller.onResult(5, 1024, 768, 60, LI_STREAM_RESIZE_OK, 650);
    CHECK(host.calls.empty());
    CHECK(!host.gate.isOpen());
    CHECK(controller.isRequestInFlight());

    controller.onResult(1, 1280, 720, 60, LI_STREAM_RESIZE_OK, 700);
    controller.onDecoderRecreated(800);
    host.calls.clear();

    // The same answer again
    controller.onResult(1, 1280, 720, 60, LI_STREAM_RESIZE_OK, 900);
    CHECK(host.calls.empty());
    CHECK(!controller.isResetPending());
}

static void testAFailedResizeGoesBackToTheOldVideo()
{
    for (uint8_t status : {(uint8_t)LI_STREAM_RESIZE_FAILED_ROLLED_BACK, (uint8_t)LI_STREAM_RESIZE_REJECTED_INVALID, (uint8_t)LI_STREAM_RESIZE_SUPERSEDED}) {
        FakeHost host;
        StreamResizeController controller(host, 1920, 1080, 60);

        controller.onDrawableSize(1280, 720, 0);
        controller.onTick(600);
        host.frame(false, 1920);
        host.calls.clear();

        // The host kept the old size, so the old decoder stays and only needs a keyframe
        controller.onResult(1, 1920, 1080, 60, status, 700);
        std::vector<std::string> expected = {"resume", "idr"};
        CHECK(host.calls == expected);
        CHECK(!controller.isResetPending());
        CHECK(controller.isEnabled());

        host.frame(false, 1920);
        host.frame(true, 1920);
        CHECK(host.decoded.size() == 1 && host.decoded[0].keyframe && host.decoded[0].decoder == 1);
    }
}

static void testAFailedSizeIsNotAskedForAgainUntilTheWindowMoves()
{
    FakeHost host;
    StreamResizeController controller(host, 1920, 1080, 60);

    controller.onDrawableSize(1280, 720, 0);
    controller.onTick(600);
    controller.onResult(1, 1920, 1080, 60, LI_STREAM_RESIZE_FAILED_ROLLED_BACK, 700);

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
        controller.onResult(1, 1920, 1080, 60, status, 700);

        std::vector<std::string> expected = {"resume", "idr", "stop"};
        CHECK(host.calls == expected);
        CHECK(!controller.isEnabled());

        controller.onDrawableSize(1600, 900, 800);
        controller.onTick(5000);
        CHECK(host.sends() == 1);
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
    controller.onResult(1, 1600, 900, 60, LI_STREAM_RESIZE_FAILED_ROLLED_BACK, 700);
    std::vector<std::string> expected = {"apply 1600x900@60", "recreate"};
    CHECK(host.calls == expected);
    CHECK(!host.gate.isOpen());
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
        std::vector<std::string> expected = {"block", "send 1280x720", "resume", "idr", "stop"};
        CHECK(host.calls == expected);
        CHECK(!controller.isEnabled());
        CHECK(!controller.isRequestInFlight());

        host.sendResult = 0;
        controller.onDrawableSize(1600, 900, 700);
        controller.onTick(5000);
        CHECK(host.sends() == 0);

        host.frame(true, 1920);
        CHECK(host.decoded.size() == 1 && host.decoded[0].decoder == 1);
    }
}

static void testAnAnswerThatNeverComesEndsTheSession()
{
    FakeHost host;
    StreamResizeController controller(host, 1920, 1080, 60);

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
    controller.onResult(1, 1280, 720, 60, LI_STREAM_RESIZE_OK, 40000);
    CHECK(host.calls.empty());
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
    controller.onResult(1, 1280, 720, 60, LI_STREAM_RESIZE_OK, 700);
    CHECK(controller.isResetPending());
    controller.onDecoderRecreated(800);
    CHECK(host.calls.back() == "idr");

    host.frame(true, 1280);
    CHECK(host.decoded.size() == 1 && host.decoded[0].decoder == 2);

    // Nothing more is asked for while fullscreen
    controller.onDrawableSize(2560, 1440, 900);
    controller.onTick(10000);
    CHECK(host.sends() == 1);
}

int main()
{
    testTheGateDropsUntilAKeyframeOnceResumed();
    testWaitsForTheWindowToSettle();
    testADragIsAskedForOnceItStops();
    testSizesAreMadeValidOrNotAskedFor();
    testTheSizeTheStreamHasIsNotAskedFor();
    testOneRequestAtATimeAndAtMostOnePerInterval();
    testVideoIsHeldBackFromTheRequestUntilTheNewDecoderHasAKeyframe();
    testStaleAndRepeatedAnswersChangeNothing();
    testAFailedResizeGoesBackToTheOldVideo();
    testAFailedSizeIsNotAskedForAgainUntilTheWindowMoves();
    testAHostThatCannotResizeIsNotAskedAgain();
    testAFailureAtAnotherSizeStillRebuilds();
    testARequestThatCouldNotBeSentGoesBackToTheOldVideoAndStops();
    testAnAnswerThatNeverComesEndsTheSession();
    testFullscreenIsNotFollowed();
    testAnAnswerThatArrivesInFullscreenIsStillApplied();

    if (failures != 0) {
        std::fprintf(stderr, "%d check(s) failed\n", failures);
        return 1;
    }

    std::printf("stream resize controller tests passed\n");
    return 0;
}
