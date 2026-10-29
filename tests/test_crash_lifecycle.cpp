#include "faults.h"
#include "mpvcontroller_p.h"
#include "mpvplayer.h"

#include <QCoreApplication>
#include <QGuiApplication>
#include <QQuickWindow>
#include <QImage>
#include <QTemporaryDir>
#include <QFileInfo>
#include <QFile>
#include <QProcess>
#include <QProcessEnvironment>
#include <QSignalSpy>
#include <QTimer>
#include <QtTest>
#include <array>
#include <atomic>
#include <clocale>
#include <dlfcn.h>
#include <memory>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

using namespace MpvQmlPrivate;

namespace {
QString fixtureName;

struct FaultApi {
  decltype(&mpvqml_fault_configure) configure = nullptr;
  decltype(&mpvqml_fault_counter) counter = nullptr;
  decltype(&mpvqml_fault_release) release = nullptr;
  decltype(&mpvqml_fault_seed_tls) seedTls = nullptr;
  decltype(&mpvqml_fault_watch_file) watchFile = nullptr;
};

FaultApi faults;

// Even a failed assertion must release native barriers before process teardown.
struct ReleaseBarrier {
  ~ReleaseBarrier() { faults.release(); }
};

void exercised() {
  qInfo().noquote() << "FAULT_CASE_EXERCISED" << fixtureName;
}
} // namespace

class NativeFaultCases : public QObject {
  Q_OBJECT
private slots:
  void cleanupTestCase() {
    QTRY_COMPARE_WITH_TIMEOUT(MpvCoreLease::outstandingReservations(), 0, 5000);
    QCoreApplication::sendPostedEvents(nullptr, QEvent::MetaCall);
    QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
  }
  void initTestCase() {
    faults.configure = reinterpret_cast<decltype(faults.configure)>(dlsym(RTLD_DEFAULT, "mpvqml_fault_configure"));
    faults.counter = reinterpret_cast<decltype(faults.counter)>(dlsym(RTLD_DEFAULT, "mpvqml_fault_counter"));
    faults.release = reinterpret_cast<decltype(faults.release)>(dlsym(RTLD_DEFAULT, "mpvqml_fault_release"));
    faults.seedTls = reinterpret_cast<decltype(faults.seedTls)>(dlsym(RTLD_DEFAULT, "mpvqml_fault_seed_tls"));
    faults.watchFile = reinterpret_cast<decltype(faults.watchFile)>(dlsym(RTLD_DEFAULT, "mpvqml_fault_watch_file"));
    QVERIFY2(faults.configure && faults.counter && faults.release && faults.seedTls && faults.watchFile,
             "child-only fault fixture did not load; case NOT_EXERCISED");
    QVERIFY(std::setlocale(LC_NUMERIC, "C"));
    int flags = 0;
    int failThread = 0;
    if (fixtureName == "coreCreationFailure") flags = FaultCreate;
    if (fixtureName == "coreInitializationFailure") flags = FaultInitialize;
    if (fixtureName == "partialRetirementBootstrapFailure") failThread = 2;
    if (fixtureName == "workerStartupFailure") failThread = 3;
    if (fixtureName == "bothWorkersBlockedAndAllLiveLeasesReclaimed") flags = FaultRetirementBarrier;
    if (fixtureName == "workerTlsCleanupOutlivesFacade") flags = FaultTlsBarrier;
    if (fixtureName == "publicCoreCreationFailure") flags = FaultCreate;
    if (fixtureName == "publicCoreInitializationFailure") flags = FaultInitialize;
    if (fixtureName == "publicWorkerStartupFailure") failThread = 3;
    if (fixtureName == "publicRenderFailure") flags = FaultRenderCreate;
    if (fixtureName == "ordinaryFileScreenshotRetirement") flags = FaultFileBarrier;
    if (fixtureName == "synchronousHelperRegistration") flags = FaultSynchronousWakeup;
    if (fixtureName == "recycledHelperAddressRejectsOldWakeups") flags = FaultSynchronousWakeup;
    faults.configure(flags, failThread);
  }

