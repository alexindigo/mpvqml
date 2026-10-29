#include "mpvrenderbridge_p.h"

#include <QSignalSpy>
#include <QtTest>
#include <clocale>
#include <thread>
#include <vector>

using namespace MpvQmlPrivate;

class TestRetirement : public QObject {
  Q_OBJECT
private slots:
  void reservedAdmissionIsBoundedAndRecoverable() {
    QTRY_COMPARE(MpvCoreLease::outstandingReservations(), 0);
    std::vector<std::shared_ptr<MpvCoreLease>> leases;
    int error = 0;
    QString message;
    for (int i = 0; i < 32; ++i) {
      auto lease = MpvCoreLease::reserve(error, message);
      QVERIFY2(lease, qPrintable(message));
      QCOMPARE(error, MPV_ERROR_SUCCESS);
      leases.push_back(std::move(lease));
    }
    QCOMPARE(MpvCoreLease::outstandingReservations(), 32);
    auto rejected = MpvCoreLease::reserve(error, message);
    QVERIFY(!rejected);
    QCOMPARE(error, MPV_ERROR_NOMEM);
    QVERIFY(!message.isEmpty());
    leases.clear();
    QCOMPARE(MpvCoreLease::outstandingReservations(), 0);
    auto recovered = MpvCoreLease::reserve(error, message);
    QVERIFY2(recovered, qPrintable(message));
    QCOMPARE(error, MPV_ERROR_SUCCESS);
  }

  void finalNativeHandleRetiresOffCallerAndIsReclaimed() {
    QTRY_COMPARE(MpvCoreLease::outstandingReservations(), 0);
    int error = 0;
    QString message;
    auto lease = MpvCoreLease::reserve(error, message);
    QVERIFY2(lease, qPrintable(message));
    // This direct native fixture bypasses the controller's locale setup.
    QVERIFY(std::setlocale(LC_NUMERIC, "C"));
    mpv_handle *handle = mpv_create();
    QVERIFY(handle);
    QCOMPARE(mpv_initialize(handle), MPV_ERROR_SUCCESS);
    lease->publish(handle);
    QElapsedTimer deletion;
    deletion.start();
    lease.reset();
    QVERIFY2(deletion.elapsed() < 250, "last-reference release exceeded 250 ms");
    QTRY_COMPARE_WITH_TIMEOUT(MpvCoreLease::outstandingReservations(), 0, 5000);
  }

  void retiredRendererCannotReleaseReplacementOwnership() {
    int error = 0;
    QString message;
    auto lease = MpvCoreLease::reserve(error, message);
    QVERIFY2(lease, qPrintable(message));
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
