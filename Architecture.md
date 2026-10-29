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
| Private render bridge | Value snapshots, shared leases, generation-tagged/coalesced GUI dispatcher, 32-slot admission and two native retirement workers |
| Private renderer | GL-thread context/FBO/rendering, synchronized snapshot handoff, native callback unregistration and owned-context cleanup |
| Log/hook helpers | GUI-affine client ownership, immutable registration tokens, generation-aware deletion-safe event drains |

## Initialization and readiness

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
state replaces its original queue slot by canonical key (one mouse position and
one selection per track type, with sub/subtitle sharing sid); command actions
such as loads and directory loads retain every occurrence in order. State and
commands share one queue and its cancellation lifetime. Actions are detached
before iteration, rechecked after external delivery, and
cancelled by stop/source/navigation/shutdown interest changes.

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
while Qt blocks GUI mutation. Rendering, native callbacks and teardown use the
copied core lease, metadata, generation, intent epoch and bounded parameters.
The native redraw callback accesses only a strongly retained, parentless GUI
dispatcher. An atomic pending bit limits queued redraw work; item destruction
disconnects its receivers and retired generations cannot change current status.

Context retirement records a coherent generation/intent marker before native
free. A matching VO-loss termination plus newer-context readiness and unchanged
source interest permits one same-entry playlist replay. Pause completion precedes
replay admission; position restoration follows the matching file-loaded event.
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
A separately retained worker control survives Qt's finished notification until
nonblocking `wait(0)` observes actual native/TLS completion. Last core ownership
transfers its handle to preallocated native retirement storage. Two
process-lifetime workers perform final termination; each of 32 leases remains
occupied until native destruction completes. Exhaustion is a recoverable
initialization failure. Checked pins retain the actually loaded module/libmpv
code during outstanding cleanup. There is no application-destructor join or
timeout that drops live handles.

## Source and verification

See `UPSTREAM.md` for pinned derivation/notices and `API.md` for the consumer
contract. Existing suites plus controller, renderer, facade, helper, retirement,
child-scoped crash and LUT-guard targets are registered with CTest. Native rendered cases
observe consumer pixels, retained state and reclamation; mock readiness is only
unit-seam evidence. Build/test execution belongs in the owned project feature VM.
