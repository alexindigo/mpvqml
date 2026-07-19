# Architecture

## Component hierarchy

```
QQuickFramebufferObject
         |
    MpvAbstractItem            (MpvQt library)
     |            |
     |       [ownership]
     |            |
     |     MpvController       (MpvQt library)
     |     - mpv_create()
     |     - mpv_initialize()
     |     - owns mpv_handle
     |
     |  mpvController()        (accessor, returns MpvController*)
     |
     ==================== Qt/C++ boundary ====================
     |
     |  MpvControllerIface     (abstract interface, defined in mpvcontroller_iface.h)
     |     ^          ^
     |     |          |
     |  MpvControllerAdapter   MockMpvController
     |  (wraps real            (for unit tests)
     |   MpvController*)
     |
    MpvPlayer : MpvAbstractItem
     - receives MpvControllerIface* (real adapter or mock)
     - constructor initializer: MpvAbstractItem(parent) runs first
     - constructor body: wraps mpvController() in MpvControllerAdapter if no mock provided
```

## Initialization order

1. `MpvPlayer` constructor initializer list calls `MpvAbstractItem(parent)`
2. `MpvAbstractItem` creates `MpvController`, calls `init()` via `BlockingQueuedConnection`
3. Inside `init()`: `mpv_create()`, `mpv_initialize()`, property observations are set up
4. `MpvPlayer` constructor body: wraps the initialized controller, sets up signals, marks dirty defaults
5. QML engine applies property bindings (calls setters — they mark dirty, no mpv access yet)
6. Renderer up: `MpvAbstractItem::ready` fires; slot sets `m_rendererInitialized = true` and calls `syncProperties()`
7. `syncProperties()` pushes all dirty properties to mpv, runs deferred load actions

All steps after step 3 are post-`mpv_initialize()`. Options set via `setOption()` (which calls `mpv_set_option_string()`) after this point may not take effect — `mpv_set_option_string` is a pre-init API. For runtime changes, use `mpv_set_property_string` or the `set` command.

## Adapter pattern

The adapter (`MpvControllerAdapter`) exists solely to make `MpvPlayer` testable. `MpvControllerIface` defines the contract. Real production uses `MpvControllerAdapter` wrapping `MpvController`. Tests use `MockMpvController` which records calls and returns controlled values.
