#include "mpvcontroller_p.h"
#include "mpvrenderer_p.h"
#include "mpvplayer.h"

#include <QColor>
#include <QImage>
#include <QQuickWindow>
#include <QtTest>
#include <utility>

using namespace MpvQmlPrivate;

// Standalone native consumer of the private renderer, not a mock readiness or
// a second product renderer. Its GUI-owned inputs are copied only in synchronize.
class RenderFixture : public QQuickFramebufferObject {
public:
  explicit RenderFixture(QQuickItem *parent) : QQuickFramebufferObject(parent) {
    bridge = std::make_shared<MpvRenderBridge>();
    bridge->dispatcher = MpvRenderDispatcher::create();
    const auto result = worker.initialize();
    initializationError = result.error;
    bridge->core = worker.core();
    connect(worker.controller(), &MpvController::reply, this,
            [this](const MpvNativeReply &reply) { replies.insert(reply.token, reply); },
            Qt::QueuedConnection);
    connect(worker.controller(), &MpvController::playbackRecovered, this,
            [this](quint64, qlonglong entry) { ++recoveries; recoveredEntry = entry; },
            Qt::QueuedConnection);
    connect(bridge->dispatcher.get(), &MpvRenderDispatcher::redrawRequested,
            this, &QQuickFramebufferObject::update);
    connect(bridge->dispatcher.get(), &MpvRenderDispatcher::contextRetiring, this,
            [this](quint64 generation, quint64 intent) {
      worker.post([generation, intent](MpvController *controller) {
        controller->renderContextRetiring(generation, intent);
      });
    });
    connect(bridge->dispatcher.get(), &MpvRenderDispatcher::statusChanged, this,
            [this](quint64 generation, bool value, int error, const QString &) {
      if (generation != bridge->generation.load())
        return;
      ready = value;
      renderError = error;
      if (value) {
        ++successes;
        if (loaded && !replacementProperties.isEmpty()) {
          const auto properties = std::exchange(replacementProperties, {});
          for (auto it = properties.cbegin(); it != properties.cend(); ++it)
            setProperty(it.key(), it.value());
        }
        if (loaded && !replacementCommand.isEmpty()) {
          replacementToken = command(replacementCommand);
          replacementCommand.clear();
        }
        worker.post([generation](MpvController *controller) {
          controller->renderContextReady(generation);
        });
        if (!loaded) {
          loaded = true;
          const QString path = QStringLiteral(MPVQML_TEST_VIDEO);
          setProperty(QStringLiteral("loop-file"), QStringLiteral("inf"));
          command(QStringList{"loadfile", path});
        }
      }
    });
  }

  quint64 command(const QStringList &parameters) {
    const quint64 token = bridge->core->allocateNativeToken();
    worker.post([token, parameters](MpvController *controller) {
      controller->submitCommand(token, parameters);
    });
    return token;
  }

  quint64 setProperty(const QString &name, const QVariant &value) {
    const quint64 token = bridge->core->allocateNativeToken();
    worker.post([token, name, value](MpvController *controller) {
      controller->submitPropertySet(token, name, value);
    });
    return token;
  }

  quint64 readProperty(const QString &name) {
    const quint64 token = bridge->core->allocateNativeToken();
    worker.post([token, name](MpvController *controller) {
      controller->submitPropertyRead(token, name);
    });
    return token;
  }

  Renderer *createRenderer() const override {
    const quint64 generation = bridge->generation.fetch_add(1) + 1;
    return new MpvRenderer([this]() {
      MpvRenderSnapshot snapshot;
      snapshot.core = bridge->core;
      snapshot.sourceIntent = bridge->core ? bridge->core->sourceIntent() : 0;
      return snapshot;
    }, bridge->dispatcher, generation);
  }

  MpvBackendWorker worker;
  std::shared_ptr<MpvRenderBridge> bridge;
  bool ready = false;
  bool loaded = false;
  int renderError = 0;
  int initializationError = 0;
  int successes = 0;
  int recoveries = 0;
  qlonglong recoveredEntry = -1;
  QHash<quint64, MpvNativeReply> replies;
  QStringList replacementCommand;
  quint64 replacementToken = 0;
  QVariantMap replacementProperties;
};

