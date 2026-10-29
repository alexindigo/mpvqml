#include "mpvlogclient.h"
#include <QMetaObject>
#include <QPointer>
#include <atomic>
#include <utility>

struct MpvLogClient::WakeupToken {
    MpvLogClient *const receiver;
    const quint64 generation;
    std::atomic<bool> pending{false};
    WakeupToken(MpvLogClient *receiver, quint64 generation) : receiver(receiver), generation(generation) {}
};

MpvLogClient::MpvLogClient(QObject *parent) : QObject(parent) {}
MpvLogClient::~MpvLogClient() { shutdown(); }

void MpvLogClient::setupLogClient(mpv_handle *core) {
    if (m_logClient || !core) return;
    auto token = std::make_unique<WakeupToken>(this, m_generation);
    mpv_handle *client = mpv_create_client(core, "mpvqml-log");
    if (!client) return;
    m_logClient = client;
    m_wakeup = std::move(token);
    if (m_draining) m_redrain = true;
    // Registration may call onLogWakeup synchronously.
    mpv_set_wakeup_callback(client, onLogWakeup, m_wakeup.get());
}

int MpvLogClient::setLogLevel(const QString &level) {
    if (level == QStringLiteral("no")) { shutdown(); return MPV_ERROR_SUCCESS; }
    return m_logClient ? mpv_request_log_messages(m_logClient, level.toUtf8().constData())
                       : MPV_ERROR_UNINITIALIZED;
}

void MpvLogClient::shutdown() {
    mpv_handle *client = std::exchange(m_logClient, nullptr);
    auto token = std::move(m_wakeup);
    ++m_generation;
    if (!client) return;
    mpv_set_wakeup_callback(client, nullptr, nullptr); // native callback barrier
    mpv_destroy(client);
    // Already queued closures contain copied receiver/generation only.
}

void MpvLogClient::onLogWakeup(void *context) noexcept {
    auto *token = static_cast<WakeupToken *>(context);
    if (token->pending.exchange(true)) return;
    MpvLogClient *const receiver = token->receiver;
    const quint64 generation = token->generation;
    try {
        if (QMetaObject::invokeMethod(receiver, [receiver, generation]() {
            receiver->drainLogEvents(generation);
        }, Qt::QueuedConnection)) return;
    } catch (...) {}
    token->pending.store(false);
}

void MpvLogClient::drainLogEvents(quint64 generation) {
    if (!m_logClient || generation != m_generation) return;
    m_wakeup->pending.store(false);
    if (m_draining) { m_redrain = true; return; }
    m_draining = true;
    QPointer<MpvLogClient> alive(this);
    mpv_handle *const client = m_logClient;
    while (m_logClient == client && m_generation == generation) {
        const mpv_event *event = mpv_wait_event(client, 0);
        if (!event || event->event_id == MPV_EVENT_NONE) break;
        if (event->event_id == MPV_EVENT_SHUTDOWN) { shutdown(); break; }
        if (event->event_id == MPV_EVENT_LOG_MESSAGE) {
            const auto *message = static_cast<const mpv_event_log_message *>(event->data);
            if (!message) continue;
            const QString facility = QString::fromUtf8(message->prefix);
            const QString level = QString::fromUtf8(message->level);
            const QString text = QString::fromUtf8(message->text);
            emit logMessage(facility, level, text);
            if (!alive) return;
        }
    }
    m_draining = false;
    if (std::exchange(m_redrain, false) && m_logClient) {
        const quint64 current = m_generation;
        QMetaObject::invokeMethod(this, [this, current]() { drainLogEvents(current); }, Qt::QueuedConnection);
    }
}
