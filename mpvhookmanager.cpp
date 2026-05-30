#include "mpvhookmanager.h"
#include <QMetaObject>

MpvHookManager::MpvHookManager(QObject *parent)
    : QObject(parent) {}

MpvHookManager::~MpvHookManager() {
    if (m_hookClient) {
        m_hookClientActive.storeRelaxed(0);
        mpv_set_wakeup_callback(m_hookClient, nullptr, nullptr);
        mpv_destroy(m_hookClient);
    }
}

void MpvHookManager::setupHookClient(mpv_handle *core) {
    if (m_hookClient || !core)
        return;
    m_hookClient = mpv_create_client(core, "mpvqml-hooks");
    if (!m_hookClient)
        return;
    m_hookClientActive.storeRelaxed(1);
    mpv_set_wakeup_callback(m_hookClient, onHookWakeup, this);
}

void MpvHookManager::hookAdd(const QString &name, int priority) {
    if (!m_hookClient)
        return;
    uint64_t id = m_nextHookId++;
    mpv_hook_add(m_hookClient, id, name.toUtf8().constData(), priority);
}

void MpvHookManager::hookContinue(uint64_t id) {
    if (m_hookClient)
        mpv_hook_continue(m_hookClient, id);
}

void MpvHookManager::onHookWakeup(void *ctx) {
    auto *self = static_cast<MpvHookManager *>(ctx);
    QMetaObject::invokeMethod(self, [self]() { self->drainHookEvents(); },
                              Qt::QueuedConnection);
}

void MpvHookManager::drainHookEvents() {
    if (!m_hookClient || !m_hookClientActive.loadRelaxed())
        return;
    while (true) {
        mpv_event *event = mpv_wait_event(m_hookClient, 0);
        if (event->event_id == MPV_EVENT_NONE)
            break;
        if (event->event_id == MPV_EVENT_HOOK) {
            auto *hook = static_cast<mpv_event_hook *>(event->data);
            if (hook)
                emit hookTriggered(QString::fromUtf8(hook->name), hook->id);
        }
    }
}
