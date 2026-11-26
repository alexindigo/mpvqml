#pragma once
#include <QObject>
#include <QString>
#include <memory>
#include <mpv/client.h>

namespace MpvQmlPrivate { class MpvEventClient; }

class MpvLogClient : public QObject {
    Q_OBJECT
public:
    explicit MpvLogClient(QObject *parent = nullptr);
    ~MpvLogClient() override;
    void setupLogClient(mpv_handle *core);
    int setLogLevel(const QString &level);
    void shutdown();
signals:
    void logMessage(const QString &facility, const QString &level, const QString &text);
private:
    std::unique_ptr<MpvQmlPrivate::MpvEventClient> m_events;
};
