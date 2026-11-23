#pragma once

#include <vine/vn_global.hpp>

#ifdef VN_BACKEND_D3D12_LIB
#    define VN_BACKEND_D3D12_API VN_EXPORT
#else
#    define VN_BACKEND_D3D12_API VN_IMPORT
#endif

// WHAT THE NAME SAYS. `backend_d3d12` has two parts and each part carries one fact:
//   * `backend`  - this is an IMPLEMENTATION of the render seam (`vine/graphics/` is the seam itself), not a
//                  wrapper library around Direct3D and not part of the host SDK;
//   * `d3d12`    - it is the DIRECT Direct3D 12 one: it creates its own ID3D12Device / command queues / pipeline
//                  states, with no scene graph between the engine and the driver.
// The namespace mirrors the include path one for one (`vine/backend_d3d12/` -> `vn::backend_d3d12`), the rule
// `vine/backend_vk/` -> `vn::backend_vk` follows.
//
// NAMING RULE INSIDE THE MODULE. `ID3D12*` / `D3D12_*` / `DXGI_*` belong to Direct3D's own types and constants, so this
// plugin's classes are `Device`, `BackendFactory` and `Plugin`, never `D3D12Device` or `I...`: the prefixes are the
// API's, and a class that borrows one reads as an API type rather than as ours (the same rule backend_vk states for
// `Vk*`).
//
// STATUS (2026-11-23): SKELETON ONLY. The directory, this header and the plugin-root CMakeLists are the whole of it;
// no unit exists, nothing is registered with the build, and `src/plugins/CMakeLists.txt` deliberately does not add
// this directory. What a NON-VULKAN family needs that a Vulkan one did not - its shader translator, its exposure gate,
// the resource-state table - is recorded once in
// `.ai/research/graphics-2026-11-23-review-8.md` 4.7; unit names are left to that design rather than invented here.
#define VN_BACKEND_D3D12_NS_BEGIN \
    namespace VN_ROOT_NS          \
    {                             \
    namespace backend_d3d12       \
    {

#define VN_BACKEND_D3D12_NS_END \
    }                           \
    }
