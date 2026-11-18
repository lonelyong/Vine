#pragma once

#include "appfw_global.hpp"

#include <filesystem>
#include <functional>
#include <memory>
#include <stop_token>
#include <type_traits>
#include <utility>
#include <vector>

#include <vine/String.hpp>
#include <vine/Type.hpp>
#include <vine/raw_ptr.hpp>

#include "ConfigStandard.hpp"

VN_APPFW_NS_BEGIN

class Application;
class Command;
class CommandManager;
class ConfigItem;
class ConfigRegistry;
class EventBus;

/**
 * @brief Plugin load context: passed to Plugin::load(), exposing host capabilities.
 *
 * Inside load(), the plugin obtains the config registry via configRegistry(), the
 * command manager via commandManager(), or the host Application via
 * application(). More specific accessors can be added later.
 *
 * Two ports also stamp what the plugin contributes: registerConfigItem() records the item's owner, and
 * registerCommand() attributes a command registered at run time to this plugin (see its note on why a hook must use
 * it instead of CommandManager::registerCommand()).
 */
class VN_APPFW_API PluginLoadContext {
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
     * @brief Returns the cancellation token of the boot this plugin is being loaded by.
     *
     * A plugin does not have to reach for the process-wide startup sink to learn that the boot is being cancelled: the
     * context it is loaded with carries the same token (see StartupProgress::stopToken()). A hook that starts long,
     * UI-free work - the half a plugin sends to the thread pool - should hand this on so that work can stop early, and
     * a hook should check it between its own stages.
     *
     * A plugin loaded outside a boot (a test, a tool that calls PluginManager::load()) gets a token that is never
     * stopped, which is the same answer as "nobody is cancelling this".
     *
     * @return The token of the running boot, or a never-stopped token when there is none.
     */
    std::stop_token stopToken() const;

    /**
     * @brief Config registry: plugins register config items (ConfigItem) here.
     */
    raw_ptr<ConfigRegistry> configRegistry() const;

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
     * Named "ensure" rather than "dataDirectory" on purpose: unlike
     * Application::dataDirectory(), this one creates the directory, and the name
     * says so.
     *
     * @return The plugin data directory, or an empty path when no Application is
     *         set or the directory cannot be created.
     */
    std::filesystem::path ensureDataDirectory();

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
     * @brief Registers a command owned by this plugin.
     *
     * @details
     * - The command manager stamps a registration with the owner tag it reads at that moment; this call sets the tag
     *   to this plugin's name for the duration of the registration only, then restores the previous one.
     * - Use it for every command a lifecycle hook registers. CommandManager::registerCommand() called directly from a
     *   hook depends on a tag the manager would have to keep set while that hook is suspended, and every registration
     *   the main thread delivers in that window would inherit this plugin's name.
     * - Commands a plugin declares statically (VN_DECLARE_COMMAND) need none of this: the manager flushes the
     *   module queue itself, inside the plugin's own scope.
     *
     * @param command_class Meta class of the command.
     * @param name          Unique command name; must not be empty.
     * @param factory       Factory creating a new command instance; must not be empty.
     * @return true when registered; false when there is no command manager, or the name is empty or taken.
     */
    bool registerCommand(TypeId command_class, String name, std::function<Command*()> factory);

    /**
     * @brief Registers a default-constructible command type owned by this plugin.
     *
     * @details
     * - Equivalent to registerCommand(CommandT::desc(), std::move(name), [] { return new CommandT; }).
     *
     * @tparam CommandT Command type; must derive from Command and be default-constructible.
     * @param name      Unique command name.
     * @return true when registered.
     */
    template <typename CommandT>
    bool registerCommand(String name)
    {
        return registerCommand(CommandT::desc(), std::move(name), [] { return new CommandT; });
    }

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


VN_APPFW_NS_END
