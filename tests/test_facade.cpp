#include "mpvplayer.h"
#include "mpvcontroller_p.h"
#include "mock_mpvcontroller.h"
#include "test_window.h"
#include <QPointer>
#include <QSignalSpy>
#include <QtTest>
#include <QQmlComponent>
#include <QQmlEngine>
#include <QUrl>
#include <QFile>
#include <QTcpServer>
#include <QTemporaryDir>
#include <QQuickWindow>
#include <limits>

using namespace MpvQmlPrivate;

class TestFacade : public QObject {
    Q_OBJECT
private slots:
    void cleanup() {
        // Facade deletion is intentionally nonjoining. Observe retirement and
        // GUI-affine deferred deletion before LSan examines a completed fixture.
        QTRY_COMPARE_WITH_TIMEOUT(MpvCoreLease::outstandingReservations(), 0, 5000);
        QCoreApplication::sendPostedEvents(nullptr, QEvent::MetaCall);
        QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
    }
    void injectedMockCreatesNoCoreAndAllReadsUseTheSeam() {
        const int reservations = MpvCoreLease::outstandingReservations();
        MockMpvController mock;
        MpvPlayer player(nullptr, &mock);
        QCOMPARE(MpvCoreLease::outstandingReservations(), reservations);
        QVERIFY(player.coreAvailable());
        mock.m_replyValue = QVariantMap{{"nested", QVariantList{QStringLiteral("owned"), true}}};
        const QVariant expected = mock.m_replyValue;
        QSignalSpy read(&player, &MpvPlayer::mpvPropertyReadFinished);
        const int id = player.readMpvPropertyAsync(QStringLiteral("playlist"));
        QVERIFY(id > 0);
        QCOMPARE(mock.m_lastReadProperty, QStringLiteral("playlist"));
        QCOMPARE(read.size(), 0);
        mock.m_replyValue = QStringLiteral("later mutable mock state");
        QTRY_COMPARE(read.size(), 1);
        QCOMPARE(read[0][0].toInt(), id);
        QCOMPARE(read[0][2], expected);
        QCOMPARE(MpvCoreLease::outstandingReservations(), reservations);
    }

    void migratedExamplesColdConstruct_data() {
        QTest::addColumn<QString>("file");
        for (const char *name : {"AdvancedPlayer.qml", "AudioPlayer.qml", "AmbientScreensaver.qml", "MediaInspector.qml"})
            QTest::newRow(name) << QString::fromLatin1(name);
    }

    void migratedExamplesColdConstruct() {
        QFETCH(QString, file);
        QQmlEngine engine;
        QQmlComponent component(&engine, QUrl::fromLocalFile(QStringLiteral(MPVQML_EXAMPLES_DIR) + "/" + file));
        QTRY_VERIFY_WITH_TIMEOUT(component.isReady() || component.isError(), 5000);
        QVERIFY2(component.isReady(), qPrintable(component.errorString()));
        auto object = MpvQmlTestSupport::createTestWindow(component);
        QVERIFY2(object, qPrintable(component.errorString()));
        auto *window = qobject_cast<QQuickWindow *>(object.get());
        QVERIFY(window);
        QVERIFY(window->flags().testFlag(Qt::WindowTransparentForInput));
    }

    void mediaMetadataIsLiteralAndDoesNotFetchImages_data() {
        QTest::addColumn<QString>("file");
        for (const char *name : {"AdvancedPlayer.qml", "AudioPlayer.qml", "AmbientScreensaver.qml"})
            QTest::newRow(name) << QString::fromLatin1(name);
    }

