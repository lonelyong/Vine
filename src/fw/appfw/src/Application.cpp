#include <vine/appfw/Application.hpp>

#include <algorithm>
#include <atomic>
#include <filesystem>
#include <memory>
#include <system_error>
#include <utility>

#include <QCoreApplication>
#include <QMetaObject>
#include <QStandardPaths>
#include <QStringList>

#include <vine/Exception.hpp>
#include <vine/async/DetachedTask.hpp>
#include <vine/logging/Log.hpp>

#include <vine/appfw/CommandManager.hpp>
#include <vine/appfw/ConfigManager.hpp>
#include <vine/appfw/ConfigRegistry.hpp>
#include <vine/appfw/ConfigSnapshot.hpp>
#include <vine/appfw/DocumentManager.hpp>
#include <vine/appfw/EventBus.hpp>
#include <vine/appfw/MainThreadDispatcher.hpp>
#include <vine/appfw/PluginManager.hpp>
#include <vine/appfw/ProgressHost.hpp>
#include <vine/appfw/ServiceManager.hpp>
#include <vine/appfw/StartupProgress.hpp>
#include <vine/appfw/Translation.hpp>

#include "ApplicationData.hpp"
#include "ConsoleUserIO.hpp"
#include "TranslationCatalog.hpp"

VN_APPFW_NS_BEGIN

static std::atomic<Application*> s_current_app{ nullptr };

namespace
{

/// Organization assumed when the host does not set one.
constexpr char8_t kDefaultOrganization[] = u8"Vine";

/// Folder inside the data directory that holds the persisted configuration.
constexpr const char* kConfigFolder = "config";

/// Folder inside the data directory that holds plugin-owned data files.
constexpr const char* kPluginsFolder = "plugins";

/// Folder inside the data directory that holds installed-plugin registrations.
constexpr const char* kRegistrationFolder = "installed.d";

/// File name used when the process has no application name at all.
constexpr const char* kFallbackAppName = "application";

/// Converts a vn::String (UTF-8) to a QString.
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
    return name.isEmpty() ? QString::fromUtf8(kFallbackAppName) : name;
}

/// Exit code of a failed boot: a startup exception is fatal (see startupSequence()), so run() returns this code and
/// the process exits with it.
constexpr int kStartupFailureExitCode = 1;

/**
 * @brief Holds the process-wide Application slot open while an Application is being constructed.
 *
 * A constructor that throws never runs its class destructor, so storing `this` and then building the managers would
 * leave the slot pointing at an object that was destroyed member by member: Application::current() would hand out a
 * dangling pointer for the rest of the process. The constructor commits the slot as its last statement; until then an
 * exception withdraws it again.
 */
class CurrentApplicationSlot {
  public:
    /**
     * @brief Takes responsibility for the slot the caller has just filled.
     *
     * @param application The Application the slot points at.
     */
    explicit CurrentApplicationSlot(Application* application) noexcept
      : application_(application)
    {}

    ~CurrentApplicationSlot()
    {
        if (application_ != nullptr) {
            s_current_app.store(nullptr, std::memory_order_release);
        }
    }

    CurrentApplicationSlot(const CurrentApplicationSlot&)            = delete;
    CurrentApplicationSlot& operator=(const CurrentApplicationSlot&) = delete;

    /// Keeps the slot: the object was constructed.
    void commit() noexcept
    {
        application_ = nullptr;
    }

  private:
    /// What to withdraw again when construction does not complete; null once committed.
    Application* application_;
};

} // namespace

VN_OBJECT_META_IMPL(Application, Object)

ApplicationData::~ApplicationData() = default;

auto Application::dptr() -> ApplicationData*
{
    return d.get();
}

auto Application::dptr() const -> const ApplicationData*
{
    return d.get();
}

Application::Application(const AppConfig& config, int argc, char** argv)
  : Application(new ApplicationData(), config, argc, argv)
{
    // The process may hold only one Qt application object, and the base cannot know which one a leaf wants: a headless
    // host gets a QCoreApplication, a GUI a QApplication (see GuiApplication). Either leaf stores it in the application
    // data, where it is a private detail, and then calls initialize() - which is why no Qt type appears in the
    // framework's public headers. Owning it is what lets a process run one host after another: the object goes away with
    // the Application that made it.
    //
    // A throw in this body (the Qt object, initialize()) leaves a partially built object, and whether a delegating
    // constructor's destructor runs in that case is a language subtlety - so the process-wide slot is withdrawn here as
    // well. Clearing it twice is harmless; leaving it set would hand out a dangling Application for the rest of the
    // process.
    CurrentApplicationSlot slot(this);
    dptr()->app = std::make_unique<QCoreApplication>(dptr()->argc, dptr()->argv);
    initialize(config);

    slot.commit();
}

