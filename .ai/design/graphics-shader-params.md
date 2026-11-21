# graphics shader parameter ABI: block definitions, dedup and names

> **Status**: current (P0 implemented: GLSL declaration dedup + build-time inline; P1 not done, see §9.2)
> **Last verified**: 2026-11-16
> **Scope**: the declaration home, name resolution and the backend's byte-writing contract for the five parameter blocks view / draw / material / shadow / lights.
> **Sources**: `ShaderAbi.hpp` / `LightAbi.hpp` / `shaders/vine_params.glsl` (the current block layout).
> **Backlog**: `../backlog/graphics.md`

## 1. Overview

### 1.1 Problem and background

The five parameter blocks carry all the data between the engine and the shaders, and their sizes are settled (`VineViewBlock` 288 B, `VineDrawBlock` 80 B,
`VineMaterialBlock` 64 B, `VineShadowBlock` 80 B, `VineLightsBlock` 528 B: one ambient light + 8 typed light
slots, see [ADR](../decisions/ADR-2026-10-24-light-block-typed-lights.md)), and the C++ offsets are pinned by `static_assert`.
The question is not "is the layout right" but "how many owners does each declaration have":

- **The GLSL side can have only one declaration**: the parameter blocks are written in one place, `src/viz/graphics/shaders/vine_params.glsl`, and shaders pull them in with
  `#include`, expanded at build time. With two copies, one change misses the other and only half the picture reads the wrong bytes — while the picture still shows something;
- **The shadow block's binding depends on the program**: a full-screen program's low bindings are taken by its own sources, so the single declaration of `VineShadowBlock`
  is `BuiltinShaders::shadowBindings()` (it writes the numbers the caller asks for), not in the fragment;
- **The C++ side spans two modules**: the four dependency-free blocks are in the SDK's `ShaderAbi.hpp`, `VineLightsBlock` and its packing in
  `LightAbi.hpp` — the light block needs `LightValue` / `CameraSnapshot` to fill bytes, so it shares a header with the packing, while
  `ShaderAbi.hpp` stays a leaf (std dependencies only);
- **Names exist only in prose** (P1): `VineDrawBlock::params.x` is opacity, a convention written in a comment with no name in the code;
- **Backends write their own storage directly** (P1): there is no "write bytes" contract, so a second backend would rewrite the packing logic.

All of this is bearable with one backend and one maintainer; when the second backend arrives, each gets copied once.

### 1.2 Goals

- **Each declaration has one owner**: one shared GLSL fragment, one C++ definition.
- Names exist only at wiring time, resolved once into `{buffer, offset, type}`; the hot path has offsets only.
- The backend answers only "how to write bytes".
- An unknown name is an error, not a silent zero.

### 1.3 Non-goals

- No schema generator: an internal ABI, one module, a few changes a year — generation costs more than it returns; revisit with a second shader language.
- No material DSL (own language + compiler).
- No runtime reflection / automatic fill by name.
- No loose uniforms (Vulkan forbids them, see P1).
- No lazy per-draw parameter upload, no specialization-constant optimisation.

### 1.4 One-paragraph conclusion

> One declaration, written once; names exist only at wiring time; the backend writes bytes only.

## 2. Design Points

### 2.1 One declaration, one place

- **GLSL**: the five blocks are declared in `shaders/vine_params.glsl`; a shader declares which it wants with `#define VN_PARAM_<block>`,
  then `#include "vine_params.glsl"`, and the build inlines the fragment into the embedded string. The shadow block is the only exception: its set/binding
  belongs to the program that declares it, the single declaration is `BuiltinShaders::shadowBindings()`.
- **C++**: the **shape** of the four dependency-free blocks is in one place, `ShaderAbi.hpp`; the faces that need model/math values to fill bytes get their own headers:
  `LightAbi.hpp` (`kMaxLights` + `kLightKindEmpty` + `VineLightsBlock` + `packLightBlock` / `packedSlotOf`),
  `MaterialAbi.hpp` (`packMaterialBlock`), `DrawAbi.hpp` (`packDrawBlock`). There is one dependency direction:
  `ShaderAbi.hpp` <- the three fill faces <- backends (see §5.1 C7 / C8).
- **The name table** (P1): written by hand next to the struct, entries use `offsetof` instead of hand-typed numbers, so the compiler guarantees agreement with C++.

Dedup is not a discipline problem: a duplicated declaration is not in front of the maintainer, and "change one, forget the other" is caused by the **structure** — only a structural fix works.

### 2.2 Frequency decides the buffer

