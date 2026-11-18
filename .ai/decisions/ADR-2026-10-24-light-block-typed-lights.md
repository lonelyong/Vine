# ADR-2026-10-24: the light block becomes a typed light array, both paths share one UBO

- Status: Accepted
- Date: 2026-10-24
- Related: `../design/graphics-shader-params.md` · `../design/graphics-rendering.md` · `../backlog/graphics.md`

## Background

The old `VineLightsBlock` was "1 ambient light + 3 directional lights" (112 B). The content side's `LightType` already had four kinds, while neither `Light` nor `LightRef` had a position; packing flattened the kind (`Ambient` into the ambient slot, `Directional` into 0..2, everything else dropped), and the full-screen path pressed the same numbers into a 128 B push (`LightPushBlock`, whose 16 B `projparms` was always 0 and read by nobody). Three consequences followed:

- the interface promised what it could not express (point and spot lights had names and no data);
- one set of light numbers had two shapes (UBO and push), and above three lights the content path reported the drop while the full-screen path did not;
- the concept "position" had never entered the ABI, so adding a light kind was not adding a field but replacing the layout.

## Options

| Option | Benefit | Cost |
|---|---|---|
| A. Keep directional lights and defer point lights | The ABI stays | Breaks the ABI twice; the enum keeps promising effects that do not exist |
| B. A typed light array, both paths sharing one UBO (chosen) | Right in one change; `LightPushBlock` and the dead field disappear; one packing | The block goes 112 -> 528 B; the full-screen path gains one light-block binding |
| C. Keep pushing on the full-screen path, add kinds to the content path only | A small full-screen change | Two light models; point lights still unusable on the full-screen path |
| D. Do per-light shadows at the same time (point cube / spot perspective) | Complete | An independent capability that would blow up this batch; both the shadow ABI and the light cameras would have to be redone |

## Decision

1. **Block shape**: `VineLightsBlock` = `vec4 ambient` plus five `vec4[8]` parallel arrays (`position_kind` / `direction_range` / `color_intensity` / `cone`), 528 B. No array of structs: the ABI scanner's member table only knows built-in types, and `VineLight lights[8]` would be judged an unmeasurable declaration.
2. **Kind tag**: `position_kind.w` carries the `LightType` value (Directional=1 / Point=2 / Spot=3), an empty slot is -1. `LightType`'s explicit values (0/1/2/3) pin the ABI.
3. **View space**: position and direction are world -> view at packing time, so the shader needs no extra matrix (the same rule as today's direction).
4. **One binding serves both paths**: the light block is at set 0 **binding 5** (the content shadow pair stays at 3/4, which frees 2; the full-screen source colour occupies 0..N-1, depth 4, lights 5, and the shadow pair moves to 6/7). A fragment has no parametrisable binding and the ABI scanner does no macro evaluation, so the same number is the only way to avoid a second declaration.
5. **Both paths use a UBO**: `LightPushBlock`, `packLightPushBlock` and `projparms` are deleted; the full-screen pipeline declares no push range, and `ScreenDraw::push` is left to the host program.
6. **Light selection stays "in declaration order"**, and a dropped light is reported once per episode on both paths (the full-screen path used to be silent).
7. **Shading lands in one place**: point and spot evaluation (direction, distance falloff, cone) lives in `vnEvaluateLight` in `vine_params.glsl`, and forward and deferred each keep only their own material path.
8. **No per-light shadows**: a point or spot light that asks for `setCastShadow` gets no caster (shadows stay the directional light's orthographic camera); the request is reported, not silenced.

## Consequences

- Good: one light model, one packing, one drop rule; the block has a position for the first time; the dead `projparms` budget disappears; the full-screen path reports drops as well.
- Cost: `VineLightsBlock` goes from 112 to 528 B (the light area of `BlockStorage`, the `ProgramAbi` assertions and `LightBlockTest` follow); the full-screen pipeline layout gains one dynamic uniform binding; 8 slots is a hard cap (an excess is reported visibly).
- Reopening condition: per-light shadows, per-object light selection (light tables or clusters), area lights. Each needs its own design and none shares this batch.

## Evidence

- `Light.hpp`: `LightType{Ambient, Directional, Point(reserved), Spot(reserved)}`, and `Light` has only `direction_`;
- `LightRef` in `FrameRecorder.hpp` has only `has_direction` / `direction`;
- `packLightBlock` in `LightBlock.cpp` takes `default: break` for `Point`/`Spot`; <!-- drift-ok -->
  `LightBlockTest.DisabledAndUnusableLightsAreNotPacked` pins "a kind the block has no slot for";
- the comment on the full-screen path in `ContentPass.cpp` says the drop is "deliberately NOT reported";
- `LightBlock.hpp`: `projparms` is RESERVED and always 0, and no shipped program reads it; <!-- drift-ok -->
- `memberTypeLayout` in `ProgramAbi.cpp` returns false for a struct member (which is why the block shape avoids an array of structs).
