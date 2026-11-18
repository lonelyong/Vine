# Bug: the embedded render view stays blank (SceneBridge content is not drawn) (Historical record)

- **Date**: 2026-09-02
- **Module**: gfx_backend_vsg plugin (the embedded `SceneBridge` geometry build path)
- **Status**: Fixed

> **Historical code locations**: `VsgRenderer` / `SceneBridge` were removed in the backend rewrite; the file and class
> names below are kept as they were recorded then; the current structure is in `.ai/design/vsg-backend.md`.

## Symptom

Even after a successful initialisation the embedded path showed only the clear colour and no geometry, while the standalone
window path (`makeRawDemoNode`, `VN_VSG_OWN_WINDOW=1`) drew its triangle correctly.

## Root cause

1. In this vsg build (FetchContent v1.1.16, no glslang) the deserialised phong `ShaderSet` has empty
   `defaultGraphicsPipelineStates`. `GraphicsPipelineConfigurator` fills in `DepthStencilState` / `RasterizationState` /
   `ColorBlendState` / `MultisampleState` / `InputAssemblyState` but **never a `ViewportState`** -> the pipeline has no
   viewport, nothing is rasterised (the comment in `VsgRenderer::makeRawDemoNode()` already says the states must be given
   explicitly).
2. `SceneBridge::buildGeometry()` drew through a hand-assembled `VertexIndexDraw` - which was separately verified to
   **produce no draw** in this build; an explicit `Commands{BindVertexBuffers, BindIndexBuffer, DrawIndexed}` is required
   (see the vsgExamples `utils/vsggraphicspipelineconfigurator`).

## Fix

1. `VsgRenderer::initialize()` (`src/plugins/gfx_backend_vsg/src/VsgRenderer.cpp`): complete the shared phong `ShaderSet`'s
   `defaultGraphicsPipelineStates` explicitly - `ViewportState` (sized from the current window), `RasterizationState` with
   `cullMode = NONE`, `DepthStencilState` / `ColorBlendState` / `InputAssemblyState` / `MultisampleState`. Every geometry
   pipeline `SceneBridge` builds afterwards inherits these states.
2. `SceneBridge::buildGeometry()` (`src/plugins/gfx_backend_vsg/src/SceneBridge.cpp`): attach
   `Commands{BindVertexBuffers(config->baseAttributeBinding, arrays), BindIndexBuffer(indices), DrawIndexed(n, 1, 0, 0, 0)}`
   under the `StateGroup` (from `config->copyTo`) instead of `VertexIndexDraw`.
3. Add the `vsg/commands/{Commands,BindVertexBuffers,BindIndexBuffer,DrawIndexed}.h` headers and drop the now unused
   `vsg/nodes/VertexIndexDraw.h`.

## Verification

The embedded view draws the startup demo triangle and the animated triangle of `TestRenderLiveCommand` (explicit draw plus
complete pipeline states = a configuration verified to produce output).
