# Changelog

All notable changes to this project are documented here. Format loosely
follows [Keep a Changelog](https://keepachangelog.com/en/1.1.0/); versions
follow [Semantic Versioning](https://semver.org/spec/v2.0.0.html).

## [0.2.0] — 2026-10-29

### Changed (breaking)

- `MpvPlayer` now derives directly from `QQuickFramebufferObject`; the MpvQt
  `MpvAbstractItem` base is gone. Rebuild C++ consumers against the new headers
  and `libmpvqml.so.0.2` ABI.
- `commandAsync(params)` returns an allocated positive request ID. Negative
  returns reject immediately without a completion; caller-supplied IDs are
  removed. `asyncCommandFinished(id, error, result)` carries the owned result.
- Raw libmpv `setProperty(name, value)` and blocking generic reads are removed.
  Use `setMpvPropertyAsync` / `readMpvPropertyAsync` and their distinct
  `mpvPropertySetFinished` / `mpvPropertyReadFinished` signals. Typed media
  properties remain available.
- Hook IDs (`hookTriggered`, `hookContinue`) are `quint64`, separate from
  allocated async request IDs.
- MpvQt is no longer a dependency. The backend uses Qt 6 Core/Gui/Quick/OpenGL
  and libmpv directly; the QML module remains `MpvQml` version 1.0.

### Added

- Observable core/render lifecycle: `coreAvailable`, `initializationError`,
  `initializationErrorString`, `rendererReady`, `renderError`,
  `renderErrorString`; `initializationFailed`, `renderingFailed` and
  `operationFailed` signals; idempotent `shutdown()`.
- Native fault, lifetime, renderer and installed-consumer regression coverage,
  including deterministic libmpv scaler-LUT poisoning.

### Fixed

- Core creation/initialization and render-context failures no longer abort or
  crash the process. Unsupported Qt Quick graphics APIs report render failure.
- Native redraw callbacks no longer access the player item from libmpv threads.
- Converted command/property nodes are released with matching ownership, and
  flag properties use libmpv's integer representation.
- Reentrant log-level changes and player deletion with live log/hook clients
  no longer wedge delivery or helper teardown.
- Player/window destruction no longer joins outstanding screenshot file I/O
  or final native core termination. Renderer recreation can restore the same
  playlist entry, position and pause state without overriding newer intent.
- A guarded GL upload boundary handles uninitialized scaler-LUT padding in
  libmpv 0.41.0 that could otherwise cause intermittent persistent black video.
  See `UPSTREAM.md` for the workaround's scope and removal condition.
- The installed QML plugin uses a prefix-relative RUNPATH, without build-tree
  paths; relocated QML and public-header consumers are tested independently.
- Examples display media-derived text as plain text, preventing embedded
  markup from triggering image URL fetches. The API documents untrusted text.
- Native and QML filesystem fixtures use private temporary directories,
  preventing predictable-path collisions and symlink-followed truncation.

### License

- Derived private-backend material from MpvQt v1.2.0 selects LGPL-3.0-only
  (see `UPSTREAM.md`); original mpvqml code remains GPLv3. Distributions include
  the provenance, license text and corresponding module source/build inputs.

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
