# Test for what decides how clipboard content travels and when it is the same.
# It is not part of the app build:
#   qmake clipboard-test.pro && make && ./clipboard-test
TEMPLATE = app
QT = core gui
CONFIG += console c++17
CONFIG -= app_bundle
TARGET = clipboard-test

INCLUDEPATH += ../../app/streaming

HEADERS += \
    ../../app/streaming/clipboardcontent.h

SOURCES += \
    clipboard-test.cpp \
    ../../app/streaming/clipboardcontent.cpp
