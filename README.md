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

- Linux with `/proc/self/fd` and the ELF dynamic loader
- Qt 6 headers and QML modules (Core, Gui, Qml, Quick, OpenGL)
- pkg-config and libmpv headers/library with software-memory rendering

The measured envelope is Linux x86_64, Qt 6.12.0, and mpv/libmpv 0.41.0
(`mpv.pc` 2.5.0). Qt 6.5 is the configure-time minimum for the Qt CMake
API used here, not a verified support floor. Configure compiles and links the
libmpv software-render interface; decoding the installed capture probe checks
the runtime options, decoder and output path. Older dependency combinations
are unverified.

The player needs Qt Quick's OpenGL backend. An unsupported backend or failed native
initialization is reported through player status/error properties, rather than
successful readiness. Frame capture needs neither a window nor OpenGL; its
installed `mpvqml-capture` executable under libexec contains native capture
faults and enforces the deadline. Include it when assembling a runtime prefix.
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

`BUILD_TEST_MODE` defaults to `OFF` and is independent of `BUILD_TESTING`.
Use `-DBUILD_TEST_MODE=ON` for test targets. A debug test build (with optional
coverage) is also available; see [test lanes](tests/README.md):

```
scripts/build-test
scripts/build-test build-debug --coverage
```

The build-tree QML import root is the **build directory** containing `MpvQml/`.
For a prefix install, the root is `<prefix>/<CMAKE_INSTALL_LIBDIR>/qt6/qml`
(for example `/opt/mpvqml/lib/qt6/qml`); add that root to `QML_IMPORT_PATH`
when it is outside Qt's defaults. Do not point at the `MpvQml/` child itself.
`MpvQml 1.0` is the QML URI/version, not the package version or a capture
capability check. Pin candidates by full source commit; for published packages
select a release known to export the needed type and run the installed probe.

### Installed-module regression (Linux)

After building, run from a graphical session as an unprivileged user:

```sh
python3 -B tests/install-convergence.py --build /path/to/build --source-revision "$candidate_commit"
```

Use a production build with `BUILD_TEST_MODE=OFF` to prove a test-free package.
The probe itself needs Python 3, `readelf`, a C++ compiler, pkg-config and Qt
Test. It relocates a DESTDIR installation and denies access to the original
source and build directories while the external CMake consumer is built and
both consumer surfaces are exercised. The no-display QML capture subprocess
must decode the expected initial and seek pixels. The GUI subprocesses read
`mpv-version` using the existing async API. The probe checks the plugin's
relative RUNPATH, mapped installed libraries and the installed libexec helper,
restores permissions
on failure, and leaves logs and a JSON receipt outside both product trees.
Use an isolated checkout with no concurrent builds. Install library/include
directories must be prefix-relative for this proof.

To exercise termination cleanup as well:

```sh
python3 -B tests/test-install-cleanup.py --build /path/to/build
```

This uses the real install/readelf/compile flow, pauses at a controlled consumer
boundary with both trees inaccessible, and sends SIGTERM and SIGHUP. It
checks exit statuses, restored permissions and failure receipts.

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
