#pragma once

#include "appfw_global.hpp"

#include <filesystem>
#include <memory>
#include <vector>

#include <vine/Object.hpp>
#include <vine/String.hpp>
#include <vine/raw_ptr.hpp>

V_APPFW_NS_BEGIN

class CommandManager;
class PluginManager;
class ServiceManager;
class ConfigManager;
class ConfigRegistry;
class EventBus;
class MainThreadDispatcher;
class UserIO;
class ApplicationData;

class V_APPFW_API Application : public Object {
    V_OBJECT_META_DECL;

  protected:
    ApplicationData*       dptr();
    const ApplicationData* dptr() const;

    std::unique_ptr<ApplicationData> d;

  public:
    Application(int argc, char** argv);

  protected:
    Application(ApplicationData* data, int argc, char** argv);

    /**
     * @brief Creates the application's UserIO.
     *
     * The base implementation returns a headless ConsoleUserIO; GUI
     * applications override it to return a visual implementation.
     *
     * @return The newly created UserIO, owned by the application.
     */
    virtual UserIO* createUserIO();

    /**
     * @brief Creates and stores the application's UserIO.
     */
    void setupUserIO();

    /**
     * @brief Tears the application down after the main loop has stopped.
     *
     * Runs the shutdown sequence shared by every run() implementation, while the
     * application is still fully alive:
     * 1. unloads the plugins in reverse dependency order (PluginManager::unloadAll()),
     * 2. drains and stops the event bus (EventBus::shutdownGracefully()),
     * 3. persists the configuration when a config file is set (setConfigFile()).
     *
     * Calling it twice is harmless: the plugin list is empty and the bus is
     * already stopped, so only the config save repeats.
     */
    void shutdown();

  public:
    ~Application() override;

  public:
    virtual void init();

  public:
    /**
     * @brief Runs the application.
     *
     * This method starts the application's main loop and blocks until the
     * application is exited. It returns the exit code provided to the exit()
     * method.
     *
     * @return The application's exit code.
     */
    virtual int run();

    /**
     * @brief Requests that the application's main loop stops with the given code.
     *
     * The loop stops once the event being processed finishes; run() then returns
     * the provided code and stops the event bus at that point, so no delivery
     * happens from inside this method. Calling it while no loop is running does
     * nothing.
     *
     * @param code The exit code to return from run().
     */
    void exit(int code);

    /**
     * @brief Returns the application's command manager.
     *
     * The command manager is a singleton that lives with the application and
     * manages the registered commands and their execution.
     *
     * @return The command manager.
     */
    raw_ptr<CommandManager> commandManager() const;

    /**
     * @brief Returns the application's plugin manager.
     *
     * The plugin manager is a singleton that lives with the application and
     * manages the loaded plugins and their interactions.
     *
     * @return The plugin manager.
     */
    raw_ptr<PluginManager> pluginManager() const;

    /**
     * @brief Returns the application's service manager.
     *
     * The service manager is a singleton that lives with the application and
     * manages the registered services and their lifecycle.
     *
     * @return The service manager.
     */
    raw_ptr<ServiceManager> serviceManager() const;

    /**
     * @brief Returns the application's config manager.
     *
     * The config manager is a singleton that lives with the application and
     * manages the application's configuration.
     *
     * @return The config manager.
     */
    raw_ptr<ConfigManager> configManager() const;

    /**
     * @brief Returns the application's config registry.
     *
     * The config registry is a singleton that lives with the application and
     * manages the configuration items.
     *
     * @return The config registry.
     */
    raw_ptr<ConfigRegistry> configRegistry() const;

    /**
     * @brief Enables configuration persistence on the given JSON file.
     *
     * Loads the file immediately when it exists (a missing file is not an error,
     * the defaults apply), and saves the configuration to it during shutdown().
     * Passing an empty path disables persistence again.
     *
     * The path belongs to the application, not to the framework: appfw does not
     * decide where a host stores its settings, it only offers a layout through
     * defaultConfigFile(), which the application builders apply by default.
     *
     * @param file_path JSON file used to persist ConfigManager values.
     * @return true if the file was loaded or does not exist, false on a read error.
     */
    bool setConfigFile(std::filesystem::path file_path);

    /**
     * @brief Returns the file used to persist the configuration.
     *
     * @return The config file path, or an empty path when persistence is disabled.
     */
    const std::filesystem::path& configFile() const;

