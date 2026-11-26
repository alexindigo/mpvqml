#include "mpvhookmanager.h"
#include "mpveventclient_p.h"
#include <limits>

using MpvQmlPrivate::MpvEventClient;

MpvHookManager::MpvHookManager(QObject *parent) : QObject(parent),
    m_events(std::make_unique<MpvEventClient>(this, [](QObject *receiver, const mpv_event &event) {
        auto *manager = static_cast<MpvHookManager *>(receiver);
        if (event.event_id == MPV_EVENT_SHUTDOWN) {
            manager->shutdown();
        } else if (event.event_id == MPV_EVENT_HOOK) {
            const auto *hook = static_cast<const mpv_event_hook *>(event.data);
            if (!hook) return;
            const QString name = QString::fromUtf8(hook->name);
            const uint64_t id = hook->id;
            emit manager->hookTriggered(name, id);
        }
    })) {}

MpvHookManager::~MpvHookManager() = default;

void MpvHookManager::setupHookClient(mpv_handle *core) {
    if (!core || m_events->handle() || !m_events->prepare()) return;
    if (auto *client = mpv_create_client(core, "mpvqml-hooks"))
        m_events->open(client, MpvEventClient::Lifetime::Owned);
}

void MpvHookManager::shutdown() { m_events->close(); }

int MpvHookManager::hookAdd(const QString &name, int priority) {
    auto *client = m_events->handle();
    if (!client) return MPV_ERROR_UNINITIALIZED;
    if (!m_nextHookId) return MPV_ERROR_NOMEM;
    const uint64_t id = m_nextHookId;
    m_nextHookId = id == std::numeric_limits<uint64_t>::max() ? 0 : id + 1;
    return mpv_hook_add(client, id, name.toUtf8().constData(), priority);
}

int MpvHookManager::hookContinue(uint64_t id) {
    auto *client = m_events->handle();
    return client ? mpv_hook_continue(client, id) : MPV_ERROR_UNINITIALIZED;
}
