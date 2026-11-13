#pragma once

#include <vine/vn_global.hpp>

#ifdef VN_BACKEND_VK_LIB
#    define VN_BACKEND_VK_API VN_EXPORT
#else
#    define VN_BACKEND_VK_API VN_IMPORT
#endif

// WHAT THE NAME SAYS. `backend_vk` has two parts and each part carries one fact:
//   * `backend` - this is an IMPLEMENTATION of the render seam (`vine/graphics/backend/` is the seam itself), not a
//     wrapper library around Vulkan and not part of the host SDK;
//   * `vk`      - it is the DIRECT Vulkan one: it creates its own VkInstance / VkDevice / pipelines instead of driving
//     VulkanSceneGraph, which is what the `vsg` backend does. Both are Vulkan; the suffix names the path taken.
// The namespace mirrors the include path one for one (`vine/backend_vk/` -> `vn::backend_vk`), the same rule
// `vine/meshio/` -> `vn::meshio` and `vine/robotics/io/` -> `vn::robotics::io` follow.
//
// NAMING RULE INSIDE THE MODULE. `Vk*` belongs to Vulkan's own types (`VkDevice`, `VkInstance`, ...), which volk brings
// into every translation unit that includes it - so this plugin's classes are `BackendFactory`, `Plugin` and `Device`,
// never `VkSomething`: the clash would be a compile error, and even where it was not, it would read as a Vulkan type
// rather than as ours.
#define VN_BACKEND_VK_NS_BEGIN \
    namespace VN_ROOT_NS       \
    {                          \
    namespace backend_vk       \
    {

#define VN_BACKEND_VK_NS_END \
    }                        \
    }
