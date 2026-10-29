# Private backend provenance

mpvqml maintains selected Qt/libmpv integration mechanics derived from **MpvQt
v1.2.0**, commit `edb1e3213e45aeec7dfcb74b1f5e722264af6636`, maintained at
<https://invent.kde.org/libraries/mpvqt>. The local reference is
`~/Sources/kde-mpvqt` at that immutable pin; it is not a patch workspace.

| # | Local derived material | Pinned upstream source | Local modifications |
|---|---|---|---|
| 1 | `mpvcontroller_p.h`, `mpvcontroller.cpp`: input trees and value conversion | `src/mpvcontroller.cpp` (`createList`, `setNode`, `freeNode`, `nodeToVariant`), `src/mpvcontroller_p.h` | Value-initialized, noncopyable input ownership established before conversion; cleanup on rejected/exceptional conversion; allocator-matched recursive deletion; owned byte arrays; guarded property decoding with integer flags |
| 2 | `mpvrenderbridge_p.h`, `mpvrenderbridge.cpp`: shared core lifetime | `src/mpvcontroller.h` (`MpvHandleManager`) | Reserved 32-slot admission pool, two process-lifetime native retirement workers, checked loaded-module pinning and off-GUI final termination; separate parentless/coalescing GUI dispatcher and value-only snapshot declarations |
| 3 | `mpvrenderer_p.h`, `mpvrenderer.cpp`: GL/FBO integration | `src/mpvrenderer.{h,cpp}` | Synchronized value supplier, dispatcher-only native callback, guarded context/FBO/rendering, creating-GL-context and generation ownership, recoverable status, bounded render parameters, a final non-drawing pending-frame handoff before context free, and the libmpv float texture-creation upload guard (see "Known libmpv workarounds") |

Modification date: **2026-10-01**; `mpvrenderer_p.h`/`mpvrenderer.cpp` further
modified **2026-10-29** (upload guard). The owning controller/renderer/facade
integration is complete in the current source. This table records derivation and
local changes; runtime verification has its own documented scope. The local
controller also owns intent/generation-guarded same-entry render-loss recovery;
the player derives directly from QQuickFramebufferObject, with the production
adapter and native backend behind its private implementation.

Derived source retains George Florea Bănuș's upstream copyright notice. From
the upstream offer `LGPL-2.1-only OR LGPL-3.0-only OR
LicenseRef-KDE-Accepted-LGPL`, these derived files select **LGPL-3.0-only**.
The LGPL text is in `LICENSES/LGPL-3.0-only.txt`; its incorporated GPLv3 terms
are supplied in the project's existing `LICENSE`. Original mpvqml licensing
is unchanged. Distributions include these notices, both license texts and the
corresponding derived source/build material.

The upstream repository's packaging, examples, exported MpvQt ABI and build
framework are not imported. The stock system MpvQt library/reference remains
unmodified. Private backend types use `MpvQmlPrivate`; public consumption
remains through `MpvPlayer` and its documented interface seam.

## Known libmpv workarounds

| # | Upstream defect | Symptom | mpvqml workaround | Removal trigger |
|---|---|---|---|---|
| 1 | libmpv 0.41.0 `video/out/gpu/video.c:1852` `reinit_scaler()` allocates the scaler LUT with `talloc_array()` instead of `talloc_zero_array()`; for padded kernels (default lanczos: 6 taps in 8-float rows) two cells per row upload uninitialised heap bytes | Intermittent, permanent black video while playback, readiness and rendering report success: a NaN padding cell enters the used weights through zero-weight `GL_LINEAR` blending (NaN-propagating drivers such as llvmpipe) | `resolveMpvGlFunction()` hands libmpv stateless `glTexImage1D/2D/3D` wrappers that upload a sanitised copy of `GL_FLOAT` client data only when it contains non-finite values (or half-range overflow for 16-bit float storage); integer, PBO-offset and per-frame `glTexSubImage2D` uploads pass through untouched | A libmpv release that zero-initialises the LUT, verified by `mpvqml_lut_guard_tests` (poisoned-padding child) passing with the guard disabled |
