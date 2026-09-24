# Unit test for StreamResizeController, partly in front of moonlight-common-c's
# own video path. It is not part of the app build:
#   qmake streamresize-test.pro && make && ./streamresize-test
TEMPLATE = app
CONFIG += console c++17
CONFIG -= qt app_bundle
TARGET = streamresize-test

COMMON_C_DIR = ../../moonlight-common-c/moonlight-common-c

INCLUDEPATH += \
    ../../app/streaming \
    $$COMMON_C_DIR/src \
    $$COMMON_C_DIR/enet/include

# The library's asserts check what reaches the decoder, so they stay on
DEFINES += HAS_SOCKLEN_T LC_DEBUG

SOURCES += \
    streamresize-test.cpp \
    videopath.c \
    ../../app/streaming/streamresize.cpp \
    $$COMMON_C_DIR/src/ByteBuffer.c \
    $$COMMON_C_DIR/src/LinkedBlockingQueue.c \
    $$COMMON_C_DIR/src/Platform.c \
    $$COMMON_C_DIR/src/VideoDepacketizer.c

unix: LIBS += -lpthread
win32: LIBS += ws2_32.lib winmm.lib
