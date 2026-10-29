#include "mpvhookmanager.h"
#include <QMetaObject>
#include <QPointer>
#include <atomic>
#include <limits>
#include <utility>

struct MpvHookManager::WakeupToken {
    MpvHookManager *const receiver;
    const quint64 generation;
    std::atomic<bool> pending{false};
    WakeupToken(MpvHookManager *receiver, quint64 generation) : receiver(receiver), generation(generation) {}
};

MpvHookManager::MpvHookManager(QObject *parent) : QObject(parent) {}
MpvHookManager::~MpvHookManager() { shutdown(); }

void MpvHookManager::setupHookClient(mpv_handle *core) {
    if (m_hookClient || !core) return;
    auto token = std::make_unique<WakeupToken>(this, m_generation);
    mpv_handle *client = mpv_create_client(core, "mpvqml-hooks");
    if (!client) return;
    m_hookClient = client;
    m_wakeup = std::move(token);
    if (m_draining) m_redrain = true;
    mpv_set_wakeup_callback(client, onHookWakeup, m_wakeup.get());
}

void MpvHookManager::shutdown() {
    mpv_handle *client = std::exchange(m_hookClient, nullptr);
    auto token = std::move(m_wakeup);
    ++m_generation;
    if (!client) return;
    mpv_set_wakeup_callback(client, nullptr, nullptr);
    mpv_destroy(client);
}

int MpvHookManager::hookAdd(const QString &name, int priority) {
    if (!m_hookClient) return MPV_ERROR_UNINITIALIZED;
    if (!m_nextHookId) return MPV_ERROR_NOMEM;
    const uint64_t id = m_nextHookId;
    m_nextHookId = id == std::numeric_limits<uint64_t>::max() ? 0 : id + 1;
    return mpv_hook_add(m_hookClient, id, name.toUtf8().constData(), priority);
}

int MpvHookManager::hookContinue(uint64_t id) {
    return m_hookClient ? mpv_hook_continue(m_hookClient, id) : MPV_ERROR_UNINITIALIZED;
}

void MpvHookManager::onHookWakeup(void *context) noexcept {
    auto *token = static_cast<WakeupToken *>(context);
    if (token->pending.exchange(true)) return;
    MpvHookManager *const receiver = token->receiver;
    const quint64 generation = token->generation;
    try {
        if (QMetaObject::invokeMethod(receiver, [receiver, generation]() {
            receiver->drainHookEvents(generation);
        }, Qt::QueuedConnection)) return;
    } catch (...) {}
    token->pending.store(false);
}

void MpvHookManager::drainHookEvents(quint64 generation) {
    if (!m_hookClient || generation != m_generation) return;
    m_wakeup->pending.store(false);
    if (m_draining) { m_redrain = true; return; }
    m_draining = true;
    QPointer<MpvHookManager> alive(this);
    mpv_handle *const client = m_hookClient;
    while (m_hookClient == client && m_generation == generation) {
        const mpv_event *event = mpv_wait_event(client, 0);
        if (!event || event->event_id == MPV_EVENT_NONE) break;
        if (event->event_id == MPV_EVENT_SHUTDOWN) { shutdown(); break; }
        if (event->event_id == MPV_EVENT_HOOK) {
            const auto *hook = static_cast<const mpv_event_hook *>(event->data);
            if (!hook) continue;
            const QString name = QString::fromUtf8(hook->name);
            const uint64_t id = hook->id;
            emit hookTriggered(name, id);
            if (!alive) return;
        }
    }
    m_draining = false;
    if (std::exchange(m_redrain, false) && m_hookClient) {
        const quint64 current = m_generation;
        QMetaObject::invokeMethod(this, [this, current]() { drainHookEvents(current); }, Qt::QueuedConnection);
    }
}
