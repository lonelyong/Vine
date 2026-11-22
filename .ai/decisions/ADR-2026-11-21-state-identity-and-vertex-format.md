# ADR-2026-11-21: state is not data, identity carries the format, and a backend bakes only what its floor cannot move

- Status: **Accepted and implemented** (2026-11-21).
- Date: 2026-11-21
- Scope: `StateResolution.hpp` (where the assembly lives), `DrawState.hpp` (`VertexLayoutKey`, `PipelineKey`'s stated
  rule), the vsg dynamic-state declaration, and the vk pipeline key (`ContentCaches`'s `ContentPipelineKey` /
  `RasterState`).
- Related: `.ai/design/graphics-rendering.md` 2.8, `.ai/design/graphics-scene.md` F11, `.ai/design/vk-backend.md`
  F5, `.ai/design/vsg-backend.md` 4.2 / N4, `.ai/bugs/vsg-depth-bias-not-declared-dynamic.md`,
  [ADR-2026-11-16-no-backend-half](ADR-2026-11-16-no-backend-half.md) (the seam is a property, not a namespace).

## Background

A fifth review round (2026-11-21) compared what `DrawState.hpp` / `StateResolution.hpp` claim with what the two
backends do. Four statements were false or unenforceable:

1. **`ResolvedRenderState` carried a data fact.** `topology` sat in the state value with a `Triangles` default while
   its own comment said "no default here", and `applyStateDefaults()` - the content-only spelling, documented as
   "usable as a pipeline-variant key component" - could only guess it. The origin table had a `StateOrigin::Default`
   for topology that the enum's own comment called impossible. `Topology.hpp` says the assembly is the PRIMITIVE's
   fact.
2. **`VertexLayoutKey` could not identify a vertex format.** It carried a canonical-role mask and custom LOCATIONS.
   Two geometries feeding a custom channel at location 8 with two and with four components produced the same key, so
   the vsg backend served the second one the first one's compiled pipeline (a 4-component attribute read out of a
   2-component stream). The vk backend avoided it only by keeping a private `(location, components)` list in its own
   key.
3. **The seam claimed a state rule the default backend cannot follow.** The header said identity "must NOT contain
   … a cull mode or a blend choice" and `DynamicState` "must never recompile anything"; the vk backend (Vulkan 1.2,
   the host's default) bakes cull / polygon / depth test / write / compare / blend because the dynamic commands for
   them arrive with `VK_EXT_extended_dynamic_state`/`_state3`, which only the vsg backend requires. The two backends
   also used the same name, `PipelineKey`, for two different shapes.
4. **One dynamic-state list was written twice in vsg.** `internal/ContentPipeline.cpp` had its own copy of
   `makeDynamicStateDeclaration()`; it named twelve states, the support copy named ten, and both differed from the
   thirteen `SetDynamicState::record` writes. The content copy lacked `VK_DYNAMIC_STATE_DEPTH_BIAS` and the pipeline
   left `depthBiasEnable` false, so `StateNode::setPolygonOffset` was a silent no-op
   (`.ai/bugs/vsg-depth-bias-not-declared-dynamic.md`, open since 2026-11-02).

## Decision

1. **The assembly travels beside the state.** `ResolvedRenderState` and `RenderStateOrigin` lost `topology`;
   `ResolvedCommandState` gained `topology` + `topology_origin` (`Data` or `Pass`). `resolveDynamicState` takes the
   resolved COMMAND, because a set command writes both halves. `applyStateDefaults` no longer has a data fact to
   guess. (`G1`, `G4`.)
2. **A vertex slot is a location AND a width, in binding order.** `VertexLayoutKey` is
   `slots: vector<VertexLayoutSlot{ location, components }>`; `VertexLayoutKeyHash` hashes both fields. vsg fills it
   from its canonical bind order; vk fills it from the order it hands `vkCmdBindVertexBuffers`. The vk key's private
   channel list is deleted. (`G1`, `G10`.)
3. **The shared key is composed, and the deviation is named.** vk's key is
   `ContentPipelineKey{ vn::graphics::PipelineKey identity; RasterState raster; }` - the portable identity (kind,
   program id + revision, slots, compatibility, variant, topology class) plus the five items a 1.2 device must bake.
   `DynamicState` stays the maximum the ENGINE can move off identity; `DrawState.hpp` now states the rule as "move
   every item the device CAN off identity, and say which ones it cannot". The vk backend keeps its 1.2 floor on
   purpose: it is the second implementation that distinguishes "the seam requires this" from "vsg happens to do
   this" (`vk-backend.md` 1.1). (`G1`, `G6`, `G12`.)
4. **One dynamic-state list per backend, beside its writer.** vsg's lives in `support/VsgDynamicState.cpp` next to
   `SetDynamicState::record`; the content pipeline's copy is deleted, both pipeline builders call it, and the list
   now names `DEPTH_BIAS` (thirteen states). Both vsg rasterisation states bake `depthBiasEnable = VK_TRUE` - an
   enable cannot ride the value list (`graphics-rendering.md` 2.8). vk's four-entry list stays where it is and is
   documented as the same rule at a lower floor.
5. **vk writes its dynamic half through the seam value.** `ContentDraw` calls `resolveDynamicState(command)` for the
   line width and the bias, so the mapping has one implementation; the device's `lineWidthRange` clamp stays in the
   backend.

## Options rejected

- **Leaving `applyStateDefaults` with a `Triangles` default and documenting it.** That is what the tree did; the
  default was reachable by every tool and test, and one test had already pinned the guess as a "documented default".
- **Narrowing `VertexLayoutKey` to sorted locations and having vk order its channels canonically.** The ordering rule
  would still live in two places, and the key would still omit the format; the widening fixes the actual defect
  (under-keying) rather than the spelling of the order.
- **Making vk use `VK_EXT_extended_dynamic_state`/`_state3` so both backends move everything.** It would delete
  `RasterState`, but it raises vk's floor to vsg's and destroys the only implementation that tests the seam below
  that floor. Registered as a trigger instead: revisit when no consumer needs a 1.2-class device.
- **Keeping the two vsg declarations and adding `DEPTH_BIAS` to one.** The bug is the drift, not the missing line;
  a second copy would drift again.
- **Renaming `DepthMode` to `DepthPolicy`, `ModelScene` to `SceneBuild`, and unifying the four draw-kind
  enumerations** in the same batch. Each is a vocabulary decision with its own search-and-review surface; they are
  backlog `T51`-`T53`, not consequences of this one.

## Consequences

- `ResolvedRenderState` is exactly the state; a caller building a pipeline key reads `state` + `topology` and cannot
  invent either.
- A vsg pipeline compiled for a vertex layout is now compiled for its widths too; a geometry with a wider custom
  channel gets its own pipeline instead of a wrong format.
- The vk key is one value plus one named extension, and a field added to the portable key is in vk's identity by
  construction (it is the same struct).
- `StateNode::setPolygonOffset` takes effect in the vsg backend; the static path is pinned by
  `ContentPipelineTest.TheEditableHalfIsDeclaredDynamic`, the pixel proof is backlog `T50`.
- The vk `pipelines` map is `unordered_map` (the key has a hash rather than an order); iteration order was never
  observable.
- `ContentPipelineKey`/`RasterState` are backend-private types; no host-visible signature changed, and no
  `[[deprecated]]` alias is kept (the module promises no source stability, `graphics-frame-pipeline.md` 1.3).