| Buffer | Contents | Upload timing |
|---|---|---|
| view | camera matrices, viewport size, time | per view |
| draw | model matrix, opacity, point size, user slots | per draw (ring + dynamic offset) |
| material | base colour / emissive / metallic·roughness·occlusion·alpha cutoff / flags | per material (after dedup) |
| shadow | light-space matrix, shadow parameters | per pass |
| lights | one ambient light + `kMaxLights` typed lights (directional / point / spot; position and direction in view space) | per view |

Two points in the table: `params`'s `x` is the effective opacity (the only transparency channel), `y` is the point size (framebuffer pixels, written into `gl_PointSize` in the vertex stage; decided by `StateNode::setPointSize` or a pass override and after resolution **goes into the draw block, not `PipelineKey`**, so two sizes share one pipeline), `z`/`w` are the user slots. `kMaxLights` (`ShaderAbi.hpp`) is the single definition of light capacity (the GLSL array length, the backend struct size and the drop wording all derive from it, `EmbeddedShadersTest` reads the array length back), it is **not** a content limit (`Scene::addLight` has none, see `graphics-scene.md` §2.7).

Per-vertex and per-instance data **are not parameters**: position / normal / colour / UV go through the canonical attributes, custom channels keep their source
location (≥ 3, avoiding 8 and 12–15, see `ShaderAbi.hpp`).

### 2.3 Names exist only at wiring time

`find("Draw.Opacity")` returns `{Buffer::Draw, 64, Type::Float}`; `Material` and `StateNode` cache the resolution result,
every later write is a byte write at an offset. A name is `block.field`, unique across blocks.

### 2.4 GLSL has one copy

The fragment is merged into each embedded shader's string at build time (`cmake/VineEmbedShaders.cmake`); at runtime no file is needed, no
include resolution, no compiler extension; a user shader that wants the same blocks is given the same fragment (P1 decides whether that is an installed file
or a pre-compile splice).

### 2.5 The backend writes bytes only

The backend contract has two actions: write a range of bytes for a buffer, flush a range for a buffer. How values are computed and when is not the backend's,
it is in engine-side functions such as `packDrawBlock`.

### 2.6 P0 changes no bytes

The first stage's acceptance: sizes, alignment, member offsets, the public interface and the existing tests all unchanged. **Prove equivalence first, then talk about new capabilities.**

### 2.7 Prose conventions become named members

Conventions such as `params.x` / `cam_pos.w` / `frame.w` ("which component of the vec4 is what") become named members on the GLSL side
(`vn_opacity` / `vn_pointSize`); the C++ byte layout is unchanged.

## 3. Features

### 3.1 Feature index

| # | Feature | Behaviour | Status |
|---|---|---|---|
| F1 | Shared GLSL fragment | Five block declarations in one place; a shader selects blocks with guards + `#include` | implemented |
| F2 | C++ definition convergence | The five L1 blocks and their packing are all in the SDK: the shapes in `ShaderAbi.hpp`; the fill faces of lights / material / draw in `LightAbi.hpp` / `MaterialAbi.hpp` / `DrawAbi.hpp`; the backend only decides where the bytes go | implemented |
| F3 | Build-time inline | The fragment is merged into the embedded string, nothing on disk at runtime | implemented |
| F4 | Name resolution (P1) | Hand-written table + `offsetof`; resolve once, cache | not done |
| F5 | Backend contract | `ParameterSink`'s write and flush | not done |
| F6 | `Material` set by name (P1) | Typed overloads + the existing field sugar kept | not done |
| F7 | User extension points | The draw block's named user slots + a reserved binding range | not done |
| F8 | Gates | Fragment list, expansion agreement (unknown `vn_` names optional) | partly implemented |
| F9 | P0 equivalence | Bytes, interfaces, tests all unchanged | implemented |

### 3.2 F1 shared fragment

The parameter blocks are written only in `src/viz/graphics/shaders/vine_params.glsl`; a shader declares only the blocks it really reads, because the program text is
its (variant's) descriptor interface — the backend serves bindings by declaration, and a block nobody reads is still a binding somebody must fill:

```glsl
#define VN_PARAM_MATERIAL
#define VN_PARAM_DRAW
#define VN_PARAM_LIGHTS
#include "vine_params.glsl"
```

The guards are `VN_PARAM_MATERIAL` / `VN_PARAM_DRAW` / `VN_PARAM_LIGHTS`; `#include` must stand alone at the start of a line (P5).
The shadow block (§1.1) and the view block are not in the fragment: no built-in shader declares the view block — the content path implements the view as a 128 B push
range (see `PushRange.hpp`), a program that wants this block declares it with its own layout.

The light block's set/binding is set 0 / binding 6 **shared** by both paths (content set: material 0, slots 1–5, lights 6, shadow 7/8, see `graphics-material.md` §3.3).
2 is therefore free; the full-screen set: source colours 0..N-1, depth 4, lights 5, the shadow pair 6/7), so the fragment writes the same number, no
parameterisable binding needed (the ABI scanner does no macro evaluation either).

