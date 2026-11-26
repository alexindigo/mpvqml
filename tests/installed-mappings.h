#pragma once

#include <QFile>
#include <QFileInfo>
#include <QString>
#include <cstdio>

inline bool verifyInstalledMappings(const QString &libdir, const QByteArray &source,
                                    const QByteArray &build, bool module) {
    QFile maps(QStringLiteral("/proc/self/maps"));
    if (!maps.open(QIODevice::ReadOnly)) return false;
    const QByteArray mapped = maps.readAll();
    const auto plugin = (libdir + QStringLiteral("/qt6/qml/MpvQml/libmpvqmlplugin.so")).toLocal8Bit();
    const auto backing = QFileInfo(libdir + QStringLiteral("/libmpvqml.so")).canonicalFilePath().toLocal8Bit();
    if (backing.isEmpty() || !mapped.contains(backing + '\n') || mapped.contains(source + '/')
        || mapped.contains(build + '/') || mapped.contains("libMpvQt")) return false;
    if (module) {
        if (!mapped.contains(plugin + '\n')) return false;
        std::printf("INSTALLED_PLUGIN_MAP %s\n", plugin.constData());
    }
    std::printf("INSTALLED_BACKING_MAP %s\n", backing.constData());
    return true;
}
