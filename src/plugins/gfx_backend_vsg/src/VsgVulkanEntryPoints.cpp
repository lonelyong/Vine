// volk.h FIRST, and this is the only translation unit in the plugin that includes it. Two reasons, both
// measured rather than assumed:
//   * volk defines VK_NO_PROTOTYPES and pulls the Vulkan headers in itself, and it refuses to be included
//     once <vulkan/vulkan.h> reached it with prototypes (volk.h: "To use volk, you need to define
//     VK_NO_PROTOTYPES before including vulkan.h") — which is exactly how every other TU of this plugin
//     reaches Vulkan, through vsg.
//   * volk.h ends with `using namespace volk;`, and volk declares a pointer variable per extension command.
//     In a TU that also includes vsg's headers, vsg's own unqualified calls to propagated commands become
//     ambiguous (`reference to 'vkGetInstanceProcAddr' is ambiguous`, seen while wiring this up).
// So volk stays behind this one file's boundary, and this TU deals in raw VkDevice / VkInstance rather than
// a vsg::Device (see the header).
#include <volk.h>

#include <vine/vsg/VsgVulkanEntryPoints.hpp>

#include <mutex>

V_VSG_NS_BEGIN

namespace detail
{

namespace
{

/**
 * @brief volk's loader and instance tables, loaded once (volk keeps them in globals).
 *
 * `volkInitialize()` dlopens `libvulkan.so.1` / `vulkan-1.dll` and fills `vkGetInstanceProcAddr`;
 * `volkLoadInstanceOnly()` then fills the instance table, which is where volk's own `vkGetDeviceProcAddr`
 * comes from — the device table cannot be loaded before it. Both are global state, so they run once per
 * instance (a second instance would overwrite them, which is why the instance is remembered here) under a
 * mutex: slot setup can be reached from more than one thread in a session with several passes.
 *
 * A loader that cannot be opened leaves every pointer null, and the callers read that as "no entry points"
 * rather than crashing (see DynamicStateEntryPoints::complete): a session cannot be running at all without a
 * loader, so this is the honest answer for a process that is being torn down or mis-initialised.
 */
void loadInstanceOnce(VkInstance instance)
{
    static std::mutex mutex;
    static bool       loader_ready    = false;
    static VkInstance loaded_instance = VK_NULL_HANDLE;

    std::lock_guard<std::mutex> lock(mutex);
    if (!loader_ready) {
        loader_ready = volkInitialize() == VK_SUCCESS;
    }
    if (loader_ready && loaded_instance != instance) {
        volkLoadInstanceOnly(instance);
        loaded_instance = instance;
    }
}

} // namespace

DynamicStateEntryPoints fetchDynamicStateEntryPoints(VkDevice device, VkInstance instance)
{
    DynamicStateEntryPoints entry_points;

    if (device == VK_NULL_HANDLE || instance == VK_NULL_HANDLE) {
        return entry_points;
    }
    loadInstanceOnce(instance);

    // The per-device table is local on purpose: what a caller carries away is a value (four function
    // pointers), not 10 kB of table, and a second device in the same process loads its own.
    VolkDeviceTable table{};
    volkLoadDeviceTable(&table, device);

    entry_points.set_polygon_mode         = table.vkCmdSetPolygonModeEXT;
    entry_points.set_color_blend_enable   = table.vkCmdSetColorBlendEnableEXT;
    entry_points.set_color_blend_equation = table.vkCmdSetColorBlendEquationEXT;
    return entry_points;
}

} // namespace detail

V_VSG_NS_END
