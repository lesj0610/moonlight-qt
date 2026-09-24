// Tests that a resolution or bitrate given on the command line is what the
// stream gets, whatever was saved, and that the saved preferences stay as
// they are.

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

    if (failures != 0) {
        std::fprintf(stderr, "%d check(s) failed\n", failures);
        return 1;
    }

    std::printf("command line tests passed\n");
    return 0;
}