    void mediaMetadataIsLiteralAndDoesNotFetchImages() {
        QFETCH(QString, file);
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        QTcpServer requests;
        QVERIFY(requests.listen(QHostAddress::LocalHost));
        const QString title = QStringLiteral("<img src='http://127.0.0.1:%1/pixel.png'><b>media-title</b>")
                                  .arg(requests.serverPort());
        QFile source(QStringLiteral(MPVQML_EXAMPLES_DIR) + "/../tests/assets/test_audio1.mp3");
        QVERIFY(source.open(QIODevice::ReadOnly));
        QByteArray audio = source.readAll();
        // A tiny ID3v2.4 title fixture needs no media-encoding tool at test time.
        auto synchsafe = [](int size) {
            QByteArray bytes(4, '\0');
            for (int i = 3; i >= 0; --i) { bytes[i] = char(size & 127); size >>= 7; }
            return bytes;
        };
        if (audio.size() >= 10 && audio.startsWith("ID3")) {
            int size = 0;
            for (int i = 6; i < 10; ++i) size = (size << 7) | (uchar(audio[i]) & 127);
            audio.remove(0, 10 + size);
        }
        const QByteArray value = QByteArray(1, '\0') + title.toLatin1();
        const QByteArray frame = QByteArray("TIT2", 4) + synchsafe(value.size())
                                 + QByteArray(2, '\0') + value;
        const QByteArray tagged = QByteArray("ID3\4\0\0", 6) + synchsafe(frame.size()) + frame + audio;
        QFile fixture(directory.filePath("tagged.mp3"));
        QVERIFY(fixture.open(QIODevice::WriteOnly));
        QCOMPARE(fixture.write(tagged), tagged.size());
        fixture.close();

        QQmlEngine engine;
        QQmlComponent component(&engine, QUrl::fromLocalFile(QStringLiteral(MPVQML_EXAMPLES_DIR) + "/" + file));
        QTRY_VERIFY_WITH_TIMEOUT(component.isReady() || component.isError(), 5000);
        QVERIFY2(component.isReady(), qPrintable(component.errorString()));
        auto object = MpvQmlTestSupport::createTestWindow(component);
        QVERIFY2(object, qPrintable(component.errorString()));
        auto *window = qobject_cast<QQuickWindow *>(object.get());
        QVERIFY(window);
        QVERIFY(window->flags().testFlag(Qt::WindowTransparentForInput));
        auto *player = object->findChild<MpvPlayer *>();
        QVERIFY(player);
        // This metadata/UI test must not depend on a physical audio device.
        QSignalSpy audioConfigured(player, &MpvPlayer::mpvPropertySetFinished);
        const int audioRequest = player->setMpvPropertyAsync(QStringLiteral("ao"), QStringLiteral("null"));
        QVERIFY(audioRequest > 0);
        auto configured = [&]() {
            for (const auto &reply : audioConfigured)
                if (reply[0].toInt() == audioRequest) return reply[1].toInt() == 0;
            return false;
        };
        QTRY_VERIFY_WITH_TIMEOUT(configured(), 5000);
        if (file == QStringLiteral("AmbientScreensaver.qml")) object->setProperty("hasMedia", true);
        player->setPaused(true);
        player->setSource(fixture.fileName());
        QTRY_COMPARE_WITH_TIMEOUT(player->metadata().value("title").toString(), title, 5000);
        auto labels = [&]() {
            QList<QObject *> matches;
            QList<QQuickItem *> items{window->contentItem()};
            for (qsizetype i = 0; i < items.size(); ++i) {
                auto *item = items[i];
                items.append(item->childItems());
                if (item->inherits("QQuickText") && item->property("text").toString().contains(title))
                    matches.append(item);
            }
            return matches;
        };
        QTRY_VERIFY_WITH_TIMEOUT(!labels().isEmpty(), 5000);
        QTest::qWait(250); // let an AutoText image request reach the loopback trap
        QVERIFY2(!requests.hasPendingConnections(), "media markup triggered an image fetch");
        for (QObject *label : labels()) QCOMPARE(label->property("textFormat").toInt(), int(Qt::PlainText));
    }