  void coreCreationFailure() {
    {
      MpvBackendWorker worker;
      QVERIFY(worker.controller());
      QSignalSpy status(worker.controller(), &MpvController::initializationFinished);
      const auto result = worker.initialize();
      QVERIFY(result.attempted);
      QVERIFY(!result.success);
      QCOMPARE(result.error, MPV_ERROR_GENERIC);
      QVERIFY(result.message.contains("creation failed"));
      QVERIFY(!worker.core()->handle());
      QVERIFY(!worker.post([](MpvController *) {}));
      QTRY_COMPARE(status.size(), 1);
      QCOMPARE(status[0][0].toBool(), false);
      QCOMPARE(status[0][1].toInt(), MPV_ERROR_GENERIC);
      QCOMPARE(worker.initialize().error, result.error); // single attempt
    }
    QTRY_COMPARE_WITH_TIMEOUT(MpvCoreLease::outstandingReservations(), 0, 5000);
    QCOMPARE(faults.counter(FaultCreates), 1L);
    QCOMPARE(faults.counter(FaultInitializes), 0L);
    QCOMPARE(faults.counter(FaultRetirementEntered), 0L);
    exercised();
  }

  void coreInitializationFailure() {
    {
      MpvBackendWorker worker;
      QSignalSpy status(worker.controller(), &MpvController::initializationFinished);
      const auto result = worker.initialize();
      QVERIFY(result.attempted && !result.success);
      QCOMPARE(result.error, MPV_ERROR_INVALID_PARAMETER);
      QCOMPARE(result.message, QString::fromUtf8(mpv_error_string(MPV_ERROR_INVALID_PARAMETER)));
      QVERIFY(!worker.core()->handle());
      QVERIFY(!worker.post([](MpvController *) {}));
      QTRY_COMPARE(status.size(), 1);
      QCOMPARE(status[0][1].toInt(), MPV_ERROR_INVALID_PARAMETER);
    }
    QTRY_COMPARE_WITH_TIMEOUT(MpvCoreLease::outstandingReservations(), 0, 5000);
    QCOMPARE(faults.counter(FaultCreates), 1L);
    QCOMPARE(faults.counter(FaultInitializes), 1L);
    QCOMPARE(faults.counter(FaultCandidateDestroys), 1L);
    QCOMPARE(faults.counter(FaultRetirementEntered), 0L);
    exercised();
  }

  void partialRetirementBootstrapFailure() {
    int error = 0;
    QString message;
    auto lease = MpvCoreLease::reserve(error, message);
    QVERIFY(!lease);
    QCOMPARE(error, MPV_ERROR_GENERIC);
    QVERIFY(message.contains("retirement worker"));
    QCOMPARE(MpvCoreLease::outstandingReservations(), 0);
    QCOMPARE(faults.counter(FaultThreadAttempts), 2L);
    QCOMPARE(faults.counter(FaultThreadFailures), 1L);
    QCOMPARE(faults.counter(FaultBootstrapThreadsFinished), 1L);
    MpvBackendWorker worker;
    const auto result = worker.initialize();
    QVERIFY(result.attempted && !result.success);
    QCOMPARE(result.error, MPV_ERROR_GENERIC);
    QVERIFY(!worker.controller());
    QVERIFY(!worker.core());
    QCOMPARE(faults.counter(FaultCreates), 0L);
    exercised();
  }

  void workerStartupFailure() {
    {
      MpvBackendWorker worker;
      const auto result = worker.initialize();
      QVERIFY(result.attempted && !result.success);
      QCOMPARE(result.error, MPV_ERROR_GENERIC);
      QVERIFY(result.message.contains("thread failed to start"));
      QVERIFY(!worker.controller());
      QVERIFY(!worker.core());
      QVERIFY(!worker.post([](MpvController *) {}));
    }
    QCOMPARE(MpvCoreLease::outstandingReservations(), 0);
    QCOMPARE(faults.counter(FaultThreadAttempts), 3L);
    QCOMPARE(faults.counter(FaultThreadFailures), 1L);
    QCOMPARE(faults.counter(FaultCreates), 0L);
    exercised();
  }