### 3.3 F2–F3 C++ convergence and build-time inline

The C++ side stays hand-written with `static_assert`. **Shape** (`ShaderAbi.hpp`) and **the face that fills bytes** are two layers: the four dependency-free blocks are in
`ShaderAbi.hpp`; lights (capacity `kMaxLights`, label `kLightKindEmpty`, shape `VineLightsBlock`, `packLightBlock` /
`packedSlotOf`), material (`packMaterialBlock`) and draw (`packDrawBlock`) each get their own header. Reason: filling needs model/math values,
pulling them into the shape header is a back edge (G2), and every consumer that only reads the shape would pay that include fan-out (G10); the dependency direction is fixed as
`ShaderAbi.hpp` <- `LightAbi.hpp` / `MaterialAbi.hpp` / `DrawAbi.hpp` <- backends. The shadow block is the exception: its input
`ShadowFacts` is in vsg's portable half (`vine/backend_vsg/internal/FrameCompiler.hpp`, D1 not open); each backend packs
its own shadow values, but slot derivation must call
`packedSlotOf`.
The fragment and the structs are still two hand-written copies, guarded by adjacent review and `ProgramAbiTest`'s binding-count/block-size assertions.

Before embedding, `cmake/VineEmbedShaders.cmake` expands the fragment into each shader's string: no include parsing at runtime, nothing on disk,
the packaged form unchanged. The fragment file must be declared in `cmake/VineShaders.cmake`'s `INCLUDES`, otherwise changing the fragment does not rerun generation (P7).
The offline gate `vn_shader_check.sh` compiles and validates every variant with the same expansion script, so "the gate passes" = "what is embedded is what is compiled".

### 3.4 F4 name resolution (P1)

```cpp
struct ParameterDesc { const char* name; Buffer buffer; std::size_t offset; Type type; };
inline constexpr ParameterDesc kParameters[]{
    { "Draw.Opacity", Buffer::Draw, offsetof(VineDrawBlock, opacity), Type::Float },
};
```

The table and the struct live in the same C++ header, `offsetof` makes the compiler guarantee they agree; `find()` looks up once at wiring time and the result is cached.

### 3.5 F5 backend contract

```cpp
class ParameterSink
{
  public:
    virtual void write(ParameterBuffer buffer, std::span<const std::byte> bytes) = 0;
    virtual void flush(ParameterBuffer buffer) = 0;
};
```

The Vulkan backend implements it as a mapped ring + dynamic offset; another backend as cbuffer / `glUniform*`. No backend vocabulary appears at the framework level.

### 3.6 F6–F7 `Material` and user extension points

`Material::set("Material.BaseColor", color)` goes through the name table, the existing `setBaseColor()` field sugar stays and internally goes through the same entry;
an unknown name and a type mismatch both return an error from `set()`. The user extension points are the draw block's two named slots (`Draw.UserSlot0/1`) and a
reserved set/binding range; a user-defined block may only land outside the reserved range, taking it is an error.

### 3.7 F8–F9 gates and equivalence

- The embedded copy matches the source file: `vn_shader_check.sh`'s SHA check already covers it;
- Unknown `vn_` names: added as needed (optional, not a P0 precondition);
- Equivalence: P0 changes no byte and no interface, `BlockStorageTest` / `StateNodeTest` unchanged.

## 4. Public Interface

### 4.1 The shared fragment's contract

The fragment decides: block names, set/binding, member names, types and order; `#ifdef VN_PARAM_<block>` decides which blocks a shader declares. It is
the **only** shader-side declaration besides the C++ structs; any shader copying it again is a new owner. The single exception is the shadow block: it is declared in
`BuiltinShaders::shadowBindings()`, because its set/binding is decided by the program that declares it (§1.1).

### 4.2 C++ types

The five block structs (keeping their names; four in `ShaderAbi.hpp`, `VineLightsBlock` in `LightAbi.hpp`, see §3.3),
the `ParameterBuffer` enum, `ParameterRef{buffer, offset, type}`, `find()`.

### 4.3 Runtime API

