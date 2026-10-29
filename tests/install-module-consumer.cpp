#include <QFile>
#include <QGuiApplication>
#include <QMetaObject>
#include <QQmlComponent>
#include <QQmlEngine>
#include <QSignalSpy>
#include <QVariant>
#include <cstdio>
#include <memory>

int main(int argc, char **argv) {
    QGuiApplication application(argc, argv);
    if (argc != 4) return 2;
    const QString prefix = QString::fromLocal8Bit(argv[1]);
    const QByteArray build = QByteArray(argv[2]);
    const QString libdir = prefix + QLatin1Char('/') + QString::fromLocal8Bit(argv[3]);
    QQmlEngine engine;
    engine.addImportPath(libdir + QStringLiteral("/qt6/qml"));
    QQmlComponent component(&engine);
    component.setData("import QtQuick\nimport MpvQml\nMpvPlayer {}", QUrl());
    std::unique_ptr<QObject> player(component.create());
    if (!player) {
        std::fprintf(stderr, "%s\n", qPrintable(component.errorString()));
        return 3;
    }
    QFile maps(QStringLiteral("/proc/self/maps"));
    if (!maps.open(QIODevice::ReadOnly)) return 4;
    const QByteArray mapped = maps.readAll();
    const QByteArray plugin = (libdir + QStringLiteral("/qt6/qml/MpvQml/libmpvqmlplugin.so")).toLocal8Bit();
    const QByteArray backing = (libdir + QStringLiteral("/libmpvqml.so")).toLocal8Bit();
    if (!mapped.contains(plugin) || !mapped.contains(backing) || mapped.contains(build + '/')
        || mapped.contains("libMpvQt")) return 5;
    if (!player->property("coreAvailable").toBool()) return 6;
    QSignalSpy completed(player.get(), SIGNAL(mpvPropertyReadFinished(int,int,QVariant)));
    if (!completed.isValid()) return 7;
    int id = 0;
    if (!QMetaObject::invokeMethod(player.get(), "readMpvPropertyAsync", Qt::DirectConnection,
                                   Q_RETURN_ARG(int, id), Q_ARG(QString, QStringLiteral("mpv-version")))
        || id <= 0 || completed.count() != 0 || !completed.wait(5000) || completed.count() != 1)
        return 8;
    const auto reply = completed.takeFirst();
    if (reply.size() != 3 || reply[0].toInt() != id || reply[1].toInt() != 0
        || reply[2].toString().isEmpty()) return 9;
    std::printf("INSTALLED_MODULE_OWNED_READ id=%d value=%s\n", id, qPrintable(reply[2].toString()));
    std::printf("INSTALLED_PLUGIN_MAP %s\nINSTALLED_BACKING_MAP %s\n", plugin.constData(), backing.constData());
    return 0;
}
