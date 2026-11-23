# `src/plugins/gfx_backend_d3d12/` — skeleton (no implementation)

**Status 2026-11-23**: directories and the module's global header only. Nothing is registered with the build;
`src/plugins/CMakeLists.txt` deliberately does not add this directory, and `CMakeLists.txt` here refuses
configuration with the reason.

## Where the decisions live

- **Why this backend exists and what a non-Vulkan family changes**: `.ai/research/graphics-2026-11-23-review-8.md`
  §4.7 (the owner's premise + its six consequences). That page is the one home for the argument, so this file does
  not restate it.
- **What is shared with every backend** (the seam): `graphics-architecture.md` 2.9 and
  `docs/guides/graphics-backends.md` §5.
- **The role vocabulary** the classes will take (`Device`, `BackendFactory`, `Plugin`): `graphics-architecture.md`
  2.12's role table.

## Layout (mirrors `gfx_backend_vk/`)

| Path | Holds |
|---|---|
| `include/vine/backend_d3d12/backend_d3d12_global.hpp` | export macro, namespace macros, the module's naming rule |
| `include/vine/backend_d3d12/shell/` | `Plugin` and `BackendFactory` - the only headers a host can name; no Direct3D type |
| `include/vine/backend_d3d12/internal/` | everything `ID3D12*` / `DXGI_*` shaped |
| `include/vine/backend_d3d12/support/` | the one escape hatch a test/tool may use (vk's `support/Handles.hpp` analogue) |
| `src/shell/`, `src/internal/`, `src/support/` | the implementations of the above |
| `CMakeLists.txt` | the wiring, recorded for the day the first unit lands |

Unit **names** are deliberately not created yet: they belong to the design (the resource-state table, the shader
translator, the descriptor/cache layout), and an empty header with a guessed name is a decision made by inertia.
