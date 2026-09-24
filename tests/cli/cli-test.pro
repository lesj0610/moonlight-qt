# Test for how the stream command line and the saved preferences combine. It
# is not part of the app build:
#   qmake cli-test.pro && make && ./cli-test
TEMPLATE = app
QT = core qml
CONFIG += console c++17
CONFIG -= app_bundle
TARGET = cli-test

INCLUDEPATH += ../../app

HEADERS += \
    ../../app/cli/commandlineparser.h \
    ../../app/settings/streamingpreferences.h

SOURCES += \
    cli-test.cpp \
    ../../app/cli/commandlineparser.cpp \
    ../../app/settings/streamingpreferences.cpp
