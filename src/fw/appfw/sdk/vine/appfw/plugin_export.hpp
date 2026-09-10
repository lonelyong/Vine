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
#    define V_PLUGIN_EXPORT __declspec(dllexport)
#else
#    define V_PLUGIN_EXPORT __attribute__((visibility("default")))
#endif

extern "C" {
    /**
     * @brief Plugin query entry point; returns the plugin metadata.
     *
     * No side effects; doubles as the "is this a Vine plugin" detection.
     *
     * @return The plugin metadata, or nullptr.
     */
    V_PLUGIN_EXPORT const vine::appfw::PluginInfo* vinePluginQuery();

    /**
     * @brief Plugin create entry point; returns the DLL-global plugin instance.
     *
     * The instance is owned by the DLL and returned on every call.
     *
     * @return The plugin instance.
     */
    V_PLUGIN_EXPORT vine::appfw::Plugin* vinePluginCreate();

    /**
     * @brief Registers the plugin's commands (V_DECLARE_COMMAND) with the host.
     *
     * Runs inside the plugin module so its per-module command queue is flushed
     * into the given CommandManager. Called by the PluginManager on load.
     *
     * @param manager Command manager to register into.
     */
    V_PLUGIN_EXPORT void vinePluginRegisterCommands(vine::appfw::CommandManager* manager);
}

/**
 * @brief Defines a plugin's entry points in a plugin DLL.
 *
 * Usage (PluginDependencies is a braced list, empty when the plugin has no
 * dependencies):
 * @code
 * V_DECLARE_PLUGIN(MyPlugin, u8"myPlugin", u8"My Plugin", u8"1.0.0", u8"Demo plugin", u8"Vine",
 *                  u8"dev@example.com", u8"https://example.com/myplugin", u8"<svg .../>", { u8"base_plugin" })
 * @endcode
 *
 * @param PluginClass The plugin class (default-constructible, derives Plugin).
 * @param PluginUuid Stable plugin identity (see vine::Uuid::parse).
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
#define V_DECLARE_PLUGIN(PluginClass, PluginUuid, PluginName, PluginDisplayName, PluginVersion, PluginDescription, PluginVendor, PluginEmail, PluginRepo, PluginIcon, PluginDependencies) \
    extern "C" V_PLUGIN_EXPORT const vine::appfw::PluginInfo* vinePluginQuery()                                      \
    {                                                                                                                \
        static const vine::appfw::PluginInfo s_info{ vine::Uuid::parse(PluginUuid), PluginName, PluginDisplayName, PluginVersion, \
                                                     PluginDescription, PluginVendor, PluginEmail, PluginRepo, PluginIcon,     \
                                                     PluginDependencies };                                           \
        return &s_info;                                                                                              \
    }                                                                                                                \
    extern "C" V_PLUGIN_EXPORT vine::appfw::Plugin* vinePluginCreate()                                               \
    {                                                                                                                \
        static vine::appfw::Plugin* s_instance = nullptr;                                                            \
        if (s_instance == nullptr) {                                                                                 \
            s_instance = new PluginClass();                                                                          \
        }                                                                                                            \
        return s_instance;                                                                                           \
    }                                                                                                                \
    extern "C" V_PLUGIN_EXPORT void vinePluginRegisterCommands(vine::appfw::CommandManager* manager)                \
    {                                                                                                                \
        vine::appfw::detail::registerModuleCommands(manager);                                                        \
    }
