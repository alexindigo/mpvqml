# Feature Mapping: libmpv / MpvQt -> MpvPlayer

How the underlying libraries map to the `MpvPlayer` QML API.
For features not yet implemented, see `FutureDevelopment.md`.

| Status | Meaning |
|---|---|
| OK | Fully implemented and usable |
| partial | Partially implemented or pending completion |
| no | Not exposed (rationale in the description) |
| delegated | Handled by an underlying class, not our code |

---

## Coverage Summary

| libmpv area | | mpvqml support |
|---|---|---|
| Core lifecycle | delegated | Handled by MpvAbstractItem |
| Generic property set | OK | `setProperty()` Q_INVOKABLE |
| Config file loading | OK | `loadConfigFile()` |
| Commands (sync) | OK | `command()` via MpvAbstractItem |
| Commands (async) | OK | `commandAsync()` |
| Property write (typed) | OK | MPV_SETTER macros + Q_PROPERTY bindings |
| Property read (reactive) | OK | Observers + Q_PROPERTY |
| Property read (ad-hoc) | no | Reactive pattern covers it |
| Property observe / unobserve | partial | observe internal / unobserve not exposed |
| Property async read/write | no | Observer pattern preferred |
| Event loop | delegated | Handled by MpvController |
| Event filtering | no | Not needed |
| Log messages | OK | `setLogLevel()` via `MpvLogClient` |
| Hooks | partial | `hookAdd`/`hookContinue` exist |
| Render context | delegated | Handled by MpvAbstractItem |
| Render parameters | partial | Stored, not applied |
| Stream API | no | Not needed -- embedded player |
| Time utilities | no | Not needed |
| Error codes | OK | `mpvError` enum + `errorOccurred` signal |

---

## Core Lifecycle

### `mpv_create()`, `mpv_initialize()`, `mpv_destroy()`

**Status: delegated**

`MpvAbstractItem` (parent of `MpvPlayer`) manages the core lifecycle:
- `MpvController` wraps `mpv_handle*` creation and destruction
- `MpvController::init()` is called in `MpvAbstractItem` constructor via
  `BlockingQueuedConnection`. It runs `mpv_create()` then immediately
  `mpv_initialize()` with no gap for options. Pre-init-only options like
  `config-dir` are unsupported — see FutureDevelopment.md.
- `mpv_destroy()` is called when the last `MpvController` reference is released

### `mpv_create_client()`, `mpv_create_weak_client()` -- Multi-Client

**Status: no**

Creates additional `mpv_handle*` instances sharing the same playback core.
Each client has its own event queue and property observer namespace.

**Why we don't expose it:** Qt signals already fan out to any number of QML
bindings -- a single `positionChanged` connects to a slider, waveform, and time
display without extra handles. Multi-client would be useful only for C++
extensions that need property observations outside `setupPropertyObservers()`
without modifying the player class. We consider this too niche to wrap.

