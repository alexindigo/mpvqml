#pragma once

#include <QObject>
#include <QString>
#include <QAtomicInt>
#include <cstdint>
#include <mpv/client.h>

class MpvHookManager : public QObject {
    Q_OBJECT
public:
    explicit MpvHookManager(QObject *parent = nullptr);
    ~MpvHookManager() override;

    void setupHookClient(mpv_handle *core);
    void hookAdd(const QString &name, int priority = 0);
    void hookContinue(uint64_t id);

signals:
    void hookTriggered(const QString &name, uint64_t id);

private:
    void drainHookEvents();

    mpv_handle *m_hookClient = nullptr;
    QAtomicInt m_hookClientActive{false};
    uint64_t m_nextHookId = 1;

    static void onHookWakeup(void *ctx);
};
