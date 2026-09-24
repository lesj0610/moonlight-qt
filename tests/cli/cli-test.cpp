// Tests that a resolution or bitrate given on the command line is what the
// stream gets, whatever was saved, that the saved preferences stay as they
// are, and that a bitrate set by hand is kept when YUV 4:4:4 is not there.

#include "cli/commandlineparser.h"
#include "settings/streamingpreferences.h"
#include "utils.h"

#include <QCoreApplication>
#include <QSettings>
#include <QTemporaryDir>

#include <cstdio>

// The preferences ask these only to pick a default fullscreen mode
bool WMUtils::isRunningWayland() { return false; }
bool WMUtils::isGpuSlow() { return false; }

static int failures;

#define CHECK(cond)                                                          \
    do {                                                                     \
        if (!(cond)) {                                                       \
            std::fprintf(stderr, "%s:%d: CHECK(%s)\n", __FILE__, __LINE__, #cond); \
            failures++;                                                      \
        }                                                                    \
    } while (0)

static StreamingPreferences* parseStream(const QStringList& options)
{
    StreamingPreferences* preferences = StreamingPreferences::get();
    preferences->reload();

    StreamCommandLineParser parser;
    parser.parse(QStringList({"moonlight", "stream", "host", "app"}) + options, preferences);
    return preferences;
}

static void saveAuto()
{
    QSettings settings;
    settings.setValue("autoresolution", true);
    settings.setValue("autoadjustbitrate", true);
    settings.setValue("width", 3840);
    settings.setValue("height", 2160);
}

static void testAResolutionGivenIsKeptOverASavedAuto()
{
    saveAuto();

    StreamingPreferences* preferences = parseStream({"--1080"});
    CHECK(!preferences->autoResolution);
    CHECK(preferences->width == 1920 && preferences->height == 1080);

    preferences = parseStream({"--resolution", "1600x900"});
    CHECK(!preferences->autoResolution);
    CHECK(preferences->width == 1600 && preferences->height == 900);

    // The saved choice is untouched
    CHECK(QSettings().value("autoresolution").toBool());
    CHECK(QSettings().value("width").toInt() == 3840);
}

static void testWithoutAResolutionTheSavedAutoStays()
{
    saveAuto();

    StreamingPreferences* preferences = parseStream({"--fps", "120"});
    CHECK(preferences->autoResolution);
    CHECK(preferences->fps == 120);
}

static void testABitrateGivenIsKept()
{
    saveAuto();

    StreamingPreferences* preferences = parseStream({"--bitrate", "30000"});
    CHECK(preferences->autoResolution);
    CHECK(!preferences->autoAdjustBitrate);
    CHECK(preferences->bitrateKbps == 30000);
    CHECK(QSettings().value("autoadjustbitrate").toBool());
}

static void testAChosenResolutionStaysChosen()
{
    {
        QSettings settings;
        settings.setValue("autoresolution", false);
    }

    StreamingPreferences* preferences = parseStream({"--720"});
    CHECK(!preferences->autoResolution);
    CHECK(preferences->width == 1280 && preferences->height == 720);
    CHECK(!QSettings().value("autoresolution").toBool());
}

static void testWithoutYuv444ABitrateThatFollowsTheResolutionFollows()
{
    int default444 = StreamingPreferences::getDefaultBitrate(1920, 1080, 60, true);
    int default420 = StreamingPreferences::getDefaultBitrate(1920, 1080, 60, false);
    CHECK(default444 != default420);

    CHECK(StreamingPreferences::getBitrateWithoutYuv444(default444, true, 1920, 1080, 60) == default420);

    // Not the default: someone chose it, as before
    CHECK(StreamingPreferences::getBitrateWithoutYuv444(default444 + 1000, true, 1920, 1080, 60) == default444 + 1000);
}

static void testWithoutYuv444ABitrateSetByHandIsKept()
{
    int default444 = StreamingPreferences::getDefaultBitrate(1920, 1080, 60, true);

    // Even when it is the very number the default would be
    CHECK(StreamingPreferences::getBitrateWithoutYuv444(default444, false, 1920, 1080, 60) == default444);

    // One given on the command line is set by hand too
    saveAuto();
    QSettings().setValue("yuv444", true);
    StreamingPreferences* preferences = parseStream({"--1080", "--fps", "60", "--bitrate", QString::number(default444)});
    CHECK(StreamingPreferences::getBitrateWithoutYuv444(preferences->bitrateKbps, preferences->autoAdjustBitrate,
                                                        preferences->width, preferences->height, preferences->fps) == default444);

    // Left out, it follows the resolution and drops to the 4:2:0 default
    preferences = parseStream({"--1080", "--fps", "60"});
    CHECK(preferences->enableYUV444);
    CHECK(preferences->bitrateKbps == default444);
    CHECK(StreamingPreferences::getBitrateWithoutYuv444(preferences->bitrateKbps, preferences->autoAdjustBitrate,
                                                        preferences->width, preferences->height, preferences->fps) ==
          StreamingPreferences::getDefaultBitrate(1920, 1080, 60, false));

    QSettings().setValue("yuv444", false);
}

int main(int argc, char* argv[])
{
    QCoreApplication app(argc, argv);

    // Settings of their own, away from the real ones
    QTemporaryDir settingsDir;
    QSettings::setDefaultFormat(QSettings::IniFormat);
    QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, settingsDir.path());
    QCoreApplication::setOrganizationName("MoonlightCliTest");
    QCoreApplication::setApplicationName("MoonlightCliTest");

    testAResolutionGivenIsKeptOverASavedAuto();
    testWithoutAResolutionTheSavedAutoStays();
    testABitrateGivenIsKept();
    testAChosenResolutionStaysChosen();
    testWithoutYuv444ABitrateThatFollowsTheResolutionFollows();
    testWithoutYuv444ABitrateSetByHandIsKept();

    if (failures != 0) {
        std::fprintf(stderr, "%d check(s) failed\n", failures);
        return 1;
    }

    std::printf("command line tests passed\n");
    return 0;
}