  void bothWorkersBlockedAndAllLiveLeasesReclaimed() {
    ReleaseBarrier barrier;
    std::array<std::shared_ptr<MpvCoreLease>, 32> leases;
    for (auto &lease : leases) {
      int error = 0;
      QString message;
      lease = MpvCoreLease::reserve(error, message);
      QVERIFY2(lease, qPrintable(message));
      std::unique_ptr<mpv_handle, decltype(&mpv_destroy)> candidate(mpv_create(), mpv_destroy);
      QVERIFY(candidate);
      QCOMPARE(mpv_set_option_string(candidate.get(), "config", "no"), MPV_ERROR_SUCCESS);
      QCOMPARE(mpv_initialize(candidate.get()), MPV_ERROR_SUCCESS);
      lease->publish(candidate.release());
    }
    QCOMPARE(faults.counter(FaultCreates), 32L);
    QCOMPARE(faults.counter(FaultInitializes), 32L);
    QElapsedTimer teardown;
    teardown.start();
    for (auto &lease : leases) lease.reset();
    const qint64 elapsed = teardown.elapsed();
    QVERIFY2(elapsed < 250, "full-pool last-owner release blocked the caller");
    QTRY_COMPARE_WITH_TIMEOUT(faults.counter(FaultRetirementEntered), 2L, 5000);
    QCOMPARE(faults.counter(FaultRetirementCompleted), 0L);
    QCOMPARE(MpvCoreLease::outstandingReservations(), 32);
    int heartbeat = 0;
    QTimer timer;
    connect(&timer, &QTimer::timeout, this, [&heartbeat]() { ++heartbeat; });
    timer.start(5);
    QTRY_VERIFY_WITH_TIMEOUT(heartbeat >= 3, 1000);
    int error = 0;
    QString message;
    auto refused = MpvCoreLease::reserve(error, message);
    QVERIFY(!refused);
    QCOMPARE(error, MPV_ERROR_NOMEM);
    QVERIFY(message.contains("32"));
    QCOMPARE(faults.counter(FaultRetirementEntered), 2L);
    QCOMPARE(faults.counter(FaultRetirementCompleted), 0L);
    faults.release();
    QTRY_COMPARE_WITH_TIMEOUT(faults.counter(FaultRetirementCompleted), 32L, 10000);
    QTRY_COMPARE_WITH_TIMEOUT(MpvCoreLease::outstandingReservations(), 0, 5000);
    auto accepted = MpvCoreLease::reserve(error, message);
    QVERIFY2(accepted, qPrintable(message));
    QCOMPARE(error, MPV_ERROR_SUCCESS);
    accepted.reset();
    QCOMPARE(MpvCoreLease::outstandingReservations(), 0);
    qInfo() << "32 native retirements completed; heartbeat ticks" << heartbeat
            << "last-owner release ms" << elapsed;
    exercised();
  }

  void workerTlsCleanupOutlivesFacade() {
    ReleaseBarrier barrier;
    auto worker = std::make_unique<MpvBackendWorker>();
    QVERIFY(worker->initialize().success);
    std::atomic<int> seeded{-1};
    QVERIFY(worker->post([&seeded](MpvController *) { seeded.store(faults.seedTls()); }));
    QTRY_COMPARE(seeded.load(), 0);
    QElapsedTimer teardown;
    teardown.start();
    worker.reset();
    const qint64 elapsed = teardown.elapsed();
    QVERIFY2(elapsed < 250, "facade deletion joined a live worker");
    QTRY_COMPARE_WITH_TIMEOUT(faults.counter(FaultTlsEntered), 1L, 5000);
    QCOMPARE(faults.counter(FaultTlsCompleted), 0L);
    QCOMPARE(MpvCoreLease::outstandingReservations(), 1);
    QCOMPARE(faults.counter(FaultRetirementEntered), 0L);
    int heartbeat = 0;
    QTimer timer;
    connect(&timer, &QTimer::timeout, this, [&heartbeat]() { ++heartbeat; });
    timer.start(5);
    QTRY_VERIFY_WITH_TIMEOUT(heartbeat >= 3, 1000);
    QCOMPARE(MpvCoreLease::outstandingReservations(), 1);
    faults.release();
    QTRY_COMPARE_WITH_TIMEOUT(faults.counter(FaultTlsCompleted), 1L, 5000);
    QTRY_COMPARE_WITH_TIMEOUT(MpvCoreLease::outstandingReservations(), 0, 5000);
    QCOMPARE(faults.counter(FaultRetirementCompleted), 1L);
    qInfo() << "held TLS cleanup reclaimed; heartbeat ticks" << heartbeat
            << "facade deletion ms" << elapsed;
    exercised();
  }

