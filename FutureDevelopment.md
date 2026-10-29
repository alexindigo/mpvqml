# Future Developments

Features investigated but not yet implemented. Each entry includes the
rationale, research findings, and proposed API surface so implementation
can pick up where investigation left off.

---

## Full Property Coverage Test (`test_mpv_property_coverage`)

**Investigated: 2026-06-02 — Not included in v1.0**

A test was written that queries mpv's `property-list` at runtime, collects
all observed property names from `MpvPlayer::m_propertyHandlers`, and
asserts the two sets are equal. This provides a hard check that:

- Every mpv property we claim to observe actually exists in mpv
- We don't miss properties that mpv provides

The test currently fails: mpv reports 1032 properties while we observe 28.
The remaining 1004 properties are intentionally uncovered for v1.0 — most
are internal/obscure (e.g. `decoder-frame-drop-count`, `vo-delayed-frame-count`).

### To make this test pass

Each uncovered property must be either:
- Added to the observer dispatch table in `setupPropertyObservers()` if it
  provides reactive QML value
- Added to an explicit exclusion list (in the test itself or a data file)
  with a rationale for why it's not covered

### Properties in the observer table not in property-list

`video-params/w` and `video-params/h` are valid sub-properties not listed
in `property-list`. They are confirmed accessible via `mpv_get_property`.
The exclusion list should account for these.

---

## Hook System (`mpv_hook_add`, `mpv_hook_continue`)

**Investigated: 2026-05-30 — Partially implemented 2026-05-31**

The `MpvHookManager` helper (separate mpv client handle + wakeup callback)
provides the low-level API: `hookAdd`, `hookContinue`, `hookTriggered` signal.
The event routing concern from the original investigation is solved — we
no longer need MpvController internals access.

However, the deeper questions from the original investigation remain
unresolved and should inform any future hook-policy design. The current
callback/client lifetime and reentrant delivery mechanics have been hardened;
the historical questions below do not imply that migration is still pending:

### Available hook points

| Hook | When it fires |
|------|---------------|
| `on_before_start_file` | Before anything happens |
| `on_load` | Before file is opened — URL can be modified |
| `on_load_fail` | After a file fails to open |
| `on_preloaded` | After opened, before track selection |
| `on_loaded` | After tracks selected, before playback begins |
| `on_unload` | Before a file is closed |
| `on_after_end_file` | After playback ends |

### How it works (C API)

```c
// Register a handler. mpv blocks at the hook point until continue().
mpv_hook_add(ctx, reply_userdata, "on_load", priority);

// When hook fires, you receive MPV_EVENT_HOOK.
// Do your work (e.g. modify stream-open-filename), then:
mpv_hook_continue(ctx, hook_id);
```

### QML API migration

The current hook methods/signals are listed below. Any property-dependent hook
workflow uses `readMpvPropertyAsync` and `setMpvPropertyAsync`, correlates their
allocated IDs, and continues the hook after the matching terminal completion
(including failures). The original blocking-get/raw-set sketch is superseded
by the owned async contract in API.md; no synchronous compatibility methods are
provided. Missing continuation still blocks ordinary playback.

### C++ surface

```cpp
Q_INVOKABLE void hookAdd(const QString &name, int priority = 0);
Q_INVOKABLE void hookContinue(quint64 id);
signals:
    void hookTriggered(const QString &name, quint64 id);
```

### Open questions

- **Timeout safety:** If QML never calls `hookContinue()`, mpv stays
  blocked forever. Is a timeout valve needed before production use?
- **Priority ordering:** Multiple hooks of the same type ordered by
  priority (lower = first). Untested.
- **No concrete use case:** URL rewriting, per-file config, content
  filtering — none have driven real-world validation of the API.

---

---

## Pre-Init Options (setOption / mpv_set_option_string)

**Status: unsupported**

The owned backend retains a single blocking initialization attempt before QML
construction-time bindings. There is no public pre-init option gap or inherited
`setOption()` method. For runtime equivalents use `setMpvPropertyAsync`; truly
pre-init-only options (`config-dir`, `load-scripts`, `player-operation-mode`)
remain unsupported by the current constructor contract.

### Options that should be pre-init but are property-settable

Some options accept `mpv_set_property_string` after init even though they
are conceptually pre-init. These work as a workaround but are not guaranteed
by mpv:

- `keep-open-pause` — works at runtime, only consulted at EOF
- `vo` — works at runtime via `libmpv` backend
- `hwdec` — works at runtime since mpv 0.21.0

### Options that truly require pre-init

These have no property equivalent and WILL fail if set after init:

- `config-dir`
- `load-scripts`
- `player-operation-mode`
- `script`

### Future design boundary

Supporting these options would require an explicitly approved construction/API
design. The prior MpvQt inheritance/fork proposals are superseded by the owned
backend. This topic is not part of the crash/lifetime remediation.

*(Add new entries above this line)*
