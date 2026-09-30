#pragma once

#include <QByteArray>
#include <QImage>
#include <QString>

#include <optional>

// What travels between the local clipboard and the host's
namespace ClipboardContent
{
    enum class Kind { Text, Png };

    struct Content
    {
        Kind kind;
        QByteArray data;
    };

    // The most either side sends or accepts
    constexpr qsizetype k_MaxBytes = 32 * 1024 * 1024;

    // The MIME type content of a kind travels as
    QString mimeType(Kind kind);

    // The kind a Content-Type names, if it is one that is shared
    std::optional<Kind> kindOf(const QString& contentType);

    // Identifies what a clipboard holds, the same way whether it was read
    // from the clipboard or put there from what the host sent. Line endings
    // do not count, since a clipboard may rewrite them. Images are compared
    // by their pixels, since the PNG file a clipboard hands back is not the
    // one it was given.
    QByteArray fingerprint(const QString& text);
    QByteArray fingerprint(const QImage& image);
}
