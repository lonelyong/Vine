#include <atomic>

#include <QCoreApplication>

#include <vine/Exception.hpp>

#include <vine/appfw/Application.hpp>
#include <vine/appfw/CommandManager.hpp>
#include <vine/appfw/ConfigManager.hpp>
#include <vine/appfw/ConfigRegistry.hpp>
#include <vine/appfw/EventBus.hpp>
#include <vine/appfw/PluginManager.hpp>
#include <vine/appfw/ServiceManager.hpp>

#include <vine/appfw/MainThreadDispatcher.hpp>

#include <vine/progress/ProgressHost.hpp>

#include "ApplicationData.hpp"
#include "ConsoleUserIO.hpp"

V_APPFW_NS_BEGIN

static std::atomic<Application*> s_current_app{ nullptr };

V_OBJECT_META_IMPL(Application, Object)

ApplicationData::~ApplicationData() = default;

auto Application::dptr() -> ApplicationData*
{
    return d.get();
}

auto Application::dptr() const -> const ApplicationData*
{
    return d.get();
}

Application::Application(int argc, char** argv)
  : Application(new ApplicationData(), argc, argv)
{}

Application::Application(ApplicationData* data, int argc, char** argv)
  : d(data)
{
    if (s_current_app.load(std::memory_order_acquire) != nullptr) {
        throw Exception(-1);
    }

    s_current_app.store(this, std::memory_order_release);

    dptr()->plugin_manager  = std::make_unique<PluginManager>();
    dptr()->service_manager = std::make_unique<ServiceManager>();
    dptr()->command_manager = std::make_unique<CommandManager>(this);
    dptr()->config_manager  = std::make_unique<ConfigManager>();
    dptr()->config_registry = std::make_unique<ConfigRegistry>();
    dptr()->main_dispatcher = std::make_unique<MainThreadDispatcher>();
    // The marshaller is injected and outlives the bus (ApplicationData declares it
    // before the bus, so it is destroyed after it).
    dptr()->event_bus = std::make_unique<EventBus>(dptr()->main_dispatcher.get());
    dptr()->argc            = argc;
    dptr()->argv            = argv;
}

Application::~Application()
{
    s_current_app.store(nullptr, std::memory_order_release);
}

void Application::init()
{
    if (dptr()->app == nullptr) {
        dptr()->app = new QCoreApplication(dptr()->argc, dptr()->argv);
    }
    setupUserIO();
}

void Application::setupUserIO()
{
    if (dptr()->user_io == nullptr) {
        dptr()->user_io.reset(createUserIO());
        dptr()->user_io->setCommandManager(dptr()->command_manager.get());
    }
}

UserIO* Application::createUserIO()
{
    return new ConsoleUserIO;
}

int Application::run()
{
    const int code = dptr()->app->exec();
    // The main loop has stopped: deliver the events that were published just
    // before the exit (bounded by the timeout), then stop the bus before the
    // subscribers (windows, plugins) start to be torn down.
    eventBus()->shutdownGracefully(EventBus::gracefulShutdownTimeout());
    return code;
}

void Application::exit(int code)
{
    QCoreApplication::exit(code);
}

bool Application::isBusy() const
{
    // Only the foreground operation blocks new commands; background hosts run
    // in parallel and do not make the application busy.
    return vine::progress::ProgressHost::current() != nullptr;
}

raw_ptr<CommandManager> Application::commandManager() const
{
    return dptr()->command_manager.get();
}

raw_ptr<PluginManager> Application::pluginManager() const
{
    return dptr()->plugin_manager.get();
}

raw_ptr<ServiceManager> Application::serviceManager() const
{
    return dptr()->service_manager.get();
}

raw_ptr<ConfigManager> Application::configManager() const
{
    return dptr()->config_manager.get();
}

raw_ptr<ConfigRegistry> Application::configRegistry() const
{
    return dptr()->config_registry.get();
}

raw_ptr<EventBus> Application::eventBus() const
{
    return dptr()->event_bus.get();
}

raw_ptr<MainThreadDispatcher> Application::mainThreadDispatcher() const
{
    return dptr()->main_dispatcher.get();
}

raw_ptr<UserIO> Application::userIO() const
{
    return dptr()->user_io.get();
}

raw_ptr<Application> Application::current()
{
    return s_current_app.load(std::memory_order_acquire);
}

int Application::argc() const
{
    return dptr()->argc;
}

char** Application::argv() const
{
    return dptr()->argv;
}

V_APPFW_NS_END
