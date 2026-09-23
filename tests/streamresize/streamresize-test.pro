# Unit test for StreamResizeController. It is not part of the app build:
#   qmake streamresize-test.pro && make && ./streamresize-test
TEMPLATE = app
CONFIG += console c++17
CONFIG -= qt app_bundle
TARGET = streamresize-test

INCLUDEPATH += \
    ../../app/streaming \
    ../../moonlight-common-c/moonlight-common-c/src

SOURCES += \
    streamresize-test.cpp \
    ../../app/streaming/streamresize.cpp
