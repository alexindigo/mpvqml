# MpvQml Examples

## Requirements

- Qt 6.5+ with Quick, Controls, and Dialogs modules
- MpvQml module installed (build and install the parent project first)

## Running

Use the Qt 6 qml runner:

```bash
qml6 examples/AdvancedPlayer.qml
qml6 examples/AmbientScreensaver.qml
qml6 examples/AudioPlayer.qml
qml6 examples/MediaInspector.qml
```

If the MpvQml module is not found, set the QML import path to the build
or install directory:

```bash
QML_IMPORT_PATH=./build-debug qml6 examples/AdvancedPlayer.qml
QML_IMPORT_PATH=./build-debug qml6 examples/AmbientScreensaver.qml
QML_IMPORT_PATH=./build-debug qml6 examples/AudioPlayer.qml
QML_IMPORT_PATH=./build-debug qml6 examples/MediaInspector.qml
```

## Examples

### AdvancedPlayer.qml

A media player with playback controls, volume adjustment, track selection,
screenshots, seek mark/revert, and drag-and-drop support.

### AmbientScreensaver.qml

A fullscreen image slideshow that cycles through images in a user-selected
directory. Drop a folder to start. Space to pause, arrows to navigate,
ESC to exit.

### AudioPlayer.qml

A focused audio player with seek bar, speed and volume controls, pitch
correction toggle, audio delay adjustment, and metadata display.

### MediaInspector.qml

A technical inspection tool showing track listings (video, audio, subtitle),
video resolution and hardware decoder info, brightness/contrast/zoom/pan
adjustments, hwdec/tscale/interpolation configuration, and per-track
selection. Includes frame step controls and screenshot capture.
