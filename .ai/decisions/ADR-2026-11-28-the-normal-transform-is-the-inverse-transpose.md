# ADR-2026-11-28: a normal is turned by the inverse transpose, in one fragment both content stages include

- Status: Accepted
- Date: 2026-11-28
- Related: `../design/graphics-material.md` (2.12, F12, C7, R5, N7, T8), `../design/graphics-shader-params.md` (§2.2,
  §2.8), `shaders/vine_normal.glsl`, `builtin_forward.vert` / `builtin_gbuffer.vert`, `ShaderAbi.hpp`
  (`VineDrawBlock`, `VinePushRange`), review 11 `V2`, review 18 `Y8`

## Background

Both built-in content vertex stages transformed the authored normal with the upper 3x3 of the model-view:

```glsl
vn_view_normal = mat3(model_view) * vn_Normal;
```

That is the normal transform only while the placement is **rigid or uniformly scaled**. A placement is not restricted to
that shape: `PositionAttitudeTransform` states a scale VECTOR, a non-uniform parent scale leaves a shear on a rotated
child, and an instance matrix can carry a non-uniform scale without any node stating one. Under such a placement the
spelling sends the normal toward the stretched axis instead of away from it, and the surface is lit from a direction it
does not have - with no diagnostic, because every mechanism reports health.

The magnitude was measured with a probe fragment writing `abs(normalize(vn_view_normal))` for one authored normal
`(0, 1/√2, 1/√2)` on a quad filling a 64x64 target. A y-stretch of 16 turned the normal to `(0, 0.998, 0.062)`; the correct
answer is `(0, 0.062, 0.998)`. Behind a 30-degree turn the two answers were `(0, 0.833, 0.553)` and `(0, 0.445, 0.896)`.

The same round recorded review 18 `Y8`: `VineDrawBlock::model` (the world model) is packed for every draw and read by no
built-in stage, `vine_params.glsl` called it "the model matrix the host rewrites", and the two vertex stages named a local
`vn_model` that actually held `view * model`.

## Evidence

| Fact | Where |
|---|---|
| A scale vector is a documented, legal placement | `PositionAttitudeTransform.hpp` ("Scale is a vector, so non-uniform scale is legal") |
| An instance matrix can carry a non-uniform scale | `builtin_forward.vert` (`vn_InstanceModel`), review 18 `V2` |
| The old spelling is a 3x3 multiply by the placement itself | `builtin_forward.vert:68`, `builtin_gbuffer.vert:60` before this ADR |
| The draw block's `model` has no built-in reader | `grep "vn_draw.model" src tests` - no hit; review 18 `Y8`, review 19 §6.2 |
| The push range carries `view * model` only | `ShaderAbi.hpp` (`VinePushRange`), `ADR-2026-11-26-host-program-contract-is-the-push-range.md` |

## Decision

1. **The normal transform is `transpose(inverse(mat3(model_view))) * normal`.** A rigid placement and a uniform scale
   produce the same direction as before; a non-uniform scale or a shear now produces the direction the surface has.
2. **The rule has one home**, `shaders/vine_normal.glsl` (`vnTransformNormal(mat4, vec3)`), and **both** content vertex
   stages include it. The depth stage reads no normal and does not include it; a stage that later needs one gets the same
   function rather than a second spelling.
3. **The argument is the COMBINED model-view** - `vn_pc.model_view` multiplied by `vn_InstanceModel` when the draw is
   instanced - because the inverse transpose composes in the same order the transforms do, so an instanced draw with a
   non-uniform instance scale needs no second path.
4. **The local is renamed `vn_model` → `vn_model_view`** in both stages; it always held `view * model`, and review 18
   `Y8` named the misleading name beside the block's different matrix.
5. **`VineDrawBlock::model` stays, and its contract is stated**: it is the one place the world model is published (the
   push range carries none), a host stage that declares the block reads it, and no built-in stage reads it. Its packing
   is unchanged, so no host built against the block changes.

## Rejected options

| Option | Why not |
|---|---|
| Keep `mat3(model_view) * normal` and state "uniform scale only" | The SDK's own transform states a scale vector and the box/picking side honours it; the shading would keep answering a different question from the rest of the engine (`G1`, `G6`). |
| Compute a normal matrix on the CPU and pack it into `VineDrawBlock` | It cannot serve an INSTANCED draw: the instance matrix is per-instance data and never reaches the per-draw packing. Two normal paths (placed vs instanced) is the drift this fix removes (`G1`, `G9`). |
| Add a `VN_NON_UNIFORM_SCALE` variant bit and keep the cheap path | A second pipeline variant per program keyed on a placement property that can change any frame, plus a detection rule for the instance stream. The vertex-stage inverse is one rule for every placement (`G8`, `G10`). |
| Compute `transpose(inverse(...))` per draw in the shader only when `VN_INSTANCED` | Two spellings again, and a placed (non-instanced) node may carry the same non-uniform scale without the define. |
| Remove `VineDrawBlock::model` in this batch (`80 → 16` bytes) | It is the published world model a host stage may read, and removing a public member is an ABI decision with its own consequences; no in-tree consumer is not proof that an out-of-tree program has none (`G8` asks whether a capability should exist, not whether today's tree reads it). Recorded for the next batch that changes the block's shape. |

## Consequences

- **Cost**: one 3x3 inverse per vertex on the two content vertex stages (forward and G-buffer). The depth, full-screen
  and skybox stages are untouched, and every instanced placement is covered without a second path.
- **A zero scale on an axis** collapses the placement's geometry (`PositionAttitudeTransform` states it) and the normal
  along the collapsed axis has no inverse transpose. The previous spelling mapped that normal to zero as well, so the
  collapsed draw's picture was already undefined; the design page records the boundary (`graphics-material.md` N7) rather
  than inventing a fallback.
- **No ABI, size or binding change.** `VineDrawBlock` stays 80 bytes with `params` at 64; the push range is untouched.
- **Witnesses**: `VkContentDrawTest.ANonUniformlyScaledDrawTransformsItsNormalByTheInverseTranspose` drives the shipped
  forward vertex stage and reads three placements in pixels; `EmbeddedShadersTest.TheContentVertexStagesTransformNormalsThroughTheOneFragment`
  pins that both stages carry the one function. Mutations: the old 3x3 spelling reddens the pixels and the structural
  case; dropping `transpose` reddens the turned-stretch pixel (`(0, 58, 248)` instead of `(0, 113, 228)`).
