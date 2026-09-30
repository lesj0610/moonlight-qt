#include "clipboardcontent.h"

#include <QBuffer>
#include <QGuiApplication>

#include <cstdio>

static int s_Failures = 0;

#define CHECK(condition) \
    do { \
        if (!(condition)) { \
            std::fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #condition); \
            s_Failures++; \
        } \
    } while (false)

using namespace ClipboardContent;

static QByteArray png(const QImage& image)
{
    QByteArray data;
    QBuffer buffer(&data);
    buffer.open(QIODevice::WriteOnly);
    image.save(&buffer, "PNG");
    return data;
}

static void testTheSharedTypesAreNamed()
{
    CHECK(kindOf("text/plain") == Kind::Text);
    CHECK(kindOf("Text/Plain; charset=UTF-8") == Kind::Text);
    CHECK(kindOf(" image/png ") == Kind::Png);
    CHECK(kindOf(mimeType(Kind::Text)) == Kind::Text);
    CHECK(kindOf(mimeType(Kind::Png)) == Kind::Png);
}

static void testOtherTypesAreNot()
{
    CHECK(!kindOf("text/html"));
    CHECK(!kindOf("image/bmp"));
    CHECK(!kindOf(""));
}

static void testLineEndingsDoNotMakeTextDifferent()
{
    CHECK(fingerprint(QString("one\r\ntwo")) == fingerprint(QString("one\ntwo")));
    CHECK(fingerprint(QString("one two")) != fingerprint(QString("one\ntwo")));
    CHECK(fingerprint(QString("한글")) == fingerprint(QString::fromUtf8("한글")));
}

static void testAnImageIsTheSameAfterAPngRoundTrip()
{
    QImage image(3, 2, QImage::Format_ARGB32);
    image.fill(QColor(10, 20, 30, 255));
    image.setPixelColor(1, 1, QColor(200, 100, 50, 128));

    const QImage back = QImage::fromData(png(image), "PNG");
    CHECK(!back.isNull());
    CHECK(fingerprint(back) == fingerprint(image));
}

static void testAnImageIsTheSameInAnotherFormat()
{
    QImage opaque(4, 4, QImage::Format_RGB32);
    opaque.fill(QColor(1, 2, 3));

    CHECK(fingerprint(opaque) == fingerprint(opaque.convertToFormat(QImage::Format_ARGB32)));
}

static void testDifferentImagesAreDifferent()
{
    QImage one(3, 2, QImage::Format_ARGB32);
    one.fill(Qt::white);
    QImage other = one;
    other.setPixelColor(2, 1, Qt::black);
    const QImage wider(2, 3, QImage::Format_ARGB32);

    CHECK(fingerprint(one) != fingerprint(other));
    CHECK(fingerprint(one) != fingerprint(wider));
    CHECK(fingerprint(one) != fingerprint(QString("text")));
}

int main(int argc, char* argv[])
{
    QGuiApplication app(argc, argv);

    testTheSharedTypesAreNamed();
    testOtherTypesAreNot();
    testLineEndingsDoNotMakeTextDifferent();
    testAnImageIsTheSameAfterAPngRoundTrip();
    testAnImageIsTheSameInAnotherFormat();
    testDifferentImagesAreDifferent();

    if (s_Failures != 0) {
        std::fprintf(stderr, "%d clipboard check(s) failed\n", s_Failures);
        return 1;
    }
    std::printf("clipboard content tests passed\n");
    return 0;
}
