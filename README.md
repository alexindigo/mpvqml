# mpvqml

QML component wrapping libmpv via MpvQt for Qt6 applications.

```qml
import MpvQml

MpvPlayer {
    anchors.fill: parent
    source: "video.mp4"
}
```

## Requirements

- Qt 6.5+ (Core, Quick, Test for builds)
- libmpv (dev package)
- [MpvQt](https://github.com/OpenHealthcare/mpvqt) (system-installed, version 1.2.0+)

## Build & Install

```
scripts/build
sudo scripts/install
```

This builds a release library and installs the MpvQml module to the system
QML import path. To install to a custom prefix:

```
PREFIX=/opt/mpvqml scripts/install
```

A debug build with test coverage is also available:

```
scripts/build-test
```

## Usage

See [API.md](API.md) for the complete QML API reference,
including all properties, signals, and methods.

A minimal player:

```qml
import QtQuick
import QtQuick.Controls
import MpvQml

Item {
    width: 800
    height: 600

    MpvPlayer {
        id: player
        anchors.fill: parent
        source: "~/Videos/sample.mp4"
    }

    Button {
        text: player.paused ? "Play" : "Pause"
        onClicked: player.paused = !player.paused
    }
}
```

Pre-built examples are in the `examples/` directory:

```
qml6 examples/AmbientScreensaver.qml
qml6 examples/AudioPlayer.qml
qml6 examples/MediaInspector.qml
qml6 examples/Player.qml
```

## Documentation

| File | Audience |
|------|----------|
| [API.md](API.md) | QML developers (no C++ knowledge required) |
| [FeatureMapping.md](FeatureMapping.md) | libmpv / MpvQt developers |
| [FutureDevelopment.md](FutureDevelopment.md) | Contributors and roadmap planning |

## Project Status

Playback component with media control, track management, video processing,
and scripting interfaces. Feature-complete for the 1.0 release; new
development tracked in FutureDevelopment.md.

## License

GPLv3
