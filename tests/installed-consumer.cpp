#include <mpvqml/mpvplayer.h>
#include <QGuiApplication>
#include <QEventLoop>
#include <QFile>
#include <QTimer>
#include <QDebug>
#include <type_traits>

static_assert(std::is_base_of_v<QQuickFramebufferObject, MpvPlayer>);

int main(int argc, char **argv) {
    QGuiApplication application(argc, argv);
    QFile maps(QStringLiteral("/proc/self/maps"));
    if (!maps.open(QIODevice::ReadOnly) || maps.readAll().contains("libMpvQt")) return 3;
    QEventLoop loop;
    MpvPlayer player;
    if (!player.coreAvailable()) return 4;
    int expected = 0;
    int received = 0;
    int result = 5;
    QObject::connect(&player, &MpvPlayer::mpvPropertyReadFinished, &loop,
                     [&](int id, int error, const QVariant &value) {
        if (id != expected) return;
        ++received;
        if (error == 0 && !value.toString().isEmpty()) {
            result = 0;
            qInfo().noquote() << "INSTALLED_CONSUMER_OWNED_READ" << id << value.toString();
        }
        loop.quit();
    });
    expected = player.readMpvPropertyAsync(QStringLiteral("mpv-version"));
    if (expected <= 0 || received != 0) return 6;
    QTimer::singleShot(5000, &loop, &QEventLoop::quit);
    loop.exec();
    if (received != 1) return 7;
    qInfo() << "NO_MPVQT_MAPPINGS; installed public-header consumer completed";
    return result;
}