(`MpvLogClient` and `MpvHookManager` use `mpv_create_client()`
internally, but that's an implementation detail, not user-facing API.)

### `mpv_client_name()`, `mpv_client_id()`

**Status: no**

Returns the client tag string (passed to `mpv_create_client`) or numeric ID.

**Why we don't expose it:** Internal plumbing -- users don't need to identify
their mpv handle by name or ID.

---

## Configuration & Options

### `mpv_set_property_string()` -- Runtime Property Set

**Status: OK**

`setProperty(name, value)` Q_INVOKABLE on `MpvPlayer` calls
`mpv_set_property_string()` directly via `rawMpv()`. Works at any
point after `mpv_create()`, including after `mpv_initialize()`. Use
for options that have a property equivalent (e.g. `keep-open-pause`,
`image-display-duration`). For truly pre-init-only options (`config-dir`,
`load-scripts`, `player-operation-mode`), see FutureDevelopment.md.

### `mpv_load_config_file()` -- Config File Loading

**Status: OK**

`loadConfigFile(path)` Q_INVOKABLE on `MpvPlayer` calls
`mpv_load_config_file()` via `rawMpv()`. Gives QML developers an
escape hatch to apply user config without C++ changes.

Introduced in: `mpvplayer.cpp:764`

---

## Commands

### `mpv_command()` -- Synchronous Command

**Status: OK**

`MpvPlayer` inherits `MpvAbstractItem::command(QStringList)` as a
Q_INVOKABLE. Additionally, `MpvPlayer` calls `m_ctrl->command(params)`
internally for imperative actions (load, seek, filter, etc.) and exposes
convenience wrappers:

- `load(url, mode)`, `loadDirectory(path, ...)`
- `seekRelative(dt)`, `seekAbsolute(t)`, `frameStep()`, `frameBackStep()`
- `next()`, `previous()`, `stop()`
- `setVideoFilter()`, `setAudioFilter()`
- `addAudioTrack()`, `addVideoTrack()`, `addSubtitleTrack()`
- `screenshot()`, `toggleAbLoop()`, `saveWatchLaterState()`

### `mpv_command_async()` -- Asynchronous Command

**Status: OK**

`commandAsync(QStringList, int id)` Q_INVOKABLE delegates to
`m_ctrl->commandAsync()`. Results arrive via
`asyncCommandFinished(int id, int error)` signal.

### `mpv_command_node()`, `mpv_command_ret()`, `mpv_command_string()`

**Status: no**

These variants offer structured argument passing with `mpv_node`,
return-value capture, or input.conf-style parsing.

**Why we don't expose them:** The `QStringList`-based `command()` covers
99% of use cases in QML. Structured arguments can be built with
`commandAsync()` + property observers for the response. If `mpv_command_ret()`
is ever needed (e.g. for libmpv calls that return structured metadata),
it would be a small addition.

### `mpv_abort_async_command()`, `mpv_wait_async_requests()`

**Status: no**

`mpv_abort_async_command()` cancels a pending async request by its
userdata ID. `mpv_wait_async_requests()` blocks until all async requests
complete -- primarily useful in shutdown paths.

**Why we don't expose them:** Aborting is rare in QML usage; shutdown
is handled by the event loop + destruction of the mpv handle.

---

## Properties

### `mpv_set_property()` -- Property Write

**Status: OK**

Property writes flow through the `MpvControllerIface::setProperty()` virtual
method. The `MPV_SETTER_*` macros in `mpvplayer.cpp` implement the common
pattern:

```cpp
// Expands to: if (changed) { m_ctrl->setProperty("volume", val); emit ... }
MPV_SETTER_DOUBLE(volume, volume, "volume")
```

QML bindings that set `Q_PROPERTY` values before the first render are queued
and applied once `mpv_initialize()` completes, thanks to mpv's pre-init
property support (since 0.21.0).

### `mpv_set_property_async()`, `mpv_get_property_async()`

**Status: no**

`MpvController` wraps these as Q_SLOTs, but `MpvPlayer` never calls them.

**Why we don't expose them:** The synchronous `setProperty()` is fast enough
for QML property bindings (they run on the Qt main thread, same as mpv's
event loop). Async property operations add complexity without measurable
benefit for the project's use cases.

### `mpv_get_property()` -- Direct Property Read

**Status: no**

Read a property by name at any time, not just via observer callbacks.

**Decision:** No generic `getProperty()`. The Qt property system + observer
registration is the right pattern for QML -- gives reactive bindings that
update automatically, not static snapshots. Add `Q_PROPERTY` entries as new
properties are needed (e.g. `audioBitrate`, `videoCodec`). This keeps the
API declarative and discoverable by QML developers:

```qml
Text { text: "Bitrate: " + player.audioBitrate + " kbps" }
```

Currently observed properties: `video-params/w`, `video-params/h`, `height`,
`width`, `seekable`, `playback-time`, `duration`, `paused-for-cache`,
`eof-reached`, `track-list`, `playlist`, `chapter-list`, `filtered-metadata`,
`audio-device-list`, `current-audio-device`, `hwdec-current`,
`demuxer-cache-state`, `audio-bitrate`, `video-bitrate`.

### `mpv_observe_property()`, `mpv_unobserve_property()`

**Status: partial**

`observeProperty()` is called in `setupPropertyObservers()` (mpvplayer.cpp:93)
for every reactive property. The dispatch table routes incoming property
changes to the right handler via `m_propertyHandlers[name]`.

`unobserveProperty()` exists on `MpvController` but is never called. In
practice this is harmless -- observers are tied to the mpv_handle* lifetime
and cleaned up on destroy. Adding `unobserveProperty()` to the interface
would be straightforward if needed (e.g. for dynamic property subscriptions).

### `mpv_get_property_string()`, `mpv_get_property_osd_string()`

**Status: no**

Convenience wrappers that return string representations.

**Why we don't expose them:** OSD-formatted strings are a terminal-UI concept
that doesn't apply to QML. Use the Q_PROPERTY for typed values.

---

## Events & Signals

### Event Loop -- `mpv_wait_event()`, `mpv_wakeup()`

**Status: delegated**

`MpvController` owns the event loop, calling `mpv_wait_event()` with a
timeout on a dedicated thread. `mpv_wakeup()` is used internally for
interrupting the wait. `MpvPlayer` does not interact with the event loop
directly -- all event-driven data arrives through Qt signals.

### `mpv_request_event()` -- Event Filtering

**Status: no**

Controls which mpv event types get delivered to the client handle.

**Why we don't expose it:** The player already ignores events that
aren't registered in `setupPropertyObservers()`. Filtering would add
API surface for something internal -- QML developers shouldn't need to
know about mpv event types to build a player.

### `mpv_event_name()`, `mpv_event_to_node()`

**Status: no**

Debugging utilities for inspecting events.

**Why we don't expose them:** The player uses typed signal parameters
(e.g. `endFile(QString reason)`), not raw event structs. These are
redundant in a Qt application.

### MpvController Signals

`MpvController` emits these signals; they are forwarded to `MpvPlayer`
via `MpvControllerAdapter`:

| Signal | Forwarded as | Extra logic |
|---|---|---|
| `fileStarted()` | `MpvPlayer::fileStarted()` | -- |
| `fileLoaded()` | `MpvPlayer::fileLoaded()` | -- |
| `endFile(QString)` | `MpvPlayer::endFile(QString)` | Also emits `errorOccurred` if reason is `"error"` |
| `videoReconfig()` | `MpvPlayer::videoReconfig()` | -- |
| `asyncReply(QVariant, mpv_event)` | `MpvPlayer::asyncCommandFinished(int, int)` | Translates `mpv_event` to simplified `(id, error)` |
| `propertyChanged(QString, QVariant)` | Dispatch table -> property handlers | Routes to `m_propertyHandlers[name]` |

### MpvAbstractItem `ready()` Signal

**Status: no**

`MpvAbstractItem::ready()` fires when the mpv core is fully initialized.

**Why we don't connect it:** `MpvPlayer` uses `m_rendererInitialized` flag
+ deferred action queue instead. Properties set before `ready()` are queued
and applied once the renderer starts. This gives the same semantics without
requiring QML to wait for a signal before setting initial properties.

---

## Logging & Diagnostics

### `mpv_request_log_messages()` -- Log Subscription

**Status: OK**

`setLogLevel(level)` Q_INVOKABLE on `MpvPlayer` delegates to
`MpvLogClient` which creates a separate mpv client handle for log events
with dedicated wakeup callback. `logMessage(facility, level, text)` signal
emitted when messages arrive. Default: `"no"` (off).

Levels: `"no"` (off), `"fatal"`, `"error"`, `"warn"`, `"info"`, `"v"`, `"v"`, `"debug"`, `"trace"`.

```qml
Player {
    onLogMessage: (facility, level, text) => {
        if (level === "error")
            console.error(`[${facility}] ${text}`)
    }
}
```

---

## Hooks

See `FutureDevelopment.md` for full investigation.

### `mpv_hook_add()`, `mpv_hook_continue()`

**Status: partial**

The low-level API is present:
- `hookAdd(name, priority)` / `hookContinue(id)` Q_INVOKABLE methods
- `hookTriggered(name, id)` signal
- Backed by `MpvHookManager` (separate mpv client handle + wakeup callback)

Open questions remain around timeout safety, priority ordering, and
real-world validation.

---

## Render

### `mpv_render_context_create()`, `mpv_render_context_destroy()`

**Status: delegated**

The `MpvAbstractItem::Renderer` class (internal to the prebuilt MpvQt
library) manages render context creation and teardown.

### `mpv_render_context_set_parameter()`

**Status: partial**

`setRenderParameter(name, value)` Q_INVOKABLE stores pending parameters
in `m_pendingRenderParams`. Application to `mpv_render_context*` is
deferred -- requires access to the render context from
`MpvAbstractItem::Renderer` (internal to the prebuilt MpvQt library).

### `mpv_render_context_get_info()`

**Status: no**

Returns renderer metadata (VO performance, target time, etc.).

**Why we don't expose it:** No concrete use case in QML yet. Would be
relevant for advanced HDR or performance monitoring dashboards.

### `mpv_render_context_report_swap()`

**Status: no**

Called by `MpvAbstractItem` internally after each frame swap for A/V sync.

**Why we don't call it directly:** The parent class handles swap reporting.

### Other Render Functions

| Function | Status |
|---|---|
| `mpv_render_context_update()` | Handled by MpvAbstractItem |
| `mpv_render_context_render()` | Handled by MpvAbstractItem |
| `mpv_opengl_cb_*` (legacy) | Not used (we use `MPV_RENDER_API_TYPE_OPENGL`) |

---

## Stream API

### `mpv_stream_cb_add()`

**Status: no**

Allows registering custom stream protocols (e.g. `myprotocol://`) in C.

**Why we don't expose it:** This is a C-level extension mechanism for
custom I/O backends. QML applications can use Qt's network stack
(`QNetworkAccessManager`) and feed URLs to the player. Custom stream
protocols belong in a C++ plugin, not in the QML player API.

---

## Time Utilities

### `mpv_get_time_us()`, `mpv_get_time_ns()`

**Status: no**

Return mpv's internal clock for custom event loops and renderers.
Not related to playback position.

**Why we don't expose it:** Qt provides `QElapsedTimer` and frame timing
infrastructure. QML already has `player.position` for playback time and
`player.duration` for total length. mpv's internal clock is irrelevant
for QML application code -- it's designed for C-level event loop integration.

---

## Error Handling

### `mpv_error_string()` -> `mpvError` enum

**Status: OK**

- `errorOccurred(int code, QString message)` signal -- fires on
  `endFile("error")` and can be called imperatively via `emitError(code, msg)`
- `lastError` Q_PROPERTY -- `QVariantMap { code, message }` updated on each error
- `mpvError` Q_GADGET enum with all 21 mpv error codes registered to QML
  via `QML_NAMED_ELEMENT(mpvError)` -- enables `code === mpvError.LoadingFailed`

Note: return values from `command()` / `setProperty()` are still unchecked
at the `MpvPlayer` level. The `endFile("error")` path is the only automatic
error trigger. Direct callers who want error detection should check return
codes themselves, or wrap the call in `try/catch` in QML.

```qml
player.errorOccurred.connect((code, message) => {
    if (code === mpvError.LoadingFailed)
        statusBar.text = "Failed to load: " + message
})
```

---

## MpvQt-Specific: Unused MpvController & MpvAbstractItem Features

### `MpvController::unobserveProperty()`

**Status: no**

`MpvController` provides a Q_SLOT to unregister a property observer.
`MpvPlayer` never calls it.

### `MpvController::getPropertyAsync()`, `MpvController::setPropertyAsync()`

**Status: no**

`MpvController` wraps the async property read/write as Q_SLOTs.
Not used -- the synchronous variants + observer pattern cover all needs.

### `MpvController::mpvEvents()`, `MpvController::eventHandler()`

**Status: no**

Expose the raw mpv event queue and event handler. Used internally by
`MpvController`.

### `MpvController::getError()`

**Status: no**

Static helper that converts mpv error codes to human-readable strings.
`MpvPlayer` uses `mpvError` QML enum instead.

### `MpvAbstractItem::setPropertyBlocking()`, `MpvAbstractItem::commandBlocking()`

**Status: no**

Blocking variants that wait for the mpv core to process the request.
`MpvPlayer` uses the non-blocking variants, which queue the operation
and return immediately. Blocking would freeze the UI thread.

### `MpvAbstractItem::expandText()`

**Status: no**

Expands mpv property format strings (e.g. `${duration}`) using the
current playback state.

**Why we don't expose it:** The underlying `expand-text` mpv property
is queryable via a custom command. No QML use case has required it yet.

### `MpvAbstractItem::requestUpdateFromRenderer()`

**Status: no**

Forces a renderer update cycle. Part of MpvAbstractItem's internal
rendering coordination.

---

## Qt Property Reference

All `Q_PROPERTY` entries on `MpvPlayer`, grouped by category:

### Core controls
`source`, `paused`, `playbackSpeed`, `mute`, `volume`, `fillMode`, `screenshotDirectory`

### Read-only introspection
`videoWidth`, `videoHeight`, `hasVideo`, `position`, `duration`, `seeking`, `isBuffering`

### Loop
`loopFile`, `loopPlaylist`

### Subsystem state
`trackList`, `playlist`, `chapterList`, `metadata`, `audioDeviceList`, `currentAudioDevice`

### Video processing
`hwdec`, `videoSync`, `interpolation`, `tscale`, `userShaders`, `videoZoom`, `videoPanX`, `videoPanY`

### Audio
`audioDelay`, `audioPitchCorrection`, `subDelay`, `subVisibility`

### Image adjustment
`brightness`, `contrast`

### Diagnostics
`hwdecCurrent`, `demuxerCacheState`, `lastError`

---

## Architecture Summary

```
QML
 |  Q_PROPERTY bindings, signal handlers, Q_INVOKABLE calls
 v
MpvPlayer  --- owns --- MpvLogClient, MpvHookManager
 |                          (separate mpv_handle* for log/hook events)
 |  delegates to
 v
MpvControllerIface  (abstract interface)
 |  ^                     ^
 |  |                     |
 |  MpvControllerAdapter  MockMpvController
 |  (production)          (unit tests)
 |  |
 |  wraps
 v
MpvController  (MpvQt library)
 |  wraps
 v
mpv_handle *   (libmpv C API)
```
