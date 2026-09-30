#pragma once

#include "clipboardcontent.h"

#include <QByteArray>
#include <QMutex>
#include <QString>
#include <QThread>
#include <QWaitCondition>

#include <deque>
#include <functional>
#include <optional>

class NvComputer;

// Shares the clipboard with a host that offers it through /clipboard.
//
// Nothing is polled. The host clipboard is fetched when the stream window
// loses focus, which is when the user is about to paste somewhere else, and
// the local clipboard is sent when the window gains focus, which is when the
// user is about to paste on the host. Requests run on a thread of their own,
// since the stream's main loop must not wait on the network. What they bring
// back is put on the clipboard by process(), from the main loop, since the
// clipboard belongs to that thread.
class ClipboardSync
{
public:
    using Content = ClipboardContent::Content;

    // How long one request may take
    static constexpr int k_RequestTimeoutMs = 5000;

    // notifyMainLoop wakes the main loop once a request has come back. It is
    // called from the request thread.
    ClipboardSync(NvComputer* computer, std::function<void()> notifyMainLoop);
    ~ClipboardSync();

    ClipboardSync(const ClipboardSync&) = delete;
    ClipboardSync& operator=(const ClipboardSync&) = delete;

    // Called from the main loop
    void onFocusGained();
    void onFocusLost();
    void process();

private:
    struct Job
    {
        bool fetch = false;
        std::optional<quint32> since;
        Content content = {};
        QByteArray fingerprint;
    };

    struct Result
    {
        bool fetch = false;
        bool ok = false;
        std::optional<quint32> serial;
        std::optional<Content> content;
        QByteArray fingerprint;
    };

    class Worker : public QThread
    {
    public:
        explicit Worker(ClipboardSync& owner) : m_Owner(owner) {}

    protected:
        void run() override;

    private:
        ClipboardSync& m_Owner;
    };

    void queue(Job job);

    // What the local clipboard holds, unless it is what was last synced
    std::optional<Content> readLocal(QByteArray& fingerprint);

    // Put what the host sent on the local clipboard
    void applyToLocal(const Content& content);

    NvComputer* m_Computer;
    std::function<void()> m_NotifyMainLoop;
    Worker m_Worker;

    // Shared with the worker
    QMutex m_Lock;
    QWaitCondition m_Wake;
    std::deque<Job> m_Jobs;
    std::deque<Result> m_Results;
    bool m_Stopping;

    // Main loop only: the last host clipboard sequence number seen, and what
    // both clipboards were last known to hold
    std::optional<quint32> m_HostSerial;
    QByteArray m_LastSynced;
};
