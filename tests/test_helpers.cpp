#include "mpvplayer.h"
#include "mpvcontroller_p.h"
#include "mpvlogclient.h"
#include "mpvhookmanager.h"
#include <QPointer>
#include <QtTest>

using namespace MpvQmlPrivate;

static int print(mpv_handle *core, const char *text) {
    const char *args[]{"print-text", text, nullptr};
    return mpv_command(core, args);
}

class TestHelpers : public QObject {
    Q_OBJECT
private slots:
    void cleanup() { QTRY_COMPARE_WITH_TIMEOUT(MpvCoreLease::outstandingReservations(), 0, 5000); }

    void disableReenableAndNestedDeliveryDrainsOnlyCurrentGeneration() {
        MpvBackendWorker worker;
        QVERIFY(worker.initialize().success);
        mpv_handle *core = worker.core()->handle();
        MpvLogClient log;
        log.setupLogClient(core);
        QCOMPARE(log.setLogLevel(QStringLiteral("info")), 0);
        bool switched = false;
        bool fresh = false;
        connect(&log, &MpvLogClient::logMessage, this, [&](const QString &, const QString &, const QString &text) {
            if (text.contains("mpvqml-old") && !switched) {
                switched = true;
                log.setLogLevel(QStringLiteral("no"));
                log.setupLogClient(core);
                log.setLogLevel(QStringLiteral("info"));
                print(core, "mpvqml-fresh");
                QCoreApplication::processEvents(); // nested delivery while old drain is suspended
            }
            if (text.contains("mpvqml-fresh")) fresh = true;
        });
        QCOMPARE(print(core, "mpvqml-old"), 0);
        QTRY_VERIFY_WITH_TIMEOUT(switched && fresh, 5000);
        log.shutdown();
        log.shutdown();
    }

    void staleQueuedWakeupCannotDrainRecreatedClient() {
        MpvBackendWorker worker;
        QVERIFY(worker.initialize().success);
        mpv_handle *core = worker.core()->handle();
        MpvLogClient log;
        log.setupLogClient(core);
        QCOMPARE(log.setLogLevel(QStringLiteral("info")), 0);
        QCOMPARE(print(core, "mpvqml-stale"), 0);
        log.shutdown(); // callback invocation may already be queued
        log.setupLogClient(core);
        QCOMPARE(log.setLogLevel(QStringLiteral("info")), 0);
        QStringList texts;
        connect(&log, &MpvLogClient::logMessage, this, [&](const QString &, const QString &, const QString &text) { texts.append(text); });
        QCOMPARE(print(core, "mpvqml-new"), 0);
        QTRY_VERIFY_WITH_TIMEOUT(texts.join(QString()).contains("mpvqml-new"), 5000);
        QVERIFY(!texts.join(QString()).contains("mpvqml-stale"));
    }

    void helperDeletionDuringRealLogSignalIsSafe() {
        MpvBackendWorker worker;
        QVERIFY(worker.initialize().success);
        auto *log = new MpvLogClient;
        QPointer<MpvLogClient> alive(log);
        log->setupLogClient(worker.core()->handle());
        QCOMPARE(log->setLogLevel(QStringLiteral("info")), 0);
        connect(log, &MpvLogClient::logMessage, this, [log](const QString &, const QString &, const QString &text) {
            if (text.contains("mpvqml-delete-helper")) delete log;
        });
        QCOMPARE(print(worker.core()->handle(), "mpvqml-delete-helper"), 0);
        QTRY_VERIFY_WITH_TIMEOUT(!alive, 5000);
    }

    void playerDeletionDuringRealLogSignalReclaimsCore() {
        auto *player = new MpvPlayer;
        QPointer<MpvPlayer> alive(player);
        QVERIFY(player->coreAvailable());
        player->setLogLevel(QStringLiteral("info"));
        connect(player, &MpvPlayer::logMessage, this, [player](const QString &, const QString &, const QString &text) {
            if (text.contains("mpvqml-delete-player")) delete player;
        });
        QVERIFY(player->commandAsync(QStringList{"print-text", "mpvqml-delete-player"}) > 0);
        QTRY_VERIFY_WITH_TIMEOUT(!alive, 5000);
        QTRY_COMPARE_WITH_TIMEOUT(MpvCoreLease::outstandingReservations(), 0, 5000);
    }

    void outstandingHookClientDeletionDoesNotHoldFinalCore() {
        MpvBackendWorker worker;
        QVERIFY(worker.initialize().success);
        auto *hooks = new MpvHookManager;
        QPointer<MpvHookManager> alive(hooks);
        hooks->setupHookClient(worker.core()->handle());
        QCOMPARE(hooks->hookAdd(QStringLiteral("on_load")), 0);
        bool triggered = false;
        connect(hooks, &MpvHookManager::hookTriggered, this, [&](const QString &name, uint64_t) {
            if (name == "on_load") { triggered = true; delete hooks; }
        });
        const quint64 token = worker.core()->allocateNativeToken();
        QVERIFY(worker.post([token](MpvController *controller) {
            controller->submitCommand(token, QStringList{"loadfile", QStringLiteral(MPVQML_HELPER_VIDEO)});
        }));
        QTRY_VERIFY_WITH_TIMEOUT(triggered && !alive, 5000);
        worker.shutdown();
        QTRY_COMPARE_WITH_TIMEOUT(MpvCoreLease::outstandingReservations(), 0, 5000);
    }
};

QTEST_MAIN(TestHelpers)
#include "test_helpers.moc"
