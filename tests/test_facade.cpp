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

    void operationFailureKeepsSeamMessage() {
        MockMpvController mock;
        MpvPlayer player(nullptr, &mock);
        QSignalSpy failed(&player, &MpvPlayer::operationFailed);
        QSignalSpy error(&player, &MpvPlayer::errorOccurred);
        const QString nativeDetail = QStringLiteral("native observer context");
        emit mock.operationFailed("observeProperty", MPV_ERROR_GENERIC, nativeDetail);
        QCOMPARE(failed.size(), 1);
        QCOMPARE(failed[0][2].toString(), nativeDetail);
        QCOMPARE(error.size(), 1);
        QCOMPARE(error[0][1].toString(), nativeDetail);
    }

    void nativePlaybackDiagnosticsKeepEntryAndNativeCode() {
        MpvPlayer player;
        QSignalSpy details(&player, SIGNAL(playbackEvent(QVariantMap)));
        QSignalSpy done(&player, &MpvPlayer::asyncCommandFinished);
        QSignalSpy legacy(&player, &MpvPlayer::endFile);
        QTemporaryDir fixture;
        QVERIFY(fixture.isValid());
        QFile invalid(fixture.filePath("invalid.mp4"));
        QVERIFY(invalid.open(QIODevice::WriteOnly));
        QVERIFY(invalid.write("not a video container") > 0);
        invalid.close();
        const int id = player.commandAsync({"loadfile", invalid.fileName()});
        QVERIFY(id > 0);
        QTRY_COMPARE(done.size(), 1);
        QCOMPARE(done[0][0].toInt(), id);
        QCOMPARE(done[0][1].toInt(), 0);
        QTRY_COMPARE(legacy.size(), 1);
        QCOMPARE(legacy[0][0].toString(), QStringLiteral("error"));
        QVERIFY(details.isValid());
        QTRY_COMPARE(details.size(), 2);
        const auto start = details[0][0].toMap();
        const auto end = details[1][0].toMap();
        QCOMPARE(start.value("stage").toString(), QStringLiteral("start-file"));
        QCOMPARE(end.value("stage").toString(), QStringLiteral("end-file"));
        QCOMPARE(end.value("reason").toString(), QStringLiteral("error"));
        QCOMPARE(end.value("nativeCode").toInt(), MPV_ERROR_UNKNOWN_FORMAT);
        QCOMPARE(end.value("nativeReason").toInt(), MPV_END_FILE_REASON_ERROR);
        QVERIFY(!start.value("entryId").toString().isEmpty());
        QVERIFY(!start.value("generation").toString().isEmpty());
        QCOMPARE(end.value("entryId"), start.value("entryId"));
        QCOMPARE(end.value("generation"), start.value("generation"));
    }

    void nativePlaybackExampleTracksLoadSeekRestartAndShutdown() {
        QFile doc(QStringLiteral(MPVQML_EXAMPLES_DIR) + "/../API.md");
        QVERIFY(doc.open(QIODevice::ReadOnly));
        const auto bytes = doc.readAll();
        const QByteArray startMark = "<!-- playback-example -->\n```qml\n";
        const auto offset = bytes.indexOf(startMark);
        QVERIFY(offset >= 0);
        const auto start = offset + startMark.size();
        const auto end = bytes.indexOf("\n```", start);
        QVERIFY(end > start);
        QQmlEngine engine;
        QQmlComponent component(&engine);
        component.setData(bytes.mid(start, end - start), QUrl());
        QTRY_VERIFY(component.isReady() || component.isError());
        QVERIFY2(component.isReady(), qPrintable(component.errorString()));
        std::unique_ptr<QObject> object(component.create());
        QVERIFY2(object, qPrintable(component.errorString()));
        auto *player = qobject_cast<MpvPlayer *>(object.get());
        QVERIFY(player);
        QSignalSpy details(player, &MpvPlayer::playbackEvent);
        QSignalSpy writes(player, &MpvPlayer::mpvPropertySetFinished);
        QVERIFY(player->setMpvPropertyAsync("ao", "null") > 0);
        QVERIFY(player->setMpvPropertyAsync("pause", true) > 0);
        QTRY_COMPARE(writes.size(), 2);
        for (const auto &write : writes) QCOMPARE(write[1].toInt(), 0);
        const auto audio = QStringLiteral(MPVQML_EXAMPLES_DIR) + "/../tests/assets/test_audio1.mp3";
        QVERIFY(QMetaObject::invokeMethod(player, "openMedia", audio));
        QTRY_VERIFY(player->property("commandAccepted").toBool());
        QTRY_COMPARE(player->property("restartCount").toInt(), 1);
        QStringList stages;
        for (const auto &detail : details) stages << detail[0].toMap().value("stage").toString();
        QCOMPARE(stages, (QStringList{"start-file", "file-loaded", "playback-restart"}));
        player->seekAbsolute(0.2);
        QTRY_COMPARE(player->property("restartCount").toInt(), 2);
        QCOMPARE(details.size(), 5);
        QCOMPARE(details[3][0].toMap().value("stage").toString(), QStringLiteral("seek"));
        QCOMPARE(details[4][0].toMap().value("entryId"), details[0][0].toMap().value("entryId"));
        QVERIFY(player->commandAsync({"quit"}) > 0);
        QTRY_VERIFY(!player->coreAvailable());
        QCOMPARE(details.last()[0].toMap().value("stage").toString(), QStringLiteral("shutdown"));
        QCOMPARE(details.at(details.size() - 2)[0].toMap().value("reason").toString(), QStringLiteral("quit"));
    }

    void checkedRequestsKeepLegacySettlementOrder() {
        MockMpvController mock;
        mock.m_autoComplete = false;
        MpvPlayer player(nullptr, &mock);
        QStringList order;
        connect(&player, &MpvPlayer::mpvPropertyReadFinished, this, [&](int, int, const QVariant &value) {
            QVERIFY(!value.isValid());
            order << "read";
        });
        connect(&player, &MpvPlayer::operationFailed, this, [&](const QString &name, int, const QString &) {
            order << name;
        });
        const int id = player.readMpvPropertyAsync("time-pos");
        QVERIFY(id > 0);
        emit mock.asyncCommandFinished(id, 0, {}); // wrong kind cannot consume the ID
        emit mock.mpvPropertyReadFinished(id, MPV_ERROR_PROPERTY_UNAVAILABLE, {});
        emit mock.mpvPropertyReadFinished(id, MPV_ERROR_PROPERTY_UNAVAILABLE, {});
        QTRY_COMPARE(order, (QStringList{"read", "time-pos"}));
        QCoreApplication::sendPostedEvents(&player, QEvent::MetaCall);
        QCOMPARE(order.size(), 2);

        const int closed = player.readMpvPropertyAsync("time-pos");
        QVERIFY(closed > id);
        emit mock.mpvPropertyReadFinished(closed, 0, 42.0);
        player.shutdown();
        QTRY_COMPARE(order, (QStringList{"read", "time-pos", "read"}));
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

    void knownMediaEffectsSupersedeDeferredLoads_data() {
        QTest::addColumn<QString>("operation");
        for (const char *op : {"raw-stop", "prefixed-stop", "prefixed-set", "raw-next",
                               "raw-playlist-pos", "clear", "raw-clear"})
            QTest::newRow(op) << QString::fromLatin1(op);
    }

    void knownMediaEffectsSupersedeDeferredLoads() {
        QFETCH(QString, operation);
        MockMpvController mock;
        MpvPlayer player(nullptr, &mock);
        player.load("old.mp4");
        if (operation == "raw-stop") QVERIFY(player.commandAsync({"stop"}) > 0);
        if (operation == "prefixed-stop" || operation == "prefixed-set") {
            const QStringList command = operation == "prefixed-stop"
                ? QStringList{"no-osd", "async", "stop"}
                : QStringList{"no-osd", "set", "playlist-pos", "0"};
            QVERIFY(player.commandAsync(command) > 0);
            QCOMPARE(mock.m_lastCommand, command);
        }
        if (operation == "raw-next") QVERIFY(player.commandAsync({"playlist-next", "force"}) > 0);
        if (operation == "raw-playlist-pos") QVERIFY(player.setMpvPropertyAsync("playlist-pos", 1) > 0);
        if (operation == "clear") player.clearPlaylist();
        if (operation == "raw-clear") QVERIFY(player.commandAsync({"playlist-clear"}) > 0);
        mock.reset();
        emit player.ready();
        QCOMPARE(mock.m_commandCount, 0);
    }

    void nonReplacingPropertyWriteKeepsDeferredLoad_data() {
        QTest::addColumn<QString>("property");
        QTest::addColumn<QVariant>("value");
        QTest::newRow("read-only-path") << QStringLiteral("path") << QVariant("ignored.mp4");
        QTest::newRow("playlist-cursor") << QStringLiteral("playlist-current-pos") << QVariant(-1);
    }

    void nonReplacingPropertyWriteKeepsDeferredLoad() {
        QFETCH(QString, property);
        QFETCH(QVariant, value);
        MockMpvController mock;
        MpvPlayer player(nullptr, &mock);
        player.load("old.mp4");
        QVERIFY(player.setMpvPropertyAsync(property, value) > 0);
        mock.reset();
        emit player.ready();
        QCOMPARE(mock.m_lastCommand, QStringList({"loadfile", "old.mp4", "replace"}));
    }

    void reentrantClearCancelsSourceAndPreservesDeferredTrackSelection() {
        MockMpvController mock;
        MpvPlayer player(nullptr, &mock);
        player.selectTrack("audio", 2);
        connect(&player, &MpvPlayer::sourceChanged, &player, [&]() {
            player.selectTrack("video", 3);
            player.clearPlaylist();
        });
        player.setSource("deferred.mp4");
        QCOMPARE(mock.m_commands, QList<QStringList>{{"playlist-clear"}});
        emit player.ready();
        QCOMPARE(mock.m_commands, QList<QStringList>{{"playlist-clear"}});
        QCOMPARE(mock.m_setValues.value("vid"), QVariant(3));
    }

    void videoTrackSelectionHasNoUnscopedContinuation_data() {
        QTest::addColumn<QString>("superseding");
        for (const char *op : {"source", "next", "seek"}) QTest::newRow(op) << QString::fromLatin1(op);
    }

    void videoTrackSelectionHasNoUnscopedContinuation() {
        QFETCH(QString, superseding);
        MockMpvController mock;
        MpvPlayer player(nullptr, &mock);
        player.setSource("first.mp4");
        emit player.ready();
        player.load("other.mp4"); // source is not the currently loaded entry
        emit mock.propertyChanged("time-pos", 7.0);
        mock.reset();
        player.selectTrack("video", 2);
        QCOMPARE(mock.m_setValues.value("vid"), QVariant(2));
        QCOMPARE(mock.m_commandCount, 0);
        if (superseding == "source") player.setSource("new.mp4");
        if (superseding == "next") player.next();
        if (superseding == "seek") player.seekAbsolute(1.0);
        mock.reset();
        emit mock.fileLoaded();
        QCOMPARE(mock.m_commandCount, 0);
    }

    void directoryWorkReturnsBeforeDispatchAndCanBeSuperseded() {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        for (const char *name : {"b.mp4", "a.MP4", "z.mp4"}) {
            QFile file(dir.filePath(QString::fromLatin1(name)));
            QVERIFY(file.open(QIODevice::WriteOnly));
        }
        MockMpvController mock;
        MpvPlayer player(nullptr, &mock);
        emit player.ready();
        player.loadDirectory(dir.path());
        QCOMPARE(mock.m_commandCount, 0);
        player.clearPlaylist();
        player.loadDirectory(dir.path(), {}, "append-play", "name"); // legacy append-only
        player.load("after.mp4", "append");
        QTRY_COMPARE(mock.m_commandCount, 5);
        const QList<QStringList> expected{
            {"playlist-clear"}, {"loadfile", dir.filePath("a.MP4"), "append"},
            {"loadfile", dir.filePath("b.mp4"), "append"}, {"loadfile", dir.filePath("z.mp4"), "append"},
            {"loadfile", "after.mp4", "append"},
        };
        QCOMPARE(mock.m_commands, expected);
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

    void pendingPropertyIgnoresOlderObservationsUntilReadback() {
        MockMpvController mock;
        MpvPlayer player(nullptr, &mock);
        emit player.ready();
        QCoreApplication::processEvents();
        mock.m_autoComplete = false;
        emit mock.propertyChanged("volume", 100.0);
        player.setVolume(75.0);
        const int write = mock.m_nextId - 1;
        emit mock.propertyChanged("volume", 50.0);
        QCOMPARE(player.volume(), 75.0);
        emit mock.mpvPropertySetFinished(write, 0);
        QTRY_COMPARE(mock.m_lastReadProperty, QStringLiteral("volume"));
        const int olderRead = mock.m_nextId - 1;
        player.setVolume(80.0);
        const int newerWrite = mock.m_nextId - 1;
        emit mock.mpvPropertyReadFinished(olderRead, 0, 75.0);
        QCoreApplication::processEvents();
        QCOMPARE(player.volume(), 80.0);
        emit mock.mpvPropertySetFinished(newerWrite, 0);
        QTRY_VERIFY(mock.m_nextId > static_cast<quint64>(newerWrite + 1));
        emit mock.mpvPropertyReadFinished(mock.m_nextId - 1, 0, 79.0);
        QTRY_COMPARE(player.volume(), 79.0);
        emit mock.propertyChanged("volume", QVariant());
        QCOMPARE(player.volume(), 79.0);
        emit mock.propertyChanged("volume", 81.0);
        QCOMPARE(player.volume(), 81.0);
    }

    void rejectedPropertyReturnsToConfirmedState() {
        MockMpvController mock;
        MpvPlayer player(nullptr, &mock);
        emit player.ready();
        QCoreApplication::processEvents();
        mock.reset();
        mock.m_autoComplete = false;
        emit mock.propertyChanged("volume", 90.0);
        QSignalSpy failures(&player, &MpvPlayer::operationFailed);
        player.setVolume(76.0);
        const int write = mock.m_nextId - 1;
        emit mock.mpvPropertySetFinished(write, MPV_ERROR_PROPERTY_ERROR);
        QTRY_COMPARE(failures.size(), 1);
        QCOMPARE(mock.m_lastReadProperty, QStringLiteral("volume"));
        emit mock.mpvPropertyReadFinished(mock.m_nextId - 1, MPV_ERROR_PROPERTY_UNAVAILABLE, {});
        QTRY_COMPARE(player.volume(), 90.0);
        QCOMPARE(failures.size(), 1); // internal reconciliation is not a caller request
        mock.m_nextId = static_cast<quint64>(std::numeric_limits<int>::max()) + 1;
        player.setVolume(33.0);
        QCOMPARE(player.volume(), 90.0);
        QCOMPARE(failures.size(), 2);
    }

    void deferredPropertyClearsSurviveReadiness() {
        MockMpvController mock;
        MpvPlayer player(nullptr, &mock);
        player.setUserShaders(QStringLiteral("custom.glsl"));
        player.setScreenshotDirectory(QStringLiteral("/configured"));
        QVERIFY(!player.rendererReady());
        mock.reset();
        player.setUserShaders({});
        player.setScreenshotDirectory({});
        QCOMPARE(mock.m_setPropertyCount, 0);
        emit player.ready();
        QVERIFY(player.rendererReady());
        QVERIFY(mock.m_setValues.contains("glsl-shaders"));
        QCOMPARE(mock.m_setValues.value("glsl-shaders"), QVariant(QVariantList()));
        QVERIFY(mock.m_setValues.contains("screenshot-directory"));
        QVERIFY(mock.m_setValues.value("screenshot-directory").toString().isEmpty());
    }

    void clearsOverrideRawWritesBeforeObservation() {
        MockMpvController mock;
        MpvPlayer player(nullptr, &mock);
        emit player.ready();
        mock.reset();
        QVERIFY(player.screenshotDirectory().isEmpty());
        QVERIFY(player.userShaders().isEmpty());
        QVERIFY(player.setMpvPropertyAsync("screenshot-dir", "/configured") > 0);
        QVERIFY(player.setMpvPropertyAsync("glsl-shaders", QVariantList{"custom.glsl"}) > 0);
        player.setScreenshotDirectory({});
        player.setUserShaders({});
        QVERIFY(mock.m_setValues.contains("screenshot-directory"));
        QVERIFY(mock.m_setValues.value("screenshot-directory").toString().isEmpty());
        QCOMPARE(mock.m_setValues.value("glsl-shaders"), QVariant(QVariantList()));
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