    /**
     * @brief Returns the organization name assumed when the host sets none.
     *
     * The Application constructor applies it as the process organization name
     * (QCoreApplication::organizationName) when that is still empty, so the
     * per-user paths below are always well formed. A host that needs its own
     * identity sets AppConfig::organization, which the builders apply on top.
     *
     * @return The default organization name.
     */
    static const String& defaultOrganizationName();

    /**
     * @brief Returns this application's data directory.
     *
     * <user data>/appdata/<organization>/<application name>, for example
     * ~/.local/share/appdata/Vine/Vine on Linux and
     * C:/Users/<user>/AppData/Roaming/appdata/Vine/Vine on Windows.
     *
     * The framework reserves two subdirectories: config/ for the persisted
     * configuration (defaultConfigFile()) and logs/ for log files. The directory
     * is only computed, never created here.
     *
     * @return The data directory.
     */
    std::filesystem::path dataDirectory() const;

    /**
     * @brief Returns the configuration file inside dataDirectory().
     *
     * <data directory>/config/<application name>.json. It is loaded and saved
     * when persistence is enabled: the application builders enable it unless
     * AppConfig::persist_config is false (tests), and an explicit
     * AppConfig::config_file or setConfigFile() takes precedence.
     *
     * @return The default config file path.
     */
    std::filesystem::path defaultConfigFile() const;

    /**
     * @brief Returns the root of the plugin-owned data files.
     *
     * <data directory>/plugins. Each plugin owns the subdirectory named after its
     * PluginInfo::name (see PluginLoadContext::dataDirectory()); the name is the
     * plugin identity, so the directory follows a plugin that is installed
     * elsewhere later.
     *
     * This is for files only. A plugin's configuration values stay in the host
     * ConfigManager, registered through PluginLoadContext::registerConfigItem().
     * The directory is only computed, never created here.
     *
     * @return The plugin data root.
     */
    std::filesystem::path pluginDataDirectory() const;

    /**
     * @brief Returns the per-user plugin registration directory (installed.d).
     *
     * <data directory>/installed.d: the directory PluginManager::installPlugin()
     * writes a PluginScope::User registration into. One file per registration,
     * named <id>.plugin, so installing or removing one plugin never rewrites
     * another.
     *
     * @return The registration directory (not created by this call).
     */
    std::filesystem::path pluginRegistrationDirectory() const;

    /**
     * @brief Returns the system-wide plugin registration directories (installed.d).
     *
     * The per-machine data roots (Windows ``%ProgramData%``, Linux
     * ``/usr/local/share`` and ``/usr/share``, macOS ``/Library/Application
     * Support``), each with the same appdata/<organization>/<application>/
     * installed.d layout as the per-user directory. They hold the registrations
     * installed for every user, in preference order; writing there needs
     * administrator rights, so they are read here.
     *
     * @return The system registration directories, possibly empty.
     */
    std::vector<std::filesystem::path> allUsersPluginRegistrationDirectories() const;

    /**
     * @brief Returns the application's event bus.
     *
     * The event bus is a singleton that lives with the application and
     * facilitates communication between different parts of the application.
     *
     * @return The event bus.
     */
    raw_ptr<EventBus> eventBus() const;

    /**
     * @brief Returns the application's main thread dispatcher.
     *
     * The main thread dispatcher is a singleton that lives with the application and
     * is used by the event bus for main/auto delivery.
     *
     * @return The main thread dispatcher.
     */
    raw_ptr<MainThreadDispatcher> mainThreadDispatcher() const;

    /**
     * @brief Returns the application's user I/O interface.
     *
     * The user I/O interface is a singleton that lives with the application and
     * provides a way to interact with the user (e.g., console, GUI).
     *
     * @return The user I/O interface.
     */
    raw_ptr<UserIO> userIO() const;

    int argc() const;

    char** argv() const;

    /**
     * @brief Returns whether a long-running operation is in progress.
     *
     * While busy, the framework refuses new top-level commands with a
     * "another operation is in progress" result (see CommandFlags::LongRunning)
     * and the UI may show a progress bar / disable actions.
     *
     * @return true while a progress-host-backed operation is running.
     */
    bool isBusy() const;

  public:
    static raw_ptr<Application> current();
};

/*
 * @brief Returns the current application instance.
 */
inline raw_ptr<Application> getApp()
{
    return Application::current();
}

V_APPFW_NS_END
