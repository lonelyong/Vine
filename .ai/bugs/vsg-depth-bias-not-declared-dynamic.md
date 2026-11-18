# The vsg backend's polygon offset has no effect (the depth-bias state is not declared dynamic)

- **Status**: Open; the static code path is confirmed, a pixel or depth regression on a real device is missing.
- **Found**: 2026-11-02
- **Module**: `src/plugins/gfx_backend_vsg`
- **Files**: `makeDynamicStateDeclaration()` (`internal/ContentPipeline.cpp`, `support/VsgDynamicState.cpp`),
  `makeDynamicStateCommand()` (`internal/StateCommands.cpp`), `SetDynamicState::record()` (`support/VsgDynamicState.cpp`)

## Symptom and impact

`StateNode::setPolygonOffset` / `ResolvedRenderState::polygonOffset` is a promised capability: the depth offset serves
decals, coplanar surfaces and shadow offsets. The backend writes it into the dynamic-state command, but the content
pipeline's dynamic-state declaration does not list it, and the pipeline create-info's `depthBiasEnable` keeps its default
`VK_FALSE`. The offset value is written into the command stream yet takes **no part in rasterisation**, coplanar surfaces
keep z-fighting, and nothing is diagnosed anywhere - a state that is "set but has no effect".

## Root cause

Three places each true in isolation, failing together:

1. `makeDynamicStateCommand()` writes `polygon_offset.units` / `.factor` into `SetDynamicState::depth_bias_constant` /
   `.depth_bias_slope` (`StateCommands.cpp:119-120`), and `SetDynamicState::record()` really does call `vkCmdSetDepthBias`
   (`VsgDynamicState.cpp:90`).
2. The `makeDynamicStateDeclaration()` the content pipeline actually uses (`internal/ContentPipeline.cpp:183-196`) lists 12
   dynamic states and **not `VK_DYNAMIC_STATE_DEPTH_BIAS`**. The other declaration of the same name in the repository
   (`support/VsgDynamicState.cpp:117-129`) does include it, so the two lists have drifted;
   `ContentPipelineTest.TheEditableHalfIsDeclaredDynamic` asserts the pipeline declaration but does not cover depth bias.
3. The pipeline create-info's `RasterizationState` is created with vsg's defaults, so `depthBiasEnable == VK_FALSE`
   (`build/_deps/vsg-src/include/vsg/state/RasterizationState.h:33`), and nothing sets
   `VK_DYNAMIC_STATE_DEPTH_BIAS_ENABLE_EXT` to turn it on dynamically - even with the declaration added, the enable stays
   off.

The Vulkan rule: a dynamic state that a pipeline does not declare makes `vkCmdSetXxx` ineffective for that pipeline, and the
driver uses the value baked into create-info; while `depthBiasEnable == VK_FALSE` the depth bias is not applied at all. Both
apply here, so the offset is necessarily a no-op.

## Suggested fix

1. Give the dynamic-state list **one** authoritative definition: delete the duplicate and share one
   `makeDynamicStateDeclaration()` between the content and screen pipelines.
2. Add `VK_DYNAMIC_STATE_DEPTH_BIAS` to that list, and either bake `depthBiasEnable` to `VK_TRUE` per the engine
   convention (an offset of 0 equals no offset, so it is a safe default) or switch to
   `VK_DYNAMIC_STATE_DEPTH_BIAS_ENABLE_EXT` and send it with the command - pick one and state it in the header.
3. Add a "list completeness" test: every state `record()` writes must be in the set the pipeline declares, and vice versa;
   that test is the executable form of the rule in `graphics-rendering.md` section 2.8.
4. If the device lacks a required feature other than `fillModeNonSolid`, refuse the session by the existing policy instead
   of degrading to "the offset does nothing".

## Verification plan

- Unit: `ContentPipelineTest` asserts that the content pipeline declaration contains `VK_DYNAMIC_STATE_DEPTH_BIAS` and that
  the `RasterizationState`'s `depthBiasEnable` is `VK_TRUE`.
- Pixels: two coplanar triangles, one given `PolygonOffset{1,1}` and one not; after rendering and reading back, the offset
  side wins consistently (no flicker over repeated runs). Without the offset, mixed pixels appear between them as a control.
- Validation: run `scripts/vsg_rewrite_gate.sh` once and confirm 0 VUIDs (especially the ones around `vkCmdSetDepthBias`).

## Design constraints

The finding touches **G4** (writing state and declaring state must agree; there is no interface where "written but
ineffective" is acceptable), **G6** (two dynamic-state lists in one backend are an inconsistent implementation) and **G10**
(the real cost of "set but ineffective" is hidden instead of stating how the enable and the dynamic value divide the work).
