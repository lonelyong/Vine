#pragma once

#include "appfw_global.hpp"

#include "command_export.hpp"
#include "Plugin.hpp"

/**
 * @brief Export visibility for plugin entry points.
 *
 * Vine plugin DLLs use this to export their entry points. The header is meant
 * for plugin authors only; it must not be included by appfw itself.
 */
#if defined(_WIN32) || defined(_WIN64)
#    define VN_PLUGIN_EXPORT __declspec(dllexport)
#else
#    define VN_PLUGIN_EXPORT __attribute__((visibility("default")))
#endif

extern "C" {
    /**
     * @brief ABI handshake: the first entry point the host calls on a plugin.
     *
     * Reports what the library was compiled with (see PluginAbi). The host calls
     * this before reading anything else the plugin declares, and refuses the library
     * when the revision differs or when the entry point is missing entirely - which
     * is what a library built before this handshake existed looks like.
     *
     * @return The library's build description; never null.
     */
    VN_PLUGIN_EXPORT const vn::appfw::PluginAbi* vinePluginAbi();

    /**
     * @brief Plugin query entry point; returns the plugin metadata.
     *
     * No side effects; doubles as the "is this a Vine plugin" detection. The host
     * only calls it after vinePluginAbi() reported a compatible revision.
     *
     * @return The plugin metadata, or nullptr.
     */
    VN_PLUGIN_EXPORT const vn::appfw::PluginInfo* vinePluginQuery();

    /**
     * @brief Plugin create entry point; returns the DLL-global plugin instance.
     *
     * The instance is owned by the DLL and returned on every call.
     *
     * @return The plugin instance.
     */
    VN_PLUGIN_EXPORT vn::appfw::Plugin* vinePluginCreate();

    /**
     * @brief Registers the plugin's commands (VN_DECLARE_COMMAND) with the host.
     *
     * Runs inside the plugin module and flushes that module's own command queue
     * (see VN_DEFINE_MODULE_COMMAND_QUEUE()), so exactly the commands this plugin
     * declared are registered. Called by the PluginManager while it loads the
     * plugin; a plugin that is only discovered - disabled, skipped or otherwise not
     * loaded - never reaches this entry point, and its commands stay in its own
     * queue instead of being registered by another plugin.
     *
     * @param manager Command manager to register into.
     */
    VN_PLUGIN_EXPORT void vinePluginRegisterCommands(vn::appfw::CommandManager* manager);
}

/**
 * @brief Defines a plugin's entry points in a plugin DLL.
 *
 * Must be used exactly once per plugin library: besides the four entry points it
 * defines that library's command queue (VN_DEFINE_MODULE_COMMAND_QUEUE()), which is
 * what keeps the commands of one plugin from being flushed by another.
 *
 * The entry points are the ABI handshake (vinePluginAbi, reporting this SDK's
 * VN_APPFW_PLUGIN_ABI_VERSION and VN_APPFW_VERSION), the metadata query, the create
 * entry and the command registration. Adding the handshake changed no macro
 * argument, but a library built before it is refused by a current host, so plugins
 * must be rebuilt.
 *
 * Usage (PluginDependencies is a braced list, empty when the plugin has no
 * dependencies):
 * @code
 * VN_DECLARE_PLUGIN(MyPlugin, u8"myPlugin", u8"My Plugin", u8"1.0.0", u8"Demo plugin", u8"Vine",
 *                  u8"dev@example.com", u8"https://example.com/myplugin", u8"<svg .../>", { u8"base_plugin" })
 * @endcode
 *
 * @param PluginClass The plugin class (default-constructible, derives Plugin).
 * @param PluginUuid Stable plugin identity (see vn::Uuid::parse).
 * @param PluginName Unique plugin name (identifier).
 * @param PluginDisplayName Human-friendly name shown in the UI; may equal PluginName.
 * @param PluginVersion Plugin version.
 * @param PluginDescription Plugin description.
 * @param PluginVendor Plugin vendor/author.
 * @param PluginEmail Vendor contact address shown in the plugin manager; may be empty.
 * @param PluginRepo Repository or issue tracker URL shown in the plugin manager; may be empty.
 * @param PluginIcon Inline SVG source used as the plugin icon; empty uses the host's default icon.
 * @param PluginDependencies Braced list of plugin names this plugin requires.
 */
#define VN_DECLARE_PLUGIN(PluginClass, PluginUuid, PluginName, PluginDisplayName, PluginVersion, PluginDescription, PluginVendor, PluginEmail, PluginRepo, PluginIcon, PluginDependencies) \
    VN_DEFINE_MODULE_COMMAND_QUEUE()                                                                                  \
    extern "C" VN_PLUGIN_EXPORT const vn::appfw::PluginAbi* vinePluginAbi()                                        \
    {                                                                                                                \
        static const vn::appfw::PluginAbi s_abi{ VN_APPFW_PLUGIN_ABI_VERSION, VN_APPFW_VERSION };                     \
        return &s_abi;                                                                                               \
    }                                                                                                                \
    extern "C" VN_PLUGIN_EXPORT const vn::appfw::PluginInfo* vinePluginQuery()                                      \
    {                                                                                                                \
        static const vn::appfw::PluginInfo s_info{ vn::Uuid::parse(PluginUuid), PluginName, PluginDisplayName, PluginVersion, \
                                                     PluginDescription, PluginVendor, PluginEmail, PluginRepo, PluginIcon,     \
                                                     PluginDependencies };                                           \
        return &s_info;                                                                                              \
    }                                                                                                                \
    extern "C" VN_PLUGIN_EXPORT vn::appfw::Plugin* vinePluginCreate()                                               \
    {                                                                                                                \
        static vn::appfw::Plugin* s_instance = nullptr;                                                            \
        if (s_instance == nullptr) {                                                                                 \
            s_instance = new PluginClass();                                                                          \
        }                                                                                                            \
        return s_instance;                                                                                           \
    }                                                                                                                \
    extern "C" VN_PLUGIN_EXPORT void vinePluginRegisterCommands(vn::appfw::CommandManager* manager)                \
    {                                                                                                                \
        vn::appfw::detail::flushQueuedCommands(vn::appfw::detail::moduleCommandQueue(), manager);                 \
    }
