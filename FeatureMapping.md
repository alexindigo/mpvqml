# Feature Mapping: Qt/libmpv → MpvPlayer

`MpvPlayer` uses the private mpvqml backend described in `Architecture.md`.
Selected upstream mechanics/provenance are documented in `UPSTREAM.md`; external
MpvQt classes are not part of the public API or runtime dependency graph.

| libmpv area | Consumer surface | Owning implementation |
|---|---|---|
| Core creation/initialization | `coreAvailable`, initialization result/failure | Shared native permit owns creation, stage/code and unpublished cleanup; worker/controller supplies playback options |
| Rendering | `rendererReady`, `ready`, render result/failure | Private synchronized FBO renderer; OpenGL only |
| Media load/navigation/playlist | Typed methods/properties in API.md | Shared effect classification, cancellable ordered media admission, worker-side directory scan and bounded dispatch. `clearPlaylist()` preserves the playing entry. |
| Typed property writes | Writable Q_PROPERTY setters | GUI dirty/pending/revision pipeline, then queued interface writes |
| Generic property writes | `setMpvPropertyAsync(name, value)` | Adapter ID/token correlation → `mpv_set_property_async` with owned nodes |
| Generic property reads | `readMpvPropertyAsync(name)` | Adapter correlation → `mpv_get_property_async`, owned result envelopes |
| Reactive reads | Typed getters and Changed signals | Private controller observations → copied GUI registry handlers |
| Generic commands/results | `commandAsync(params)` / `asyncCommandFinished(id, error, result)` | `mpv_command_node_async`, immediate input cleanup, owned native result copies |
| Playback diagnostics | `playbackEvent(details)` alongside legacy lifecycle signals | Owned native stage, entry/generation, end reason/code; overflow refreshes observations without replaying admitted requests |
| Configuration files | `loadConfigFile(path)` | Worker-affine native loading; operation failures reported to the facade |
| Screenshots | `screenshot(mode)` or explicit caller screenshot command | Async submission; final core/file I/O retires off GUI/render threads |
| One-shot local video frame | `MpvFrameCapture.capture` / `tryCapture` / `cancel` / `finished` / `finishedDetailed` | Worker-owned load/seek and software-frame transaction, shared native admission, bounded no-clobber staged PNG |
| Capture backpressure | `capacityReleased(id)` / `capacityAvailable()` | Local slot release follows native cleanup and shared slot release; cancellation can settle earlier |
| Logs | `setLogLevel` / `logMessage` | Separate helper client, barrier-safe immutable wakeup registration |
| Hooks | `hookAdd`, `hookContinue`, `hookTriggered` | Separate helper client; hooks must be continued during ordinary operation |
| Render parameters | `setRenderParameter("ambient-light", integer)` | Bounded snapshot coalescing and matching-context native application |
| Errors | `mpvError`, `lastError`, `errorOccurred`, `operationFailed` | Numeric compile-time checks plus GUI-owned error/status publication |
| Shutdown | `shutdown()` and destructor | Receiver settlement/helper teardown → asynchronous worker/core retirement |

## Async semantics

Commands and property operations share positive monotonically allocated public
int IDs. Native uint64 tokens are independent and private. Positive return means
local admission; later native rejection is still one queued completion for that
ID. Negative return means immediate rejection with no completion. IDs never
wrap/reuse; exhaustion is a rejection. There are no caller-ID overloads, borrowed
`mpv_event` signals, raw-handle bypasses or inherited GUI-blocking operations.

Hook IDs are uint64/quint64 values supplied by libmpv and returned to
`hookContinue`; they are separate from allocated int command/read/write IDs and
the adapter's private uint64 correlation tokens.

Convenience media/screenshot and typed-property operations use the same seam
and own their completions. Only explicit caller requests publish the matching
command/read/set completion signals. All reads, including diagnostics, can be
mocked without constructing a native core or worker.
Typed properties project observed or pending values without blocking. Their
internal read-backs reconcile accepted/rejected writes without any additional
caller completion signals. Real libmpv tests cover raw round-trips and explicit
clears across renderer recreation.

## Rendering/lifecycle details

Initialization succeeds independently from actual FBO readiness. Unsupported
Qt graphics APIs and native context failures are observable and recoverable.
Snapshot delivery is synchronization-only; native redraw callbacks never read
item state. Context/ready status is generation-tagged and redraw posting is
coalesced. Owned same-entry recovery preserves playlist identity, position and
pause only after matching render-loss termination and unchanged intent.

Renderer teardown uses a context owner independent of replaceable snapshots.
It unregisters callbacks, makes the documented final non-drawing
frame handoff, frees its context on the original GL thread, then releases its
renderer claim and acquisition lease. Explicit shutdown, native core loss and
item destruction share facade detach; a weak job on the creating window also
releases a live hidden item's context without another item synchronization.
Neither late snapshots nor queued status can reopen a detached player. Core leases
and helpers retain native ownership until cleanup is safe. Two independent
native retirement workers and 32 pre-reserved leases keep final-core file I/O
out of GUI/window destruction; no live handle is dropped on a timeout.

## Deliberately unexposed native surfaces

Raw clients/events, event filtering, dynamic observation/unobservation, stream
protocol callbacks, native render-context handles and pre-init-only options are
not public QML APIs. Render parameters other than supported ambient-light are
rejected; no target-peak mapping is invented. Native screenshot file writes are
not represented as abortable. See `FutureDevelopment.md` for research topics.
