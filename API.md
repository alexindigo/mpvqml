# MpvPlayer QML API Reference

A media player component backed by libmpv. Import with `import MpvQml`
and use like any other Qt Quick item.

```qml
import MpvQml

MpvPlayer {
    id: player
    anchors.fill: parent
    source: "video.mp4"
    paused: false
    volume: 75
}
```

---

## Properties

### Media source

| Property | Type | Description |
|---|---|---|
| `source` | string | URL or file path of the current media. Setting it loads and starts playback. Assigning the same value is a no-op; call `load(url)` to force a reload. |
| `loopFile` | var | Loop mode for the current file. `"no"` (default), `"inf"`, or a number. |
| `loopPlaylist` | var | Loop mode for the playlist. Same values as `loopFile`. |

### Playback state

| Property | Type | Description |
|---|---|---|
| `paused` | bool | Pause or resume playback. |
| `position` | double | Current playback position in seconds. Read-only. |
| `duration` | double | Total duration of the current media in seconds. Read-only. |
| `playbackSpeed` | double | Playback speed multiplier (0.01 -- 100.0, default 1.0). |
| `seeking` | bool | True while a seek operation is in progress. Read-only. |
| `isBuffering` | bool | True while the player is buffering. Read-only. |

### Volume and audio

| Property | Type | Description |
|---|---|---|
| `volume` | double | Audio volume (0.0 -- 150.0, default 100.0). |
| `mute` | bool | Mute audio output. |
| `audioDelay` | double | Audio delay in seconds (negative = audio plays earlier). |
| `audioPitchCorrection` | bool | Preserve pitch when changing playback speed (default true). |
| `currentAudioDevice` | string | Name of the active audio output device. |
| `audioDeviceList` | var | List of available audio devices. Read-only. |

### Video display

| Property | Type | Description |
|---|---|---|
| `fillMode` | string | How video fits the item: `"preserveAspectFit"`, `"preserveAspectCrop"`, `"stretch"`. |
| `videoWidth` | int | Width of the video stream in pixels. Read-only. |
| `videoHeight` | int | Height of the video stream in pixels. Read-only. |
| `hasVideo` | bool | True if the current media has a video stream. Read-only. |
| `brightness` | double | Brightness adjustment (-100.0 -- 100.0, default 0.0). |
| `contrast` | double | Contrast adjustment (-100.0 -- 100.0, default 0.0). |
| `videoZoom` | double | Video zoom factor (default 1.0). |
| `videoPanX` | double | Horizontal pan offset (default 0.0). |
| `videoPanY` | double | Vertical pan offset (default 0.0). |

### Subtitles

| Property | Type | Description |
|---|---|---|
| `subDelay` | double | Subtitle delay in seconds. |
| `subVisibility` | bool | Show or hide subtitles (default true). |

### Tracks and playlist

| Property | Type | Description |
|---|---|---|
| `trackList` | var | List of track objects from mpv's `track-list` property. Read-only. |
| `playlist` | var | List of playlist entries from mpv's `playlist` property. Read-only. |
| `chapterList` | var | Chapter start times in seconds. Read-only. |
| `metadata`| var | Media metadata key-value pairs. Read-only. |

### Video processing

| Property | Type | Description |
|---|---|---|
| `hwdec` | string | Hardware decoding method: `"auto"`, `"vaapi"`, `"cuda"`, `"no"`, etc. |
| `hwdecCurrent` | string | Currently active hardware decoder. Read-only. |
| `videoSync` | string | Video sync method: `"audio"`, `"display-resample"`, `"display-adrop"`, etc. |
| `interpolation` | bool | Enable motion-interpolated video playback. |
| `tscale` | string | Temporal scaler for interpolation: `"mitchell"`, `"oversample"`, etc. |
| `userShaders` | string | Path to a GLSL shader file for custom video processing. |

### Network

| Property | Type | Description |
|---|---|---|
| `ytdl` | bool | Enable youtube-dl/yt-dlp integration for URL resolution (default true). |

### Screenshots

| Property | Type | Description |
|---|---|---|
| `screenshotDirectory` | string | Directory where screenshots are saved. |

### Diagnostics

| Property | Type | Description |
|---|---|---|
| `lastError` | var | Most recent error: `{ code: int, message: string }`. Read-only. |
| `demuxerCacheState` | var | Demuxer cache statistics. Read-only. |

### Backend lifecycle

