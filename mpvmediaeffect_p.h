#pragma once

#include <QStringList>

namespace MpvQmlPrivate {

enum class MediaEffect { None, Replace, ReplaceIfIdle, ClearPendingLoads, Seek, Pause };

inline MediaEffect propertyMediaEffect(const QString &name) {
    if (name == "playlist-pos" || name == "playlist-pos-1" || name == "playlist-current-pos" || name == "path")
        return MediaEffect::Replace;
    if (name == "time-pos" || name == "time-pos/full" || name == "percent-pos")
        return MediaEffect::Seek;
    if (name == "pause") return MediaEffect::Pause;
    return MediaEffect::None;
}

inline MediaEffect commandMediaEffect(const QStringList &args) {
    const auto command = args.value(0);
    if (command == "loadfile" || command == "loadlist") {
        const auto mode = args.value(2, QStringLiteral("replace"));
        if (mode == "append" || mode == "insert-next" || mode == "insert-at") return MediaEffect::None;
        if (mode == "append-play" || mode == "insert-next-play" || mode == "insert-at-play")
            return MediaEffect::ReplaceIfIdle;
        return MediaEffect::Replace;
    }
    if (command == "stop" || command == "quit" || command == "quit-watch-later" || command == "playlist-next"
        || command == "playlist-prev" || command == "playlist-play-index"
        || (command == "playlist-remove" && args.value(1) == "current")) return MediaEffect::Replace;
    if (command == "playlist-clear") return MediaEffect::ClearPendingLoads;
    if (command == "seek" || (command == "revert-seek" && args.value(1) != "mark")) return MediaEffect::Seek;
    if (command == "set" || command == "add" || command == "cycle" || command == "multiply" || command == "cycle-values")
        return propertyMediaEffect(args.value(1));
    return MediaEffect::None;
}

} // namespace MpvQmlPrivate
