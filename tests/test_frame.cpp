// SPDX-License-Identifier: LGPL-3.0-only
#include <QFile>
#include <QImage>
#include <QProcess>
#include <QProcessEnvironment>
#include <QQmlComponent>
#include <QQmlEngine>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QtTest>
#include <clocale>
#include <memory>

class FrameTest : public QObject {
  Q_OBJECT
private:
  QQmlEngine engine;
  std::unique_ptr<QObject> capture;
  QTemporaryDir scratch;

  int start(const QUrl &input, double fraction, const QUrl &output) {
    int id = 0;
    const bool ok = QMetaObject::invokeMethod(capture.get(), "capture", Q_RETURN_ARG(int, id),
        Q_ARG(QUrl, input), Q_ARG(double, fraction), Q_ARG(int, 123), Q_ARG(int, 88), Q_ARG(QUrl, output));
    return ok ? id : 0;
  }

  QUrl asset(const char *file) const {
    return QUrl::fromLocalFile(QStringLiteral(TEST_ASSET_PATH "/") + QString::fromLatin1(file));
  }

  QUrl target(const char *file) const {
    return QUrl::fromLocalFile(scratch.filePath(QString::fromLatin1(file)));
  }

private slots:
  void initTestCase() {
    QVERIFY(scratch.isValid());
    engine.addImportPath(QStringLiteral(TEST_IMPORT_PATH));
    QQmlComponent component(&engine);
    component.setData("import MpvQml\n"
                      "MpvFrameCapture {\n"
                      "  function hasCapacity(): bool { return capacityAvailable() }\n"
                      "  function reportsInvalidArguments(): bool {\n"
                      "    const result = tryCapture('https://example.invalid/video', 0, 64, 64, 'file:///staging/frame.png')\n"
                      "    return result.status === MpvFrameCapture.InvalidArguments && result.id === 0 && result.error < 0\n"
                      "  }\n"
                      "}\n", QUrl());
    QVERIFY2(component.isReady(), qPrintable(component.errorString()));
    capture.reset(component.create());
    QVERIFY(capture);
  }

  void exposesTypedAdmissionInQml() {
    bool value = false;
    QVERIFY(QMetaObject::invokeMethod(capture.get(), "hasCapacity", Q_RETURN_ARG(bool, value)));
    QVERIFY(value);
    QVERIFY(QMetaObject::invokeMethod(capture.get(), "reportsInvalidArguments", Q_RETURN_ARG(bool, value)));
    QVERIFY(value);
  }

  void capturesUnderNonCNumericLocale() {
    if (!qEnvironmentVariableIsSet("MPVQML_NON_C_TEST_CHILD")) {
      QProcess child;
      auto environment = QProcessEnvironment::systemEnvironment();
      environment.insert("MPVQML_NON_C_TEST_CHILD", "1");
      child.setProcessEnvironment(environment);
      child.setProcessChannelMode(QProcess::MergedChannels);
      child.start(QCoreApplication::applicationFilePath(), {"capturesUnderNonCNumericLocale"});
      QVERIFY(child.waitForStarted(5000));
      if (!child.waitForFinished(15000)) {
        child.kill();
        child.waitForFinished(5000);
        QFAIL("Mandatory non-C locale child timed out");
      }
      const auto output = child.readAll();
      qInfo().noquote() << output;
      QCOMPARE(child.exitStatus(), QProcess::NormalExit);
      QCOMPARE(child.exitCode(), 0);
      QVERIFY(output.contains("NON_C_CAPTURE_EXERCISED"));
      return;
    }
    const char *selected = std::setlocale(LC_NUMERIC, "en_US.UTF-8");
    if (!selected)
      selected = std::setlocale(LC_NUMERIC, "de_DE.UTF-8");
    QVERIFY2(selected, "Install en_US.UTF-8 or de_DE.UTF-8 for mandatory non-C coverage");
    const QByteArray active(std::setlocale(LC_NUMERIC, nullptr));
    QVERIFY(!active.isEmpty() && active != "C" && active != "C.UTF-8");
    qInfo() << "Starting headless capture with LC_NUMERIC" << active;

    auto output = target("non-c.png");
    QSignalSpy replies(capture.get(), SIGNAL(finished(int,QString,QString)));
    const int id = start(asset("test_frame_seek.webm"), 0.75, output);
    QVERIFY(id > 0);
    QTRY_COMPARE_WITH_TIMEOUT(replies.size(), 1, 8000);
    const auto reply = replies.takeFirst();
    QCOMPARE(reply[0].toInt(), id);
    QVERIFY2(reply[1].toString().isEmpty(), qPrintable(reply[1].toString() + ": " + reply[2].toString()));
    const QImage image(output.toLocalFile());
    QCOMPARE(image.size(), QSize(123, 69));
    const auto pixel = image.pixelColor(image.width() / 2, image.height() / 2);
    QVERIFY2(pixel.blue() > 210 && pixel.red() < 30, qPrintable(pixel.name()));
    QCOMPARE(QByteArray(std::setlocale(LC_NUMERIC, nullptr)), QByteArray("C"));
    qInfo() << "NON_C_CAPTURE_EXERCISED";
  }