| Interface | Contract |
|---|---|
| `ParameterTable::find(name)` | Name → reference; unknown returns empty |
| `Material::set(name, value)` | Validates the type then writes; an unknown name returns an error |
| `ParameterSink::write / flush` | Backend-implemented; the framework does not interpret values |
| `packLightBlock(lights, camera, out)` | Fills the lights one draw packet declared into `VineLightsBlock` by the one rule (view space); returns the number of lights the block represents (not counting the ambient fallback) |
| `packedSlotOf(lights, identity)` | The slot a light takes under the same rule; the shadow block's `params.w` names it with this, avoiding a second slot derivation |
| `packMaterialBlock(values, present_slots, out)` | Fills resolved material values into `VineMaterialBlock` field by field; no clamping or rewriting, the slot mask is given by the caller (otherwise a slot that failed to upload would make the shader sample the stand-in) |
| `packDrawBlock(model, opacity, point_size, out)` | Fills the model matrix (column-major), the effective opacity and the point size into `VineDrawBlock`; the reserved words `params.z/w` are always written 0 |

### 4.4 How a user shader uses it

Built-in and user shaders share the same fragment: a typo → undeclared identifier (compile failure), a duplicate declaration of a block → redefinition
(compile failure). Both failures are visible.

## 5. Constraints, Requirements and Invariants

### 5.1 Constraints (C#)

| # | Constraint |
|---|---|
| C1 | Every block is declared once on the GLSL side: the parameter blocks in `vine_params.glsl`, the shadow block in `shadowBindings()`. |
| C2 | A name is `block.field` and unique across blocks. |
| C3 | An unknown name and a type mismatch must both be errors: at `set()` time and at compile time. |
| C4 | The framework takes a range of set/binding; a user block must live outside the reserved range. |
| C5 | Per-vertex / per-instance data goes through attributes, not into a parameter block. |
| C6 | A fragment may only be pulled in by an `#include` alone at the start of a line, expanded at build time; no include appears in the embedded string. |
| C7 | The SDK's ABI headers have two layers: **shape** (dependency-free, `ShaderAbi.hpp`, std only) and **the face that fills bytes** (`LightAbi.hpp` / `MaterialAbi.hpp` / `DrawAbi.hpp`, which may depend on the model and math). The dependency direction can only be shape ← fill face ← backend; a shape header must not include a model/math header (back edge), a new block's shape goes into `ShaderAbi.hpp` and a fill face that needs values gets its own header. |
| C8 | Each L1 block has one packing function, in the SDK's fill face; the backend only decides where the bytes go (buffer / offset / descriptor) and must not rewrite the field mapping or re-derive slots. An exception must state its reason and be written in §3.3: today only the shadow block (its input `ShadowFacts` is in the backend half), and its slot derivation must still call `packedSlotOf`. |

### 5.2 Requirements (R#)

| # | Requirement |
|---|---|
| R1 | P0 changes no byte, no interface and no test. |
| R2 | The embedded copy agrees with the expanded source, guaranteed by `vn_shader_check.sh`. |
| R3 | The backend writes bytes only, it does not interpret values or resolve names. |

### 5.3 Invariants (N#)

| # | Invariant |
|---|---|
| N1 | Names exist only at wiring time; there is no string in the per-frame loop. |
| N2 | Every block is 16-byte aligned and laid out per std140, so D3D cbuffers and push constants can reuse the same layout. |
| N3 | One byte has one name only; one name points to one byte only. |

## 6. Caveats and Traps

| # | Trap | Symptom / criterion |
|---|---|---|
| P1 | Loose uniforms are illegal in Vulkan | `glslangValidator -V` reports `non-opaque uniforms outside a block` outright; do not wrap it "to look like osg" |
| P2 | A `vec3` followed by a `float` crosses a 16-byte boundary | Use only mat4/vec4/scalar combinations; the struct's `static_assert` pins the offsets |
| P3 | The GLSL declaration copied a second time | Only half the picture reads the wrong bytes, the hardest to find; the criterion is T2's search, the fix is deleting the copy and using the fragment |
| P4 | The fragment and the C++ struct drift | Only this pair is left; guarded by adjacent review, `ProgramAbiTest`'s block-size assertion pins the byte total |
| P5 | The fragment never made it into the embedded string | Only an `#include` starting at the beginning of a line is expanded; an indented one stays as written and glslang reports "include needs an extension" at runtime — visible, not silent |
| P6 | A user declares a block with the same name | Redefinition → compile failure; this is a feature, not a limitation |
| P7 | The fragment changed but embedding did not rerun | The fragment must be in `VineShaders.cmake`'s `INCLUDES`: otherwise the generation rule does not depend on it and the embedded copy silently goes stale; T3's expanded hash is the fallback |
| P8 | A wrong guard (a block declared twice or not at all) | Declared twice → the program gains a binding that must be filled; not declared → undeclared identifier. Both surface in `ProgramAbiTest`'s binding-count assertion or at compile time |
| P9 | Treating the point size (or any value the vertex stage reads) as pipeline state | In Vulkan the point size is the `gl_PointSize` output, not raster state; putting it into `PipelineKey` = one pipeline per size, and changing it would need a rebuild. Its destination is the draw block's `params.y` (floor 1.0, `largePoints` is a device requirement), `PrimitiveDrawTest` pins the whole path by pixel count |

