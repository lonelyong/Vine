#pragma once

#include "appfw_global.hpp"

#include <filesystem>
#include <memory>
#include <vector>

#include <vine/raw_ptr.hpp>
#include <vine/String.hpp>

#include "ConfigStandard.hpp"

V_APPFW_NS_BEGIN

class Application;
class CommandManager;
class ConfigItem;
class ConfigRegistry;
class EventBus;

/**
 * @brief Plugin load context: passed to Plugin::load(), exposing host capabilities.
 *
 * Inside load(), the plugin obtains the config registry via configs(), the
 * command manager via commandManager(), or the host Application via
 * application(). More specific accessors can be added later.
 */
class V_APPFW_API PluginLoadContext {
  public:
    /**
     * @brief Constructs the load context with Application as the host.
     *
     * @param app Host application.
     * @param plugin_name Name of the plugin this context belongs to.
     */
    explicit PluginLoadContext(Application* app, String plugin_name = {});
    ~PluginLoadContext();

    /**
     * @brief The host Application.
     *
     * @return The Application this context was created for, or nullptr if none.
     */
    raw_ptr<Application> application() const;

    /**
     * @brief Config registry: plugins register config items (ConfigItem) here.
     */
    raw_ptr<ConfigRegistry> configs() const;

    /**
     * @brief Command manager: plugins register their commands here during load().
     *
     * @return The command manager, or nullptr if no Application is set.
     */
    raw_ptr<CommandManager> commandManager() const;

    /**
     * @brief The host publish/subscribe bus.
     *
     * @return The EventBus owned by the host Application, or nullptr if none.
     */
    raw_ptr<EventBus> eventBus() const;

    /**
     * @brief Name of the plugin this context belongs to.
     *
     * @return The plugin name.
     */
    const String& pluginName() const;

    /**
     * @brief Returns this plugin's data directory, creating it on first use.
     *
     * <Application::pluginDataDirectory()>/<plugin name>, i.e.
     * <user data>/<organization>/<application>/plugins/<plugin name>.
     * Plugin-owned files (caches, downloaded content, per-plugin logs, layout
     * state) belong here; the plugin's configuration values stay in the host
     * ConfigManager through registerConfigItem().
     *
     * The directory is keyed by the plugin name, so it follows the plugin
     * wherever its library is installed from, and it is intentionally not removed
     * when the plugin is unloaded or disabled: user data outlives a plugin build.
     * A plugin that needs the path outside a lifecycle call recomputes it as
     * Application::pluginDataDirectory()/<PluginInfo::name>.
     *
     * @return The plugin data directory, or an empty path when no Application is
     *         set or the directory cannot be created.
     */
    std::filesystem::path dataDirectory();

    /**
     * @brief Registers a config item under a standard category/group, owned by
     * this plugin.
     *
     * @param cat Standard category.
     * @param grp Standard group.
     * @param item Item descriptor.
     * @return true if registered, false if the key already exists.
     */
    bool registerConfigItem(StandardCategory cat, StandardGroup grp, const ConfigItem& item);

    /**
     * @brief Config items registered by this plugin.
     *
     * @return The plugin's registered items.
     */
    std::vector<const ConfigItem*> registeredConfigs() const;

  private:
    struct Impl;
    std::unique_ptr<Impl> d;
};

V_APPFW_NS_END