| Property | Type | Description |
|---|---|---|
| `coreAvailable` | bool | Native initialization succeeded and the player accepts operations. |
| `initializationError` / `initializationErrorString` | int / string | Immutable result of the single core-initialization attempt. |
| `rendererReady` | bool | The current renderer has a native context and valid FBO. False before rendering and during context loss. |
| `renderError` / `renderErrorString` | int / string | Current render-attempt failure, including unsupported Qt graphics backends. |

`ready()` marks successful renderer readiness, including successful recreation;
core availability alone does not imply visible video. Core/render failures emit
`initializationFailed(code, message)` / `renderingFailed(code, message)` and
update `lastError`. Applications can retain a solid background and independent
controls. Core admission can fail recoverably when all 32 retirement leases are
occupied by live or still-retiring cores.

While the core is available, typed properties and source/load intent survive a
render failure and apply on the next successful renderer generation. Loads,
directory loads, track selections and mouse positions wait for readiness;
stop/source/navigation/shutdown still cancel superseded deferred actions.
Explicit async operations and core-only convenience commands remain admitted
through the core and report their native completion/error without automatic retry.

---

## Methods

### Loading media

```qml
// Load a single file or URL
player.load("video.mp4")
player.load("https://example.com/stream.m3u8", "append")

// Replace playlist with all mp4 and mkv files, sorted by name
player.loadDirectory("/path/to/videos", ["*.mp4", "*.mkv"], "replace", "name")
```

| Method | Description |
|---|---|
| `load(url, mode)` | Load a media file or URL. `mode`: `"replace"` (default), `"append"`, `"append-play"`. |
| `loadDirectory(path, fileFilters, mode, sortOrder)` | Scan a directory and load matching files. `sortOrder`: `"name"`, `"date"`, `"size"`. |
| `clearPlaylist()` | Remove all items from the playlist. |
| `stop(keepPlaylist)` | Stop playback. `keepPlaylist`: `false` clears the playlist, `true` keeps it. |

### Playback control

```qml
player.paused = true       // pause
player.paused = false      // resume
player.stop(false)         // stop and clear playlist
player.seekAbsolute(120.0) // seek to 2 minutes
player.seekRelative(-10.0) // jump back 10 seconds
player.frameStep()         // advance one frame
player.frameBackStep()     // go back one frame
player.next()              // next playlist item
player.previous()          // previous playlist item
```

| Method | Description |
|---|---|
| `seekRelative(seconds)` | Seek relative to current position. Negative goes backward. |
| `seekAbsolute(timestamp)` | Seek to an absolute time in seconds. |
| `next()` | Jump to the next playlist item. |
| `previous()` | Jump to the previous playlist item. |
| `frameStep()` | Advance by one video frame. |
| `frameBackStep()` | Go back by one video frame. |
| `markSeekPosition()` | Remember the current position for later reversion. |
| `revertSeek()` | Jump back to the position saved by `markSeekPosition()`. |

### Tracks

```qml
player.addAudioTrack("audio_french.mka")
player.addSubtitleTrack("subtitles.srt")
player.selectTrack("subtitle", 1)     // select subtitle track by ID
player.selectTrack("subtitle", 0)     // disable subtitles
```

| Method | Description |
|---|---|
| `addAudioTrack(url)` | Append an external audio track. |
| `addVideoTrack(url)` | Append an external video track. |
| `addSubtitleTrack(url)` | Append an external subtitle file. |
| `selectTrack(type, id)` | Select a track by type (`"audio"`, `"video"`, `"sub"`, `"subtitle"`) and ID. ID `0` disables the track type. |

### Video and audio filters

```qml
player.setVideoFilter("add", "gradfun")       // add a filter
player.setAudioFilter("add", "loudnorm=i=-16") // add an audio filter
player.setVideoFilter("remove", "gradfun")     // remove a filter
```

| Method | Description |
|---|---|
| `setVideoFilter(op, val)` | Modify video filters. `op`: `"add"`, `"remove"`, `"toggle"`, `"append"`, `"clr"`. |
| `setAudioFilter(op, val)` | Modify audio filters. Same operations. |

### Property manipulation

```qml
player.adjustProperty("volume", -5.0)       // increment by -5
player.multiplyProperty("speed", 1.2)       // multiply by 1.2
player.cycleProperty("sub-visibility", "up") // cycle through values
player.deleteProperty("volume")              // reset to default
```

| Method | Description |
|---|---|
| `adjustProperty(name, value)` | Add `value` to the given mpv property. |
| `multiplyProperty(name, factor)` | Multiply the given mpv property by `factor`. |
| `cycleProperty(name, direction)` | Cycle a property value `"up"` or `"down"`. |
| `deleteProperty(name)` | Reset a property to its default value. |

