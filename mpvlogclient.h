#pragma once

#include <QObject>
#include <QString>
#include <QAtomicInt>
#include <mpv/client.h>

class MpvLogClient : public QObject {
    Q_OBJECT
public:
    explicit MpvLogClient(QObject *parent = nullptr);
    ~MpvLogClient() override;

    void setupLogClient(mpv_handle *core);
    void setLogLevel(const QString &level);

signals:
    void logMessage(const QString &facility, const QString &level,
                    const QString &text);

private:
    void teardownLogClient();
    void drainLogEvents();

    mpv_handle *m_logClient = nullptr;
    QAtomicInt m_logClientActive{false};

    static void onLogWakeup(void *ctx);
};
