#pragma once
#include <QObject>
#include <QString>
#include <memory>
#include <mpv/client.h>

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
    struct WakeupToken;
    static void onLogWakeup(void *context) noexcept;
    void drainLogEvents(quint64 generation);
    mpv_handle *m_logClient = nullptr;
    std::unique_ptr<WakeupToken> m_wakeup;
    quint64 m_generation = 1;
    bool m_draining = false;
    bool m_redrain = false;
};