    void idsAreSharedMonotonicAndTerminalRepliesAreExactlyOnce() {
        MockMpvController mock;
        mock.m_autoComplete = false;
        MpvPlayer player(nullptr, &mock);
        QSignalSpy command(&player, &MpvPlayer::asyncCommandFinished);
        QSignalSpy set(&player, &MpvPlayer::mpvPropertySetFinished);
        QSignalSpy read(&player, &MpvPlayer::mpvPropertyReadFinished);
        const int a = player.commandAsync(QStringList{"expand-text", "result"});
        const int b = player.setMpvPropertyAsync(QStringLiteral("pause"), true);
        const int c = player.readMpvPropertyAsync(QStringLiteral("pause"));
        QVERIFY(a > 0 && b > a && c > b);
        emit mock.asyncCommandFinished(a, 0, QStringLiteral("result"));
        emit mock.asyncCommandFinished(a, 0, QStringLiteral("duplicate"));
        emit mock.mpvPropertySetFinished(b, MPV_ERROR_PROPERTY_ERROR);
        emit mock.mpvPropertyReadFinished(c, 0, true);
        emit mock.asyncCommandFinished(c, 0, QStringLiteral("wrong kind"));
        QCOMPARE(command.size(), 0);
        QTRY_COMPARE(command.size(), 1);
        QTRY_COMPARE(set.size(), 1);
        QTRY_COMPARE(read.size(), 1);
        QCOMPARE(command[0][2].toString(), QStringLiteral("result"));
        QCOMPARE(set[0][1].toInt(), MPV_ERROR_PROPERTY_ERROR);
        QCOMPARE(read[0][2].toBool(), true);
        mock.m_nextId = static_cast<quint64>(std::numeric_limits<int>::max());
        QCOMPARE(player.readMpvPropertyAsync(QStringLiteral("pause")), std::numeric_limits<int>::max());
        QCOMPARE(player.commandAsync(QStringList{"stop"}), MPV_ERROR_NOMEM);
        QCOMPARE(player.setMpvPropertyAsync(QStringLiteral("pause"), false), MPV_ERROR_NOMEM);
    }

    void shutdownSettlesAdmissionsWithoutLateRevival() {
        MockMpvController mock;
        mock.m_autoComplete = false;
        MpvPlayer player(nullptr, &mock);
        QSignalSpy command(&player, &MpvPlayer::asyncCommandFinished);
        QSignalSpy read(&player, &MpvPlayer::mpvPropertyReadFinished);
        const int a = player.commandAsync(QStringList{"expand-text", "old"});
        const int b = player.readMpvPropertyAsync(QStringLiteral("pause"));
        player.shutdown();
        QCOMPARE(command.size(), 0);
        QTRY_COMPARE(command.size(), 1);
        QTRY_COMPARE(read.size(), 1);
        QCOMPARE(command[0][0].toInt(), a);
        QCOMPARE(command[0][1].toInt(), MPV_ERROR_UNINITIALIZED);
        QCOMPARE(read[0][0].toInt(), b);
        emit mock.asyncCommandFinished(a, 0, "late");
        emit mock.mpvPropertyReadFinished(b, 0, true);
        QCoreApplication::processEvents();
        QCOMPARE(command.size(), 1);
        QCOMPARE(read.size(), 1);
        QVERIFY(mock.available()); // an injected controller is not owned/shut down
        QCOMPARE(player.commandAsync(QStringList{"stop"}), MPV_ERROR_UNINITIALIZED);
    }

    void earlyStopCancelsDeferredSourceAndShutdownNeverFlushesIt() {
        MockMpvController mock;
        MpvPlayer player(nullptr, &mock);
        player.load(QStringLiteral("old.mp4"));
        player.stop(false);
        mock.reset();
        emit player.ready();
        QCOMPARE(mock.m_commandCount, 0);
        player.shutdown();
        emit player.ready();
        player.load(QStringLiteral("after-shutdown.mp4"));
        QCOMPARE(mock.m_commandCount, 0);
    }

    void navigationIsAdmittedEvenWhileRendererIsUnavailable() {
        MockMpvController mock;
        MpvPlayer player(nullptr, &mock);
        QVERIFY(!player.rendererReady());
        player.next();
        QCOMPARE(mock.m_lastCommand, QStringList({"playlist-next", "force"}));
        player.previous();
        QCOMPARE(mock.m_lastCommand, QStringList({"playlist-prev", "force"}));
        QCOMPARE(mock.m_commandCount, 2);
    }

