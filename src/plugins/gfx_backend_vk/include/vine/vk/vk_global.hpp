#pragma once

#include <vine/vn_global.hpp>

#ifdef VN_VK_LIB
#    define VN_VK_API VN_EXPORT
#else
#    define VN_VK_API VN_IMPORT
#endif

// The `vk` backend is the DIRECT Vulkan one (see .ai/research/graphics-2026-11-13-gpu-resource-abstraction.md §8): it
// creates its own VkInstance / VkDevice / pipelines instead of driving VulkanSceneGraph, which is what the `vsg`
// backend does. The name states the implementation path, not the API family - both are Vulkan.
//
// NAMING RULE. Inside this module the name `Vk*` belongs to Vulkan's own types (`VkDevice`, `VkInstance`, ...): this
// plugin's classes are `vn::vk::Device`, `vn::vk::BackendFactory`, `vn::vk::Plugin`, never `VkSomething`, because a
// translation unit that includes volk sees both spellings and a clash is a hard error.
#define VN_VK_NS_BEGIN \
    namespace VN_ROOT_NS \
    {                    \
    namespace vk         \
    {

#define VN_VK_NS_END \
    }                \
    }
