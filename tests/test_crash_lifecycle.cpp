#include "faults.h"
#include "mpvcontroller_p.h"
#include "mpvframecapture.h"
#include "mpvplayer.h"
#include "test_window.h"

#include <QCoreApplication>
#include <QGuiApplication>
#include <QQuickWindow>
#include <QImage>
#include <QTemporaryDir>
#include <QFileInfo>
#include <QFile>
#include <QDir>
#include <QProcess>
#include <QProcessEnvironment>
#include <QSignalSpy>
#include <QThread>
#include <QTimer>
#include <QtTest>
#include <array>
#include <atomic>
#include <clocale>
#include <dlfcn.h>
#include <memory>
#include <signal.h>
#include <sys/resource.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

using namespace MpvQmlPrivate;
using MpvQmlTestSupport::TestWindow;

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
    if (fixtureName == "captureInitializationFailure") flags = FaultInitialize;
    if (fixtureName == "captureLateWorkerSkipsOpen") flags = FaultCaptureLateWorker;
    if (fixtureName == "captureWithoutLua") flags = FaultCaptureWithoutLua;
    if (fixtureName == "captureDisablesBuiltinScripts") flags = FaultCaptureScriptOptions;
    if (fixtureName == "playbackOverflowLeavesReservedRepliesIntact") flags = FaultPlaybackOverflow;
    if (fixtureName == "captureSuccessReusesReleasedSlot") flags = FaultRetirementBarrier;
    if (fixtureName == "captureIgnoresUncorrelatedEvents") flags = FaultCaptureEvents;
    if (fixtureName == "captureCancellationWins" || fixtureName == "captureExpiresBeforeEncoding")
      flags = FaultCaptureBeforePublish;
    if (fixtureName == "capturePublicationWins" || fixtureName == "captureCompetingWriter"
        || fixtureName == "capturePublishesPastDeadline") flags = FaultCapturePublish;
    if (fixtureName == "partialRetirementBootstrapFailure") failThread = 2;
    if (fixtureName == "workerStartupFailure") failThread = 3;
    if (fixtureName == "bothWorkersBlockedAndAllLiveLeasesReclaimed"
        || fixtureName == "attachedShutdownRetiresWhileAlive"
        || fixtureName == "attachedHiddenShutdownRetiresWhileAlive") flags = FaultRetirementBarrier;
    if (fixtureName == "workerTlsCleanupOutlivesFacade") flags = FaultTlsBarrier;
    if (fixtureName == "publicCoreCreationFailure") flags = FaultCreate;
    if (fixtureName == "publicCoreInitializationFailure") flags = FaultInitialize;
    if (fixtureName == "publicWorkerStartupFailure") failThread = 3;
    if (fixtureName == "publicRenderFailure") flags = FaultRenderCreate;
    if (fixtureName == "publicRenderFailureRecoversCallerIntent"
        || fixtureName == "publicRenderFailureRetainsInitialIntent") flags = FaultRenderCreate;
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
      QCOMPARE(result.coreStage, MpvCoreStage::Create);
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
      QCOMPARE(result.coreStage, MpvCoreStage::Initialize);
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

  void captureInitializationFailure() {
    QTemporaryDir stage;
    QVERIFY(stage.isValid());
    MpvFrameCapture capture;
    QSignalSpy replies(&capture, &MpvFrameCapture::finished);
    const auto target = QUrl::fromLocalFile(stage.filePath("frame.png"));
    const int id = capture.capture(QUrl::fromLocalFile(MPVQML_CRASH_VIDEO), 0, 64, 64, target);
    QVERIFY(id > 0);
    QTRY_COMPARE(replies.size(), 1);
    QCOMPARE(replies[0][0].toInt(), id);
    QCOMPARE(replies[0][1].toString(), QStringLiteral("initialization-failed"));
    QVERIFY(!QFile::exists(target.toLocalFile()));
    QCOMPARE(faults.counter(FaultCreates), 1L);
    QCOMPARE(faults.counter(FaultInitializes), 1L);
    QCOMPARE(faults.counter(FaultCandidateDestroys), 1L);
    QCOMPARE(faults.counter(FaultRetirementEntered), 0L);
    exercised();
  }

  void playbackOverflowLeavesReservedRepliesIntact() {
    MpvPlayer player;
    QVERIFY(player.coreAvailable());
    QSignalSpy details(&player, SIGNAL(playbackEvent(QVariantMap)));
    QSignalSpy replies(&player, &MpvPlayer::mpvPropertyReadFinished);
    const auto id = player.readMpvPropertyAsync(QStringLiteral("time-pos"));
    QVERIFY(id > 0);
    QTRY_COMPARE(replies.size(), 1);
    QCOMPARE(replies[0][0].toInt(), id);
    QCOMPARE(replies[0][1].toInt(), MPV_ERROR_PROPERTY_UNAVAILABLE);
    QVERIFY(!replies[0][2].isValid());
    QVERIFY(details.isValid());
    QTRY_COMPARE(details.size(), 1);
    const auto diagnostic = details[0][0].toMap();
    QCOMPARE(diagnostic.value("stage").toString(), QStringLiteral("queue-overflow"));
    QCOMPARE(diagnostic.value("nativeCode").toInt(), MPV_ERROR_EVENT_QUEUE_FULL);
    QCOMPARE(diagnostic.value("entryId").toString(), QString());
    QVERIFY(player.coreAvailable());
    exercised();
  }

  void mixedAdmissionHoldsCancelledSlots() {
    QTemporaryDir stage;
    QVERIFY(stage.isValid());
    MpvBackendWorker playback;
    QVERIFY(playback.initialize().success);
    MpvCoreDiagnostic diagnostic;
    std::array<std::shared_ptr<MpvCoreLease>, MpvBackendCore::Capacity - 3> otherPlayback;
    for (auto &lease : otherPlayback) {
      lease = MpvCoreLease::reserve(diagnostic);
      QVERIFY2(lease, qPrintable(diagnostic.message));
    }
    MpvFrameCapture capture, other;
    QSignalSpy replies(&capture, &MpvFrameCapture::finished);
    QSignalSpy capacity(&capture, &MpvFrameCapture::capacityReleased);
    ReleaseBarrier barrier;
    faults.configure(FaultInitializeBarrier, 0);
    const auto source = QUrl::fromLocalFile(MPVQML_CRASH_VIDEO);
    const auto a = QUrl::fromLocalFile(stage.filePath("a.png"));
    const auto b = QUrl::fromLocalFile(stage.filePath("b.png"));
    const auto c = QUrl::fromLocalFile(stage.filePath("c.png"));
    const int first = capture.capture(source, 0, 64, 64, a);
    const int second = capture.capture(source, 0, 64, 64, b);
    QVERIFY(first > 0 && second > first);
    QTRY_COMPARE(faults.counter(FaultInitializeEntered), 2L);
    QVERIFY(capture.cancel(first));
    QVERIFY(!capture.cancel(first));
    QVERIFY(capture.cancel(second));
    // A worker may already have been killed; the owner has not reaped it
    // yet because this thread has not pumped Qt's retirement notifications.
    QVERIFY(replies.isEmpty());
    QVERIFY(capacity.isEmpty());
    QVERIFY(!capture.capacityAvailable());
    QVERIFY(!other.capacityAvailable());
    QCOMPARE(capture.tryCapture(source, 0, 64, 64, c)["status"].toInt(), int(MpvFrameCapture::LocalBusy));
    const auto rejected = other.tryCapture(source, 0, 64, 64, c);
    QCOMPARE(rejected["status"].toInt(), int(MpvFrameCapture::SharedBusy));
    QCOMPARE(rejected["id"].toInt(), 0);
    QCOMPARE(rejected["error"].toInt(), MPV_ERROR_NOMEM);
    QCOMPARE(other.capture(source, 0, 64, 64, c), MPV_ERROR_NOMEM);
    MpvBackendWorker refusedPlayback;
    QCOMPARE(refusedPlayback.initialize().coreStage, MpvCoreStage::Capacity);
    QCOMPARE(MpvBackendCore::outstandingReservations(), MpvBackendCore::Capacity);
    QTRY_COMPARE(capacity.size(), 2);
    QCOMPARE(replies.size(), 2);
    QCOMPARE(replies[0][0].toInt(), first);
    QCOMPARE(replies[0][1].toString(), QStringLiteral("cancelled"));
    QCOMPARE(replies[1][0].toInt(), second);
    QCOMPARE(replies[1][1].toString(), QStringLiteral("cancelled"));
    QCOMPARE(faults.counter(FaultRetirementCompleted), 0L);
    QVERIFY(capture.capacityAvailable());
    QVERIFY(other.capacityAvailable());
    QVERIFY(!QFile::exists(a.toLocalFile()) && !QFile::exists(b.toLocalFile()));
    exercised();
  }

  void captureIgnoresUncorrelatedEvents() {
    QTemporaryDir temp;
    QVERIFY(temp.isValid());
    const auto path = temp.filePath("seek.png");
    MpvFrameCapture capture;
    QSignalSpy replies(&capture, &MpvFrameCapture::finished);
    QVERIFY(capture.capture(QUrl::fromLocalFile(MPVQML_CRASH_SEEK_VIDEO), 0.75, 123, 88,
                           QUrl::fromLocalFile(path)) > 0);
    QTRY_COMPARE_WITH_TIMEOUT(replies.size(), 1, 8000);
    QCOMPARE(replies[0][1].toString(), QString());
    const QImage image(path);
    QCOMPARE(image.size(), QSize(123, 69));
    const auto color = image.pixelColor(61, 34);
    QVERIFY2(color.red() < 30 && color.blue() > 210, qPrintable(color.name()));
    QCOMPARE(faults.counter(FaultCaptureSeekSubmitted), 1);
    qInfo() << "FAULT_CASE_EXERCISED captureIgnoresUncorrelatedEvents";
  }

  void captureScriptProfile() {
    QTemporaryDir temp;
    QVERIFY(temp.isValid());
    const auto path = temp.filePath("frame.png");
    MpvFrameCapture capture;
    QSignalSpy replies(&capture, &MpvFrameCapture::finished);
    QVERIFY(capture.capture(QUrl::fromLocalFile(MPVQML_CRASH_SEEK_VIDEO), 0, 123, 88,
                            QUrl::fromLocalFile(path)) > 0);
    QTRY_COMPARE_WITH_TIMEOUT(replies.size(), 1, 8000);
    QVERIFY2(replies[0][1].toString().isEmpty(), qPrintable(replies[0][2].toString()));
    QCOMPARE(faults.counter(FaultCaptureScriptDisables), 0x1ffL);
    const QImage image(path);
    QCOMPARE(image.size(), QSize(123, 69));
    QVERIFY(image.pixelColor(61, 34).red() > 210);
    exercised();
  }

  void captureWithoutLua() { captureScriptProfile(); }
  void captureDisablesBuiltinScripts() { captureScriptProfile(); }

  void captureRaces(bool cancelWins, bool competitor) {
    QTemporaryDir temp;
    QVERIFY(temp.isValid());
    const auto path = temp.filePath("publish.png");
    MpvFrameCapture capture;
    QSignalSpy replies(&capture, &MpvFrameCapture::finished);
    QSignalSpy releases(&capture, &MpvFrameCapture::capacityReleased);
    const auto id = capture.capture(QUrl::fromLocalFile(MPVQML_CRASH_SEEK_VIDEO), 0, 123, 88,
                                    QUrl::fromLocalFile(path));
    QVERIFY(id > 0);
    struct Release { ~Release() { faults.release(); } } release;
    QTRY_COMPARE_WITH_TIMEOUT(faults.counter(cancelWins ? FaultCaptureRenderEntered : FaultCapturePublishEntered), 1, 5000);
    QVERIFY(!QFile::exists(path));
    QCOMPARE(capture.cancel(id), cancelWins);
    int next = 0;
    const auto nextPath = temp.filePath("after-cancel.png");
    if (cancelWins) {
      QVERIFY(releases.isEmpty());
      next = capture.capture(QUrl::fromLocalFile(MPVQML_CRASH_SEEK_VIDEO), 0, 123, 88,
                             QUrl::fromLocalFile(nextPath));
      QVERIFY(next > id);
    }
    if (competitor) {
      QFile sentinel(path);
      QVERIFY(sentinel.open(QIODevice::WriteOnly | QIODevice::NewOnly));
      QCOMPARE(sentinel.write("other writer"), 12);
    }
    faults.release();
    const int expectedJobs = cancelWins ? 2 : 1;
    QTRY_COMPARE_WITH_TIMEOUT(releases.size(), expectedJobs, 8000);
    QCOMPARE(replies.size(), expectedJobs);
    QCOMPARE(replies[0][0].toInt(), id);
    QCOMPARE(replies[0][1].toString(), cancelWins ? "cancelled" : competitor ? "write-failed" : "");
    QCOMPARE(MpvCoreLease::outstandingReservations(), 0);
    if (cancelWins) {
      QVERIFY(!QFile::exists(path));
      QCOMPARE(replies[1][0].toInt(), next);
      QVERIFY2(replies[1][1].toString().isEmpty(), qPrintable(replies[1][2].toString()));
      const QImage image(nextPath);
      QCOMPARE(image.size(), QSize(123, 69));
      QVERIFY(image.pixelColor(61, 34).red() > 210);
    } else if (competitor) {
      QFile sentinel(path);
      QVERIFY(sentinel.open(QIODevice::ReadOnly));
      QCOMPARE(sentinel.readAll(), QByteArray("other writer"));
    } else {
      const QImage image(path);
      QCOMPARE(image.size(), QSize(123, 69));
      QVERIFY(image.pixelColor(61, 34).red() > 210);
    }
    QCOMPARE(QDir(temp.path()).entryList(QDir::Files | QDir::Hidden).size(), 1);
    qInfo().noquote() << "FAULT_CASE_EXERCISED" << fixtureName;
  }

  void captureCancellationWins() { captureRaces(true, false); }
  void capturePublicationWins() { captureRaces(false, false); }
  void captureCompetingWriter() { captureRaces(false, true); }

  void captureDeadline(bool publishStarted) {
    QTemporaryDir temp;
    QVERIFY(temp.isValid());
    const auto path = temp.filePath("publish.png");
    MpvFrameCapture capture;
    QSignalSpy details(&capture, &MpvFrameCapture::finishedDetailed);
    QSignalSpy releases(&capture, &MpvFrameCapture::capacityReleased);
    QVERIFY(capture.capture(QUrl::fromLocalFile(MPVQML_CRASH_SEEK_VIDEO), 0, 123, 88,
                           QUrl::fromLocalFile(path)) > 0);
    struct Release { ~Release() { faults.release(); } } release;
    QTRY_COMPARE_WITH_TIMEOUT(faults.counter(publishStarted ? FaultCapturePublishEntered : FaultCaptureRenderEntered), 1, 5000);
    // Hold until the whole admission budget has elapsed, not to win a race.
    if (publishStarted) {
      QTest::qWait(7100);
      QVERIFY(releases.isEmpty());
      faults.release();
    }
    QTRY_COMPARE_WITH_TIMEOUT(releases.size(), 1, 8500);
    QCOMPARE(details.size(), 1);
    const auto outcome = details[0][1].toMap();
    QCOMPARE(outcome["stage"].toString(), publishStarted ? "complete" : "helper");
    QCOMPARE(outcome["code"].toString(), publishStarted ? "" : "deadline");
    QCOMPARE(QFile::exists(path), publishStarted);
    QCOMPARE(QDir(temp.path()).entryList(QDir::Files | QDir::Hidden).size(), publishStarted ? 1 : 0);
    qInfo().noquote() << "FAULT_CASE_EXERCISED" << fixtureName;
  }

  void captureExpiresBeforeEncoding() { captureDeadline(false); }
  void capturePublishesPastDeadline() { captureDeadline(true); }

  void captureHelperFault(const QString &expectedCode) {
    QTemporaryDir staging;
    QVERIFY(staging.isValid());
    MpvFrameCapture capture;
    QSignalSpy finished(&capture, &MpvFrameCapture::finishedDetailed);
    QSignalSpy released(&capture, &MpvFrameCapture::capacityReleased);
    const auto source = QUrl::fromLocalFile(QStringLiteral(MPVQML_TEST_ASSETS "/test_frame_seek.webm"));
    const auto failedPath = staging.filePath("failed.png");
    const auto recoveredPath = staging.filePath("recovered.png");
    QElapsedTimer elapsed;
    elapsed.start();
    const int request = capture.capture(source, 0, 64, 64, QUrl::fromLocalFile(failedPath));
    QVERIFY(request > 0);
    const auto markerPath = qEnvironmentVariable("MPVQML_TEST_CAPTURE_MARKER");
    if (expectedCode == "deadline") {
      QTRY_VERIFY_WITH_TIMEOUT(QFileInfo(markerPath).size() > 0, 4000);
      QFile marker(markerPath);
      QVERIFY(marker.open(QIODevice::ReadOnly));
      const auto pid = marker.readAll().trimmed().toInt();
      QVERIFY(pid > 1 && pid != ::getpid());
      QCOMPARE(::kill(pid, 0), 0);
      // No GUI events while the absolute deadline expires. A GUI timer
      // alone would leave the native process alive until this test yields.
      QThread::msleep(qMax<qint64>(1, 7300 - elapsed.elapsed()));
      QFile status(QStringLiteral("/proc/%1/stat").arg(pid));
      if (status.open(QIODevice::ReadOnly)) {
        const auto stat = status.readAll();
        QCOMPARE(stat.mid(stat.lastIndexOf(')') + 2, 1), QByteArray("Z"));
      } else {
        QCOMPARE(::kill(pid, 0), -1);
        QCOMPARE(errno, ESRCH);
      }
      QVERIFY(finished.isEmpty());
    }
    QTRY_COMPARE_WITH_TIMEOUT(finished.size(), 1, 9000);
    const auto outcome = finished.at(0).at(1).toMap();
    QCOMPARE(finished.at(0).at(0).toInt(), request);
    QCOMPARE(outcome.value("code").toString(), expectedCode);
    QVERIFY(!outcome.value("reason").toString().isEmpty());
    QCOMPARE(outcome.value("stage").toString(), QStringLiteral("helper"));
    QTRY_COMPARE_WITH_TIMEOUT(released.size(), 1, 2000);
    QCOMPARE(released.at(0).at(0).toInt(), request);
    QVERIFY(!QFileInfo::exists(failedPath));
    QFile marker(markerPath);
    QVERIFY(marker.open(QIODevice::ReadOnly));
    const auto pid = marker.readAll().trimmed().toInt();
    QVERIFY(pid > 1 && pid != ::getpid());
    QCOMPARE(::kill(pid, 0), -1);
    QCOMPARE(errno, ESRCH);
    const int retry = capture.capture(source, 0.75, 64, 64, QUrl::fromLocalFile(recoveredPath));
    QVERIFY(retry > request);
    QTRY_COMPARE_WITH_TIMEOUT(finished.size(), 2, 8000);
    QCOMPARE(finished.at(1).at(0).toInt(), retry);
    QCOMPARE(finished.at(1).at(1).toMap().value("code").toString(), QString());
    const QImage image(recoveredPath);
    QCOMPARE(image.size(), QSize(64, 36));
    QVERIFY(image.pixelColor(32, 18).blue() > 210);
    QTRY_COMPARE_WITH_TIMEOUT(released.size(), 2, 2000);
    exercised();
  }

  void captureHelperAbort() { captureHelperFault(QStringLiteral("helper-crashed")); }
  void captureHelperDeadline() { captureHelperFault(QStringLiteral("deadline")); }

  void captureRespectsInheritedAddressLimit() {
#if !defined(__SANITIZE_ADDRESS__) && !defined(__SANITIZE_THREAD__)
    constexpr rlim_t bytes = 1024ull * 1024 * 1024;
    const rlimit limit{bytes, bytes};
    QVERIFY(::setrlimit(RLIMIT_AS, &limit) == 0);
#endif
    QTemporaryDir stage;
    QVERIFY(stage.isValid());
    MpvFrameCapture capture;
    QSignalSpy replies(&capture, &MpvFrameCapture::finishedDetailed);
    const auto path = stage.filePath("limited.png");
    QVERIFY(capture.capture(QUrl::fromLocalFile(MPVQML_CRASH_SEEK_VIDEO), 0, 64, 64, QUrl::fromLocalFile(path)) > 0);
    QTRY_COMPARE_WITH_TIMEOUT(replies.size(), 1, 8000);
    const auto result = replies.at(0).at(1).toMap();
    QVERIFY2(result.value("code").toString().isEmpty(), qPrintable(result.value("reason").toString()));
    QCOMPARE(QImage(path).size(), QSize(64, 36));
    exercised();
  }

  void captureLateWorkerSkipsOpen() {
    QTemporaryDir stage;
    QVERIFY(stage.isValid());
    MpvFrameCapture capture;
    QSignalSpy replies(&capture, &MpvFrameCapture::finishedDetailed);
    QVERIFY(capture.capture(QUrl::fromLocalFile(stage.filePath("missing-video")), 0, 64, 64,
                           QUrl::fromLocalFile(stage.filePath("unused.png"))) > 0);
    QTRY_COMPARE_WITH_TIMEOUT(replies.size(), 1, 9000);
    QCOMPARE(replies.at(0).at(1).toMap().value("code").toString(), QStringLiteral("deadline"));
    exercised();
  }

  void captureHelperQueuedCancel() {
    QTemporaryDir staging;
    QVERIFY(staging.isValid());
    MpvFrameCapture running, queued;
    QSignalSpy finished(&queued, &MpvFrameCapture::finishedDetailed);
    QSignalSpy released(&queued, &MpvFrameCapture::capacityReleased);
    QSignalSpy runningFinished(&running, &MpvFrameCapture::finishedDetailed);
    QSignalSpy runningReleased(&running, &MpvFrameCapture::capacityReleased);
    const auto source = QUrl::fromLocalFile(QStringLiteral(MPVQML_TEST_ASSETS "/test_frame_seek.webm"));
    const auto submit = [&](MpvFrameCapture &owner, const QString &name) {
      return owner.capture(source, 0, 64, 64, QUrl::fromLocalFile(staging.filePath(name)));
    };
    const int first = submit(running, "first.png");
    const int second = submit(running, "second.png");
    QVERIFY(first > 0 && second > first);
    const auto marker = qEnvironmentVariable("MPVQML_TEST_CAPTURE_MARKER");
    QTRY_VERIFY_WITH_TIMEOUT(QFileInfo(marker + ".2").size() > 0, 4000);
    const int third = submit(queued, "third.png");
    QVERIFY(third > 0);
    QTest::qWait(750);
    QVERIFY2(!QFileInfo::exists(marker + ".3"), "third capture entered native code instead of queuing");
    QVERIFY(queued.cancel(third));
    QTRY_COMPARE_WITH_TIMEOUT(finished.size(), 1, 2000);
    QTRY_COMPARE_WITH_TIMEOUT(released.size(), 1, 2000);
    QCOMPARE(finished.at(0).at(0).toInt(), third);
    QCOMPARE(finished.at(0).at(1).toMap().value("code").toString(), QStringLiteral("cancelled"));
    QCOMPARE(released.at(0).at(0).toInt(), third);
    QVERIFY(running.cancel(first));
    QVERIFY(running.cancel(second));
    QTRY_COMPARE_WITH_TIMEOUT(runningFinished.size(), 2, 2000);
    QTRY_COMPARE_WITH_TIMEOUT(runningReleased.size(), 2, 2000);
    QCOMPARE(faults.counter(FaultInitializes), 2L);
    for (const auto &reply : runningFinished)
      QCOMPARE(reply.at(1).toMap().value("code").toString(), QStringLiteral("cancelled"));
    QCOMPARE(QDir(staging.path()).entryList(QDir::Files | QDir::Hidden), QStringList());
    QVERIFY(!QFileInfo::exists(marker + ".3"));
    exercised();
  }

  void captureCancellationBeforeRetiredDelivery() {
    QTemporaryDir temp;
    QVERIFY(temp.isValid());
    MpvFrameCapture capture;
    struct CancelAtDelivery : QObject {
      MpvFrameCapture &owner;
      int id = 0;
      bool cancelled = false;
      explicit CancelAtDelivery(MpvFrameCapture &capture) : owner(capture) {}
      bool eventFilter(QObject *, QEvent *event) override {
        if (event->type() == QEvent::MetaCall && id > 0)
          cancelled = owner.cancel(std::exchange(id, 0));
        return false;
      }
    } filter(capture);
    capture.installEventFilter(&filter);
    QStringList order;
    connect(&capture, &MpvFrameCapture::finished, this, [&](int, const QString &code) { order.append(code); });
    connect(&capture, &MpvFrameCapture::capacityReleased, this, [&](int) { order.append("released"); });
    filter.id = capture.capture(QUrl::fromLocalFile(temp.filePath("missing-video")), 0, 123, 88,
                                QUrl::fromLocalFile(temp.filePath("frame.png")));
    QVERIFY(filter.id > 0);
    QTRY_COMPARE_WITH_TIMEOUT(order.size(), 2, 5000);
    QVERIFY(filter.cancelled);
    QCoreApplication::sendPostedEvents(&capture, QEvent::MetaCall);
    QCOMPARE(order, QStringList({"cancelled", "released"}));
    qInfo() << "FAULT_CASE_EXERCISED captureCancellationBeforeRetiredDelivery";
  }

  void captureSuccessReusesReleasedSlot() {
    QTemporaryDir stage;
    QVERIFY(stage.isValid());
    MpvCoreDiagnostic diagnostic;
    std::array<std::shared_ptr<MpvCoreLease>, MpvBackendCore::Capacity - 1> playback;
    for (auto &lease : playback) {
      lease = MpvCoreLease::reserve(diagnostic);
      QVERIFY2(lease, qPrintable(diagnostic.message));
    }
    MpvFrameCapture capture;
    QSignalSpy replies(&capture, &MpvFrameCapture::finished);
    QSignalSpy capacity(&capture, &MpvFrameCapture::capacityReleased);
    ReleaseBarrier barrier;
    const auto source = QUrl::fromLocalFile(MPVQML_CRASH_VIDEO);
    const auto a = QUrl::fromLocalFile(stage.filePath("a.png"));
    const auto b = QUrl::fromLocalFile(stage.filePath("b.png"));
    const int first = capture.capture(source, 0, 64, 64, a);
    QVERIFY(first > 0);
    int second = 0;
    connect(&capture, &MpvFrameCapture::finished, &capture, [&](int id, const QString &code) {
      if (id != first || !code.isEmpty())
        return;
      QVERIFY(capture.capacityAvailable());
      second = capture.capture(source, 0, 64, 64, b);
    });
    QTRY_COMPARE(faults.counter(FaultRetirementEntered), 1L);
    QVERIFY(!QFile::exists(a.toLocalFile())); // Publication follows native teardown.
    QVERIFY(replies.isEmpty());
    QVERIFY(capacity.isEmpty());
    QVERIFY(!capture.capacityAvailable());
    faults.release();
    QTRY_COMPARE(replies.size(), 2);
    QVERIFY(second > first);
    QCOMPARE(replies[1][0].toInt(), second);
    QVERIFY2(replies[1][1].toString().isEmpty(), qPrintable(replies[1][2].toString()));
    QCOMPARE(capacity.size(), 2);
    QCOMPARE(faults.counter(FaultRetirementCompleted), 2L);
    const QImage firstFrame(a.toLocalFile()), secondFrame(b.toLocalFile());
    QVERIFY(!firstFrame.isNull() && firstFrame == secondFrame);
    exercised();
  }

  void partialRetirementBootstrapFailure() {
    MpvCoreDiagnostic diagnostic;
    auto lease = MpvCoreLease::reserve(diagnostic);
    QVERIFY(!lease);
    QCOMPARE(diagnostic.error, MPV_ERROR_GENERIC);
    QCOMPARE(diagnostic.stage, MpvCoreStage::Service);
    QVERIFY(diagnostic.message.contains("retirement worker"));
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
    std::array<std::shared_ptr<MpvCoreLease>, MpvBackendCore::Capacity> leases;
    MpvCoreDiagnostic diagnostic;
    for (auto &lease : leases) {
      lease = MpvCoreLease::reserve(diagnostic);
      QVERIFY2(lease, qPrintable(diagnostic.message));
      diagnostic = lease->initialize({{"config", "no"}});
      QCOMPARE(diagnostic.error, MPV_ERROR_SUCCESS);
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
    auto refused = MpvCoreLease::reserve(diagnostic);
    QVERIFY(!refused);
    QCOMPARE(diagnostic.error, MPV_ERROR_NOMEM);
    QCOMPARE(diagnostic.stage, MpvCoreStage::Capacity);
    QCOMPARE(faults.counter(FaultRetirementEntered), 2L);
    QCOMPARE(faults.counter(FaultRetirementCompleted), 0L);
    faults.release();
    QTRY_COMPARE_WITH_TIMEOUT(faults.counter(FaultRetirementCompleted), 32L, 10000);
    QTRY_COMPARE_WITH_TIMEOUT(MpvCoreLease::outstandingReservations(), 0, 5000);
    auto accepted = MpvCoreLease::reserve(diagnostic);
    QVERIFY2(accepted, qPrintable(diagnostic.message));
    QCOMPARE(diagnostic.error, MPV_ERROR_SUCCESS);
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
  void attachedShutdownThenDestroyWindow() { attachedDetach(false, false); }
  void attachedShutdownRetiresWhileAlive() { attachedDetach(false, true); }
  void attachedNativeQuitRetiresWhileAlive() { attachedDetach(true, false); }
  void attachedHiddenShutdownRetiresWhileAlive() { attachedDetach(false, true, true); }
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
    TestWindow window;
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

  void publicRenderFailureRecoversCallerIntent() { renderFailureRecovery(true); }
  void publicRenderFailureRetainsInitialIntent() { renderFailureRecovery(false); }

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
    TestWindow window;
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
  void renderFailureRecovery(bool replaceSource) {
    TestWindow window;
    window.resize(160, 120);
    window.setPersistentSceneGraph(false);
    window.setPersistentGraphics(false);
    auto *player = new MpvPlayer(window.contentItem());
    player->setSize(window.size());
    connect(&window, &QQuickWindow::widthChanged, player, [player](int value) { player->setWidth(value); });
    connect(&window, &QQuickWindow::heightChanged, player, [player](int value) { player->setHeight(value); });
    player->setHwdec("no");
    player->setLoopFile("inf");
    player->setVolume(64);
    player->setSource(QStringLiteral(MPVQML_CRASH_VIDEO));
    QSignalSpy failed(player, &MpvPlayer::renderingFailed);
    QSignalSpy ready(player, &MpvPlayer::ready);
    QSignalSpy loaded(player, &MpvPlayer::fileLoaded);
    window.show();
    QVERIFY(QTest::qWaitForWindowExposed(&window));
    QTRY_COMPARE(failed.size(), 1);
    QVERIFY(player->coreAvailable());
    QVERIFY(!player->rendererReady());
    QCOMPARE(player->renderError(), MPV_ERROR_UNSUPPORTED);
    QCOMPARE(ready.size(), 0);
    QCOMPARE(faults.counter(FaultRenderCreates), 1L);
    if (replaceSource) {
      player->setVolume(36);
      player->setPaused(true);
      player->setPlaybackSpeed(0.75);
      player->setFillMode("stretch");
      player->setSource(QFileInfo(QStringLiteral(MPVQML_CRASH_VIDEO)).dir().filePath("test_two_video.mp4"));
    }
    faults.configure(0, 0);
    window.hide();
    window.releaseResources();
    window.show();
    QVERIFY(QTest::qWaitForWindowExposed(&window));
    QTRY_VERIFY_WITH_TIMEOUT(player->rendererReady(), 5000);
    QCOMPARE(player->renderError(), MPV_ERROR_SUCCESS);
    QCOMPARE(ready.size(), 1);
    QCOMPARE(faults.counter(FaultRenderCreates), 2L);
    QTRY_VERIFY_WITH_TIMEOUT(loaded.size() >= 1, 5000);
    auto expectedPixels = [&]() {
      const QImage image = window.grabWindow();
      if (image.isNull()) return false;
      const QColor pixel = image.pixelColor(image.width()/2, image.height()/2);
      return replaceSource ? pixel.red() > 150 && pixel.blue() < 80
                           : pixel.blue() > 150 && pixel.red() < 80;
    };
    QTRY_VERIFY_WITH_TIMEOUT(expectedPixels(), 5000);
    QSignalSpy reads(player, &MpvPlayer::mpvPropertyReadFinished);
    const QStringList properties{"volume", "pause", "speed", "hwdec", "tscale", "keepaspect"};
    QMap<int, QString> requests;
    for (const QString &name : properties) {
      const int id = player->readMpvPropertyAsync(name);
      QVERIFY(id > 0);
      requests.insert(id, name);
    }
    QTRY_COMPARE(reads.size(), properties.size());
    QVariantMap values;
    for (const auto &reply : reads) {
      QCOMPARE(reply[1].toInt(), MPV_ERROR_SUCCESS);
      values.insert(requests.value(reply[0].toInt()), reply[2]);
    }
    QCOMPARE(values.value("volume").toDouble(), replaceSource ? 36.0 : 64.0);
    QCOMPARE(values.value("pause").toBool(), replaceSource);
    QCOMPARE(values.value("speed").toDouble(), replaceSource ? 0.75 : 1.0);
    // Generic node reads expose this libmpv string-list option as an array.
    QCOMPARE(values.value("hwdec").toList(), QVariantList{QStringLiteral("no")});
    QCOMPARE(values.value("tscale").toString(), QStringLiteral("mitchell"));
    QCOMPARE(values.value("keepaspect").toBool(), !replaceSource);
    delete player;
    window.hide();
    window.releaseResources();
    QTRY_COMPARE_WITH_TIMEOUT(MpvCoreLease::outstandingReservations(), 0, 5000);
    exercised();
  }

  void attachedDetach(bool nativeQuit, bool holdRetirement, bool hideItem = false) {
    ReleaseBarrier barrier;
    std::atomic<int> syncCount{0};
    auto window = std::make_unique<TestWindow>();
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
    auto center = [&]() {
      const QImage image = window->grabWindow();
      return image.isNull() ? QColor{} : image.pixelColor(image.width() / 2, image.height() / 2);
    };
    QTRY_VERIFY_WITH_TIMEOUT([&] {
      const QColor pixel = center();
      return pixel.blue() > 150 && pixel.red() < 80 && pixel.green() < 80;
    }(), 5000);
    if (hideItem) player->setVisible(false);
    connect(window.get(), &QQuickWindow::afterSynchronizing, this,
            [&] { ++syncCount; }, Qt::DirectConnection);
    window->setColor(QColor("#345678"));
    if (nativeQuit) {
      QVERIFY(player->commandAsync(QStringList{"quit"}) > 0);
      QTRY_VERIFY_WITH_TIMEOUT(!player->coreAvailable(), 5000);
    } else {
      player->shutdown();
    }
    QVERIFY(!player->coreAvailable());
    QVERIFY(!player->rendererReady());
    const int before = syncCount.load();
    window->resize(220, 160);
    player->setSize(window->size());
    player->update();
    auto stoppedBackground = [&] {
      const QColor pixel = center();
      return pixel.isValid() && qAbs(pixel.red() - 0x34) <= 2
          && qAbs(pixel.green() - 0x56) <= 2 && qAbs(pixel.blue() - 0x78) <= 2;
    };
    QTRY_VERIFY_WITH_TIMEOUT(stoppedBackground(), 5000);
    QVERIFY(syncCount.load() > before);
    if (!nativeQuit && !holdRetirement)
      window.reset(); // Previously dereferenced the cleared snapshot at native free.
    QTRY_COMPARE_WITH_TIMEOUT(faults.counter(FaultRetirementEntered), 1L, 5000);
    QCOMPARE(faults.counter(FaultRenderCreates), 1L);
    QCOMPARE(faults.counter(FaultRenderLive), 0L);
    QCOMPARE(faults.counter(FaultRenderFrees), 1L);
    QCOMPARE(faults.counter(FaultRenderWrongThread), 0L);
    QCOMPARE(faults.counter(FaultRenderFreeWithCallback), 0L);
    if (holdRetirement) {
      QCOMPARE(faults.counter(FaultRetirementCompleted), 0L);
      QCOMPARE(MpvCoreLease::outstandingReservations(), 1);
      faults.release();
    }
    QTRY_COMPARE_WITH_TIMEOUT(MpvCoreLease::outstandingReservations(), 0, 5000);
    QCOMPARE(faults.counter(FaultRetirementCompleted), 1L);
    if (window) {
      QVERIFY(stoppedBackground());
      QVERIFY(player->window() == window.get()); // No item/window teardown needed to retire.
      QCOMPARE(player->commandAsync(QStringList{"stop"}), MPV_ERROR_UNINITIALIZED);
    }
    exercised();
  }

  void screenshotRetirement(bool fifo) {
    ReleaseBarrier barrier;
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    const QString path = directory.filePath(QStringLiteral("shot.jpg"));
    const QByteArray nativePath = path.toUtf8();
    if (fifo) QCOMPARE(::mkfifo(nativePath.constData(), 0600), 0);
    QCOMPARE(faults.watchFile(nativePath.constData()), 0);
    auto window = std::make_unique<TestWindow>();
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
    for (const char *name : {"coreCreationFailure", "coreInitializationFailure", "captureInitializationFailure",
                            "captureWithoutLua", "captureDisablesBuiltinScripts",
                            "playbackOverflowLeavesReservedRepliesIntact",
                            "mixedAdmissionHoldsCancelledSlots", "captureSuccessReusesReleasedSlot",
                            "captureIgnoresUncorrelatedEvents", "captureCancellationWins",
                            "capturePublicationWins", "captureCompetingWriter",
                            "captureCancellationBeforeRetiredDelivery",
                            "captureExpiresBeforeEncoding", "capturePublishesPastDeadline",
                            "captureHelperAbort", "captureHelperDeadline", "captureHelperQueuedCancel",
                            "captureRespectsInheritedAddressLimit", "captureLateWorkerSkipsOpen",
                            "partialRetirementBootstrapFailure", "workerStartupFailure",
                            "bothWorkersBlockedAndAllLiveLeasesReclaimed", "workerTlsCleanupOutlivesFacade",
                            "moduleUnloadWhileBlocked", "publicCoreCreationFailure",
                            "publicCoreInitializationFailure", "publicWorkerStartupFailure",
                            "publicRenderFailure", "publicRenderFailureRecoversCallerIntent",
                            "publicRenderFailureRetainsInitialIntent", "ordinaryFileScreenshotRetirement", "fifoScreenshotRetirement",
                            "synchronousHelperRegistration", "recycledHelperAddressRejectsOldWakeups", "unsupportedGraphicsBackend",
                            "attachedShutdownThenDestroyWindow", "attachedShutdownRetiresWhileAlive",
                            "attachedNativeQuitRetiresWhileAlive", "attachedHiddenShutdownRetiresWhileAlive"})
      QTest::newRow(name) << QString::fromLatin1(name);
  }

  void nativeFaults() {
    QFETCH(QString, name);
    QTemporaryDir faultMarkers;
    QVERIFY(faultMarkers.isValid());
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
    const auto captureMarker = faultMarkers.filePath("native-pid");
    if (name.startsWith("capture") || name == "mixedAdmissionHoldsCancelledSlots") {
      QFile controls(faultMarkers.filePath("controls"));
      QVERIFY(controls.open(QIODevice::ReadWrite | QIODevice::NewOnly));
      QVERIFY(controls.resize(4096));
      environment.insert("MPVQML_TEST_CAPTURE_CONTROLS", controls.fileName());
    }
    if (name.startsWith("captureHelper")) {
      environment.insert("MPVQML_TEST_CAPTURE_FAULT", name == "captureHelperAbort" ? "abort"
          : name == "captureHelperDeadline" ? "hang" : "hold");
      environment.insert("MPVQML_TEST_CAPTURE_MARKER", captureMarker);
    }
    environment.insert("QT_FORCE_STDERR_LOGGING", "1");
    if (name.startsWith("capture") || name == "mixedAdmissionHoldsCancelledSlots") {
      environment.remove("DISPLAY");
      environment.remove("WAYLAND_DISPLAY");
      environment.insert("QT_QPA_PLATFORM", "offscreen");
      environment.insert("QT_QUICK_BACKEND", "software");
    }
    child.setProcessEnvironment(environment);
    child.setProcessChannelMode(QProcess::MergedChannels);
    const QString executable = name == QStringLiteral("moduleUnloadWhileBlocked")
        ? QStringLiteral(MPVQML_UNLOAD_EXECUTABLE) : QCoreApplication::applicationFilePath();
    child.start(executable, QStringList{"--fixture", name});
    QVERIFY2(child.waitForStarted(5000), qPrintable(child.errorString()));
    const auto consumerPid = child.processId();
    if (!child.waitForFinished(30000)) {
      child.kill(); // this owned native fixture is not qs or the locker
      child.waitForFinished(5000);
      QFAIL("native fault child timed out; case NOT_EXERCISED");
    }
    const QByteArray output = child.readAll();
    qInfo().noquote() << QString::fromUtf8(output);
    if (QFile marker(captureMarker); marker.open(QIODevice::ReadOnly))
      qInfo().noquote() << "Capture fault PID:" << marker.readAll().trimmed() << "consumer:" << consumerPid;
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
