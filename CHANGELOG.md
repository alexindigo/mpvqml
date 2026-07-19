# Changelog

All notable changes to this project are documented here. Format loosely
follows [Keep a Changelog](https://keepachangelog.com/en/1.1.0/); versions
follow [Semantic Versioning](https://semver.org/spec/v2.0.0.html).

## [0.1.1] — 2026-07-19

### Fixed

- Packaging: `cmake --install` now ships the actual QML module. Prior to
  this the install rules only exported the C++ headers; the shared
  library `libmpvqml.so`, the QML plugin (`libmpvqmlplugin.so`),
  `qmldir`, and `mpvqml.qmltypes` were built but never installed, so
  distro packages (AUR, custom prefixes) produced a headers-only package
  that could not be `import`ed from QML at runtime. Replaced the ignored
  `INSTALL_DIRECTORY` argument on `qt_add_qml_module` with explicit
  `install(TARGETS)` + `install(DIRECTORY .../MpvQml/)` rules.

## [0.1.0] — 2026-07-19

Initial release. QML component wrapping libmpv via MpvQt for Qt 6.5+
applications, distributed as the `MpvQml` QML module and exposed as a
single `MpvPlayer` element.

### Added

- `MpvPlayer` QQuickItem (extends `MpvAbstractItem`) exposing playback
  as declarative QML properties: `source`, `paused`, `playbackSpeed`,
  `mute`, `volume`, `fillMode`, `screenshotDirectory`, and read-only
  introspection (`videoWidth`, `videoHeight`, `hasVideo`, `position`,
  `duration`, `seeking`, `isBuffering`).
- Track and playlist model exposure: `trackList`, `playlist`,
  `chapterList`, `metadata`, `audioDeviceList`, `currentAudioDevice`.
  Hyphenated mpv keys (e.g. `codec-desc`, `demux-samplerate`) are
  auto-converted to camelCase on the way into QML.
- Video and audio processing controls: `hwdec`, `videoSync`,
  `interpolation`, `tscale`, `userShaders`, `ytdl`, `audioDelay`,
  `subDelay`, `videoZoom`, `videoPan{X,Y}`, `audioPitchCorrection`,
  `subVisibility`, `brightness`, `contrast`. Read-only diagnostics:
  `hwdecCurrent`, `demuxerCacheState`, `lastError`.
- Loop and playlist controls: `loopFile`, `loopPlaylist` with support
  for numeric, `"inf"`, and `"no"` values matching mpv semantics.
- Content loading: `load(url, mode)`, `loadDirectory(path, filters,
  mode, sortOrder)`, `clearPlaylist()`, `stop(keepPlaylist)`.
- Navigation: `seekRelative`, `seekAbsolute`, `next`, `previous`,
  `frameStep`, `frameBackStep`, `markSeekPosition`, `revertSeek`.
- Track composition: `addAudioTrack`, `addVideoTrack`,
  `addSubtitleTrack`, `selectTrack`.
- Filters and property manipulation: `setVideoFilter`, `setAudioFilter`,
  `adjustProperty`, `cycleProperty`, `multiplyProperty`,
  `deleteProperty`.
- Utility surface: `setLogLevel`, `loadConfigFile`, `setProperty` (raw
  pass-through), `commandAsync`, `setRenderParameter`, `hookAdd`,
  `hookContinue`, `toggleAbLoop`, `saveWatchLaterState`,
  `dropPlaybackBuffers`, `screenshot`.
- Scripting and mouse: `runExternalProcess`, `sendScriptMessage`,
  `triggerScriptBinding`, `sendMousePosition`, `sendMouseClick`.
- Isolated helper clients: `MpvLogClient` (log-level routed to a
  `logMessage` signal) and `MpvHookManager` (hook events routed to
  `hookTriggered`), each on its own mpv sub-client.
- Adapter/interface split (`MpvControllerIface` +
  `MpvControllerAdapter`) enabling a `MockMpvController` for unit tests
  without a live libmpv instance.
- Deferred-init pipeline: QML property bindings evaluated before the
  render context is up are recorded in a pending map and flushed on
  `MpvAbstractItem::ready()`, ensuring construction-time bindings take
  effect once mpv is capable of receiving them.
- `MpvError` Q_ENUM mirroring `MPV_ERROR_*` values, cross-checked at
  compile time with `static_assert` to guard against future libmpv
  renumbering.
- C++ test suite (63 tests) covering the setter macros, deferred-init
  reconciliation, property observers, hook and log clients, and the
  full public API surface. Coverage instrumentation available via
  `ENABLE_COVERAGE=ON`.
- Example QML apps: `Player`, `AudioPlayer`, `AmbientScreensaver`,
  `MediaInspector`.
- Documentation: [`API.md`](API.md), [`Architecture.md`](Architecture.md),
  [`FeatureMapping.md`](FeatureMapping.md),
  [`FutureDevelopment.md`](FutureDevelopment.md).

### Requirements

- Qt 6.5 or newer (Core, Quick, Test for the test build).
- libmpv (development package).
- [MpvQt](https://invent.kde.org/libraries/mpvqt) 1.2.0 or newer.

### License

GPLv3.
