#include "mpvrenderbridge_p.h"

#include <QSignalSpy>
#include <QtTest>
#include <thread>
#include <vector>

using namespace MpvQmlPrivate;

class TestRetirement : public QObject {
  Q_OBJECT
private slots:
  void reservedAdmissionIsBoundedAndRecoverable() {
    QTRY_COMPARE(MpvCoreLease::outstandingReservations(), 0);
    std::vector<std::shared_ptr<MpvCoreLease>> leases;
    MpvCoreDiagnostic diagnostic;
    for (int i = 0; i < MpvBackendCore::Capacity; ++i) {
      auto lease = MpvCoreLease::reserve(diagnostic);
      QVERIFY2(lease, qPrintable(diagnostic.message));
      QCOMPARE(diagnostic.error, MPV_ERROR_SUCCESS);
      leases.push_back(std::move(lease));
    }
    QCOMPARE(MpvCoreLease::outstandingReservations(), MpvBackendCore::Capacity);
    auto rejected = MpvCoreLease::reserve(diagnostic);
    QVERIFY(!rejected);
    QCOMPARE(diagnostic.error, MPV_ERROR_NOMEM);
    QCOMPARE(diagnostic.stage, MpvCoreStage::Capacity);
    QVERIFY(!diagnostic.message.isEmpty());
    leases.clear();
    QCOMPARE(MpvCoreLease::outstandingReservations(), 0);
    auto recovered = MpvCoreLease::reserve(diagnostic);
    QVERIFY2(recovered, qPrintable(diagnostic.message));
    QCOMPARE(diagnostic.error, MPV_ERROR_SUCCESS);
  }

  void finalNativeHandleRetiresOffCallerAndIsReclaimed() {
    QTRY_COMPARE(MpvCoreLease::outstandingReservations(), 0);
    MpvCoreDiagnostic diagnostic;
    auto lease = MpvCoreLease::reserve(diagnostic);
    QVERIFY2(lease, qPrintable(diagnostic.message));
    QCOMPARE(lease->initialize({{"config", "no"}}).error, MPV_ERROR_SUCCESS);
    QVERIFY(lease->handle());
    QElapsedTimer deletion;
    deletion.start();
    lease.reset();
    QVERIFY2(deletion.elapsed() < 250, "last-reference release exceeded 250 ms");
    QTRY_COMPARE_WITH_TIMEOUT(MpvCoreLease::outstandingReservations(), 0, 5000);
  }

  void retiredRendererCannotReleaseReplacementOwnership() {
    MpvCoreDiagnostic diagnostic;
    auto lease = MpvCoreLease::reserve(diagnostic);
    QVERIFY2(lease, qPrintable(diagnostic.message));
    QVERIFY(lease->claimRenderer(1));
    QVERIFY(!lease->claimRenderer(2));
    lease->releaseRenderer(1);
    QVERIFY(lease->claimRenderer(2));
    lease->releaseRenderer(1);
    QVERIFY(!lease->claimRenderer(3));
    lease->releaseRenderer(2);
    QVERIFY(lease->claimRenderer(3));
    lease->releaseRenderer(3);
  }

  void redrawPostingCoalescesWhileGuiIsBlocked() {
    auto dispatcher = MpvRenderDispatcher::create();
    QSignalSpy redraw(dispatcher.get(), &MpvRenderDispatcher::redrawRequested);
    std::thread producer([dispatcher]() {
      for (int i = 0; i < 10000; ++i)
        dispatcher->postRedraw();
    });
    producer.join();
    QCOMPARE(redraw.count(), 0);
    QCoreApplication::processEvents();
    QCOMPARE(redraw.count(), 1);
    dispatcher->postRedraw();
    QCoreApplication::processEvents();
    QCOMPARE(redraw.count(), 2);
  }
};

QTEST_GUILESS_MAIN(TestRetirement)
#include "test_retirement.moc"
