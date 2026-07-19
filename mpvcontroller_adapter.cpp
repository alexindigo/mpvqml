#include "mpvcontroller_adapter.h"
#include <mpv/client.h>

MpvControllerAdapter::MpvControllerAdapter(MpvController *real, QObject *parent)
    : MpvControllerIface(parent), m_real(real) {
    connect(m_real, &MpvController::propertyChanged, this,
            &MpvControllerIface::propertyChanged);
    connect(m_real, &MpvController::fileStarted, this,
            &MpvControllerIface::fileStarted);
    connect(m_real, &MpvController::fileLoaded, this,
            &MpvControllerIface::fileLoaded);
    connect(m_real, &MpvController::endFile, this,
            &MpvControllerIface::endFile);
    connect(m_real, &MpvController::videoReconfig, this,
            &MpvControllerIface::videoReconfig);
    connect(m_real, &MpvController::asyncReply, this,
            [this](const QVariant &, mpv_event event) {
        if (event.event_id == MPV_EVENT_COMMAND_REPLY
            || event.event_id == MPV_EVENT_SET_PROPERTY_REPLY
            || event.event_id == MPV_EVENT_GET_PROPERTY_REPLY) {
            emit asyncReply(static_cast<int>(event.reply_userdata),
                            event.error);
        }
    });
}

int MpvControllerAdapter::setProperty(const QString &property,
                                      const QVariant &value) {
    return m_real->setProperty(property, value);
}

QVariant MpvControllerAdapter::command(const QStringList &params) {
    return m_real->command(params);
}

void MpvControllerAdapter::observeProperty(const QString &property,
                                            mpv_format format, uint64_t id) {
    m_real->observeProperty(property, format, id);
}

int MpvControllerAdapter::commandAsync(const QVariant &params, int id) {
    return m_real->commandAsync(params, id);
}

int MpvControllerAdapter::commandAsync(const QStringList &params, int id) {
    return m_real->commandAsync(QVariant(params), id);
}

int MpvControllerAdapter::rawSetPropertyString(const QString &property,
                                               const QString &value) {
    mpv_handle *handle = m_real->mpv();
    if (!handle)
        return MPV_ERROR_UNINITIALIZED;
    return mpv_set_property_string(handle,
                                   property.toUtf8().constData(),
                                   value.toUtf8().constData());
}

mpv_handle *MpvControllerAdapter::rawMpv() const {
    return m_real->mpv();
}