Application::Application(int argc, char** argv)
  : Application(AppConfig{}, argc, argv)
{}

Application::Application(ApplicationData* data, const AppConfig& config, int argc, char** argv)
  : d(data)
{
    if (s_current_app.load(std::memory_order_acquire) != nullptr) {
        // One Application per process: the managers and the static registries reach it through Application::current(),
        // and a second one would make every one of those reads ambiguous. Name the rule - a host that trips over it has
        // to be able to read why, which a bare code cannot say.
        throw Exception(Exception::INVALID_OPERATION, u8"only one Application may exist per process");
    }

    // Published before the managers are built (they read it while they are constructed) and withdrawn again if one of
    // them throws: see CurrentApplicationSlot.
    s_current_app.store(this, std::memory_order_release);
    CurrentApplicationSlot slot(this);

    dptr()->argc = argc;
    dptr()->argv = argv;

    // The process identity comes first, because everything below reads it: the per-user data and config paths
    // (QStandardPaths), the configuration file, and the startup frame, which resolves an empty title to the application
    // name. The framework assumes an organization so those paths are well formed even when the host only names the
    // application; an identity the host set before this constructor is kept.
    if (!config.organization.empty()) {
        QCoreApplication::setOrganizationName(toQString(config.organization));
    }
    else if (QCoreApplication::organizationName().isEmpty()) {
        QCoreApplication::setOrganizationName(toQString(Application::defaultOrganizationName()));
    }
    if (!config.name.empty()) {
        QCoreApplication::setApplicationName(toQString(config.name));
    }

    dptr()->load_plugins = config.load_plugins;

    // The about text and the process identity are one fact: an unnamed AppInfo takes the application name.
    dptr()->app_info = config.info;
    if (dptr()->app_info.name.empty()) {
        dptr()->app_info.name = config.name.empty() ? String::fromUtf8(kFallbackAppName) : config.name;
    }

    dptr()->plugin_manager   = std::make_unique<PluginManager>();
    dptr()->service_manager  = std::make_unique<ServiceManager>();
    dptr()->command_manager  = std::make_unique<CommandManager>(this);
    dptr()->config_manager   = std::make_unique<ConfigManager>();
    dptr()->config_registry  = std::make_unique<ConfigRegistry>();
    dptr()->document_manager = std::make_unique<DocumentManager>(this);
    dptr()->main_dispatcher  = std::make_unique<MainThreadDispatcher>();
    // The marshaller is injected and outlives the bus (ApplicationData declares it
    // before the bus, so it is destroyed after it).
    dptr()->event_bus        = std::make_unique<EventBus>(dptr()->main_dispatcher.get());

    slot.commit();
}

Application::~Application()
{
    s_current_app.store(nullptr, std::memory_order_release);
}

void Application::initialize(const AppConfig& config)
{
    setupUserIO();

    // An explicit file always wins; otherwise the framework layout under the user's data directory is used unless the
    // host opted out (tests, tools). A file that cannot be read is logged by setConfigFile() and the defaults apply.
    if (!config.config_file.empty()) {
        static_cast<void>(setConfigFile(config.config_file));
    }
    else if (config.persist_config) {
        static_cast<void>(setConfigFile(defaultConfigFile()));
    }

    // The values this run starts with, taken before anything applies them: that is the baseline pendingRestartKeys()
    // measures against, and it has to be the stored state rather than a resolved result.
    dptr()->startup_config = dptr()->config_manager->snapshot();

    // The language comes before anything that builds a window, and before the items are registered: an item's label is
    // translated text itself, so registering it earlier would freeze it in the wrong language. The setting is the
    // configuration item - readable only now, after the file above was loaded - and an absent value follows the system.
    // The two catalogs live in the data block, so the process gets a clean slate when the application goes away.
    dptr()->language_setting  = dptr()->config_manager->getString(languageConfigKey(), kLanguageSystem);
    dptr()->language_tag      = resolveLanguageTag(dptr()->language_setting);
    dptr()->qt_catalog        = TranslationCatalog::forQt(dptr()->language_tag);
    dptr()->framework_catalog = TranslationCatalog::forFramework(dptr()->language_tag);
    VN_LOGI("UI language '{}' (setting '{}'): Qt catalogs {}, framework catalog {}",
            dptr()->language_tag.as_std_str(),
            dptr()->language_setting.as_std_str(),
            dptr()->qt_catalog.isInstalled() ? "loaded" : "absent",
            dptr()->framework_catalog.isInstalled() ? "loaded" : "absent");
    static_cast<void>(dptr()->config_registry->addItem(StandardCategory::General, StandardGroup::Language, languageConfigItem()));
}