### Screenshots

```qml
player.screenshot("video")   // capture with subtitles
player.screenshot("window")  // capture with OSD and subtitles
```

| Method | Description |
|---|---|
| `screenshot(mode)` | Capture a screenshot. `mode`: `"video"` (default), `"window"`, `"single"`. |

### Ab-loop

```qml
player.toggleAbLoop()          // set A/B loop points
player.saveWatchLaterState()   // save resume position
player.dropPlaybackBuffers()   // clear internal buffers
```

| Method | Description |
|---|---|
| `toggleAbLoop()` | Cycle through A/B loop point states (none -> A -> B -> loop). |
| `saveWatchLaterState()` | Save the current playback position for later resumption. |
| `dropPlaybackBuffers()` | Clear all buffered data. |

### Scripting and external processes

```qml
player.runExternalProcess(["echo", "hello"])
player.sendScriptMessage(["script-binding", "command"])
player.triggerScriptBinding("cycle pause")
```

| Method | Description |
|---|---|
| `runExternalProcess(arguments)` | Run an external command. |
| `sendScriptMessage(arguments)` | Send a message to a running Lua script. |
| `triggerScriptBinding(name)` | Trigger a named input binding. |

### Mouse interaction

```qml
player.sendMousePosition(320, 240)
player.sendMouseClick(1, true)   // left button press
player.sendMouseClick(1, false)  // left button release
```

| Method | Description |
|---|---|
| `sendMousePosition(x, y)` | Report mouse position to mpv's internal mouse tracking. |
| `sendMouseClick(button, isPress)` | Report a mouse click. `button` uses mpv ordering: 1 (left), 2 (middle), 3 (right). This does not match `Qt::MouseButton`; remap when forwarding a Qt event. |

### Logging

```qml
player.setLogLevel("info")   // enable log messages at info level
player.setLogLevel("no")     // disable logging
```

| Method | Description |
|---|---|
| `setLogLevel(level)` | Set the log verbosity. Levels: `"no"` (off), `"fatal"`, `"error"`, `"warn"`, `"info"`, `"debug"`, `"trace"`. |

### Advanced

```qml
player.setMpvPropertyAsync("keep-open-pause", true)
player.loadConfigFile("/path/to/mpv.conf")
var id = player.commandAsync(["loadfile", "video.mp4", "append"])
var readId = player.readMpvPropertyAsync("mpv-version")
player.setRenderParameter("ambient-light", 64)
player.hookAdd("on_load", 0)
player.hookContinue(hookId)
```

| Method | Description |
|---|---|
| `setMpvPropertyAsync(name, value)` | Queue a generic property write through the controller interface; completes with `mpvPropertySetFinished(id, error)`. |
| `readMpvPropertyAsync(name)` | Queue a generic read; completes with `mpvPropertyReadFinished(id, error, value)`. |
| `loadConfigFile(path)` | Queue configuration loading on the worker; failures use the ordinary operation/error signals. Pre-init-only options remain unsupported. |
| `commandAsync(params)` | Allocate a request ID and queue a native command; completes with `asyncCommandFinished(id, error, result)`. |
| `setRenderParameter(name, value)` | Coalesce the supported `"ambient-light"` nonnegative integer parameter and apply it on the rendering thread. Unsupported names/types report errors. |
| `hookAdd(name, priority)` | Register a hook handler. Lower priority runs first. |
| `hookContinue(id)` | Resume playback with the uint64/quint64 ID delivered by `hookTriggered`, not an async request ID. Must be called or mpv stays blocked. |
| `shutdown()` | Stop admissions/sampling interest, detach helpers, cancel deferred loads and settle admitted operations while the receiver exists. Native resources retire asynchronously. Idempotent. |

Async returns are **positive allocated int IDs**, shared monotonically across
commands and property reads/writes for this player. They are never wrapped or
reused. A positive ID means local admission, not native acceptance or completion.
Every admitted operation gets exactly one matching terminal signal after the
call returns, including later worker/native rejection. A negative return is an
immediate rejection and gets no terminal signal; zero is never a request ID.
Results contain owned Qt values, not borrowed libmpv event storage. Native
64-bit correlation tokens are private. Caller-supplied IDs and inherited
blocking read/command methods are removed; there is no raw `setProperty` API.

Typed setters and convenience actions use the same queued interface. Their
errors use `operationFailed(operation, code, message)` / `errorOccurred`, and
their completions are not reported as caller `asyncCommandFinished` requests.
Screenshot convenience submission is asynchronous; shutdown does not promise
to cancel an already-running file write.

