#pragma once

#include "vsg_global.hpp"

#include <vsg/vk/vulkan.h>

VN_VSG_NS_BEGIN

namespace detail
{

/**
 * @brief The entry points of the extension commands this backend calls by pointer.
 *
 * These three are the only Vulkan calls the backend cannot make by name: `vkCmdSetPolygonModeEXT`,
 * `vkCmdSetColorBlendEnableEXT` and `vkCmdSetColorBlendEquationEXT` are extension commands, and the loader
 * exports no symbol for extension commands that are not promoted to core — measured, not assumed:
 * `nm -D libvulkan.so.1` has `vkCmdSetCullMode`, `vkCmdSetDepthTestEnable` and `vkCmdSetPrimitiveTopology`
 * (all promoted) and none of these three. A direct call links on Windows and fails to LINK on Linux.
 *
 * So they are loaded (see fetchDynamicStateEntryPoints) and carried BY the command that needs them: the
 * pointers belong to one device, which is why this is a value rather than a process-wide table — two
 * sessions on two devices must not inherit each other's.
 */
struct DynamicStateEntryPoints
{
    PFN_vkCmdSetPolygonModeEXT        set_polygon_mode         = nullptr;
    PFN_vkCmdSetColorBlendEnableEXT   set_color_blend_enable   = nullptr;
    PFN_vkCmdSetColorBlendEquationEXT set_color_blend_equation = nullptr;

    /// @brief Whether every entry point is present (a device that cannot deliver these states is refused).
    [[nodiscard]] bool complete() const noexcept
    {
        return set_polygon_mode != nullptr && set_color_blend_enable != nullptr && set_color_blend_equation != nullptr;
    }
};

/**
 * @brief Loads the entry points of @p device with volk (third_party/volk) and returns them.
 *
 * volk is a meta-loader: `volkInitialize()` dlopens the loader, `volkLoadInstanceOnly()` fills the instance
 * table (including the `vkGetDeviceProcAddr` volk itself needs), and `volkLoadDeviceTable()` fills a
 * per-device table of every device entry point — ours included, without naming any of them by hand. The
 * device table is LOCAL to this call, because what a caller needs from it is a value it can carry (see
 * DynamicStateEntryPoints); a future extension command plugs in here by copying one more member out of it.
 *
 * volk is built with `VOLK_NAMESPACE` and `VOLK_NO_DEVICE_PROTOTYPES` (see third_party/volk/CMakeLists.txt).
 * The first is required, not cosmetic: this application also calls Vulkan BY NAME through vsg, and volk
 * declares a pointer variable for every command — without its namespace those variables are C-linkage
 * symbols named `vkCreateDevice` etc., which would interpose the loader's functions for vsg's calls. With
 * the namespace they are mangled (`nm -D` on the built plugin: `_ZN4volk14volkLoadDeviceEP10VkDevice_T`, and
 * zero exported symbols named like a Vulkan entry point).
 * The second makes "a device command reached without its table" a compile error instead of a null call; it is
 * an INTERFACE-only definition, because volk.c still has to see those globals to fill them.
 *
 * Any entry point the device does not offer comes back null (see DynamicStateEntryPoints::complete): a
 * session refuses such a device when it is created (the extensions and feature bits are requested in
 * makeWindowTraits), so a live session's set is complete.
 *
 * The handles are RAW Vulkan ones rather than a vsg::Device on purpose: this is the one header of the plugin
 * whose implementation includes volk.h, and volk.h pulls the Vulkan headers in with VK_NO_PROTOTYPES and
 * appends `using namespace volk;`. A translation unit that mixed that with vsg's headers would see both
 * volk's pointer variables and vsg's calls to the same names (measured: `reference to
 * 'vkGetInstanceProcAddr' is ambiguous`). So the volk side stays a raw-Vulkan file and every vsg-aware caller
 * passes `device->vk()` / `device->getInstance()->vk()`.
 *
 * @param device   Device whose entry points to load (its instance table is loaded once, see the implementation).
 * @param instance Instance @p device belongs to.
 * @return The entry points; individual ones are null when the device does not offer them.
 */
[[nodiscard]] DynamicStateEntryPoints fetchDynamicStateEntryPoints(VkDevice device, VkInstance instance);

} // namespace detail

VN_VSG_NS_END
