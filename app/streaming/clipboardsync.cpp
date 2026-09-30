#include "clipboardsync.h"

#include "backend/nvhttp.h"

#include <QBuffer>
#include <QClipboard>
#include <QGuiApplication>
#include <QMimeData>
#include <QNetworkReply>

#include <memory>

#include "SDL_compat.h"

#if defined(Q_OS_WIN32) || defined(Q_OS_DARWIN)
// Where the clipboard answers without the Qt event loop running, which it
// does not while streaming. Elsewhere only text is shared, through SDL.
#define CLIPBOARD_IMAGES 1
#endif

ClipboardSync::ClipboardSync(NvComputer* computer, std::function<void()> notifyMainLoop)
    : m_Computer(computer),
      m_NotifyMainLoop(std::move(notifyMainLoop)),
      m_Worker(*this),
      m_Stopping(false)
{
    m_Worker.start();
}

ClipboardSync::~ClipboardSync()
{
    {
        QMutexLocker locker(&m_Lock);
        m_Stopping = true;
        m_Jobs.clear();
    }
    m_Wake.wakeAll();

    // A request in flight gives up on its own after k_RequestTimeoutMs
    m_Worker.wait();
}

void ClipboardSync::onFocusLost()
{
    Job job;
    job.fetch = true;
    job.since = m_HostSerial;
    queue(std::move(job));
}

void ClipboardSync::onFocusGained()
{
    QByteArray fingerprint;
    std::optional<Content> content = readLocal(fingerprint);
    if (!content) {
        return;
    }

    Job job;
    job.content = std::move(*content);
    job.fingerprint = fingerprint;
    queue(std::move(job));
}

void ClipboardSync::queue(Job job)
{
    {
        QMutexLocker locker(&m_Lock);

        // Only the newest of each kind of request matters
        for (auto it = m_Jobs.begin(); it != m_Jobs.end();) {
            it = it->fetch == job.fetch ? m_Jobs.erase(it) : it + 1;
        }
        m_Jobs.push_back(std::move(job));
    }
    m_Wake.wakeAll();
}

void ClipboardSync::process()
{
    std::deque<Result> results;
    {
        QMutexLocker locker(&m_Lock);
        if (m_Results.empty()) {
            return;
        }
        results.swap(m_Results);
    }

    for (const Result& result : results) {
        if (result.serial) {
            m_HostSerial = result.serial;
        }
        if (!result.ok) {
            continue;
        }

        if (!result.fetch) {
            m_LastSynced = result.fingerprint;
        }
        else if (result.content) {
            applyToLocal(*result.content);
        }
    }
}

std::optional<ClipboardSync::Content> ClipboardSync::readLocal(QByteArray& fingerprint)
{
    if (SDL_HasClipboardText()) {
        char* text = SDL_GetClipboardText();
        if (text == nullptr) {
            return std::nullopt;
        }
        QByteArray utf8(text);
        SDL_free(text);

        fingerprint = ClipboardContent::fingerprint(QString::fromUtf8(utf8));
        if (utf8.isEmpty() || utf8.size() > ClipboardContent::k_MaxBytes || fingerprint == m_LastSynced) {
            return std::nullopt;
        }
        return Content{ClipboardContent::Kind::Text, utf8};
    }

#ifdef CLIPBOARD_IMAGES
    const QMimeData* mime = QGuiApplication::clipboard()->mimeData();
    if (mime != nullptr && mime->hasImage()) {
        const QImage image = qvariant_cast<QImage>(mime->imageData());
        if (image.isNull()) {
            return std::nullopt;
        }

        // Checked before encoding, which is the expensive part
        fingerprint = ClipboardContent::fingerprint(image);
        if (fingerprint == m_LastSynced) {
            return std::nullopt;
        }

        QByteArray png;
        QBuffer buffer(&png);
        buffer.open(QIODevice::WriteOnly);
        if (!image.save(&buffer, "PNG") || png.size() > ClipboardContent::k_MaxBytes) {
            SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                        "The copied image is too large to share");
            return std::nullopt;
        }
        return Content{ClipboardContent::Kind::Png, png};
    }
#endif

    return std::nullopt;
}

void ClipboardSync::applyToLocal(const Content& content)
{
    if (content.kind == ClipboardContent::Kind::Text) {
        // QByteArray keeps its data terminated
        if (SDL_SetClipboardText(content.data.constData()) == 0) {
            m_LastSynced = ClipboardContent::fingerprint(QString::fromUtf8(content.data));
        }
        else {
            SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                        "Unable to put the host's text on the clipboard: %s",
                        SDL_GetError());
        }
        return;
    }

#ifdef CLIPBOARD_IMAGES
    const QImage image = QImage::fromData(content.data, "PNG");
    if (image.isNull()) {
        SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                    "Unable to decode the image the host copied");
        return;
    }
    QGuiApplication::clipboard()->setImage(image);
    m_LastSynced = ClipboardContent::fingerprint(image);
#else
    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                "The host copied an image, which this platform does not take while streaming");
#endif
}

void ClipboardSync::Worker::run()
{
    // Made on this thread, since the network access it uses belongs to it
    NvHTTP http(m_Owner.m_Computer);

    for (;;) {
        Job job;
        {
            QMutexLocker locker(&m_Owner.m_Lock);
            while (!m_Owner.m_Stopping && m_Owner.m_Jobs.empty()) {
                m_Owner.m_Wake.wait(&m_Owner.m_Lock);
            }
            if (m_Owner.m_Stopping) {
                return;
            }
            job = std::move(m_Owner.m_Jobs.front());
            m_Owner.m_Jobs.pop_front();
        }

        Result result;
        result.fetch = job.fetch;
        result.fingerprint = job.fingerprint;

        std::unique_ptr<QNetworkReply> reply;
        if (job.fetch) {
            reply.reset(http.openHttpsDataConnection("clipboard",
                                                     job.since ? QString("since=%1").arg(*job.since) : QString(),
                                                     nullptr, QString(), k_RequestTimeoutMs));
        }
        else {
            reply.reset(http.openHttpsDataConnection("clipboard", QString(), &job.content.data,
                                                     ClipboardContent::mimeType(job.content.kind), k_RequestTimeoutMs));
        }

        if (reply) {
            const int status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();

            bool serialValid = false;
            const quint32 serial = reply->rawHeader("X-Clipboard-Serial").toUInt(&serialValid);
            if (serialValid) {
                result.serial = serial;
            }

            if (status == 204 || (!job.fetch && status == 200)) {
                result.ok = true;
            }
            else if (job.fetch && status == 200) {
                const auto kind = ClipboardContent::kindOf(reply->header(QNetworkRequest::ContentTypeHeader).toString());
                const QByteArray data = reply->read(ClipboardContent::k_MaxBytes + 1);
                if (kind && !data.isEmpty() && data.size() <= ClipboardContent::k_MaxBytes) {
                    result.ok = true;
                    result.content = Content{*kind, data};
                }
            }
            else {
                qWarning() << "Clipboard" << (job.fetch ? "fetch" : "send") << "failed with status" << status << reply->errorString();
            }
        }

        {
            QMutexLocker locker(&m_Owner.m_Lock);
            m_Owner.m_Results.push_back(std::move(result));
        }
        if (m_Owner.m_NotifyMainLoop) {
            m_Owner.m_NotifyMainLoop();
        }
    }
}