---

## Signals

```qml
onFileLoaded: console.log("file loaded")
onFileStarted: console.log("file started")
onEndFile: function(reason) { console.log("playback ended:", reason) }
onVideoReconfig: console.log("video reconfigured")
```

| Signal | Arguments | Description |
|---|---|---|
| `playbackEnded()` | -- | Playback reached the end of the file. |
| `fileStarted()` | -- | A new file has started loading. |
| `fileLoaded()` | -- | A file has finished loading and is ready to play. |
| `endFile(reason)` | `reason`: string | Playback ended. `reason` is `"eof"`, `"stop"`, `"error"`, etc. |
| `videoReconfig()` | -- | Video parameters changed (resolution, aspect ratio, etc.). |

```qml
onErrorOccurred: function(code, msg) {
    console.error("error", code, msg)
}
onLogMessage: function(facility, level, text) {
    if (level === "error") console.error(facility, text)
}
```

| Signal | Arguments | Description |
|---|---|---|
| `errorOccurred(code, message)` | `code`: int, `message`: string | A playback error occurred. |
| `logMessage(facility, level, text)` | `facility`: string, `level`: string, `text`: string | A log message from mpv. Enable with `setLogLevel()`. |

```qml
onAsyncCommandFinished: function(id, error, result) {
    console.log("async command", id, "completed with error", error)
}
onHookTriggered: function(name, id) {
    console.log("hook", name, "fired, id =", id)
    player.hookContinue(id)
}
```

| Signal | Arguments | Description |
|---|---|---|
| `asyncCommandFinished(id, error, result)` | `id`: int, `error`: int, `result`: var | A caller command's terminal completion; error is 0 on success, negative on failure. |
| `mpvPropertySetFinished(id, error)` | `id`: int, `error`: int | A caller property write completed. |
| `mpvPropertyReadFinished(id, error, value)` | `id`: int, `error`: int, `value`: var | A caller read completed with an owned value, or an invalid value on failure. |
| `hookTriggered(name, id)` | `name`: string, `id`: uint64/quint64 | A hook point was reached. Pass this same hook ID to `hookContinue(id)` to resume; it is not a command/read/write request ID. |

Each writable property also has a matching `Changed` signal:
`onSourceChanged`, `onPausedChanged`, `onVolumeChanged`, etc.

---

## Error handling

The `mpvError` type is available in QML and provides named error codes:

```qml
import MpvQml

player.onErrorOccurred: function(code, message) {
    switch (code) {
    case mpvError.LoadingFailed:
        statusBar.text = "Failed to load: " + message
        break
    case mpvError.Command:
        statusBar.text = "Command rejected: " + message
        break
    }
}
```

See `mpvError` values: `Success`, `EventQueueFull`, `NoMem`,
`Uninitialized`, `InvalidParameter`, `OptionNotFound`, `OptionFormat`,
`OptionError`, `PropertyNotFound`, `PropertyFormat`,
`PropertyUnavailable`, `PropertyError`, `Command`, `LoadingFailed`,
`AoInitFailed`, `VoInitFailed`, `NothingToPlay`, `UnknownFormat`,
`Unsupported`, `NotImplemented`, `Generic`.

---

## Track and playlist data

`trackList` and `playlist` return mpv data as a list of objects with converted
keys. In both lists, mpv's `id` is exposed as `trackId`. Hyphenated keys from
mpv (e.g. `codec-desc`) are automatically converted to camelCase (`codecDesc`).

Common fields in each track object:

