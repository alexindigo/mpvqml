# mpvqml

QML component using an mpvqml-owned Qt/libmpv backend for Qt6 applications.

```qml
import MpvQml

MpvPlayer {
    anchors.fill: parent
    source: "video.mp4"
}
```

## Requirements

- Qt 6.5+ (Core, Gui, Quick, OpenGL; Test for regression builds)
- libmpv (dev package)
- pkg-config and the Linux native loader/thread APIs

Qt Quick must use OpenGL. An unsupported graphics backend or failed native
initialization is reported through player status/error properties, rather than
successful readiness. The measured verification lane uses Qt 6.11.2/libmpv 0.41.
MpvQt is not a build or runtime dependency. Selected integration mechanics are
derived from the pinned upstream described in [UPSTREAM.md](UPSTREAM.md).

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
qml6 examples/AdvancedPlayer.qml
```

## Documentation

| File | Audience |
|------|----------|
| [API.md](API.md) | QML developers (no C++ knowledge required) |
| [FeatureMapping.md](FeatureMapping.md) | Qt/libmpv integration developers |
| [FutureDevelopment.md](FutureDevelopment.md) | Contributors and roadmap planning |

## Project Status

Playback component with media control, track management, video processing,
and scripting interfaces. The owned-backend migration intentionally changes
the C++ base/ABI and generic async API: rebuild C++ consumers and migrate
caller-supplied IDs, blocking reads, and raw `setProperty` calls as described
in [API.md's migration contract](API.md#c-apiabi-and-caller-migration).
Hook continuation IDs remain uint64/quint64 and are separate from positive int
async request IDs. New development is tracked in FutureDevelopment.md.

## License

Original mpvqml code is GPLv3. Derived private-backend material selects
LGPL-3.0-only from upstream's offered license options and retains its notices.
Installed distributions include the notices/license texts and corresponding
module source/build inputs under `share/mpvqml/source`. To rebuild those inputs:

```sh
cmake -S /path/to/share/mpvqml/source -B build -DBUILD_SHARED_LIBS=ON -DBUILD_TEST_MODE=OFF
cmake --build build
```