## 7. Rejected Options

| # | Rejected | Reason |
|---|---|---|
| D1 | Runtime reflection filling by name | Names become a protocol: typos, wrong types and wrong layouts all go silent; multiple backends need multiple matchers. See [ADR](../decisions/ADR-2026-10-24-shader-parameter-abi.md). |
| D2 | Loose uniforms (osg `StateSet` style) | Vulkan GLSL forbids them (P1); osg's Vulkan successor vsg has no `Uniform` class either. |
| D3 | A material DSL (own language + compiler) | The gain is automatic variants and cross-backend source, the cost is a compiler; no need today. |
| D4 | A schema generator now | An internal ABI, one module, a few changes a year; after dedup only the "fragment ↔ C++" pair is left and generation costs more than it returns. Reopen when a second shader language appears. See the ADR. |
| D5 | Lazy per-draw parameter upload / specialization constants | The block is written once for many frames, there is nothing to save. |

## 8. Tests and Acceptance

| # | Criterion | Case / command |
|---|---|---|
| T1 | Bytes and interfaces unchanged | `test_graphics` / `test_vsg` all green, `vsg_rewrite_gate.sh` 0 VUID |
| T2 | Each block has one declaration | `grep -rn 'uniform Vine\w*Block' src/`: hits only `shaders/vine_params.glsl` and `shadowBindings()` in `BuiltinShaders.cpp` |
| T3 | The embedded copy agrees with the expanded source | `scripts/vn_shader_check.sh` (expanded hash and byte count) |
| T4 | Unknown names are refused (P1) | `Material::set("Nope", 1.0F)` returns an error |
| T5 | The fragment really made it into the program text | Delete the declaration → undeclared identifier, the build-time gate fails (the same command as T3) |
| T6 | Each program declares only its own blocks | `ProgramAbiTest`: forward 5 bindings, gbuffer 1 |

## 9. Status and Evolution

### 9.1 Current status

P0 is implemented: the parameter blocks have one GLSL declaration (`shaders/vine_params.glsl`, expanded at build time), the C++ shapes of the four dependency-free blocks are in
`ShaderAbi.hpp`, the packing faces of lights / material / draw in `LightAbi.hpp` / `MaterialAbi.hpp` / `DrawAbi.hpp`, and both backends use
the same copy. Bytes and the existing tests are unchanged.

The light block is a "typed light array" (position / direction / range / cone angle, one UBO shared by both paths),
see [ADR](../decisions/ADR-2026-10-24-light-block-typed-lights.md).

### 9.2 Known gaps / backlog

P1 (the name table and `Material::set`, user extension points, `ParameterSink`) is trigger-gated, see
`../backlog/graphics.md` §2.5. The material block is already PBR (see `graphics-material.md` F4), and the texture set / BRDF / instances advance per that file's §9.2 M2–M4.

### 9.3 Compatibility / migration

P0 changes no byte and no interface and can land on its own; P1 only adds interfaces, the existing field sugar stays; the generator is evaluated only when a second shader language appears.

## 10. References

- `shaders/vine_params.glsl` — the only GLSL declaration of the parameter blocks (material / draw / lights, selected by guards).
- `cmake/VineEmbedShaders.cmake` — build-time expansion and embedding; `cmake/VineShaders.cmake`'s `INCLUDES` declares the dependency.
- `ShaderAbi.hpp` — the C++ layout and `static_assert`s of the four dependency-free L1 blocks (a leaf: std only).
- `LightAbi.hpp` — the light block's capacity, shape and only packing rule (`packLightBlock` / `packedSlotOf`).
- `MaterialAbi.hpp` — `packMaterialBlock`: material values → `VineMaterialBlock` (the slot mask is the caller's declaration).
- `DrawAbi.hpp` — `packDrawBlock`: model matrix / opacity / point size → `VineDrawBlock` (column-major, reserved slots always 0).
- Why the three fill faces are not in `ShaderAbi.hpp`: see §3.3 and §5.1 C7.
- `../decisions/ADR-2026-10-24-shader-parameter-abi.md` — why hand-written, why no reflection, why no generator yet.
- glslang `-V`'s `non-opaque uniforms outside a block` as measured (P1).
