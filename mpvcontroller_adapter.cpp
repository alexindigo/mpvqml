#include "mpvcontroller_adapter.h"
#include <QPointer>
#include <limits>
#include <utility>

using namespace MpvQmlPrivate;

MpvControllerAdapter::MpvControllerAdapter(QObject *parent) : MpvControllerIface(parent) {
    auto *controller = m_worker.controller();
    if (!controller) return;
    connect(controller, &MpvController::propertyChanged, this, &MpvControllerIface::propertyChanged);
    connect(controller, &MpvController::fileStarted, this, &MpvControllerIface::fileStarted);
    connect(controller, &MpvController::fileLoaded, this, &MpvControllerIface::fileLoaded);
    connect(controller, &MpvController::endFile, this, &MpvControllerIface::endFile);
    connect(controller, &MpvController::videoReconfig, this, &MpvControllerIface::videoReconfig);
    connect(controller, &MpvController::operationFailed, this, &MpvControllerIface::operationFailed);
    connect(controller, &MpvController::reply, this, &MpvControllerAdapter::receive);
    connect(controller, &MpvController::coreUnavailable, this, [this]() {
        m_available = false;
        settleRequests(MPV_ERROR_UNINITIALIZED);
        emit coreUnavailable();
    });
}

MpvControllerAdapter::~MpvControllerAdapter() { shutdown(); }

MpvInitializationStatus MpvControllerAdapter::initialize() {
    if (m_result.attempted) return m_result;
    const auto result = m_worker.initialize();
    m_result = {result.attempted, result.success, result.error, result.message};
    m_available = result.success && !m_closing;
    const auto snapshot = m_result;
    QMetaObject::invokeMethod(this, [this, snapshot]() {
        if (!m_closing)
            emit initializationFinished(snapshot.success, snapshot.error, snapshot.message);
    }, Qt::QueuedConnection);
    return m_result;
}

int MpvControllerAdapter::admit(MpvRequestKind kind,
        std::function<void(MpvController *, quint64)> submit) {
    if (!available()) return MPV_ERROR_UNINITIALIZED;
    if (m_nextPublicId > static_cast<quint64>(std::numeric_limits<int>::max()))
        return MPV_ERROR_NOMEM;
    auto lease = m_worker.core();
    const quint64 token = lease ? lease->allocateNativeToken() : 0;
    if (!token) return MPV_ERROR_NOMEM;
    const int id = static_cast<int>(m_nextPublicId++);
    m_requests.insert(token, {id, kind, m_generation});
    if (!m_worker.post([token, submit = std::move(submit)](MpvController *controller) {
        submit(controller, token);
    })) {
        m_requests.remove(token);
        return MPV_ERROR_UNINITIALIZED;
    }
    return id;
}

int MpvControllerAdapter::commandAsync(const QStringList &params) {
    return admit(MpvRequestKind::Command, [params](MpvController *controller, quint64 token) {
        controller->submitCommand(token, params);
    });
}
int MpvControllerAdapter::setMpvPropertyAsync(const QString &name, const QVariant &value) {
    return admit(MpvRequestKind::PropertySet, [name, value](MpvController *controller, quint64 token) {
        controller->submitPropertySet(token, name, value);
    });
}
int MpvControllerAdapter::readMpvPropertyAsync(const QString &name) {
    return admit(MpvRequestKind::PropertyRead, [name](MpvController *controller, quint64 token) {
        controller->submitPropertyRead(token, name);
    });
}
int MpvControllerAdapter::loadConfigurationAsync(const QString &path) {
    return admit(MpvRequestKind::Configuration, [path](MpvController *controller, quint64 token) {
        controller->submitConfiguration(token, path);
    });
}

void MpvControllerAdapter::receive(const MpvNativeReply &reply) {
    auto found = m_requests.find(reply.token);
    if (found == m_requests.end() || found->kind != reply.kind
        || found->generation != m_generation || reply.generation != m_generation)
        return;
    const Request request = found.value();
    m_requests.erase(found);
    finish(request, reply.error, reply.value);
}

void MpvControllerAdapter::finish(const Request &request, int error, const QVariant &value) {
    QMetaObject::invokeMethod(this, [this, request, error, value]() {
        switch (request.kind) {
        case MpvRequestKind::Command: emit asyncCommandFinished(request.id, error, value); break;
        case MpvRequestKind::PropertySet: emit mpvPropertySetFinished(request.id, error); break;
        case MpvRequestKind::PropertyRead: emit mpvPropertyReadFinished(request.id, error, value); break;
        case MpvRequestKind::Configuration: emit configurationFinished(request.id, error); break;
        }
    }, Qt::QueuedConnection);
}

void MpvControllerAdapter::settleRequests(int error) {
    const auto pending = std::exchange(m_requests, {});
    for (const Request &request : pending) finish(request, error);
}

void MpvControllerAdapter::shutdown() {
    if (m_closing) return;
    m_closing = true;
    m_available = false;
    ++m_generation;
    settleRequests(MPV_ERROR_UNINITIALIZED);
    if (auto *controller = m_worker.controller()) disconnect(controller, nullptr, this, nullptr);
    m_worker.shutdown();
}

void MpvControllerAdapter::observeProperty(const QString &name, mpv_format format, uint64_t id) {
    if (!available() || !m_worker.post([name, format, id](MpvController *controller) {
        controller->observe(name, format, id);
    })) {
        QMetaObject::invokeMethod(this, [this]() {
            emit operationFailed(QStringLiteral("observeProperty"), MPV_ERROR_UNINITIALIZED,
                                 QStringLiteral("mpv core unavailable"));
        }, Qt::QueuedConnection);
    }
}

mpv_handle *MpvControllerAdapter::helperCore() const {
    auto lease = core();
    return available() && lease ? lease->handle() : nullptr;
}
void MpvControllerAdapter::contextRetiring(quint64 generation, quint64 intent) {
    m_worker.post([generation, intent](MpvController *controller) {
        controller->renderContextRetiring(generation, intent);
    });
}
void MpvControllerAdapter::contextReady(quint64 generation) {
    m_worker.post([generation](MpvController *controller) { controller->renderContextReady(generation); });
}