  void publicCoreCreationFailure() { unavailablePlayer(MPV_ERROR_GENERIC); }
  void publicCoreInitializationFailure() { unavailablePlayer(MPV_ERROR_INVALID_PARAMETER); }
  void publicWorkerStartupFailure() { unavailablePlayer(MPV_ERROR_GENERIC); }
  void ordinaryFileScreenshotRetirement() { screenshotRetirement(false); }
  void fifoScreenshotRetirement() { screenshotRetirement(true); }
  void synchronousHelperRegistration() {
    auto player = std::make_unique<MpvPlayer>();
    QVERIFY(player->coreAvailable());
    for (int i = 0; i < 20; ++i) {
      player->setLogLevel(QStringLiteral("info"));
      player->setLogLevel(QStringLiteral("no"));
    }
    player->setLogLevel(QStringLiteral("info"));
    bool logged = false;
    bool hooked = false;
    connect(player.get(), &MpvPlayer::logMessage, this, [&](const QString &, const QString &, const QString &text) {
      if (text.contains("registered-current-client")) logged = true;
    });
    player->hookAdd(QStringLiteral("on_load"));
    connect(player.get(), &MpvPlayer::hookTriggered, this, [&](const QString &name, quint64 id) {
      player->hookContinue(id);
      if (name == "on_load") hooked = true;
    });
    QVERIFY(player->commandAsync(QStringList{"print-text", "registered-current-client"}) > 0);
    QVERIFY(player->commandAsync(QStringList{"loadfile", QStringLiteral(MPVQML_CRASH_VIDEO)}) > 0);
    QTRY_VERIFY_WITH_TIMEOUT(logged && hooked, 5000);
    QVERIFY(faults.counter(FaultSynchronousRegistrations) >= 23);
    player.reset();
    QTRY_COMPARE_WITH_TIMEOUT(MpvCoreLease::outstandingReservations(), 0, 5000);
    exercised();
  }

  void publicRenderFailure() {
    QQuickWindow window;
    window.setColor(QColor("#345678"));
    window.resize(160, 120);
    window.setPersistentSceneGraph(false);
    window.setPersistentGraphics(false);
    auto *player = new MpvPlayer(window.contentItem());
    player->setSize(window.size());
    connect(&window, &QQuickWindow::widthChanged, player, [player](int value) { player->setWidth(value); });
    connect(&window, &QQuickWindow::heightChanged, player, [player](int value) { player->setHeight(value); });
    QSignalSpy failure(player, &MpvPlayer::renderingFailed);
    QSignalSpy ready(player, &MpvPlayer::ready);
    QSignalSpy loaded(player, &MpvPlayer::fileLoaded);
    player->setSource(QStringLiteral("never-loaded.mp4"));
    window.show();
    QVERIFY(QTest::qWaitForWindowExposed(&window));
    QTRY_COMPARE_WITH_TIMEOUT(failure.size(), 1, 5000);
    QVERIFY(player->coreAvailable());
    QVERIFY(!player->rendererReady());
    QCOMPARE(player->renderError(), MPV_ERROR_UNSUPPORTED);
    QCOMPARE(ready.size(), 0);
    QCOMPARE(loaded.size(), 0);
    const QImage blank = window.grabWindow();
    QVERIFY(!blank.isNull());
    const QColor pixel = blank.pixelColor(blank.width() / 2, blank.height() / 2);
    QVERIFY(qAbs(pixel.red() - 0x34) <= 2 && qAbs(pixel.green() - 0x56) <= 2 && qAbs(pixel.blue() - 0x78) <= 2);
    window.resize(200, 150);
    QTest::qWait(50);
    QCOMPARE(faults.counter(FaultRenderCreates), 1L); // no per-frame/resize retry
    QCOMPARE(failure.size(), 1);
    delete player;
    window.hide();
    window.releaseResources();
    QTRY_COMPARE_WITH_TIMEOUT(MpvCoreLease::outstandingReservations(), 0, 5000);
    exercised();
  }