  void choosesRequestedFrame_data() {
    QTest::addColumn<double>("fraction");
    QTest::addColumn<bool>("red");
    QTest::newRow("initial red frame") << 0.0 << true;
    QTest::newRow("seek to blue frame") << 0.75 << false;
  }

  void choosesRequestedFrame() {
    QFETCH(double, fraction);
    QFETCH(bool, red);
    auto output = target(red ? "first.png" : "seek.png");
    QSignalSpy replies(capture.get(), SIGNAL(finished(int,QString,QString)));
    QVERIFY(replies.isValid());
    int id = start(asset("test_frame_seek.webm"), fraction, output);
    QVERIFY(id > 0);
    QVERIFY(replies.isEmpty());
    QTRY_COMPARE_WITH_TIMEOUT(replies.size(), 1, 8000);
    auto reply = replies.takeFirst();
    QCOMPARE(reply[0].toInt(), id);
    QVERIFY2(reply[1].toString().isEmpty(), qPrintable(reply[1].toString() + ": " + reply[2].toString()));
    const QImage image(output.toLocalFile());
    QCOMPARE(image.size(), QSize(123, 69));
    const auto pixel = image.pixelColor(image.width() / 2, image.height() / 2);
    QVERIFY2(red ? pixel.red() > 210 && pixel.blue() < 30 : pixel.blue() > 210 && pixel.red() < 30,
             qPrintable(pixel.name()));
  }

  void failureAndLocalOnly() {
    auto output = target("audio.png");
    QSignalSpy replies(capture.get(), SIGNAL(finished(int,QString,QString)));
    QVERIFY(start(QUrl("https://example.invalid/no.webm"), 0, output) < 0);
    QVERIFY(replies.isEmpty());
    int id = start(asset("test_audio1.mp3"), 0, output);
    QVERIFY(id > 0);
    QTRY_COMPARE_WITH_TIMEOUT(replies.size(), 1, 8000);
    auto reply = replies.takeFirst();
    QCOMPARE(reply[0].toInt(), id);
    QVERIFY(!reply[1].toString().isEmpty());
    QVERIFY(!QFile::exists(output.toLocalFile()));
  }

  void cancelThenCapture() {
    auto input = asset("test_frame_seek.webm");
    auto output = target("cancelled.png");
    QSignalSpy replies(capture.get(), SIGNAL(finished(int,QString,QString)));
    int id = start(input, 0.75, output);
    QVERIFY(id > 0);
    QVERIFY(QMetaObject::invokeMethod(capture.get(), "cancel", Q_ARG(int, id)));
    QVERIFY(replies.isEmpty());
    int next = start(input, 0, target("after.png"));
    QVERIFY(next > id);
    QTRY_COMPARE_WITH_TIMEOUT(replies.size(), 2, 8000);
    QCOMPARE(replies[0][0].toInt(), id);
    QCOMPARE(replies[0][1].toString(), QString("cancelled"));
    QCOMPARE(replies[1][0].toInt(), next);
    QVERIFY2(replies[1][1].toString().isEmpty(), qPrintable(replies[1][2].toString()));
    QVERIFY(!QFile::exists(output.toLocalFile()));
  }
};

QTEST_GUILESS_MAIN(FrameTest)
#include "test_frame.moc"