class TestRenderer : public QObject {
  Q_OBJECT
private slots:
  void survivorPixelsContinueAcrossPeerDeletion() {
    QQuickWindow survivor;
    survivor.resize(160, 120);
    survivor.setPersistentSceneGraph(false);
    survivor.setPersistentGraphics(false);
    auto addPlayer = [](QQuickWindow *window) {
      auto *player = new MpvPlayer(window->contentItem());
      player->setSize(window->size());
      connect(window, &QQuickWindow::widthChanged, player, [player](int value) { player->setWidth(value); });
      connect(window, &QQuickWindow::heightChanged, player, [player](int value) { player->setHeight(value); });
      player->setHwdec(QStringLiteral("no"));
      player->setLoopFile(QStringLiteral("inf"));
      player->setSource(QStringLiteral(MPVQML_TEST_VIDEO));
      return player;
    };
    auto blue = [](QQuickWindow *window) {
      const QImage image = window->grabWindow();
      return !image.isNull() && image.pixelColor(image.width() / 2, image.height() / 2).blue() > 150;
    };
    auto *player = addPlayer(&survivor);
    survivor.show();
    QVERIFY(QTest::qWaitForWindowExposed(&survivor));
    QTRY_VERIFY_WITH_TIMEOUT(player->rendererReady() && blue(&survivor), 5000);
    for (int i = 0; i < 10; ++i) {
      auto peer = std::make_unique<QQuickWindow>();
      peer->resize(160, 120);
      peer->setPersistentSceneGraph(false);
      peer->setPersistentGraphics(false);
      auto *other = addPlayer(peer.get());
      peer->show();
      QVERIFY(QTest::qWaitForWindowExposed(peer.get()));
      QTRY_VERIFY_WITH_TIMEOUT(other->rendererReady() && blue(peer.get()), 5000);
      delete other;
      peer.reset();
      QCoreApplication::processEvents();
      QTRY_VERIFY_WITH_TIMEOUT(player->rendererReady() && blue(&survivor), 5000);
    }
    delete player;
    survivor.hide();
    survivor.releaseResources();
    QTRY_COMPARE_WITH_TIMEOUT(MpvCoreLease::outstandingReservations(), 0, 5000);
  }

  void publicFacadeRendersAndAutomaticallyRecoversVideo() {
    QQuickWindow window;
    window.setColor(Qt::black);
    window.resize(160, 120);
    window.setPersistentSceneGraph(false);
    window.setPersistentGraphics(false);
    auto *player = new MpvPlayer(window.contentItem());
    player->setSize(window.size());
    connect(&window, &QQuickWindow::widthChanged, player, [player](int value) { player->setWidth(value); });
    connect(&window, &QQuickWindow::heightChanged, player, [player](int value) { player->setHeight(value); });
    player->setHwdec(QStringLiteral("no"));
    player->setLoopFile(QStringLiteral("inf"));
    player->setLogLevel(QStringLiteral("info"));
    connect(player, &MpvPlayer::hookTriggered, player, [player](const QString &, quint64 id) {
      player->hookContinue(id);
    });
    player->hookAdd(QStringLiteral("on_load"));
    QSignalSpy hooks(player, &MpvPlayer::hookTriggered);
    player->setSource(QStringLiteral(MPVQML_TEST_VIDEO));
    QSignalSpy readiness(player, &MpvPlayer::ready);
    window.show();
    QVERIFY(QTest::qWaitForWindowExposed(&window));
    QTRY_VERIFY_WITH_TIMEOUT(player->rendererReady(), 5000);
    auto blue = [&window]() {
      const QImage image = window.grabWindow();
      if (image.isNull()) return false;
      const QColor pixel = image.pixelColor(image.width() / 2, image.height() / 2);
      return pixel.blue() > 150 && pixel.red() < 80;
    };
    QTRY_VERIFY_WITH_TIMEOUT(blue(), 5000);
    QVERIFY(hooks.size() >= 1);
    const int initialReady = readiness.size();
    window.hide();
    window.releaseResources();
    QTRY_VERIFY(!player->rendererReady());
    window.show();
    QVERIFY(QTest::qWaitForWindowExposed(&window));
    QTRY_VERIFY_WITH_TIMEOUT(player->rendererReady() && readiness.size() > initialReady, 5000);
    QTRY_VERIFY_WITH_TIMEOUT(blue(), 5000);
    QVERIFY(hooks.size() >= 2); // rendered recovery continues every registered hook
    delete player;
    window.hide();
    window.releaseResources();
    QTRY_COMPARE_WITH_TIMEOUT(MpvCoreLease::outstandingReservations(), 0, 5000);
  }

