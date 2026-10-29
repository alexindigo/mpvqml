#include "mpvcontroller_p.h"

#include <QDateTime>
#include <QtTest>
#include <clocale>
#include <memory>
#include <type_traits>

using namespace MpvQmlPrivate;

class TestController : public QObject {
  Q_OBJECT
private slots:
  void nestedOwnedValuesSurviveInputRelease() {
    const QVariantMap original{
        {"string", QString::fromUtf8("owned \xE2\x98\x83")},
        {"bytes", QByteArray("a\0b", 3)},
        {"list", QVariantList{true, false, qlonglong(4294967297LL), 1.25}},
        {"map", QVariantMap{{"inner", QStringLiteral("value")}}}};
    QVariant copy;
    {
      MpvInputNode input;
      QCOMPARE(input.assign(original), MPV_ERROR_SUCCESS);
      copy = copyNodeValue(input.get());
    }
    QCOMPARE(copy.toMap(), original);
  }

  void conversionRejectionReleasesPartialTree() {
    for (int i = 0; i < 500; ++i) {
      MpvInputNode input;
      const QVariantMap invalid{{"a-owned", QStringLiteral("allocated")},
                                {"z-unsupported", QDateTime::currentDateTime()}};
      QCOMPARE(input.assign(invalid), MPV_ERROR_INVALID_PARAMETER);
      QCOMPARE(input.get()->format, MPV_FORMAT_NONE);
      QCOMPARE(input.assign(QStringList{"set", "pause", "yes"}), MPV_ERROR_SUCCESS);
      QCOMPARE(copyNodeValue(input.get()).toList().size(), 3);
    }
  }

  void propertyFlagsAndUnavailablePayloads() {
    static_assert(std::is_same_v<decltype(mpv_node{}.u.flag), int>);
    int flag = 0;
    mpv_event_property property{"pause", MPV_FORMAT_FLAG, &flag};
    int error = MPV_ERROR_SUCCESS;
    QCOMPARE(copyPropertyValue(&property, error), QVariant(false));
    flag = 1;
    QCOMPARE(copyPropertyValue(&property, error), QVariant(true));
    property.data = nullptr;
    QVERIFY(!copyPropertyValue(&property, error).isValid());
    QCOMPARE(error, MPV_ERROR_PROPERTY_UNAVAILABLE);
    error = MPV_ERROR_PROPERTY_NOT_FOUND;
    QVERIFY(!copyPropertyValue(nullptr, error).isValid());
    QCOMPARE(error, MPV_ERROR_PROPERTY_NOT_FOUND);
    property.format = MPV_FORMAT_NONE;
    error = MPV_ERROR_SUCCESS;
    QVERIFY(!copyPropertyValue(&property, error).isValid());
    QCOMPARE(error, MPV_ERROR_SUCCESS);
  }

  void nativeAsyncSubmissionsReleaseInputBeforeReply() {
    // This direct native fixture bypasses the controller's locale setup.
    QVERIFY(std::setlocale(LC_NUMERIC, "C"));
    std::unique_ptr<mpv_handle, decltype(&mpv_destroy)> core(mpv_create(), mpv_destroy);
    QVERIFY(core);
    QCOMPARE(mpv_set_option_string(core.get(), "config", "no"), MPV_ERROR_SUCCESS);
    QCOMPARE(mpv_initialize(core.get()), MPV_ERROR_SUCCESS);
    for (quint64 token = 1; token <= 200; ++token) {
      const bool desired = token % 2 != 0;
      {
        MpvInputNode input;
        QCOMPARE(input.assign(desired), MPV_ERROR_SUCCESS);
        QCOMPARE(mpv_set_property_async(core.get(), token, "pause", MPV_FORMAT_NODE,
                                         input.get()), MPV_ERROR_SUCCESS);
      }
      bool completed = false;
      QElapsedTimer deadline;
      deadline.start();
      while (!completed && deadline.elapsed() < 2000) {
        const mpv_event *event = mpv_wait_event(core.get(), 0.05);
        if (event->event_id == MPV_EVENT_SET_PROPERTY_REPLY && event->reply_userdata == token) {
          QCOMPARE(event->error, MPV_ERROR_SUCCESS);
          completed = true;
        }
      }
      QVERIFY2(completed, "native property submission was not exercised to completion");
      MpvInputNode rejected;
      QCOMPARE(rejected.assign(QStringList{"__mpvqml_unknown_command__"}), MPV_ERROR_SUCCESS);
      QVERIFY(mpv_command_node_async(core.get(), token + 4294967296ULL, rejected.get()) < 0);
    }
  }

  void workerPublishesOwnedRepliesAndCleansUpWithoutFacadeJoin() {
    QTRY_COMPARE(MpvCoreLease::outstandingReservations(), 0);
    QObject consumer;
    QList<MpvNativeReply> received;
    MpvBackendWorker worker;
    QVERIFY(worker.controller());
    connect(worker.controller(), &MpvController::reply, &consumer,
            [&received](const MpvNativeReply &value) { received.append(value); },
            Qt::QueuedConnection);
    const auto initialized = worker.initialize();
    QVERIFY2(initialized.success, qPrintable(initialized.message));
    QCOMPARE(initialized.error, MPV_ERROR_SUCCESS);
    QVERIFY(worker.core()->handle());
    const quint64 token = 4294967296ULL + 17;
    QVERIFY(worker.post([token](MpvController *controller) {
      controller->submitCommand(token, QStringList{"expand-text", "retained-result"});
    }));
    QThread::msleep(20); // consumer intentionally cannot drain the queued value
    QTRY_COMPARE(received.size(), 1);
    QCOMPARE(received[0].token, token);
    QCOMPARE(received[0].kind, MpvRequestKind::Command);
    QCOMPARE(received[0].error, MPV_ERROR_SUCCESS);
    QCOMPARE(received[0].value.toString(), QStringLiteral("retained-result"));
    QVERIFY(worker.post([token](MpvController *controller) {
      controller->submitPropertyRead(token + 1, QStringLiteral("__mpvqml_missing_property__"));
    }));
    QTRY_COMPARE(received.size(), 2);
    QCOMPARE(received[1].token, token + 1);
    QCOMPARE(received[1].kind, MpvRequestKind::PropertyRead);
    QVERIFY(received[1].error < 0);
    QVERIFY(!received[1].value.isValid());
    QElapsedTimer teardown;
    teardown.start();
    worker.shutdown();
    QVERIFY(teardown.elapsed() < 250);
    QVERIFY(!worker.post([](MpvController *) {}));
    QTRY_COMPARE_WITH_TIMEOUT(MpvCoreLease::outstandingReservations(), 0, 5000);
  }
};

QTEST_GUILESS_MAIN(TestController)
#include "test_controller.moc"
