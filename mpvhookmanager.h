#pragma once
#include <QObject>
#include <QString>
#include <memory>
#include <mpv/client.h>

class MpvHookManager : public QObject {
    Q_OBJECT
public:
    explicit MpvHookManager(QObject *parent = nullptr);
    ~MpvHookManager() override;
    void setupHookClient(mpv_handle *core);
    int hookAdd(const QString &name, int priority = 0);
    int hookContinue(uint64_t id);
    void shutdown();
signals:
    void hookTriggered(const QString &name, uint64_t id);
private:
    struct WakeupToken;
    static void onHookWakeup(void *context) noexcept;
    void drainHookEvents(quint64 generation);
    mpv_handle *m_hookClient = nullptr;
    std::unique_ptr<WakeupToken> m_wakeup;
    quint64 m_generation = 1;
    uint64_t m_nextHookId = 1;
    bool m_draining = false;
    bool m_redrain = false;
};