  void actualVideoPixelsAndContextRecreation() {
    QQuickWindow window;
    window.setColor(Qt::black);
    window.setWidth(160);
    window.setHeight(120);
    window.setPersistentSceneGraph(false);
    window.setPersistentGraphics(false);
    auto *item = new RenderFixture(window.contentItem());
    item->setSize(window.size());
    connect(&window, &QQuickWindow::widthChanged, item,
            [item](int width) { item->setWidth(width); });
    connect(&window, &QQuickWindow::heightChanged, item,
            [item](int height) { item->setHeight(height); });
    QCOMPARE(item->initializationError, MPV_ERROR_SUCCESS);
    window.show();
    QVERIFY(QTest::qWaitForWindowExposed(&window));
    QTRY_VERIFY_WITH_TIMEOUT(item->ready, 5000);
    auto blue = [&window]() {
      const QImage image = window.grabWindow();
      if (image.isNull())
        return false;
      const QColor pixel = image.pixelColor(image.width() / 2, image.height() / 2);
      return pixel.blue() > 150 && pixel.red() < 80 && pixel.green() < 80;
    };
    QTRY_VERIFY_WITH_TIMEOUT(blue(), 5000);
    const quint64 appended = item->command(QStringList{
        "loadfile", QStringLiteral(MPVQML_TEST_VIDEO), "append"});
    QTRY_VERIFY(item->replies.contains(appended));
    QCOMPARE(item->replies[appended].error, MPV_ERROR_SUCCESS);
    const quint64 paused = item->setProperty(QStringLiteral("pause"), true);
    QTRY_VERIFY(item->replies.contains(paused));
    const quint64 seeked = item->command(QStringList{"seek", "0.4", "absolute+exact"});
    QTRY_VERIFY(item->replies.contains(seeked));
    QCOMPARE(item->replies[seeked].error, MPV_ERROR_SUCCESS);
    const quint64 beforePosition = item->readProperty(QStringLiteral("time-pos"));
    const quint64 beforePlaylist = item->readProperty(QStringLiteral("playlist"));
    QTRY_VERIFY(item->replies.contains(beforePosition) && item->replies.contains(beforePlaylist));
    const double position = item->replies[beforePosition].value.toDouble();
    const QVariantList playlist = item->replies[beforePlaylist].value.toList();
    QCOMPARE(playlist.size(), 2);
    QVERIFY(position >= 0.3);
    const int initialSuccesses = item->successes;
    window.hide();
    window.releaseResources();
    QTRY_VERIFY_WITH_TIMEOUT(!item->ready, 5000);
    window.show();
    QVERIFY(QTest::qWaitForWindowExposed(&window));
    QTRY_VERIFY_WITH_TIMEOUT(item->ready && item->successes > initialSuccesses, 5000);
    QTRY_VERIFY_WITH_TIMEOUT(blue(), 5000);
    QTRY_COMPARE_WITH_TIMEOUT(item->recoveries, 1, 5000);
    const quint64 afterPosition = item->readProperty(QStringLiteral("time-pos"));
    const quint64 afterPlaylist = item->readProperty(QStringLiteral("playlist"));
    const quint64 afterPause = item->readProperty(QStringLiteral("pause"));
    QTRY_VERIFY(item->replies.contains(afterPosition) && item->replies.contains(afterPlaylist)
                && item->replies.contains(afterPause));
    QVERIFY(qAbs(item->replies[afterPosition].value.toDouble() - position) < 0.08);
    QVERIFY(item->replies[afterPause].value.toBool());
    const QVariantList restored = item->replies[afterPlaylist].value.toList();
    QCOMPARE(restored.size(), playlist.size());
    for (int i = 0; i < restored.size(); ++i) {
      QCOMPARE(restored[i].toMap().value("id"), playlist[i].toMap().value("id"));
      QCOMPARE(restored[i].toMap().value("filename"), playlist[i].toMap().value("filename"));
    }
    QCOMPARE(item->recoveredEntry, playlist[0].toMap().value("id").toLongLong());
    QCOMPARE(item->renderError, MPV_ERROR_SUCCESS);
    delete item;
    window.hide();
    window.releaseResources();
    QTRY_COMPARE_WITH_TIMEOUT(MpvCoreLease::outstandingReservations(), 0, 5000);
  }

  void recoveryDoesNotOverrideNewIntent_data() {
    QTest::addColumn<bool>("sourceChange");
    QTest::newRow("stop") << false;
    QTest::newRow("new-source") << true;
  }

  void newerPauseAndSeekSupersedeSavedState_data() {
    QTest::addColumn<bool>("newSeek");
    QTest::newRow("new-pause") << false;
    QTest::newRow("new-seek") << true;
  }

