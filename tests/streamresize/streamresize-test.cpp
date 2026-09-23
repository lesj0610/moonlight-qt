// Tests for StreamResizeController: when the window is asked for, which
// answers are taken, and the order a decoder rebuild happens in.

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

struct FakeHost : StreamResizeController::Host
{
    std::vector<std::string> calls;
    std::vector<uint32_t> ticks;
    int sendResult = 0;
    uint32_t nextId = 1;
    bool blocked = false;

    struct Request { int width, height, fps; uint32_t id; };
    std::vector<Request> requests;

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

    void setDecoderInputBlocked(bool block) override
    {
        blocked = block;
        calls.push_back(block ? "block" : "unblock");
    }

    void applyStreamSize(int width, int height, int fps) override
    {
        calls.push_back("apply " + std::to_string(width) + "x" + std::to_string(height) + "@" + std::to_string(fps));
    }

    void recreateDecoder() override
    {
        calls.push_back("recreate");
    }

    void requestIdrFrame() override
    {
        calls.push_back("idr");
    }

    void scheduleTick(uint32_t delayMs) override
    {
        ticks.push_back(delayMs);
    }

    int sends() const
    {
        return (int)requests.size();
    }
};

}

static void testWaitsForTheWindowToSettle()
{
    FakeHost host;
    StreamResizeController controller(host, 1920, 1080, 60, true);

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
    StreamResizeController controller(host, 1920, 1080, 60, true);

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
    StreamResizeController controller(host, 1920, 1080, 60, true);

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
    StreamResizeController controller(host, 1920, 1080, 60, true);

    controller.onDrawableSize(1920, 1080, 0);
    controller.onTick(1000);
    CHECK(host.sends() == 0);
}

static void testOneRequestAtATimeAndAtMostOnePerInterval()
{
    FakeHost host;
    StreamResizeController controller(host, 1920, 1080, 60, true);

    controller.onDrawableSize(1280, 720, 0);
    controller.onTick(600);
    CHECK(host.sends() == 1);

    // The window changes while the host is still switching
    controller.onDrawableSize(1600, 900, 650);
    controller.onTick(5000);
    CHECK(host.sends() == 1);

    // Answered: the waiting size goes out, but not before a second has passed since the last one
    controller.onResult(1, 1280, 720, 60, LI_STREAM_RESIZE_OK, 700);
    controller.onDecoderRecreated(750);
    controller.onTick(1599);
    CHECK(host.sends() == 1);
    controller.onTick(1600);
    CHECK(host.sends() == 2);
    CHECK(host.requests[1].width == 1600 && host.requests[1].height == 900);
}

static void testTheDecoderIsRebuiltInOrder()
{
    FakeHost host;
    StreamResizeController controller(host, 1920, 1080, 60, true);

    controller.onDrawableSize(2560, 1080, 0);
    controller.onTick(600);
    host.calls.clear();

    controller.onResult(1, 2560, 1080, 60, LI_STREAM_RESIZE_OK, 700);

    // Frames are held back before anything changes, and stay held back
    // while the decoder is rebuilt.
    std::vector<std::string> expected = {"block", "apply 2560x1080@60", "recreate"};
    CHECK(host.calls == expected);
    CHECK(host.blocked);
    CHECK(controller.isResetPending());

    // An unrelated answer during the rebuild does not let frames through
    controller.onResult(77, 1920, 1080, 60, LI_STREAM_RESIZE_OK, 710);
    CHECK(host.blocked);

    controller.onDecoderRecreated(800);

    // The keyframe is asked for before frames are let through again
    expected = {"block", "apply 2560x1080@60", "recreate", "idr", "unblock"};
    CHECK(host.calls == expected);
    CHECK(!host.blocked);
    CHECK(!controller.isResetPending());

    // Rebuilds with other causes are not this controller's
    controller.onDecoderRecreated(900);
    CHECK(host.calls == expected);
}

