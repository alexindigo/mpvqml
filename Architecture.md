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
libmpv sees it. The host must not change this locale while using playback.
The playback owner records locale,
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
writes have distinct completion signals. The GUI ledger checks the reply kind
before removing its owner; native replies and close share its terminal routing.
Convenience actions own their results and do not masquerade as caller commands.

Hook continuation IDs are the separate libmpv uint64/quint64 hook identifiers,
not allocated int request IDs or the adapter's private correlation tokens. See
`API.md` for coordinated caller signatures and the C++ API/ABI migration.

The controller copies strings, maps, arrays and bytes before the next native
event wait. User-built input nodes have noncopyable, value-initialized ownership
and allocator-matched recursive cleanup, including rejected submissions. Native
event-owned nodes are never freed as input/output objects. Flag events read the
API's integer type. Getters never synchronously invoke a live backend.
The worker stamps lifecycle diagnostics with the native entry and playback
generation, independently of caller intent and request tokens. It copies end
reason and code before delivery. The additive GUI signal retains those values
without changing the historical end/error signals. Overflow discards recovery
and current entry correlation, refreshes registered observations, and does not
infer lost lifecycle events or settle still-reserved request replies.

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
controller/bridge ownership. Worker and helpers share the event-client drain
protocol, not renderer or capture callbacks. The worker borrows its handle;
helpers own theirs. Closing advances generation, nulls the handle and unregisters
callbacks as a barrier before releasing the token and any owned handle. Queued
wakeups are receiver-bound and do not carry token pointers. Each drain is
single-entry, checks the receiver after each event, and reposts after at most
32 events or when nested delivery has consumed a pending wakeup.

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
It reserves from the shared admission/DSO pin budget without creating a native
capture handle in the consumer. A bounded FIFO feeds two lazy helper slots per
owner thread; each helper is reused for at most 32 transactions. Filesystem
workers open a regular input and publish the final PNG off the GUI thread.
There is no player command-queue or GL renderer involvement.

The imvqml-derived transport uses a versioned, length-prefixed
`SOCK_SEQPACKET` header with an independent nonzero wire serial. The consumer
passes a read-only source descriptor and a growth-sealed output memfd, not
paths or a whole file copy. The helper sees only its socket at exec besides
standard streams. The consumer accepts pixels only with a correlated reply,
matching bounded geometry and exact byte length, and immutable seals on the
original output descriptor. The helper is resolved from the mapped backing
library with a prefix-relative libexec path, not from cwd or `PATH`.

The private frame transaction requires a matching command reply and an
ordered file-loaded/restart pair for initial readiness, or a seek/restart
pair for requested-position readiness. Software frame handoff keeps the
native VO progressing while events are pending. Terminal details are owned
values, separate from native event storage. Only the helper initializes and
terminates the capture core; it frees the software render context and retires
the core while the input descriptor remains live, before replying. It limits
virtual address space to 1.5 GiB and glibc malloc to two arenas before native
startup. Bounded native diagnostics are attached to failures only.

A seven-second monotonic budget starts at admission. The GUI pool rejects late
replies and kills timed-out children; the helper's kernel watchdog also kills
the process if the GUI event loop is stalled. Crash, timeout, protocol error
and running cancellation retire the child, with replacement on demand. The
job retains capacity until the process is reaped and any application-side
work retires. The terminal ledger keeps cancellation before capacity release
without waiting for reaping. Destruction drops late delivery and does not
join native work. This is crash containment, not an OS sandbox.

The encoder writes a temporary PNG beside the target in a descriptor-held
directory. Cancellation and publication compete at one gate. Publication
atomically links the finished PNG into an unused caller-owned name without
replacing aliases or competing writers, then retires the temporary name. This
publication gate is in the consumer: the helper cannot publish a file. The
deadline is cooperative for application-side filesystem operations and does
not change an already committed publication into a timeout.
The guiless suite observes real first/seek pixels through the registered QML
type; isolated fault children hold native and publication boundaries.

The installed plugin resolves the backing library relative to its own prefix.
C++ playback consumers discover the backing library via `mpvqml::mpvqml`, whose
public dependencies include the native headers used by the interface seam.
QML capture uses plugin discovery instead of a C++ link. Relocation is tested
with both original trees denied: the external project links only the exported
target, and a distinct no-display process imports the installed type and
checks decoded pixels and mapped DSOs.

See `UPSTREAM.md` for pinned derivation/notices, `API.md` for discovery and
consumer contracts, and `tests/README.md` for fixture and lane ownership.
Native rendered cases observe consumer pixels, retained state and reclamation;
mock readiness is only unit-seam evidence. Build/test execution belongs in
the owned project feature VM.
