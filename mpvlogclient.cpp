#include "mpvlogclient.h"
#include <QMetaObject>

MpvLogClient::MpvLogClient(QObject *parent)
    : QObject(parent) {}

MpvLogClient::~MpvLogClient() {
    teardownLogClient();
}

void MpvLogClient::setupLogClient(mpv_handle *core) {
    if (m_logClient || !core)
        return;
    m_logClient = mpv_create_client(core, "mpvqml-log");
    if (!m_logClient)
        return;
    m_logClientActive.storeRelaxed(1);
    mpv_set_wakeup_callback(m_logClient, onLogWakeup, this);
}

void MpvLogClient::setLogLevel(const QString &level) {
    if (level == QStringLiteral("no")) {
        teardownLogClient();
        return;
    }
    if (m_logClient)
        mpv_request_log_messages(m_logClient, level.toUtf8().constData());
}

void MpvLogClient::teardownLogClient() {
    if (!m_logClient)
        return;
    m_logClientActive.storeRelaxed(0);
    mpv_set_wakeup_callback(m_logClient, nullptr, nullptr);
    mpv_destroy(m_logClient);
    m_logClient = nullptr;
}

void MpvLogClient::onLogWakeup(void *ctx) {
    auto *self = static_cast<MpvLogClient *>(ctx);
    QMetaObject::invokeMethod(self, [self]() { self->drainLogEvents(); },
                              Qt::QueuedConnection);
}

void MpvLogClient::drainLogEvents() {
    if (!m_logClient || !m_logClientActive.loadRelaxed())
        return;
    while (true) {
        mpv_event *event = mpv_wait_event(m_logClient, 0);
        if (event->event_id == MPV_EVENT_NONE)
            break;
        if (event->event_id == MPV_EVENT_LOG_MESSAGE) {
            auto *msg = static_cast<mpv_event_log_message *>(event->data);
            if (msg)
                emit logMessage(QString::fromUtf8(msg->prefix),
                                QString::fromUtf8(msg->level),
                                QString::fromUtf8(msg->text));
        }
    }
}