    void reentrantSetterKeepsNewerIntentAndDeletionIsSafe() {
        MockMpvController mock;
        auto *player = new MpvPlayer(nullptr, &mock);
        emit player->ready();
        connect(player, &MpvPlayer::volumeChanged, player, [player]() {
            if (player->volume() == 70) player->setVolume(80);
        });
        mock.reset();
        player->setVolume(70);
        QCOMPARE(player->volume(), 80.0);
        QCOMPARE(mock.m_lastSetValue.toDouble(), 80.0);
        QCOMPARE(mock.m_setPropertyCount, 1);
        QPointer<MpvPlayer> guarded(player);
        connect(player, &MpvPlayer::pausedChanged, player, [player]() { delete player; });
        player->setPaused(true);
        QVERIFY(!guarded);
        QCoreApplication::processEvents(); // already queued mock completions cannot touch it
    }

    void constructionTimeIntentSurvivesNativeDefaultObservations() {
        MockMpvController mock;
        MpvPlayer player(nullptr, &mock);
        player.setVolume(75);
        player.setPaused(true);
        emit mock.propertyChanged(QStringLiteral("volume"), 100.0);
        emit mock.propertyChanged(QStringLiteral("pause"), false);
        QCOMPARE(player.volume(), 75.0);
        QVERIFY(player.paused());
        emit player.ready();
        QCOMPARE(mock.m_lastSetProperty, QStringLiteral("pause"));
        QCOMPARE(mock.m_lastSetValue.toString(), QStringLiteral("yes"));
    }

    void completionCanDeleteFacade() {
        MockMpvController mock;
        mock.m_autoComplete = false;
        auto *player = new MpvPlayer(nullptr, &mock);
        QPointer<MpvPlayer> guarded(player);
        const int id = player->commandAsync(QStringList{"expand-text", "owned"});
        connect(player, &MpvPlayer::asyncCommandFinished, player, [player]() { delete player; });
        emit mock.asyncCommandFinished(id, MPV_ERROR_COMMAND, {});
        QTRY_VERIFY(!guarded);
    }

    void initialFailureIsVisibleAndOperationsDoNotCreatePendingWork() {
        MockMpvController mock;
        mock.m_initialization = {true, false, MPV_ERROR_GENERIC, QStringLiteral("fixture failure")};
        MpvPlayer player(nullptr, &mock);
        QSignalSpy failure(&player, &MpvPlayer::initializationFailed);
        QVERIFY(!player.coreAvailable());
        QCOMPARE(player.initializationError(), MPV_ERROR_GENERIC);
        QCOMPARE(player.initializationErrorString(), QStringLiteral("fixture failure"));
        QCOMPARE(failure.size(), 0);
        QTRY_COMPARE(failure.size(), 1);
        player.load(QStringLiteral("failed.mp4"));
        player.setVolume(75);
        emit player.ready();
        QCOMPARE(mock.m_commandCount, 0);
        QCOMPARE(mock.m_setPropertyCount, 0);
        QVERIFY(!player.rendererReady());
        QCOMPARE(player.readMpvPropertyAsync(QStringLiteral("pause")), MPV_ERROR_UNINITIALIZED);
    }

    void nativeSubmissionRejectionStillCompletesItsPositiveId() {
        MpvPlayer player;
        QVERIFY(player.coreAvailable());
        QSignalSpy reply(&player, &MpvPlayer::asyncCommandFinished);
        const int id = player.commandAsync(QStringList{"__mpvqml_rejected_command__"});
        QVERIFY(id > 0);
        QCOMPARE(reply.size(), 0);
        QTRY_COMPARE(reply.size(), 1);
        QCOMPARE(reply[0][0].toInt(), id);
        QVERIFY(reply[0][1].toInt() < 0);
        player.shutdown();
    }
};

QTEST_MAIN(TestFacade)
#include "test_facade.moc"