static void testStaleAndRepeatedAnswersAreIgnored()
{
    FakeHost host;
    StreamResizeController controller(host, 1920, 1080, 60, true);

    // Nothing in flight
    controller.onResult(1, 1280, 720, 60, LI_STREAM_RESIZE_OK, 0);
    CHECK(!controller.isResetPending());
    CHECK(host.calls.size() == 1 && host.calls[0] == "unblock");

    controller.onDrawableSize(1280, 720, 0);
    controller.onTick(600);
    host.calls.clear();

    // Some other request's answer
    controller.onResult(5, 1024, 768, 60, LI_STREAM_RESIZE_OK, 650);
    CHECK(!controller.isResetPending());
    CHECK(host.calls.size() == 1 && host.calls[0] == "unblock");

    controller.onResult(1, 1280, 720, 60, LI_STREAM_RESIZE_OK, 700);
    CHECK(controller.isResetPending());
    controller.onDecoderRecreated(800);
    host.calls.clear();

    // The same answer again
    controller.onResult(1, 1280, 720, 60, LI_STREAM_RESIZE_OK, 900);
    CHECK(!controller.isResetPending());
    CHECK(host.calls.size() == 1 && host.calls[0] == "unblock");
}

static void testAFailedSizeIsNotAskedForAgainUntilTheWindowMoves()
{
    FakeHost host;
    StreamResizeController controller(host, 1920, 1080, 60, true);

    controller.onDrawableSize(1280, 720, 0);
    controller.onTick(600);
    host.calls.clear();

    // The host put the old stream back, so nothing is rebuilt
    controller.onResult(1, 1920, 1080, 60, LI_STREAM_RESIZE_FAILED_ROLLED_BACK, 700);
    CHECK(host.calls.size() == 1 && host.calls[0] == "unblock");
    CHECK(!controller.isResetPending());

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
        StreamResizeController controller(host, 1920, 1080, 60, true);

        controller.onDrawableSize(1280, 720, 0);
        controller.onTick(600);
        controller.onResult(1, 1920, 1080, 60, status, 700);
        CHECK(!controller.isEnabled());

        controller.onDrawableSize(1600, 900, 800);
        controller.onTick(5000);
        CHECK(host.sends() == 1);
    }

    FakeHost host;
    host.sendResult = LI_ERR_UNSUPPORTED;
    StreamResizeController controller(host, 1920, 1080, 60, true);
    controller.onDrawableSize(1280, 720, 0);
    controller.onTick(600);
    CHECK(!controller.isEnabled());

    FakeHost off;
    StreamResizeController disabled(off, 1920, 1080, 60, false);
    disabled.onDrawableSize(1280, 720, 0);
    disabled.onTick(600);
    CHECK(off.calls.empty());
}

static void testAFailureAtAnotherSizeStillRebuilds()
{
    FakeHost host;
    StreamResizeController controller(host, 1920, 1080, 60, true);

    controller.onDrawableSize(1280, 720, 0);
    controller.onTick(600);
    host.calls.clear();

    // Whatever was asked for, the host says what it streams
    controller.onResult(1, 1600, 900, 60, LI_STREAM_RESIZE_FAILED_ROLLED_BACK, 700);
    std::vector<std::string> expected = {"block", "apply 1600x900@60", "recreate"};
    CHECK(host.calls == expected);
}

static void testASendThatFailsIsRetried()
{
    FakeHost host;
    host.sendResult = -1;
    StreamResizeController controller(host, 1920, 1080, 60, true);

    controller.onDrawableSize(1280, 720, 0);
    controller.onTick(600);
    CHECK(host.sends() == 0);
    CHECK(controller.isEnabled());
    CHECK(!host.ticks.empty() && host.ticks.back() == StreamResizeController::k_MinIntervalMs);

    host.sendResult = 0;
    controller.onTick(1599);
    CHECK(host.sends() == 0);
    controller.onTick(1600);
    CHECK(host.sends() == 1);
}

int main()
{
    testWaitsForTheWindowToSettle();
    testADragIsAskedForOnceItStops();
    testSizesAreMadeValidOrNotAskedFor();
    testTheSizeTheStreamHasIsNotAskedFor();
    testOneRequestAtATimeAndAtMostOnePerInterval();
    testTheDecoderIsRebuiltInOrder();
    testStaleAndRepeatedAnswersAreIgnored();
    testAFailedSizeIsNotAskedForAgainUntilTheWindowMoves();
    testAHostThatCannotResizeIsNotAskedAgain();
    testAFailureAtAnotherSizeStillRebuilds();
    testASendThatFailsIsRetried();

    if (failures != 0) {
        std::fprintf(stderr, "%d check(s) failed\n", failures);
        return 1;
    }

    std::printf("stream resize controller tests passed\n");
    return 0;
}