  void recycledHelperAddressRejectsOldWakeups() {
    auto player = std::make_unique<MpvPlayer>();
    QVERIFY(player->coreAvailable());
    QStringList messages;
    connect(player.get(), &MpvPlayer::logMessage, this,
            [&](const QString &, const QString &, const QString &text) { messages.append(text); });
    // Native allocator reuse is observed, not simulated. Registration queues
    // old generation wakeups before unregister/destroy/recreate at the same
    // helper receiver; delivery waits until the final native client is current.
    for (int round = 0; round < 256; ++round) {
      player->setLogLevel(QStringLiteral("info"));
      player->setLogLevel(QStringLiteral("no"));
    }
    QVERIFY2(faults.counter(FaultHelperAddressReuses) > 0,
             "native client address reuse NOT_EXERCISED");
    player->setLogLevel(QStringLiteral("info"));
    QCoreApplication::sendPostedEvents(nullptr, QEvent::MetaCall);
    // Every stale registration queued a drain. Only the current generation
    // may touch the recycled native client, whose empty queue ends one drain.
    QCOMPARE(faults.counter(FaultHelperEmptyDrains), 1L);
    QVERIFY(player->commandAsync(QStringList{"print-text", "recycled-current-generation"}) > 0);
    QTRY_VERIFY_WITH_TIMEOUT(messages.join(QString()).contains("recycled-current-generation"), 5000);
    QVERIFY(faults.counter(FaultSynchronousRegistrations) >= 257);
    qInfo() << "native helper address reuses" << faults.counter(FaultHelperAddressReuses)
            << "native helper clients" << faults.counter(FaultHelperClients);
    player.reset();
    QTRY_COMPARE_WITH_TIMEOUT(MpvCoreLease::outstandingReservations(), 0, 5000);
    exercised();
  }

