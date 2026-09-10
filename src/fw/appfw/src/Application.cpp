#include <algorithm>
#include <atomic>
#include <filesystem>
#include <system_error>
#include <utility>

#include <QCoreApplication>
#include <QStandardPaths>
#include <QStringList>

#include <vine/Exception.hpp>

#include <vine/appfw/Application.hpp>
#include <vine/appfw/CommandManager.hpp>
#include <vine/appfw/ConfigManager.hpp>
#include <vine/appfw/ConfigRegistry.hpp>
#include <vine/appfw/EventBus.hpp>
#include <vine/appfw/PluginManager.hpp>
#include <vine/appfw/ServiceManager.hpp>
#include <vine/logging/Log.hpp>

#include <vine/appfw/MainThreadDispatcher.hpp>

#include <vine/progress/ProgressHost.hpp>

#include "ApplicationData.hpp"
#include "ConsoleUserIO.hpp"

V_APPFW_NS_BEGIN

static std::atomic<Application*> s_current_app{ nullptr };

namespace
{

/// Organization assumed when the host does not set one.
constexpr char8_t s_default_organization[] = u8"Vine";

/// Folder inside the data directory that holds the persisted configuration.
constexpr const char* s_config_folder = "config";

/// Folder inside the data directory that holds plugin-owned data files.
constexpr const char* s_plugins_folder = "plugins";

/// Folder inside the data directory that holds installed-plugin registrations.
constexpr const char* s_registration_folder = "installed.d";

/// File name used when the process has no application name at all.
constexpr const char* s_fallback_app_name = "application";

/// Converts a vine::String (UTF-8) to a QString.
QString toQString(const String& text)
{
    return QString::fromUtf8(reinterpret_cast<const char*>(text.data()), static_cast<int>(text.size()));
}

/// Converts a QString to a filesystem path.
std::filesystem::path toPath(const QString& text)
{
    return std::filesystem::path(text.toStdU16String());
}

/// Root that holds the per-user data directories.
std::filesystem::path userDataRoot()
{
    const QString root = QStandardPaths::writableLocation(QStandardPaths::GenericDataLocation);
    if (root.isEmpty()) {
        // No user data location (headless/service accounts): keep the layout
        // valid instead of producing a relative path from an empty root.
        return std::filesystem::temp_directory_path();
    }
    return toPath(root);
}

/**
 * @brief Returns the process application name, or a fallback when unset.
 *
 * Qt defaults the application name to the executable name, so the fallback only
 * applies to a process that cleared it again.
 */
QString applicationNameOrFallback()
{
    const QString name = QCoreApplication::applicationName();
    return name.isEmpty() ? QString::fromUtf8(s_fallback_app_name) : name;
}

} // namespace

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

    // The framework assumes an organization so that the per-user data and config
    // paths (QStandardPaths) are well formed even when the host only names the
    // application. A host identity set before this constructor is kept.
    if (QCoreApplication::organizationName().isEmpty()) {
        QCoreApplication::setOrganizationName(toQString(Application::defaultOrganizationName()));
    }

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
    shutdown();
    return code;
}