  void newerPauseAndSeekSupersedeSavedState() {
    QFETCH(bool, newSeek);
    QQuickWindow window;
    window.resize(160, 120);
    window.setPersistentSceneGraph(false);
    window.setPersistentGraphics(false);
    auto *item = new RenderFixture(window.contentItem());
    item->setSize(window.size());
    connect(&window, &QQuickWindow::widthChanged, item, [item](int value) { item->setWidth(value); });
    connect(&window, &QQuickWindow::heightChanged, item, [item](int value) { item->setHeight(value); });
    window.show();
    QVERIFY(QTest::qWaitForWindowExposed(&window));
    QTRY_VERIFY(item->ready);
    auto blue = [&]() {
      const QImage image = window.grabWindow();
      return !image.isNull() && image.pixelColor(image.width() / 2, image.height() / 2).blue() > 150;
    };
    QTRY_VERIFY_WITH_TIMEOUT(blue(), 5000);
    const quint64 pause = item->setProperty(QStringLiteral("pause"), true);
    const quint64 seek = item->command(QStringList{"seek", "0.4", "absolute+exact"});
    QTRY_VERIFY(item->replies.contains(pause) && item->replies.contains(seek));
    const quint64 position = item->readProperty(QStringLiteral("time-pos"));
    QTRY_VERIFY(item->replies.contains(position));
    QVERIFY(item->replies[position].value.toDouble() >= 0.3);
    item->replacementProperties = newSeek
        ? QVariantMap{{QStringLiteral("time-pos"), 0.7}}
        : QVariantMap{{QStringLiteral("pause"), false}};
    window.hide();
    window.releaseResources();
    QTRY_VERIFY(!item->ready);
    window.show();
    QVERIFY(QTest::qWaitForWindowExposed(&window));
    QTRY_VERIFY_WITH_TIMEOUT(item->ready && item->recoveries == 1, 5000);
    QTRY_VERIFY_WITH_TIMEOUT(blue(), 5000);
    const quint64 finalPosition = item->readProperty(QStringLiteral("time-pos"));
    const quint64 finalPause = item->readProperty(QStringLiteral("pause"));
    QTRY_VERIFY(item->replies.contains(finalPosition) && item->replies.contains(finalPause));
    QCOMPARE(item->replies[finalPause].value.toBool(), newSeek);
    if (newSeek) QVERIFY(qAbs(item->replies[finalPosition].value.toDouble() - 0.7) < 0.08);
    delete item;
    window.hide();
    window.releaseResources();
    QTRY_COMPARE_WITH_TIMEOUT(MpvCoreLease::outstandingReservations(), 0, 5000);
  }

  void recoveryDoesNotOverrideNewIntent() {
    QFETCH(bool, sourceChange);
    QQuickWindow window;
    window.setColor(Qt::black);
    window.resize(160, 120);
    window.setPersistentSceneGraph(false);
    window.setPersistentGraphics(false);
    auto *item = new RenderFixture(window.contentItem());
    item->setSize(window.size());
    connect(&window, &QQuickWindow::widthChanged, item, [item](int value) { item->setWidth(value); });
    connect(&window, &QQuickWindow::heightChanged, item, [item](int value) { item->setHeight(value); });
    window.show();
    QVERIFY(QTest::qWaitForWindowExposed(&window));
    QTRY_VERIFY(item->ready);
    auto pixel = [&window]() {
      QImage image = window.grabWindow();
      return image.isNull() ? QColor() : image.pixelColor(image.width() / 2, image.height() / 2);
    };
    QTRY_VERIFY_WITH_TIMEOUT(pixel().blue() > 150, 5000);
    item->replacementCommand = sourceChange
        ? QStringList{"loadfile", QStringLiteral(MPVQML_TEST_RED_VIDEO)} : QStringList{"stop"};
    window.hide();
    window.releaseResources();
    QTRY_VERIFY(!item->ready);
    window.show();
    QVERIFY(QTest::qWaitForWindowExposed(&window));
    QTRY_VERIFY(item->ready && item->replacementToken != 0);
    QTRY_VERIFY(item->replies.contains(item->replacementToken));
    QCOMPARE(item->replies[item->replacementToken].error, MPV_ERROR_SUCCESS);
    if (sourceChange) {
      QTRY_VERIFY_WITH_TIMEOUT(pixel().red() > 150 && pixel().blue() < 80, 5000);
    } else {
      const quint64 idle = item->readProperty(QStringLiteral("idle-active"));
      QTRY_VERIFY(item->replies.contains(idle));
      QVERIFY(item->replies[idle].value.toBool());
      QVERIFY(pixel().blue() < 80);
    }
    QTest::qWait(100); // allow delayed terminal/recovery deliveries to drain
    QCOMPARE(item->recoveries, 0);
    delete item;
    window.hide();
    window.releaseResources();
    QTRY_COMPARE_WITH_TIMEOUT(MpvCoreLease::outstandingReservations(), 0, 5000);
  }
};

QTEST_MAIN(TestRenderer)
#include "test_renderer.moc"
