#pragma once

// Production-only adapter. Not installed or exposed as a QML type.
#include "mpvcontroller_iface.h"
#include "mpvcontroller_p.h"
#include <QHash>

class MpvControllerAdapter final : public MpvControllerIface {
    Q_OBJECT
public:
    explicit MpvControllerAdapter(QObject *parent = nullptr);
    ~MpvControllerAdapter() override;
    MpvInitializationStatus initialize() override;
    MpvInitializationStatus initializationResult() const override { return m_result; }
    bool available() const override { return !m_closing && m_available; }
    void shutdown() override;
    int commandAsync(const QStringList &params) override;
    int setMpvPropertyAsync(const QString &name, const QVariant &value) override;
    int readMpvPropertyAsync(const QString &name) override;
    int loadConfigurationAsync(const QString &path) override;
    void observeProperty(const QString &name, mpv_format format, uint64_t id = 0) override;
    std::shared_ptr<MpvQmlPrivate::MpvCoreLease> core() const { return m_worker.core(); }
    mpv_handle *helperCore() const;
    void contextRetiring(quint64 generation, quint64 intent);
    void contextReady(quint64 generation);
private:
    struct Request {
        int id;
        MpvQmlPrivate::MpvRequestKind kind;
        quint64 generation;
    };
    int admit(MpvQmlPrivate::MpvRequestKind kind,
              std::function<void(MpvQmlPrivate::MpvController *, quint64)> submit);
    void receive(const MpvQmlPrivate::MpvNativeReply &reply);
    void finish(const Request &request, int error, const QVariant &value = {});
    void settleRequests(int error);
    MpvQmlPrivate::MpvBackendWorker m_worker;
    MpvInitializationStatus m_result;
    QHash<quint64, Request> m_requests;
    quint64 m_nextPublicId = 1;
    quint64 m_generation = 1;
    bool m_available = false;
    bool m_closing = false;
};
