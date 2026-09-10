#pragma once

#include "appfw_global.hpp"

#include <memory>

#include <vine/Object.hpp>
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
     * @brief Exits the application's main loop with the given exit code.
     *
     * This method terminates the application's main loop and causes the run()
     * method to return with the provided exit code.
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
