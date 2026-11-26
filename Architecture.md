# Architecture

## Ownership and layers

`MpvPlayer` derives directly from `QQuickFramebufferObject`. Its public header
exposes typed media properties, allocated async requests, lifecycle status and
the injectable `MpvControllerIface`; native backend types live behind its
private implementation. There is no system MpvQt inheritance or linkage.

| Component | Responsibility |
|---|---|
| `MpvPlayer` | GUI-owned state, intent/reentrancy guards, property registry, deferred media actions, request ownership and helper lifecycle |
| `MpvControllerIface` | GUI-affine availability/admission/completion seam for every application command and generic property operation |
| Private production adapter | Positive monotonic public IDs, independent uint64 native tokens, request-kind/generation correlation and exactly-once queued terminal delivery |
| Injected mock | Controlled availability/owned queued replies with no native core or worker |
| Private controller/worker | One blocking bootstrap attempt, then worker-affine submissions, observations, owned event decoding and bounded render-loss recovery |
| Private native backend | 32-slot admission, pinned single-attempt core owner, mode-specific retirement and two native retirement workers; no GL dependency |
| Private render bridge | Value snapshots, shared playback session state and a generation-tagged/coalesced GUI dispatcher |
| Private renderer | GL-thread context/FBO/rendering, synchronized snapshot handoff, native callback unregistration and owned-context cleanup |
| Log/hook helpers | GUI-affine client ownership, immutable registration tokens, generation-aware deletion-safe event drains |

## Initialization and readiness

Only a reserved `MpvBackendCore` can create and initialize a core. It serializes
creation in this library and sets the process's `LC_NUMERIC` to `C` before
libmpv sees it. The host must not change this locale while using libmpv. Playback
and frame capture supply their own option profiles; the owner records locale,
creation, option and initialization failures with the native code where supplied.
Unpublished candidates are destroyed on the creating worker. Log and hook clients
attach to an already initialized playback core.

Allocate controller state before startup. Start/check the native Qt worker,
move the controller only after successful startup, and finish initial native
creation/initialization before construction-time QML bindings are applied.
Publish the core lease only after initialization succeeds. Failure remains
observable through immutable initialization getters and queued failure delivery;
there is no abort or successful renderer-ready transition on failure.

Core availability and renderer readiness are separate. Only a successfully
created native context and valid FBO publish `ready()`. The attached window's
actual graphics API is inspected after scene-graph initialization; unsupported
backends report a render failure without changing the consumer's graphics API.
Typed intent is retained whenever the core is available, including across a
failed render attempt, until a successful renderer generation flushes it.
Loads, directory loads, track selections and mouse positions share deferred
renderer admission; core commands remain independent of render failures. Deferred
state removes its old canonical-key entry and appends the replacement after any
intervening commands (one mouse position and one selection per track type, with
sub/subtitle sharing sid); command actions
such as loads and directory loads retain every occurrence in order. State and
commands share a queue. Directory enumeration returns an owned result off the
GUI thread. Its ticket gates delivery and ordered bounded batches block the
following renderer-gated actions until that directory settles. Known raw and
typed stops and navigation share effect classification with worker recovery;
clear cancels only pending loads while preserving any playing entry. Unknown
raw commands, aliases and script effects remain an escape hatch. Video selection
uses native `vid`; it has no reload or facade seek continuation.

One descriptor per owned property defines the native format, conversions,
typed normalization, initial writes and renderer-ready order. Its state keeps
the latest observation separate from the desired revision, queued intent and
active native write. Typed writes to the same property serialize through the
write reply; a matching read-back settles the current revision without relying
on libmpv to send a change notification. A late read-back cannot settle a newer
revision. Unavailable values do not overwrite the last available observation.
Explicit clears stay valid values through renderer loss and readiness; an empty
shader list is encoded as a native array, not a string containing one empty path.

## Async operations and event ownership

Application work always passes through the interface. A positive int ID is local
admission, not native acceptance. Insert the private-token mapping before queue
admission, remove it before terminal delivery, and settle later submission
failures or explicit shutdown exactly once while the receiver exists. Native
uint64 tokens never become QML numbers. Commands, property reads and property
writes have distinct completion signals. Convenience actions own their results
and do not masquerade as caller commands.

Hook continuation IDs are the separate libmpv uint64/quint64 hook identifiers,
not allocated int request IDs or the adapter's private correlation tokens. See
`API.md` for coordinated caller signatures and the C++ API/ABI migration.

The controller copies strings, maps, arrays and bytes before the next native
event wait. User-built input nodes have noncopyable, value-initialized ownership
and allocator-matched recursive cleanup, including rejected submissions. Native
event-owned nodes are never freed as input/output objects. Flag events read the
API's integer type. Getters never synchronously invoke a live backend.

## Rendering and recovery