void Application::shutdown()
{
    // Commands go first: their frames hold the manager, the user IO and plugin
    // services, and a command that is still suspended when the manager is
    // destroyed would resume into freed memory. The wait is bounded, so a command
    // that ignores its cancellation token cannot hang the teardown; it only makes
    // the teardown unclean, which is what the warning reports.
    if (auto* commands = dptr()->command_manager.get(); commands != nullptr) {
        // A command waiting for user input holds no cancellation token: the read
        // owns it. Nobody is going to answer at this point, so unblock it first,
        // otherwise the drain would sit out its whole bound and fail.
        if (auto* io = dptr()->user_io.get(); io != nullptr) {
            io->cancelPendingInput();
        }
        if (!commands->cancelAllAndWait()) {
            V_LOGW("Application shutdown: command chains did not stop within the drain bound");
        }
    }

    // The main loop has stopped but the application is still fully alive:
    // plugins are unloaded first so they can still publish on a working bus and
    // reach the managers they registered into.
    if (auto* plugins = dptr()->plugin_manager.get(); plugins != nullptr) {
        // Ignored on purpose: a shutdown cannot act on a plugin that threw, and
        // PluginManager::unloadAll() has already logged it.
        static_cast<void>(plugins->unloadAll());
    }

    // Deliver the events that were published just before the exit (bounded by
    // the timeout), then stop the bus before the subscribers (windows, plugins)
    // start to be torn down. A false result is not fatal - the bus is stopped
    // either way - but it means some parked delivery never arrived, which is
    // exactly what a subscriber would otherwise never learn.
    if (!eventBus()->shutdownGracefully(EventBus::gracefulShutdownTimeout())) {
        V_LOGW("Application shutdown: pending events were dropped instead of delivered");
    }

    // Persist last: plugin unload and the final events may still change values.
    if (!dptr()->config_file.empty()) {
        const String path(dptr()->config_file.u8string());
        std::error_code ec;
        std::filesystem::create_directories(dptr()->config_file.parent_path(), ec);
        if (!dptr()->config_manager->save(path)) {
            V_LOGW("Failed to save the configuration to '{}'", dptr()->config_file.string());
        }
    }
}

bool Application::setConfigFile(std::filesystem::path file_path)
{
    dptr()->config_file = std::move(file_path);
    if (dptr()->config_file.empty()) {
        return true;
    }

    std::error_code ec;
    if (!std::filesystem::exists(dptr()->config_file, ec)) {
        return true; // First run: the defaults apply and are saved on shutdown.
    }
    if (!dptr()->config_manager->load(String(dptr()->config_file.u8string()))) {
        V_LOGW("Failed to load the configuration from '{}'; using the defaults", dptr()->config_file.string());
        return false;
    }
    return true;
}

const std::filesystem::path& Application::configFile() const
{
    return dptr()->config_file;
}

const String& Application::defaultOrganizationName()
{
    static const String s_name(s_default_organization);
    return s_name;
}

std::filesystem::path Application::dataDirectory() const
{
    std::filesystem::path dir = userDataRoot();

    const QString organization = QCoreApplication::organizationName();
    if (!organization.isEmpty()) {
        dir /= toPath(organization);
    }
    dir /= toPath(applicationNameOrFallback());
    return dir;
}

std::filesystem::path Application::defaultConfigFile() const
{
    std::filesystem::path file = dataDirectory() / s_config_folder;
    file /= toPath(applicationNameOrFallback() + QStringLiteral(".json"));
    return file;
}

std::filesystem::path Application::pluginDataDirectory() const
{
    return dataDirectory() / s_plugins_folder;
}

std::filesystem::path Application::pluginRegistrationDirectory() const
{
    return dataDirectory() / s_registration_folder;
}

std::vector<std::filesystem::path> Application::allUsersPluginRegistrationDirectories() const
{
    std::vector<std::filesystem::path> directories;

    // standardLocations() lists the per-user location first and the per-machine
    // ones after it; those are the ones a registration for every user goes into.
    const QStringList locations = QStandardPaths::standardLocations(QStandardPaths::GenericDataLocation);
    for (int i = 1; i < locations.size(); ++i) {
        const std::filesystem::path root = toPath(locations.at(i));
        if (root.empty()) {
            continue;
        }

        // Same layout as dataDirectory(), but below the system root: the
        // organization/application part always comes from the process identity.
        std::filesystem::path dir = root;
        const QString         organization = QCoreApplication::organizationName();
        if (!organization.isEmpty()) {
            dir /= toPath(organization);
        }
        dir /= toPath(applicationNameOrFallback());
        dir /= s_registration_folder;

        if (std::find(directories.begin(), directories.end(), dir) == directories.end()) {
            directories.push_back(dir);
        }
    }
    return directories;
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