void Application::setupUserIO()
{
    if (dptr()->user_io == nullptr) {
        dptr()->user_io.reset(createUserIO());
        dptr()->user_io->setCommandManager(dptr()->command_manager.get());
    }
}

StartupProgress* Application::beginStartupProgress()
{
    // A boot starts un-abandoned: the flag belongs to one boot, and shutdown() sets it for the boot it interrupted.
    dptr()->boot_abandoned = false;
    if (dptr()->startup_progress == nullptr) {
        dptr()->startup_progress = std::make_unique<StartupProgress>();
    }
    return dptr()->startup_progress.get();
}

UserIO* Application::createUserIO()
{
    return new ConsoleUserIO;
}

vn::async::Task<void> Application::startupStart()
{
    co_return; // Nothing to put up by default: a headless host has no face for the boot; a leaf (the GUI) shows its
               // frame here and waits for the first paint.
}

vn::async::Task<void> Application::startup()
{
    auto* const d = dptr();

    // The framework's own startup work comes first: the plugins are what the host's work most easily depends on
    // (commands, services and UI are what they register), and "load the plugins" is a step every application has to
    // take - it should not have to be rewritten in every main (turn AppConfig::load_plugins off to manage it).
    // The timing is the contract: this runs after the startup frame painted its first frame (waited for by
    // startupStart()), so a plugin that builds windows and render surfaces finds a window system able to dispatch its
    // notices already (on X11, no dispatch means an empty window). It also has to run on the main thread: a plugin
    // builds its windows and views in load().
    if (d->load_plugins && d->plugin_manager != nullptr) {
        // The asynchronous door (loadAllAsync()): the plugin lifecycle has to run on the application thread (a plugin
        // builds windows and views in load()), so "a long operation does not block the event loop" can only come from
        // splitting the work - the plugin sends the half that does not touch the UI to the pool (`vn::async::run`),
        // and the loop keeps turning until it comes back, which is also how the boot's progress is reported from that
        // half.
        // Progress is per PHASE: the manager names a stage before each pass and relabels for each plugin (finding /
        // creating / loading / finishing), and the startup frame repaints itself synchronously on every report (see
        // BootSplash::onStartupChanged()) - so a phase boundary is always visible, while the continuous occupancy
        // INSIDE a phase (the session init() takes a couple of hundred milliseconds) leaves the picture still. That is
        // deliberate: the startup phase does not turn the event loop for it (that would run everyone else's timers
        // too, see the comment on PluginManager::loadAllAsync()).
        const bool loaded = co_await d->plugin_manager->loadAllAsync();

        // The loop may have stopped while the load was running: the boot is abandoned then, and "some plugins failed"
        // would blame the plugins for a shutdown (see bootAbandoned()).
        if (bootAbandoned()) {
            co_return;
        }
        if (!loaded && !startupCancelled()) {
            // A cancellation is not a failure: the manager also returns false when it stops early. And a plugin that
            // merely failed to load is not one either - the application starts without it.
            VN_LOGW("Some plugins failed to load; the application starts without them");
        }
    }

    co_return;
}

bool Application::startupCancelled() const
{
    const StartupProgress* const progress = dptr()->startup_progress.get();
    return progress != nullptr && progress->stopToken().stop_requested();
}

bool Application::bootAbandoned() const
{
    // Two ways a boot stops being this application's business: shutdown() found it in flight and closed the reporting
    // door while the application is still alive, or the application is already gone and one of the boot's detached
    // frames is running here. The identity is checked first, because reading anything off a destroyed Application would
    // be a use-after-free while comparing the pointer is not.
    return Application::current() != this || dptr()->boot_abandoned;
}