  void unsupportedGraphicsBackend() {
    // A deliberate unsupported-API fault, not an alternative video/soak lane.
    // Keep the normal runner on OpenGL; this child alone selects Qt software.
    QQuickWindow::setGraphicsApi(QSGRendererInterface::Software);
    QQuickWindow window;
    window.setColor(QColor("#345678"));
    window.resize(160, 120);
    window.setPersistentSceneGraph(false);
    window.setPersistentGraphics(false);
    auto *player = new MpvPlayer(window.contentItem());
    player->setSize(window.size());
    QSignalSpy failure(player, &MpvPlayer::renderingFailed);
    QSignalSpy ready(player, &MpvPlayer::ready);
    QSignalSpy loaded(player, &MpvPlayer::fileLoaded);
    player->setSource(QStringLiteral("unsupported.mp4"));
    window.show();
    QVERIFY(QTest::qWaitForWindowExposed(&window));
    QTRY_COMPARE_WITH_TIMEOUT(failure.size(), 1, 5000);
    QCOMPARE(player->renderError(), MPV_ERROR_UNSUPPORTED);
    QVERIFY(!player->rendererReady());
    QCOMPARE(ready.size(), 0);
    QCOMPARE(loaded.size(), 0);
    QCOMPARE(faults.counter(FaultRenderCreates), 0L);
    delete player;
    window.hide();
    window.releaseResources();
    QTRY_COMPARE_WITH_TIMEOUT(MpvCoreLease::outstandingReservations(), 0, 5000);
    exercised();
  }

private:
  void screenshotRetirement(bool fifo) {
    ReleaseBarrier barrier;
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    const QString path = directory.filePath(QStringLiteral("shot.jpg"));
    const QByteArray nativePath = path.toUtf8();
    if (fifo) QCOMPARE(::mkfifo(nativePath.constData(), 0600), 0);
    QCOMPARE(faults.watchFile(nativePath.constData()), 0);
    auto window = std::make_unique<QQuickWindow>();
    window->setColor(Qt::black);
    window->resize(160, 120);
    window->setPersistentSceneGraph(false);
    window->setPersistentGraphics(false);
    auto *player = new MpvPlayer(window->contentItem());
    player->setSize(window->size());
    connect(window.get(), &QQuickWindow::widthChanged, player, [player](int value) { player->setWidth(value); });
    connect(window.get(), &QQuickWindow::heightChanged, player, [player](int value) { player->setHeight(value); });
    player->setHwdec(QStringLiteral("no"));
    player->setLoopFile(QStringLiteral("inf"));
    player->setSource(QStringLiteral(MPVQML_CRASH_VIDEO));
    window->show();
    QVERIFY(QTest::qWaitForWindowExposed(window.get()));
    QTRY_VERIFY_WITH_TIMEOUT(player->rendererReady() && player->hasVideo(), 5000);
    auto blue = [&]() {
      const QImage image = window->grabWindow();
      return !image.isNull() && image.pixelColor(image.width() / 2, image.height() / 2).blue() > 150;
    };
    QTRY_VERIFY_WITH_TIMEOUT(blue(), 5000);
    QVERIFY(player->commandAsync(QStringList{"screenshot-to-file", path, "video"}) > 0);
    if (fifo) QTRY_COMPARE_WITH_TIMEOUT(faults.counter(FaultFileOpens), 1L, 5000);
    else QTRY_COMPARE_WITH_TIMEOUT(faults.counter(FaultFileWrites), 1L, 5000);
    QCOMPARE(faults.counter(FaultFileWritesCompleted), 0L);
    QElapsedTimer teardown;
    teardown.start();
    delete player;
    window.reset();
    const qint64 elapsed = teardown.elapsed();
    qInfo() << "screenshot teardown elapsed ms" << elapsed << "fifo" << fifo;
    QVERIFY2(elapsed < 250, "player/window deletion waited for screenshot file I/O");
    int heartbeat = 0;
    QTimer timer;
    connect(&timer, &QTimer::timeout, this, [&heartbeat]() { ++heartbeat; });
    timer.start(5);
    QTRY_VERIFY_WITH_TIMEOUT(heartbeat >= 3, 1000);
    QTRY_COMPARE_WITH_TIMEOUT(faults.counter(FaultRetirementEntered), 1L, 5000);
    QCOMPARE(faults.counter(FaultRetirementCompleted), 0L);
    QCOMPARE(MpvCoreLease::outstandingReservations(), 1);
    QCOMPARE(faults.counter(FaultFileWritesCompleted), 0L);
    QFile reader;
    if (fifo) {
      const int fd = ::open(nativePath.constData(), O_RDONLY | O_NONBLOCK | O_CLOEXEC);
      QVERIFY(fd >= 0);
      QVERIFY(reader.open(fd, QIODevice::ReadOnly, QFileDevice::AutoCloseHandle));
    }
    faults.release();
    QTRY_COMPARE_WITH_TIMEOUT(faults.counter(FaultFileWritesCompleted), 1L, 5000);
    QTRY_COMPARE_WITH_TIMEOUT(MpvCoreLease::outstandingReservations(), 0, 5000);
    QCOMPARE(faults.counter(FaultRetirementCompleted), 1L);
    if (fifo) {
      const QByteArray image = reader.readAll();
      QVERIFY(image.startsWith(QByteArray::fromHex("ffd8")));
      QVERIFY(!QImage::fromData(image, "JPEG").isNull());
    } else {
      QVERIFY(QFileInfo(path).size() > 0);
      const QImage image(path);
      QVERIFY(!image.isNull());
      QVERIFY(image.pixelColor(image.width() / 2, image.height() / 2).blue() > 150);
    }
    qInfo() << "screenshot writer exercised; player/window deletion ms" << elapsed
            << "heartbeat ticks before release" << heartbeat;
    exercised();
  }

  void unavailablePlayer(int expectedError) {
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    {
      MpvPlayer player;
      QSignalSpy failure(&player, &MpvPlayer::initializationFailed);
      QSignalSpy ready(&player, &MpvPlayer::ready);
      QVERIFY(!player.coreAvailable());
      QCOMPARE(player.initializationError(), expectedError);
      QVERIFY(!player.initializationErrorString().isEmpty());
      QCOMPARE(failure.size(), 0);
      QTRY_COMPARE(failure.size(), 1);
      QCOMPARE(failure[0][0].toInt(), expectedError);
      player.load(QStringLiteral("unavailable.mp4"));
      player.loadDirectory(directory.path());
      player.stop(false);
      player.setPaused(true);
      player.setVolume(75);
      player.setLogLevel(QStringLiteral("info"));
      player.hookAdd(QStringLiteral("on_load"));
      player.hookContinue(1);
      player.screenshot();
      player.loadConfigFile(directory.filePath(QStringLiteral("unavailable.conf")));
      player.setRenderParameter(QStringLiteral("ambient-light"), 10);
      QCOMPARE(player.commandAsync(QStringList{"stop"}), MPV_ERROR_UNINITIALIZED);
      QCOMPARE(player.setMpvPropertyAsync(QStringLiteral("pause"), true), MPV_ERROR_UNINITIALIZED);
      QCOMPARE(player.readMpvPropertyAsync(QStringLiteral("pause")), MPV_ERROR_UNINITIALIZED);
      QCOMPARE(ready.size(), 0);
      QVERIFY(!player.rendererReady());
    }
    QTRY_COMPARE_WITH_TIMEOUT(MpvCoreLease::outstandingReservations(), 0, 5000);
    exercised();
  }
};

