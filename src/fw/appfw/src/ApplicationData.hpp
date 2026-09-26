#pragma once

#include <filesystem>
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

    /// Startup progress sink of this boot; created just before the first phase and destroyed right after the last one
    /// (both by Application::startupSequence(), which owns the boot).
    std::unique_ptr<StartupProgress> startup_progress;

    /// When the host asked the application to run (run()), for the diagnostic that says how long the boot took to reach
    /// the point where the startup work may start. The reader is a subobject that runs from inside the loop, so a local
    /// variable in run() cannot carry the start point.
    QElapsedTimer startup_requested_at;

    int    argc = 0;
    char** argv = nullptr;

    /// JSON file used to persist the ConfigManager; empty disables persistence.
    std::filesystem::path config_file;

    /// Whether the framework loads the plugins during the startup phase (AppConfig::load_plugins).
    bool load_plugins = true;

    virtual ~ApplicationData();
};

VN_APPFW_NS_END