void Application::cancelStartup()
{
    // The user (or the host) does not want to wait for this boot: that is not a failure, so it does not take
    // failStartup()'s non-zero exit - but it must not pretend the boot succeeded either. The main window never goes up,
    // the plugins that were installed are unloaded in reverse dependency order (what a plugin built in load() must not
    // stay in the process), the reporting door is closed, and the loop stops with 0: run() returns, shutdown() runs as
    // usual (by then the plugin list is empty).
    VN_LOGI("startup cancelled: the application stops without finishing the boot");

    if (auto* const plugins = dptr()->plugin_manager.get(); plugins != nullptr) {
        static_cast<void>(plugins->unloadAll());
    }

    endStartupProgress();

    // Stop the loop with 0 (not a failure): run() returns at once and shutdown() runs as usual. The qualification is
    // deliberate - this exit() has to be this class's (which reroutes a call from another thread), not ::exit().
    Application::exit(0);
}

void Application::failStartup()
{
    VN_LOGE("startup failed: the application exits with code {}", kStartupFailureExitCode);

    // Close the reporting door first: nobody may keep reporting into a dead boot (the presenters go back to "what is
    // next" with it).
    endStartupProgress();

    // Stop the loop with a non-zero code: run() returns at once, the process exits with it, and shutdown() still runs
    // to the end (commands, plugins and the configuration are torn down cleanly).
    Application::exit(kStartupFailureExitCode);
}

void Application::endStartupProgress()
{
    // The sink is destroyed, not just completed: a live host would stay in the foreground stack and shadow the progress
    // of every command that runs afterwards.
    if (dptr()->startup_progress != nullptr) {
        dptr()->startup_progress->complete();
        dptr()->startup_progress.reset();
    }
}

vn::async::Task<void> Application::startupEnd()
{
    co_return; // Nothing to put away by default: a leaf (the GUI) takes its frame down and shows the main window here.
}

int Application::run()
{
    auto* const d = dptr();

    // The clock starts when the host asked the application to run, so the boot's diagnostics share one origin (a
    // headless host has no reader, but reading the clock once more costs nothing).
    d->startup_requested_at.start();

    // The language was resolved in initialize(), when the host's log sink was usually not installed yet (hosts install
    // it after the constructor), so it is reported again here: the startup log has to answer "why is the UI in this
    // language", and `system` only means something next to the tag it resolved to.
    VN_LOGI("UI language '{}' for setting '{}'", d->language_tag.as_std_str(), d->language_setting.as_std_str());

    // Let the loop run first and push the startup phase onto it: the window system's half (X11's expose, Windows'
    // WM_PAINT) is dispatched by the loop anyway, and a window shown before the loop runs has not a single pixel -
    // while the boot's first door is "the startup frame really is on screen". Two things follow: the headless and the
    // windowed boot walk one time line (one event queue) and differ only in what goes on screen, and the host's work
    // runs on the pool so the loop never has to stop for it.
    //
    // The step has to be queued: without it the loop would serve the host with nothing on screen and nothing loaded,
    // and no phase would ever run. Only a leaf that used the protected constructor and did not store the Qt
    // application object can get here without one, which is what this check reports.
    if (d->app == nullptr
        || !QMetaObject::invokeMethod(d->app.get(), [this] { static_cast<void>(startupSequence()); }, Qt::QueuedConnection)) {
        VN_LOGE("the startup step could not be queued: the application has no Qt application object");
        return kStartupFailureExitCode;
    }

    // Tearing down while the loop still runs is a usage error (see shutdown()): this flag is what that line reads.
    d->loop_running = true;
    const int code  = d->app->exec();
    d->loop_running = false;

    shutdown();
    return code;
}