| Property | Type | Description |
|---|---|---|
| `trackId` | int | Track ID number (mpv's `id`). |
| `type` | string | `"audio"`, `"video"`, or `"sub"`. |
| `title` | string | Human-readable track title. |
| `lang` | string | Language code (e.g. `"eng"`, `"fra"`). |
| `selected` | bool | True if this track is currently selected. |
| `codec` | string | Short codec name (e.g. `"hevc"`, `"aac"`). |
| `codecDesc` | string | Long codec description. |
| `default` | bool | True if this is the default track. |
| `forced` | bool | True if this is a forced track. |

Common fields in each playlist entry object:

| Property | Type | Description |
|---|---|---|
| `trackId` | int | Playlist entry ID (mpv's `id`). |
| `filename` | string | URL or file path of the entry. |
| `current` | bool | True if this is the currently playing entry. |
| `playing` | bool | True if this entry is currently being played. |

Additional fields from mpv are passed through automatically.

Metadata, track labels, file names and error messages may contain untrusted
media-file content, including HTML-like markup. Display these strings with
`textFormat: Text.PlainText`; do not rely on `Text.AutoText`, which can interpret
embedded image tags and fetch their URLs. The bundled examples use plain text
for media-derived labels.

## Render loss and teardown

The private renderer's snapshot supplier runs only during Qt synchronization;
native redraw callbacks never access a player item. Readiness/status delivery is
generation-tagged. A newer successful context can recover the same retained
playlist entry after matching VO-loss termination, preserving playlist identity,
position and pause. Stop/source/navigation changes cancel recovery interest;
there is no unconditional media reload.

`stop(false)` also cancels pre-render deferred media loads. Player/window
destruction does not join final-core file I/O: reserved native retirement workers
retain live handles until destruction actually completes. Native render cleanup
still uses the original GL thread, and this does not bound arbitrary graphics
driver stalls or delete accepted screenshot files.

---

## C++ API/ABI and caller migration

The current `MpvPlayer` directly derives from `QQuickFramebufferObject`, with a
private Qt/libmpv backend. Rebuild C++ consumers against the current headers and
library together: the former MpvQt base/casts and adapter constructor are not
ABI-compatible. Installed public headers are `mpvqml/mpvplayer.h` and
`mpvqml/mpvcontroller_iface.h`; the production adapter/controller/renderer are
private. Consumers do not include or link MpvQt.

The coordinated caller signatures are:

```cpp
Q_INVOKABLE int commandAsync(const QStringList &params);
Q_INVOKABLE int setMpvPropertyAsync(const QString &name, const QVariant &value);
Q_INVOKABLE int readMpvPropertyAsync(const QString &name);
void asyncCommandFinished(int id, int error, const QVariant &result);
void mpvPropertySetFinished(int id, int error);
void mpvPropertyReadFinished(int id, int error, const QVariant &value);
Q_INVOKABLE void hookContinue(quint64 id);
void hookTriggered(const QString &name, quint64 id);
```

| # | Earlier caller pattern | Current contract |
|---|---|---|
| 1 | Supply a command correlation ID or interpret return as native status | Store the returned positive int request ID and wait for its matching queued completion; negative return is immediate rejection. |
| 2 | Blocking generic property reads or raw `setProperty(name, value)` | Use `readMpvPropertyAsync` / `setMpvPropertyAsync` and their distinct completion signals. Typed media setters remain available. |
| 3 | Borrow `mpv_event` or native reply storage | Consume the terminal signal's owned Qt value; native correlation tokens stay private. |
| 4 | Treat a hook ID as an allocated async request ID | Keep the supplied uint64/quint64 hook ID separate and return it unchanged to `hookContinue`. Property-dependent hook work waits for its own read/write completion before continuation. |

Migrate examples and other callers together with the rebuilt
module. The existing `import MpvQml`, typed media API and helper functionality
remain. These source/ABI changes do not choose a release version or rewrite
historical CHANGELOG entries.

## Basic examples

### Minimal video player

```qml
import QtQuick
import MpvQml

MpvPlayer {
    anchors.fill: parent
    source: "video.mp4"

    MouseArea {
        anchors.fill: parent
        onClicked: parent.paused = !parent.paused
    }
}
```

### Player with controls

```qml
import QtQuick
import MpvQml

Item {
    MpvPlayer {
        id: player
        anchors.fill: parent
        source: "video.mp4"
    }

    Rectangle {
        anchors.bottom: parent.bottom
        width: parent.width; height: 40
        color: "black"

        Row {
            anchors.centerIn: parent; spacing: 8

            Button { text: "|<"; onClicked: player.seekRelative(-10) }
            Button { text: player.paused ? "Play" : "Pause"
                     onClicked: player.paused = !player.paused }
            Button { text: ">|"; onClicked: player.seekRelative(10) }

            Text { color: "white"
                   text: formatTime(player.position) + " / "
                       + formatTime(player.duration)
                   anchors.verticalCenter: parent.verticalCenter }
        }
    }

    function formatTime(s) {
        var m = Math.floor(s / 60)
        var sec = Math.floor(s % 60)
        return m + ":" + (sec < 10 ? "0" : "") + sec
    }
}
```

### Volume slider

```qml
Slider {
    from: 0; to: 150; value: player.volume
    onMoved: player.volume = value
}
```

### Loading from a directory

```qml
MpvPlayer {
    id: player
    Component.onCompleted: {
        player.loadDirectory("/path/to/videos",
                             ["*.mp4", "*.mkv"],
                             "replace", "name")
    }
}
```
