#include "clipboardcontent.h"

#include <QCryptographicHash>

namespace ClipboardContent
{

QString mimeType(Kind kind)
{
    return kind == Kind::Text ? QStringLiteral("text/plain; charset=utf-8") : QStringLiteral("image/png");
}

std::optional<Kind> kindOf(const QString& contentType)
{
    const QString type = contentType.section(';', 0, 0).trimmed().toLower();
    if (type == QStringLiteral("text/plain")) {
        return Kind::Text;
    }
    if (type == QStringLiteral("image/png")) {
        return Kind::Png;
    }
    return std::nullopt;
}

QByteArray fingerprint(const QString& text)
{
    QString normalized = text;
    normalized.remove(QChar('\r'));
    return "t" + QCryptographicHash::hash(normalized.toUtf8(), QCryptographicHash::Sha1);
}

QByteArray fingerprint(const QImage& image)
{
    const QImage argb = image.convertToFormat(QImage::Format_ARGB32);

    QCryptographicHash hash(QCryptographicHash::Sha1);
    hash.addData(QByteArray::number(argb.width()) + "x" + QByteArray::number(argb.height()));
    for (int y = 0; y < argb.height(); y++) {
        hash.addData(QByteArray::fromRawData(reinterpret_cast<const char*>(argb.constScanLine(y)), argb.width() * 4));
    }
    return "i" + hash.result();
}

}
