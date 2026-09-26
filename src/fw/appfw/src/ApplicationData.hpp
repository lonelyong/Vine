#pragma once

#include <filesystem>
#include <functional>
#include <memory>

#include <QElapsedTimer>

#include <vine/appfw/CommandManager.hpp>
#include <vine/appfw/ConfigManager.hpp>
#include <vine/appfw/ConfigRegistry.hpp>
#include <vine/appfw/EventBus.hpp>
#include <vine/appfw/MainThreadDispatcher.hpp>
#include <vine/appfw/PluginManager.hpp>
#include <vine/appfw/ServiceManager.hpp>
#include <vine/appfw/UserIO.hpp>

class QCoreApplication;

VN_APPFW_NS_BEGIN

class StartupProgress;

struct ApplicationData {
    // Members are destroyed in reverse declaration order; the order below is
    // deliberately reversed so destruction matches the legacy explicit delete
    // order (user_io first ... main_dispatcher last).
    std::unique_ptr<MainThreadDispatcher> main_dispatcher;
    std::unique_ptr<EventBus>             event_bus;
    std::unique_ptr<ConfigRegistry>       config_registry;
    std::unique_ptr<ConfigManager>        config_manager;
    std::unique_ptr<CommandManager>       command_manager;
    std::unique_ptr<ServiceManager>       service_manager;
    std::unique_ptr<PluginManager>        plugin_manager;
    std::unique_ptr<UserIO>               user_io;
    QCoreApplication*                     app = nullptr;

    /// Startup progress sink of this boot; only alive between beginStartupProgress() and finishStartup().
    std::unique_ptr<StartupProgress> startup_progress;

    /// Host's startup work, handed over by runStartup(): run once the user interface is up. Empty is legal.
    std::function<void()> startup_work;

    /// Whether the startup phase has moved on (the work ran and the phase ended); the notice and its backstop can both
    /// arrive, and this is what keeps the phase to one move.
    bool startup_started = false;

    /// When runStartup() was called, for the diagnostic that says how long the interface took to come up.
    QElapsedTimer startup_handed_over;

    int    argc = 0;
    char** argv = nullptr;

    /// JSON file used to persist the ConfigManager; empty disables persistence.
    std::filesystem::path config_file;

    virtual ~ApplicationData();
};

VN_APPFW_NS_END
