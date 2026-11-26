#include "mpvlogclient.h"
#include "mpveventclient_p.h"

using MpvQmlPrivate::MpvEventClient;

MpvLogClient::MpvLogClient(QObject *parent) : QObject(parent),
    m_events(std::make_unique<MpvEventClient>(this, [](QObject *receiver, const mpv_event &event) {
        auto *log = static_cast<MpvLogClient *>(receiver);
        if (event.event_id == MPV_EVENT_SHUTDOWN) {
            log->shutdown();
        } else if (event.event_id == MPV_EVENT_LOG_MESSAGE) {
            const auto *message = static_cast<const mpv_event_log_message *>(event.data);
            if (!message) return;
            const QString facility = QString::fromUtf8(message->prefix);
            const QString level = QString::fromUtf8(message->level);
            const QString text = QString::fromUtf8(message->text);
            emit log->logMessage(facility, level, text);
        }
    })) {}

MpvLogClient::~MpvLogClient() = default;

void MpvLogClient::setupLogClient(mpv_handle *core) {
    if (!core || m_events->handle() || !m_events->prepare()) return;
    if (auto *client = mpv_create_client(core, "mpvqml-log"))
        m_events->open(client, MpvEventClient::Lifetime::Owned);
}

int MpvLogClient::setLogLevel(const QString &level) {
    if (level == QStringLiteral("no")) { shutdown(); return MPV_ERROR_SUCCESS; }
    auto *client = m_events->handle();
    return client ? mpv_request_log_messages(client, level.toUtf8().constData())
                  : MPV_ERROR_UNINITIALIZED;
}

void MpvLogClient::shutdown() { m_events->close(); }