vn::async::DetachedTask Application::startupSequence()
{
    // The three phases laid out flat (startupStart() → startup() → startupEnd()), readable like sequential code.
    //
    // They cannot be three bare calls: a phase returning is not a phase being done (the GUI's first one waits for the
    // startup frame to really paint). The coroutine straightens "wait for the next phase" into `co_await`: the event
    // loop gets the control back while the boot waits - which is what "wait for it without holding the message loop"
    // means.
    //
    // After every phase comes a `resumeOnMainThread()`: a phase may finish on another thread (a timer, I/O, the host's
    // own work posted to the pool), and both the next phase and the end of this boot belong on the application thread.
    // On the application thread it is a no-op, so the fast path pays nothing.
    //
    // Two things end a boot before its last phase without it being a failure: a cancellation (asked for through the
    // sink, or thrown by token-aware work as TaskCancelledException) and abandonment (the loop stopped while a phase was
    // waiting). Only a real exception takes failStartup().
    beginStartupProgress()->stage(translate("appfw", "Starting"));

    bool ok        = true;
    bool cancelled = false;
    try {
        co_await startupStart();
        co_await MainThreadDispatcher::resumeOnMainThread();
        if (bootAbandoned()) {
            co_return;
        }
        if (startupCancelled()) {
            cancelStartup();
            co_return;
        }
        co_await startup();
        co_await MainThreadDispatcher::resumeOnMainThread();
        if (bootAbandoned()) {
            co_return;
        }
        if (startupCancelled()) {
            cancelStartup();
            co_return;
        }
        co_await startupEnd();
        co_await MainThreadDispatcher::resumeOnMainThread();
    }
    catch (const Exception& error) {
        // A cancellation can reach here as an exception: the async library spells "the user asked to stop" as
        // TaskCancelledException (code CANCELLED), and a host that hands the boot's token to sleepFor()/whenAll()/
        // withCancellation() gets exactly that. Classifying it as a failure would turn a cancel into exit(1), which is
        // the one thing this shape must not do - the command layer classifies the same exception the same way.
        if (error.code() == Exception::CANCELLED) {
            cancelled = true;
        }
        else {
            VN_LOGE("the startup phase failed: {}", error.what());
            ok = false;
        }
    }
    catch (const std::exception& error) {
        VN_LOGE("the startup phase failed: {}", error.what());
        ok = false;
    }
    catch (...) {
        VN_LOGE("the startup phase failed with an unknown exception");
        ok = false;
    }

    // A startup exception is NOT swallowed: a failure in any phase (including one the host's own startup work carries
    // up) closes the reporting door and stops the loop with a non-zero code (see failStartup()) - a failed boot must
    // not leave a half-started application running. A cancellation takes the other exit (cancelStartup(): not a
    // failure, but not a finished boot either). The teardown belongs on the application thread as well, and `co_await`
    // cannot be written inside a catch handler (the failing phase may have thrown on another thread), so both paths
    // rejoin after the try.
    if (!ok || cancelled) {
        co_await MainThreadDispatcher::resumeOnMainThread();
        if (bootAbandoned()) {
            // The loop is gone and shutdown() has already closed the door: there is nothing left to stop, and both
            // exits below would only try to stop a loop that no longer runs.
            co_return;
        }
        if (cancelled) {
            VN_LOGI("the startup phase was cancelled by the work it was waiting for");
            cancelStartup();
        }
        else {
            failStartup();
        }
        co_return;
    }

    // The three phases are through: the boot is over, and the reporting door closes last (so the presenters go back to
    // following whatever runs next).
    endStartupProgress();
}

