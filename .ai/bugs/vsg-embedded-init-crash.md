# Bug: the embedded render backend crashes while initialising (closing the window, or when Qt rebuilds the surface) (Historical record)

- **Date**: 2026-09-02
- **Module**: gfx_backend_vsg plugin (the embedded Qt child-HWND render path)
- **Status**: Fixed

> **Historical code locations**: `VsgRenderer` / `SceneBridge` were removed in the backend rewrite; the file and class
> names below are kept as they were recorded then; the current structure is in `.ai/design/vsg-backend.md`.

## Symptom

The process crashed while running `TestRenderLiveCommand` or closing the window; the stack landed in `vsg::Device::Device`
(Device.cpp:63) on an uncaught C++ exception. The embedded view (`RenderControl`, Qt child HWND) had also been blank before
that.

## Root cause

vsg defaults to `VSG_MAX_DEVICES == 1` (which is 1 in this build's `Version.h`). When Qt rebuilds the native surface (the
HWND is rebuilt on layout, first show and close), `RenderControl` shuts the backend down and initialises it again. But
`VsgRenderer::shutdown()` only released `window` / `viewer`, while the **compiled pipelines and descriptors** kept in
`vsg_scene`, `render_graph`, the `SceneBridge` caches and the `VsgMaterialManager` cache **still referenced the old
`vsg::Device`**, so the old device was never destructed and its device id was never released. The next `Window::create()`
allocated device id 1 and triggered:

```
Number of vsg:Device allocated exceeds number supported
```

`initialize()` had no try/catch, so the exception travelled to the Qt event loop and terminated the process.

## Fix

File: `src/plugins/gfx_backend_vsg/src/VsgRenderer.cpp`

1. `initialize()` is wrapped in `try/catch`: on a catch it prints `[VsgRenderer] ...`, calls `shutdown()` and returns
   `false` (the exception no longer reaches Qt).
2. `shutdown()` now also releases `render_graph`, `vsg_scene` and `vsg_camera`, and calls `sceneBridge.clearCache()` and
   `materialManager.clear()`, so the old device is destroyed and its id reclaimed before the next `initialize()` creates a
   new one.
3. The failure paths after the window is created - a failed `Window::create`, a failed `CameraBridge::create`, a failed
   `compile()` - all call `shutdown()` (with a log line) and then `return false`, so a live but uninitialised device cannot
   leak and trip the device limit on the retry.
4. `traits->debugLayer = false`: avoids loading a validation layer this machine does not have.

## Verification

After the fix the embedded path initialises successfully on the first attempt (no `[VsgRenderer]` error in the startup
log), and rebuilding the surface or closing the window no longer triggers the device-limit crash.
