# Bug: the demo's four G-buffer previews share one pass name, so only the last survives and the layout re-ordering never takes effect

- **Status**: Fixed (2026-11-14, `demo_plugin`)
- **Found**: 2026-11-14
- **Module**: `src/plugins/demo_plugin` (host-side wiring; the engine's `(owner, name)` plan semantics are correct)
- **Files**: `src/plugins/demo_plugin/src/DemoScene.cpp` (`addDemoPipeline`),
  `src/viz/graphics/src/EngineCompile.cpp` (`applyPlanRows`: same owner and name is one row, `setDesc` in place)

## Symptom and impact

The G-buffer preview strip at the top of the deferred demo should show four tiles (albedo / shading normal / occlusion /
view position), but only the **fourth** appeared. After the fix the frame table has four independent rows, and the four
tiles have different means:

| tile | name in the frame table | mean (RGB) |
|---|---|---|
| 0 | `gbuffer_preview_albedo` | (65.2, 65.5, 66.5) |
| 1 | `gbuffer_preview_normal` | (3.0, 18.5, 13.7) |
| 2 | `gbuffer_preview_emissive` (occlusion rides in alpha) | (30.1, 30.1, 30.1) |
| 3 | `gbuffer_preview_view_position` | (12.3, 3.1, 0.0) |

## Root cause

Two problems on top of each other, both rooted in "a plan identifies a row by `(owner, name)`":

1. All four previews were named `gbuffer_preview` with the render control as owner, so `applyPlanRows` read them as four
   declarations of **one row**, and the last one's desc (attachment 3) overwrote the first three in place (`setDesc`). The
   frame therefore held one preview pass, sampling attachment 3.
2. The layout step's "re-arrange the preview rectangles by the source image's aspect ratio" mutated only the `ScreenPass`
   objects the demo held: `previews[i]->setViewport(...)`. Engine nodes are **built from declarations**, and mutating the
   object does not feed back; the re-ordering therefore never took effect (the rectangles in the frame table stayed at the
   original 160x90 slots).

## Fix

- One name per preview, by the channel it shows (albedo / normal / emissive / view position) - under one owner the name is
  the row identity.
- The layout step **re-declares** the pass after `setViewport` (`addDemoPass` applied once more, which updates the same row
  in place), so the rectangle reaches the frame.

## Verification

- `VN_FRAME_PLAN=1`: the frame table goes from one `gbuffer_preview` row to four (order 120-123, each with a different
  rectangle).
- A frame captured with the window widened to 1032x421 shows the four tile means above (all different, none the background
  colour).
- The same scene's render area is **byte-identical** between vk and vsg (0 of 82004 pixels differ); `test_vk` 72/72 and
  `BackendParityTest` 12/12.

## Design constraints

- **G4** (interface semantics): `(owner, name)` is a plan's row identity; the name is not a label. A contributor that wants
  four tiles has to declare four rows.
- **G9** (extensible): the preview strip iterates over attachments, so adding a channel is one more entry in the table and
  does not change the shape of the layout step.
- **G11** (debuggable): the frame table answers "how many rows does this pipeline have, and what are their rectangles" at a
  glance; the symptom can be read straight off the table instead of guessed.