void Application::shutdown()
{
    // Two ways to call this wrong, both reported and neither refused: the caller may know
    // something the framework does not (a host that stops its own loop and tears down from
    // inside it), and a teardown that refuses to run would leave the process worse off than
    // an unclean one. The order below is what makes the difference: commands are drained
    // before the managers they hold, plugins are unloaded before the bus they publish on.
    if (dptr()->loop_running) {
        VN_LOGW("Application::shutdown() was called while the main loop is still running: the application is being torn "
                "down under it; ask the loop to exit() instead");
    }
    if (MainThreadDispatcher::hasEventLoop() && !MainThreadDispatcher::isMainThread()) {
        VN_LOGW("Application::shutdown() was called from a thread that is not the application thread: plugin unload() runs "
                "here, and a plugin takes its windows and graphics objects down there");
    }

    // A boot still in flight here was abandoned: the loop stopped while one of its phases was waiting. Its reporting
    // door is closed here, or it would stay open for the rest of the process (isBusy() true, so every top-level command
    // would be refused), and the phases still to come are skipped (see bootAbandoned()): a teardown is not where a main
    // window should still go up.
    if (dptr()->startup_progress != nullptr) {
        VN_LOGW("Application::shutdown() was called with the boot still in flight (the loop stopped while a phase was "
                "waiting): the reporting door is closed and the rest of the boot is skipped");
        dptr()->boot_abandoned = true;
        endStartupProgress();
    }

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
            VN_LOGW("Application shutdown: command chains did not stop within the drain bound");
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
        VN_LOGW("Application shutdown: pending events were dropped instead of delivered");
    }

    // Persist last: plugin unload and the final events may still change values.
    if (!dptr()->config_file.empty()) {
        const String    path(dptr()->config_file.u8string());
        std::error_code ec;
        std::filesystem::create_directories(dptr()->config_file.parent_path(), ec);
        if (!dptr()->config_manager->save(path)) {
            VN_LOGW("Failed to save the configuration to '{}'", dptr()->config_file.string());
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
        VN_LOGW("Failed to load the configuration from '{}'; using the defaults", dptr()->config_file.string());
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
    static const String kName(kDefaultOrganization);
    return kName;
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
    std::filesystem::path file = dataDirectory() / kConfigFolder;
    file /= toPath(applicationNameOrFallback() + QStringLiteral(".json"));
    return file;
}

std::filesystem::path Application::pluginDataDirectory() const
{
    return dataDirectory() / kPluginsFolder;
}

std::filesystem::path Application::pluginRegistrationDirectory() const
{
    return dataDirectory() / kRegistrationFolder;
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
        std::filesystem::path dir          = root;
        const QString         organization = QCoreApplication::organizationName();
        if (!organization.isEmpty()) {
            dir /= toPath(organization);
        }
        dir /= toPath(applicationNameOrFallback());
        dir /= kRegistrationFolder;

        if (std::find(directories.begin(), directories.end(), dir) == directories.end()) {
            directories.push_back(dir);
        }
    }
    return directories;
}

void Application::exit(int code)
{
    // The loop is stopped from the application thread only; a caller elsewhere (the host's own startup work on the pool
    // is the usual case) does not have to know that rule, so its request is queued for the application thread.
    if (MainThreadDispatcher::hasEventLoop() && !MainThreadDispatcher::isMainThread()) {
        // "While no loop is running, it does nothing" is part of the contract, so a request that arrives before the
        // loop runs is dropped instead of being queued to stop a loop that has not started: a worker that finishes
        // early must not end a boot nobody has begun yet.
        if (!dptr()->loop_running.load(std::memory_order_acquire)) {
            VN_LOGW("Application::exit() was called from another thread while no loop is running: the request is ignored");
            return;
        }
        static_cast<void>(MainThreadDispatcher::postToMainThread([code] { QCoreApplication::exit(code); }));
        return;
    }

    QCoreApplication::exit(code);
}

bool Application::isBusy() const
{
    // Only the foreground operation blocks new commands; background hosts run in parallel and do not make the
    // application busy. Read as a snapshot, not through ProgressHost::current(): that pointer is the operation's own
    // entry point and stays valid only while the operation runs, while this question is asked by an observer - command
    // admission, on whichever thread is admitting - that may outlive the operation it is looking at.
    return vn::appfw::ProgressHost::currentSnapshot().has_value();
}

raw_ptr<CommandManager> Application::commandManager() const
{
    return dptr()->command_manager.get();
}

raw_ptr<PluginManager> Application::pluginManager() const
{
    return dptr()->plugin_manager.get();
}

const AppInfo& Application::appInfo() const
{
    return dptr()->app_info;
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

std::vector<String> Application::pendingRestartKeys() const
{
    const std::vector<String> keys = dptr()->config_registry->restartRequiredKeys();
    if (keys.empty()) {
        return {};
    }
    return dptr()->startup_config.divergedKeys(dptr()->config_manager->snapshot(), keys);
}

raw_ptr<DocumentManager> Application::documentManager() const
{
    return dptr()->document_manager.get();
}

void Application::setRegistrationOwner(String owner)
{
    const std::lock_guard lock(dptr()->registration_owner_mutex);
    dptr()->registration_owner = std::move(owner);
}

String Application::registrationOwner() const
{
    const std::lock_guard lock(dptr()->registration_owner_mutex);
    return dptr()->registration_owner;
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

char* const* Application::argv() const
{
    return dptr()->argv;
}

VN_APPFW_NS_END