#if defined(__SANITIZE_ADDRESS__)
extern "C" void __asan_init();
#endif

class TestCrashLifecycle : public QObject {
  Q_OBJECT
private slots:
  void nativeFaults_data() {
    QTest::addColumn<QString>("name");
    for (const char *name : {"coreCreationFailure", "coreInitializationFailure",
                            "partialRetirementBootstrapFailure", "workerStartupFailure",
                            "bothWorkersBlockedAndAllLiveLeasesReclaimed", "workerTlsCleanupOutlivesFacade",
                            "moduleUnloadWhileBlocked", "publicCoreCreationFailure",
                            "publicCoreInitializationFailure", "publicWorkerStartupFailure",
                            "publicRenderFailure", "ordinaryFileScreenshotRetirement", "fifoScreenshotRetirement",
                            "synchronousHelperRegistration", "recycledHelperAddressRejectsOldWakeups", "unsupportedGraphicsBackend"})
      QTest::newRow(name) << QString::fromLatin1(name);
  }

  void nativeFaults() {
    QFETCH(QString, name);
    QProcess child;
    QProcessEnvironment environment = QProcessEnvironment::systemEnvironment();
    environment.remove("LD_PRELOAD");
    QString preload = QStringLiteral(MPVQML_FAULT_LIBRARY);
#if defined(__SANITIZE_ADDRESS__)
    Dl_info runtime{};
    QVERIFY(dladdr(reinterpret_cast<void *>(&__asan_init), &runtime));
    QVERIFY(runtime.dli_fname);
    preload.prepend(QString::fromLocal8Bit(runtime.dli_fname) + QLatin1Char(':'));
    if (name == QStringLiteral("recycledHelperAddressRejectsOldWakeups")) {
      // This child must exercise actual native address reuse. Quarantine would
      // prevent its trigger; retain all diagnostics/leak detection and leave
      // the ordinary lifetime/UAF children on their default quarantine.
      environment.insert("ASAN_OPTIONS", environment.value("ASAN_OPTIONS")
          + QStringLiteral(":quarantine_size_mb=0:thread_local_quarantine_size_kb=0"));
    }
#endif
    environment.insert("LD_PRELOAD", preload);
    environment.insert("QT_FORCE_STDERR_LOGGING", "1");
    child.setProcessEnvironment(environment);
    child.setProcessChannelMode(QProcess::MergedChannels);
    const QString executable = name == QStringLiteral("moduleUnloadWhileBlocked")
        ? QStringLiteral(MPVQML_UNLOAD_EXECUTABLE) : QCoreApplication::applicationFilePath();
    child.start(executable, QStringList{"--fixture", name});
    QVERIFY2(child.waitForStarted(5000), qPrintable(child.errorString()));
    if (!child.waitForFinished(30000)) {
      child.kill(); // this owned native fixture is not qs or the locker
      child.waitForFinished(5000);
      QFAIL("native fault child timed out; case NOT_EXERCISED");
    }
    const QByteArray output = child.readAll();
    qInfo().noquote() << QString::fromUtf8(output);
    QCOMPARE(child.exitStatus(), QProcess::NormalExit);
    QCOMPARE(child.exitCode(), 0);
    QVERIFY2(output.contains(("FAULT_CASE_EXERCISED " + name).toUtf8()),
             "child did not observe its required outcome; case NOT_EXERCISED");
  }
};

int main(int argc, char **argv) {
  QGuiApplication application(argc, argv);
  const QStringList arguments = application.arguments();
  if (arguments.size() == 3 && arguments[1] == "--fixture") {
    fixtureName = arguments[2];
    NativeFaultCases cases;
    return QTest::qExec(&cases, QStringList{arguments[0], fixtureName, "-v1"});
  }
  TestCrashLifecycle tests;
  return QTest::qExec(&tests, argc, argv);
}

#include "test_crash_lifecycle.moc"