The renderer invokes its item-supplied value callback only in `synchronize()`
while Qt blocks GUI mutation. Replaceable configuration is separate from the
GL-thread context owner. That owner retains its acquisition lease, renderer
claim, generation, synchronized intent epoch and dispatcher through callback
unregistration, pending-frame handoff, native free and claim release. Empty
snapshots cannot erase ownership, and no lease remains in a stale snapshot.
The native redraw callback accesses only a strongly retained, parentless GUI
dispatcher. An atomic pending bit limits queued redraw work; item destruction
disconnects its receivers and retired generations cannot change current status.

Context retirement records a coherent generation/intent marker before native
free. A matching VO-loss termination plus newer-context readiness and unchanged
source interest permits one same-entry playlist replay. GUI source replacement
advances the shared recovery epoch even before renderer admission; native starts
are tagged with the worker's admitted epoch. Pause completion precedes replay
admission; position restoration follows the matching file-loaded event.
Owned observations and newer pause/seek intent supersede saved state. Stop,
source/navigation changes and shutdown cancel recovery; late replies cannot
revive it. Other errors never cause unconditional reloads.

Supported render parameters are coalesced by name and applied only on the
matching current GL context. At teardown, unregister redraw callbacks, consume
a pending frame with a non-drawing/non-timing handoff, then free that renderer's
context on its original GL thread. This prevents the VO's missing-render timeout
from delaying cleanup and does not cancel screenshot writes.

libmpv resolves every GL entry point through the renderer's creating-context
loader. That loader is the single place where mpvqml works around libmpv GL
defects: texture-creation calls carrying float client data are sanitised before
upload (uninitialised scaler-LUT padding in libmpv 0.41.0 otherwise renders
intermittent permanent black). The wrappers are stateless and resolve the real
entry point and unpack state from the calling thread's current context. See
`UPSTREAM.md` "Known libmpv workarounds" for scope and removal trigger.

## Destruction and retirement

Mark shutdown before external calls, disconnect derived-class/self/window
delivery before base destruction, and shut down/delete helpers before releasing
controller/bridge ownership. Helpers exchange handles to null, advance
generation, unregister callbacks as a barrier, then destroy clients/tokens.
Queued wakeups contain receiver/generation values, never token pointers. Event
drains are single-entry and check a same-thread QPointer immediately after each
external signal, then recheck generation/handle identity.

Player/window destruction never joins the live controller or final native core.
Explicit shutdown, native core loss and item destruction share one detach
transition: close GUI admission and dispatcher delivery, retire helpers, schedule
a weak generation-specific release job on the context's original window, stop
the owned worker and release the facade lease. The release job can retire a
hidden item without its next synchronization; it never owns an item or native
resource. Qt runs it with the original GL context, or destroys an unrun job
without moving native cleanup to the GUI thread. A non-renderable window defers
release until rendering or scene-graph cleanup. Renderer destruction is the
same owner's fallback release path. Injected controllers remain borrowed.
A separately retained worker control survives Qt's finished notification until
nonblocking `wait(0)` observes actual native/TLS completion. Worker controls use
the backend's reservation index and capacity. Last shared core ownership
transfers its handle to preallocated native retirement storage. Two
process-lifetime workers perform final termination; each of 32 leases remains
occupied until native destruction completes. Exhaustion is a recoverable
initialization failure. Checked pins retain the actually loaded module/libmpv
code during outstanding cleanup. There is no application-destructor join or
timeout that drops live handles.

## Source and verification

`MpvFrameCapture` is a separate non-item facade for a one-shot transaction.
It uses the shared native admission/DSO pin budget directly, not the player
command queue or GL renderer. Each bounded job owns a regular input descriptor,
an exclusive core and a software memory render context on the same worker.
Job outcome (including early cancellation) is separate from native retirement.
The worker frees the render context, terminates the core while the input
descriptor is still live, closes the input and releases the shared slot. Guarded
GUI delivery then frees the local slot before non-cancelled completion and
capacity notification. Cancellation and retirement callbacks share one terminal
delivery ledger, keeping cancellation before capacity regardless of which was
queued first. Destruction does not join the worker.

The private frame transaction requires a matching command reply and an
ordered file-loaded/restart pair for initial readiness, or a seek/restart
pair for requested-position readiness. Software frame handoff keeps the
native VO progressing while events are pending. Terminal details are owned
values, separate from native event storage. A 7-second cooperative budget
checks work boundaries; it cannot preempt native or filesystem calls.

The encoder writes a temporary PNG beside the target in a descriptor-held
directory. Cancellation and publication compete at one gate. Publication
atomically links the finished PNG into an unused caller-owned name without
replacing aliases or competing writers, then retires the temporary name.
The guiless suite observes real first/seek pixels through the registered QML
type; isolated fault children hold native and publication boundaries.

See `UPSTREAM.md` for pinned derivation/notices and `API.md` for the consumer
contract. Existing suites plus controller, renderer, facade, helper, retirement,
child-scoped crash and LUT-guard targets are registered with CTest. Native rendered cases
observe consumer pixels, retained state and reclamation; mock readiness is only
unit-seam evidence. Build/test execution belongs in the owned project feature VM.
