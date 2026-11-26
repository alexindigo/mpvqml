#pragma once
#include <QObject>
#include <QString>
#include <memory>
#include <mpv/client.h>

namespace MpvQmlPrivate { class MpvEventClient; }

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
    std::unique_ptr<MpvQmlPrivate::MpvEventClient> m_events;
    uint64_t m_nextHookId = 1;
};
