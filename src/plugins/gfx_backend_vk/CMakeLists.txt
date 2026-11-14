# gfx_backend_vk: the DIRECT Vulkan backend, delivered as an appfw plugin.
#
# WHAT IT IS FOR. The backend that REPLACES VulkanSceneGraph: it creates its own VkInstance / VkDevice / pipelines
# instead of driving VulkanSceneGraph, so the SDK's backend seam gets a second implementation to be judged against.
# The name says that path: "vsg" goes through VulkanSceneGraph, "vk" calls Vulkan itself. It is NOT the default yet -
# the switch is gated (see ADR-2026-11-14-vk-becomes-the-default-target): releaseRenderTarget, readDepthBuffer or a
# stated difference, device-loss reporting, and platform evidence come first.
#
# PLATFORMS. Linux/X11 is the first supported target; Win32 is next and joins through WindowSystem (one
# implementation file, no change to Device). A platform this build does not serve answers UnsupportedSurface.
#
# WHAT IT CONSUMES. The SDK's backend half (vine/graphics/backend/), which is why it defines
# VN_GRAPHICS_BACKEND_CONSUMER: the same opt-in the vsg plugin states. Its shell headers expose no Vulkan type (the
# factory and the plugin entry only name RenderBackend); everything Vulkan-shaped lives under src/internal.
#
# NAMING. Vulkan's own spellings (`VkDevice`, `VkInstance`, ...) belong to the driver layer; this module's classes are
# `vn::backend_vk::Device` / `BackendFactory` / `Plugin` (see backend_vk_global.hpp).
#
# Tests cannot link a MODULE library; tests/test_vk compiles the implementation sources directly, exactly as
# tests/test_vsg does.

vn_add_plugin(VK_BACKEND_TARGET gfx_backend_vk)

# vn_add_plugin derives the export macro from the short name (gfx_backend_vk -> VN_GFX_BACKEND_VK_LIB), but
# backend_vk_global.hpp checks VN_BACKEND_VK_LIB. Define it explicitly so VN_BACKEND_VK_API expands to dllexport when building.
target_compile_definitions(${VK_BACKEND_TARGET} PRIVATE VN_BACKEND_VK_LIB)

# This plugin consumes the backend half of graphics (`vine/graphics/backend/`), so it opts in
# (see that half's BackendHalfGuard.hpp).
target_compile_definitions(${VK_BACKEND_TARGET} PRIVATE VN_GRAPHICS_BACKEND_CONSUMER)

set(CMAKE_POSITION_INDEPENDENT_CODE ON)

target_link_libraries(${VK_BACKEND_TARGET} PRIVATE
    vn::Appfw
    vn::Graphics
    # THE META-LOADER. vk reaches every Vulkan entry point through volk (the loader exports no symbol for the extension
    # commands), and volk's own target carries the Vulkan headers (`Vulkan::Headers`) so the plugin does not link
    # VulkanSceneGraph just to get them.
    tp::volk
    # GLSL -> SPIR-V at run time, through glslang's own front end (M2). Provisioned ONCE for every shader-compiling
    # backend by cmake/VineGlslang.cmake, so this plugin never depends on the vsg plugin being configured first.
    glslang::glslang
    glslang::SPIRV
    # GetDefaultResources(): the resource limits every parse call takes (header-only declaration, library symbol).
    glslang::glslang-default-resource-limits)
