# ADR-2026-10-24: shader parameters are declared by hand and de-duplicated, with no reflection and no material DSL

- Status: Accepted
- Date: 2026-10-24
- Related: `../design/graphics-shader-params.md`

## Background

Five parameter blocks (view / draw / material / shadow / lights) carry all the data between the engine and the shaders. Their capacities and offsets are settled and pinned by `static_assert`, but "how many owners does each declaration have" had no answer: `VineMaterialBlock` was declared twice inside the built-in shaders, `VineLightsBlock` was defined in the backend, and the names were still prose in comments (`params.x` is opacity).

The question to decide was: **how many places hold the declaration, and by what mechanism the parameters reach the shader.**

## Options

| Option | Benefit | Cost |
|---|---|---|
| A. Runtime reflection, filled in by name | A user declares only what it uses; a new parameter leaves existing shaders alone | The name becomes a protocol: a typo, a wrong type or a wrong layout are all silent; one matcher per backend |
| B. Loose uniforms (the osg `StateSet` shape) | Closest to osg; a single value takes effect | Not legal in Vulkan GLSL; supporting it means wrapping the source or building a default block |
| C. A material DSL (its own language plus a compiler) | Automatic variants, cross-backend source, a statically checkable parameter list | A compiler and a toolchain; there is no requirement today |
| D. Schema generation (C++, GLSL and the name table produced from one list) | Consistency by construction | A generator, a build step and a gate; the internal ABI changes a few times a year, so the cost outweighs the benefit |
| E. Hand-written plus de-duplication (chosen) | One owner per declaration; no generator; the smallest surface | The shared fragment and the C++ struct remain two copies, held together by adjacent review |

## Decision

Chose **E**:

- the GLSL declarations of the five blocks are gathered into one shared fragment that the built-in shaders `#include`, inlined into the embedded strings at build time;
- the C++ block definitions are gathered in one place (whether `VineLightsBlock` moves up is decided separately);
- the name table (P1) sits next to the struct, with `offsetof` entries, so the compiler keeps it in step with C++;
- schema generation (D) is deferred until a second shader language appears.

Reasons:

- **A's failure mode is the most expensive kind in this repository**: an undeclared binding is a silent no-op, an `#ifdef` condition decides a fact, a hard-coded binding reads undefined data - three times the story was "the name did not match, and then nothing was said".
- **B is illegal in Vulkan**: `glslangValidator -V` rejects a non-opaque uniform outside a block; osg's Vulkan successor vsg no longer has an `Uniform` class either.
- **C is ahead of its time**: there is no need for automatic variants or cross-backend source.
- **D's cost lands on an internal ABI only developers maintain**: the real defect was not "nobody checks" but "the declaration exists twice". With the second copy gone, the remaining two places (fragment and struct) are adjacent and can be read side by side by one person.

## Consequences

- Good: the duplicated GLSL declarations disappear (`VineMaterialBlock` is declared once); the C++ definitions are gathered; a new backend writes one `ParameterSink` and one fragment.
- Cost: the shared fragment and the C++ struct are still two hand-written copies, held by adjacent review and, where needed, a layout test.
- Reopening condition: a second shader language appears (HLSL / Metal), or the parameter list exceeds about 20 items and "I only want two of them" becomes a real burden.

## Evidence

- Measured with `glslangValidator -V` on a `uniform float` outside a block: `'non-opaque uniforms outside a block' : not allowed when using GLSL for Vulkan`; the same declaration inside a `std140` block passes.
- No `Uniform` class exists in `build/_deps/vsg-src`, and `state/` holds only descriptors and push constants.
- Today: `VineMaterialBlock` is declared once in `builtin_forward.frag` and once in `builtin_gbuffer.frag`; `VineLightsBlock` is defined in `LightBlock.hpp`, not in the SDK's `ShaderAbi.hpp`. <!-- drift-ok -->
