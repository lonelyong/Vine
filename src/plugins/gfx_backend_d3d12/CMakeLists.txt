# gfx_backend_d3d12: the DIRECT Direct3D 12 backend, delivered as an appfw plugin.
#
# STATUS (2026-11-23): SKELETON ONLY - no unit, and deliberately NOT registered.
#
# `src/plugins/CMakeLists.txt` does not add this directory, and it must not until the first unit lands: an empty
# `vn_add_plugin` would either produce a module with nothing in it or fail at link time, and a target nobody can
# build is worse than a directory nobody builds.
#
# WHY THE DIRECTORY EXISTS NOW. D3D12 and Metal are committed as the next backends (owner's premise, 2026-11-23),
# and that commitment moves three things out of "later": the last Vulkan-shaped type leaves the SDK, the shader
# source language becomes a decision, and a family - not just a backend - needs its own exposure gate and shader
# translator. Those are written once in `.ai/research/graphics-2026-11-23-review-8.md` 4.7; this file only records
# what the wiring will look like, so it does not have to be re-derived.
#
# WHAT IT CONSUMES (the vk plugin's rule, unchanged): the graphics SDK `vine/graphics/` only. The shell headers
# expose no Direct3D type - the factory and the plugin entry name `RenderBackend` and nothing else - and everything
# `ID3D12*` / `DXGI_*` shaped lives under `src/internal/`.
#
# SHAPE WHEN IT LANDS (see src/plugins/gfx_backend_vk/CMakeLists.txt for the worked example):
#   vn_add_plugin(D3D12_BACKEND_TARGET gfx_backend_d3d12)
#   target_compile_definitions(${D3D12_BACKEND_TARGET} PRIVATE VN_BACKEND_D3D12_LIB)
#   target_link_libraries(${D3D12_BACKEND_TARGET} PRIVATE vn::Appfw vn::Graphics <Direct3D 12 headers/loader>
#                                                              <the shader translator>)
#
# The registry name will be "d3d12" (RenderBackendRegistry), the classes `vn::backend_d3d12::Device` /
# `BackendFactory` / `Plugin`, and the tests a `tests/test_d3d12` that compiles these sources directly - a MODULE
# cannot be linked, which is why test_vk and test_vsg do the same.
#
# Tests cannot link a MODULE library; tests/test_d3d12 will compile the implementation sources directly, exactly as
# tests/test_vk and tests/test_vsg do.

message(FATAL_ERROR
    "gfx_backend_d3d12 is a skeleton: it has no sources yet, so it must not be added to the build. "
    "See .ai/research/graphics-2026-11-23-review-8.md 4.7 for what lands first.")
