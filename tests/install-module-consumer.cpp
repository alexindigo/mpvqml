#include "installed-mappings.h"
#include <QGuiApplication>
#include <QImage>
#include <QLibraryInfo>
#include <QMetaObject>
#include <QQmlComponent>
#include <QQmlEngine>
#include <QSignalSpy>
#include <QVariant>
#include <cstdio>
#include <memory>

static int readPlayer(QObject *player) {
    if (!player->property("coreAvailable").toBool()) return 6;
    QSignalSpy completed(player, SIGNAL(mpvPropertyReadFinished(int,int,QVariant)));
    if (!completed.isValid()) return 7;
    int id = 0;
    if (!QMetaObject::invokeMethod(player, "readMpvPropertyAsync", Qt::DirectConnection,
                                   Q_RETURN_ARG(int, id), Q_ARG(QString, QStringLiteral("mpv-version")))
        || id <= 0 || completed.count() != 0 || !completed.wait(5000) || completed.count() != 1)
        return 8;
    const auto reply = completed.takeFirst();
    if (reply.size() != 3 || reply[0].toInt() != id || reply[1].toInt() != 0
        || reply[2].toString().isEmpty()) return 9;
    std::printf("INSTALLED_MODULE_OWNED_READ id=%d value=%s\n", id, qPrintable(reply[2].toString()));
    return 0;
}

static int captureTimeline(QObject *capture, const QUrl &video, const QString &output) {
    QSignalSpy done(capture, SIGNAL(finished(int,QString,QString)));
    if (!done.isValid()) return 10;
    for (bool red : {true, false}) {
        const QString path = output + (red ? "/red.png" : "/blue.png");
        int id = 0;
        if (!QMetaObject::invokeMethod(capture, "capture", Q_RETURN_ARG(int, id), Q_ARG(QUrl, video),
               Q_ARG(double, red ? 0.0 : 0.75), Q_ARG(int, 123), Q_ARG(int, 88),
               Q_ARG(QUrl, QUrl::fromLocalFile(path))) || id <= 0 || !done.isEmpty()
            || !done.wait(9000) || done.size() != 1) return 11;
        const auto reply = done.takeFirst();
        if (reply[0].toInt() != id || !reply[1].toString().isEmpty()) {
            std::fprintf(stderr, "capture %d: %s: %s\n", id, qPrintable(reply[1].toString()),
                         qPrintable(reply[2].toString()));
            return 12;
        }
        const QImage image(path);
        if (image.size() != QSize(123, 69)) return 13;
        const auto px = image.pixelColor(image.width() / 2, image.height() / 2);
        if (px.green() >= 30 || !(red ? px.red() > 210 && px.blue() < 30 : px.blue() > 210 && px.red() < 30))
            return 14;
        std::printf("INSTALLED_CAPTURE_PIXEL %s 123x69 %s\n", red ? "initial" : "seek", qPrintable(px.name()));
    }
    return 0;
}

int main(int argc, char **argv) {
    QGuiApplication application(argc, argv);
    if (argc != 4 && argc != 6) return 2;
    const bool capture = argc == 6;
    const QString libdir = QString::fromLocal8Bit(argv[1]);
    QQmlEngine engine;
    engine.setImportPathList({libdir + QStringLiteral("/qt6/qml"), QLibraryInfo::path(QLibraryInfo::QmlImportsPath)});
    QQmlComponent component(&engine);
    component.setData(capture ? "import MpvQml\nMpvFrameCapture {}" : "import QtQuick\nimport MpvQml\nMpvPlayer {}", QUrl());
    std::unique_ptr<QObject> object(component.create());
    if (!object) {
        std::fprintf(stderr, "%s\n", qPrintable(component.errorString()));
        return 3;
    }
    if (!verifyInstalledMappings(libdir, argv[2], argv[3], true)) return 5;
    return capture ? captureTimeline(object.get(), QUrl::fromLocalFile(QString::fromLocal8Bit(argv[4])),
                                     QString::fromLocal8Bit(argv[5])) : readPlayer(object.get());
}
