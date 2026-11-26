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
#include <unistd.h>

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

  void captureAppliesDisplayRotation_data() {
    QTest::addColumn<int>("angle");
    QTest::addColumn<QSize>("size");
    QTest::addColumn<QString>("quadrants");
    QTest::newRow("90") << 90 << QSize(49, 88) << QStringLiteral("byrg");
    QTest::newRow("180") << 180 << QSize(123, 69) << QStringLiteral("ygbr");
    QTest::newRow("270") << 270 << QSize(49, 88) << QStringLiteral("gryb");
  }

  void captureAppliesDisplayRotation() {
    QFETCH(int, angle);
    QFETCH(QSize, size);
    QFETCH(QString, quadrants);
    const auto path = target(qPrintable(QStringLiteral("rotated-%1.png").arg(angle)));
    QSignalSpy replies(capture.get(), SIGNAL(finished(int,QString,QString)));
    QVERIFY(start(asset(qPrintable(QStringLiteral("test_frame_rotated_%1.mp4").arg(angle))), 0, path) > 0);
    QTRY_COMPARE_WITH_TIMEOUT(replies.size(), 1, 8000);
    QVERIFY2(replies[0][1].toString().isEmpty(), qPrintable(replies[0][2].toString()));
    const QImage image(path.toLocalFile());
    QCOMPARE(image.size(), size);
    for (int i = 0; i < 4; ++i) {
      const auto pixel = image.pixelColor(size.width() * (i % 2 ? 3 : 1) / 4,
                                          size.height() * (i / 2 ? 3 : 1) / 4);
      const auto expected = quadrants.at(i);
      QCOMPARE(pixel.red() > 128, expected == 'r' || expected == 'y');
      QCOMPARE(pixel.green() > 128, expected == 'g' || expected == 'y');
      QCOMPARE(pixel.blue() > 128, expected == 'b');
    }
  }

  void documentedCaptureExampleStagesPixels() {
    QFile doc(QStringLiteral(TEST_ASSET_PATH "/../../API.md"));
    QVERIFY(doc.open(QIODevice::ReadOnly));
    const auto bytes = doc.readAll();
    const QByteArray marker = "<!-- capture-example -->\n```qml\n";
    const auto offset = bytes.indexOf(marker);
    QVERIFY(offset >= 0);
    const auto start = offset + marker.size();
    const auto end = bytes.indexOf("\n```", start);
    QVERIFY(end > start);
    QQmlComponent component(&engine);
    component.setData(bytes.mid(start, end - start), QUrl());
    QVERIFY2(component.isReady(), qPrintable(component.errorString()));
    std::unique_ptr<QObject> example(component.create());
    QVERIFY2(example, qPrintable(component.errorString()));
    const auto path = target("example.png");
    QVERIFY(QMetaObject::invokeMethod(example.get(), "requestFrame", asset("test_frame_seek.webm"), path));
    QVERIFY(example->property("ticket").toInt() > 0);
    QTRY_VERIFY(example->property("staged").toBool());
    QCOMPARE(example->property("ticket").toInt(), 0);
    const QImage image(path.toLocalFile());
    QCOMPARE(image.size(), QSize(123, 69));
    const auto pixel = image.pixelColor(61, 34);
    QVERIFY2(pixel.blue() > 210 && pixel.red() < 30, qPrintable(pixel.name()));
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
    QCOMPARE(reply[1].toString(), QString("no-frame"));
    QVERIFY(!QFile::exists(output.toLocalFile()));
  }

  void detailedFailures_data() {
    QTest::addColumn<QString>("kind");
    for (const char *kind : {"missing input", "directory input", "malformed video", "audio only", "missing staging directory"})
      QTest::newRow(kind) << QString::fromLatin1(kind);
  }

  void detailedFailures() {
    QFETCH(QString, kind);
    QUrl input = asset("test_frame_seek.webm");
    QUrl output = target("should-not-exist.png");
    QString code, stage;
    if (kind == "missing input" || kind == "directory input") {
      input = kind == "missing input" ? target("missing.webm") : QUrl::fromLocalFile(scratch.path());
      code = "unreadable";
      stage = "open-input";
    } else if (kind == "missing staging directory") {
      output = target("missing-parent/frame.png");
      code = "write-failed";
      stage = "publish";
    } else {
      if (kind == "audio only") {
        input = asset("test_audio1.mp3");
      } else {
        input = target("malformed.webm");
        QFile file(input.toLocalFile());
        QVERIFY(file.open(QIODevice::WriteOnly | QIODevice::NewOnly));
        QCOMPARE(file.write("not a video"), 11);
      }
      code = "no-frame";
      stage = "initial-frame";
    }
    QSignalSpy replies(capture.get(), SIGNAL(finished(int,QString,QString)));
    QSignalSpy details(capture.get(), SIGNAL(finishedDetailed(int,QVariantMap)));
    QVERIFY(details.isValid());
    const int id = start(input, 0, output);
    QVERIFY(id > 0);
    QTRY_COMPARE_WITH_TIMEOUT(details.size(), 1, 8000);
    QCOMPARE(replies.size(), 1);
    QCOMPARE(details[0][0].toInt(), id);
    auto detail = details[0][1].toMap();
    QCOMPARE(detail["stage"].toString(), stage);
    QCOMPARE(detail["code"].toString(), code);
    QCOMPARE(replies[0][1].toString(), code);
    QCOMPARE(detail["reason"].toString(), replies[0][2].toString());
    QCOMPARE(detail["mpvError"].toInt() < 0, code == "no-frame");
    QCOMPARE(detail["systemError"].toInt() > 0, kind == "missing input" || kind == "missing staging directory");
    QVERIFY(!QFile::exists(output.toLocalFile()));
  }

  void protectsExistingTargets_data() {
    QTest::addColumn<QString>("kind");
    for (const char *kind : {"collision", "parent alias", "target symlink", "hard link"})
      QTest::newRow(kind) << QString::fromLatin1(kind);
  }

  void protectsExistingTargets() {
    QFETCH(QString, kind);
    QTemporaryDir dir(scratch.filePath("aliases-XXXXXX"));
    QVERIFY(dir.isValid());
    const QString copiedSource = dir.filePath("media.webm");
    QVERIFY(QFile::copy(asset("test_frame_seek.webm").toLocalFile(), copiedSource));
    QString path = dir.filePath("existing.png");
    if (kind == "parent alias") {
      QVERIFY(QFile::link(dir.path(), dir.filePath("self")));
      path = dir.filePath("self/media.webm");
    } else if (kind == "target symlink") {
      QVERIFY(QFile::link(copiedSource, path));
    } else if (kind == "hard link") {
      QCOMPARE(::link(QFile::encodeName(copiedSource).constData(), QFile::encodeName(path).constData()), 0);
    } else {
      QFile sentinel(path);
      QVERIFY(sentinel.open(QIODevice::WriteOnly));
      QCOMPARE(sentinel.write("sentinel"), 8);
    }
    QFile original(path);
    QVERIFY(original.open(QIODevice::ReadOnly));
    const auto before = original.readAll();
    original.close();
    QSignalSpy replies(capture.get(), SIGNAL(finished(int,QString,QString)));
    const auto id = start(QUrl::fromLocalFile(copiedSource), 0, QUrl::fromLocalFile(path));
    QVERIFY(id > 0);
    QTRY_COMPARE_WITH_TIMEOUT(replies.size(), 1, 8000);
    QCOMPARE(replies[0][0].toInt(), id);
    QCOMPARE(replies[0][1].toString(), QString("write-failed"));
    QVERIFY(original.open(QIODevice::ReadOnly));
    QCOMPARE(original.readAll(), before);
    if (kind == "target symlink")
      QCOMPARE(QFile::symLinkTarget(path), copiedSource);
  }

  void rejectsUnknownCancellationThroughQmlType() {
    QSignalSpy replies(capture.get(), SIGNAL(finished(int,QString,QString)));
    bool cancelled = true;
    QVERIFY(QMetaObject::invokeMethod(capture.get(), "cancel", Q_RETURN_ARG(bool, cancelled), Q_ARG(int, -1)));
    QVERIFY(!cancelled);
    QVERIFY(replies.isEmpty());
  }
};

QTEST_GUILESS_MAIN(FrameTest)
#include "test_frame.moc"
