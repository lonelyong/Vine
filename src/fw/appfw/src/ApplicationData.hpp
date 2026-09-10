#pragma once

#include <filesystem>
#include <memory>

#include <vine/appfw/CommandManager.hpp>
#include <vine/appfw/ConfigManager.hpp>
#include <vine/appfw/ConfigRegistry.hpp>
#include <vine/appfw/EventBus.hpp>
#include <vine/appfw/MainThreadDispatcher.hpp>
#include <vine/appfw/PluginManager.hpp>
#include <vine/appfw/ServiceManager.hpp>
#include <vine/appfw/UserIO.hpp>

class QCoreApplication;

V_APPFW_NS_BEGIN

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

    int    argc = 0;
    char** argv = nullptr;

    /// JSON file used to persist the ConfigManager; empty disables persistence.
    std::filesystem::path config_file;

    virtual ~ApplicationData();
};

V_APPFW_NS_END
