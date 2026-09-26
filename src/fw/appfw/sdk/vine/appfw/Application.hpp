#pragma once

#include "appfw_global.hpp"

#include <filesystem>
#include <memory>
#include <vector>

#include <vine/async/DetachedTask.hpp>
#include <vine/async/Task.hpp>

#include <vine/Object.hpp>
#include <vine/String.hpp>
#include <vine/raw_ptr.hpp>

#include <vine/appfw/AppConfig.hpp>

VN_APPFW_NS_BEGIN

class CommandManager;
class PluginManager;
class ServiceManager;
class ConfigManager;
class ConfigRegistry;
class EventBus;
class MainThreadDispatcher;
class StartupProgress;
class UserIO;
class ApplicationData;

class VN_APPFW_API Application : public Object {
    VN_OBJECT_META_DECL;

  protected:
    /// Returns the private data block (see the private ApplicationData): leaves read their state through this, and
    /// nothing outside the framework can name the type (its header is private and not installed).
    ApplicationData*       dptr();
    const ApplicationData* dptr() const;

  private:
    /// The data block. Private on purpose: a leaf reaches it through dptr() and never replaces it.
    std::unique_ptr<ApplicationData> d;

  public:
    /**
     * @brief Builds the application from its configuration.
     *
     * The constructor does the whole initialization, in order: it applies the process identity (AppConfig::name and
     * AppConfig::organization, the default organization when the config sets none), builds the managers, and the leaf
     * constructor creates the Qt application object (see the protected constructor and initialize()), which is why no
     * Qt object may exist when this runs. The configuration file is loaded last (see AppConfig::config_file and
     * AppConfig::persist_config).
     *
     * A host has nothing left to call before run().
     *
     * @param config Application configuration.
     * @param argc Command line argument count.
     * @param argv Command line arguments.
     */
    Application(const AppConfig& config, int argc, char** argv);

    /**
     * @brief Builds the application with a default configuration.
     *
     * For a test or a tool: the framework's services come up, the process identity is whatever the host set itself, and
     * the configuration is read from and saved to Application::defaultConfigFile() (pass an AppConfig with
     * persist_config = false to keep the process out of the user's data directory).
     *
     * @param argc Command line argument count.
     * @param argv Command line arguments.
     */
    Application(int argc, char** argv);

  protected:
    /**
     * @brief Builds the application data and everything that does not depend on the Qt application object.
     *
     * The leaf constructor finishes the job, because creating the Qt object cannot happen here: the process may hold
     * only one such object and the base cannot know whether a headless host or a GUI is being built. So the leaf stores
     * it in the application data (ApplicationData::app, a private detail of the framework) and then calls
     * initialize().
     *
     * @param data   Application data; a leaf inside this framework may pass its own extension of it (the data header
     *               is private and not installed, so a host outside this tree cannot name the type - it derives from
     *               Application or GuiApplication instead).
     * @param config Application configuration.
     * @param argc   Command line argument count.
     * @param argv   Command line arguments.
     */
    Application(ApplicationData* data, const AppConfig& config, int argc, char** argv);

    /**
     * @brief Completes construction: creates the user IO and applies the configuration file.
     *
     * The configuration file is loaded here (immediately, not at the start of run()) and saved by shutdown(); an
     * explicit AppConfig::config_file wins over AppConfig::persist_config, which only decides whether the framework
     * layout under the user's data directory is used at all.
     *
     * The Qt application object has to be in place before this is called (see the protected constructor).
     *
     * @param config Application configuration, for the configuration file.
     */
    void initialize(const AppConfig& config);

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
     * The teardown run() performs once the loop has stopped, while the application is
     * still fully alive (a host that writes its own run() uses the same one):
     * 1. cancels a pending user-input read (UserIO::cancelPendingInput()) and then
     *    cancels every command chain and waits, bounded, for them to finish
     *    (CommandManager::cancelAllAndWait()) - a command frame holds the manager,
     *    the user IO and plugin services, so it must not outlive them, and a command
     *    parked on user input can only be woken by the first of the two,
     * 2. unloads the plugins in reverse dependency order (PluginManager::unloadAll()),
     * 3. drains and stops the event bus (EventBus::shutdownGracefully()),
     * 4. persists the configuration when a config file is set (setConfigFile()).
     *
     * Calling it twice is harmless: the chains are already gone, the plugin list is
     * empty and the bus is already stopped, so only the config save repeats.
     *
     * Protected on purpose: the teardown belongs to the run() being torn down. A host, a
     * command or a plugin that wants the application to stop asks the loop to exit
     * (exit()) - which is what the framework itself uses to end a boot early
     * (cancelStartup(), failStartup()) - so the loop returns and run() tears down, and
     * the order above cannot be entered from the middle.
     *
     * Called while the loop is still running, or from a thread that is not the application
     * thread, it reports that and continues: the caller may know something the framework does
     * not, and a teardown that refused to run would leave the process worse off.
     *
     * It must not be called from inside a command or a plugin's unload() either, although
     * the two fail differently: the drain cancels every live chain - including the one
     * making the call - so a command doing this sits out the whole bound and then tears
     * the application down under a frame that is still running, and a plugin unloading
     * would stop the event bus before the remaining plugins have been unloaded.
     */
    void shutdown();

  public:
    ~Application() override;

  public:
    /**
     * @brief Runs the application: starts the loop, then boot and serves the host from inside it.
     *
     * This method starts the application's main loop and blocks until the application is exited. It returns the exit
     * code provided to the exit() method.
     *
     * The whole startup phase happens inside the loop, because the window system needs it: a window is on screen only
     * once the loop has dispatched the notification that says so, and what depends on a window being on screen - the
     * plugin load, an embedded render surface - can only run after that. So run() posts one step and starts the loop;
     * that step walks the boot's three phases in order (startupStart(), startup(), startupEnd()), each of them deciding
     * on the main thread when the next one may run. A host without a window walks the same three, which is what makes the
     * two differ only in what goes on screen.
     *
     * The loop keeps turning for the whole boot except where a phase has to block it: the plugin load runs on the main
     * thread (a plugin's load() builds windows and views) - so what the boot reports, and anything the user does, stays
     * live while the loading goes on. A host whose own startup work is heavy overrides startup() and hands that work to
     * another thread itself (vn::async::run() is what the async library offers for a blocking callable, and it is what
     * the framework would do for the host anyway); blocking inside a phase instead stops the loop for as long as the
     * work takes, which is the "the window does not repaint, the progress does not update" case this shape exists to
     * avoid.
     *
     * The boot also loads the plugins (AppConfig::load_plugins) as the first thing its second phase does, which is what
     * lets a host call run() and nothing else.
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
     * <user data>/<organization>/<application name>, the same organization-and
     * application split Qt uses, for example ~/.local/share/Vine/Vine on Linux and
     * C:/Users/<user>/AppData/Local/Vine/Vine on Windows.
     *
     * The framework reserves three subdirectories: config/ for the persisted
     * configuration (defaultConfigFile()), logs/ for log files, plugins/ for
     * plugin-owned files and installed.d/ for plugin registrations. The directory
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
     * PluginInfo::name (see PluginLoadContext::ensureDataDirectory()); the name is the
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
     * Support``), each with the same <organization>/<application>/installed.d
     * layout as the per-user directory. They hold the registrations
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
     * @brief Returns the main-thread marshaller the EventBus was built with.
     *
     * Everything that moves work between threads is on the type (its statics - see MainThreadDispatcher); this accessor
     * hands over the one object the bus pumps through and keeps the "was this bus given a marshaller at all" answer.
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

    /**
     * @brief Returns the argument count the application was constructed with.
     *
     * @return The count passed to the constructor.
     */
    int argc() const;

    /**
     * @brief Returns the arguments the application was constructed with.
     *
     * A read-only view of what the constructor stored: Qt keeps the array for the application's lifetime, so the
     * pointer stays valid, but nothing here may change it (Qt registered the command line it saw).
     *
     * @return The arguments, valid for the application's lifetime.
     */
    char* const* argv() const;

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

  protected:
    // Declared after every other virtual function on purpose: a new virtual inserted anywhere earlier in this class
    // moves the vtable slots of the ones after it. The slots of everything below were reset by the ABI revision that
    // folded init() into the constructor, so keeping the additions at the end is what lets the next one be appended
    // without another bump. (The three boot phases live here as one set; their signatures changed when they became
    // async, which is why VN_APPFW_PLUGIN_ABI_VERSION moved to 5u, and the plugin lifecycle hooks themselves became
    // tasks in 6u.)

    /**
     * @brief Phase one of the boot: puts up what the user should see while it runs, and returns once it is up.
     *
     * The framework calls this from inside the event loop, once the boot has started, and the boot does not go on -
     * neither the plugin load nor the host's own startup work - until this returns: what it puts up has to be really on
     * screen first, or the user spends the boot looking at an empty window (a window is painted only once the loop has
     * dispatched the window system's notice that it is there - see Window::hasPainted()).
     *
     * Nothing to put up by default: a headless application has nothing to show. A GUI application shows the startup frame
     * the configuration asked for and waits for its first paint here - which is what makes the two differ only in what
     * goes on screen.
     *
     * Waiting is written as `co_await`, and that is the point of the async shape: the phase suspends and the loop keeps
     * turning, so the very notification it waits for can be dispatched. Blocking here would stop that loop, and pumping
     * the queue to force a paint is what the startup phase must not do (it would run the timers and settles of everything
     * else that is starting up).
     *
     * A leaf may well finish on another thread (a timer, an I/O completion): the framework comes back to the application
     * thread itself after every phase, because everything the boot touches - windows, the sink, the managers - belongs
     * there. What a leaf does *inside* its own phase still has to respect that (see
     * MainThreadDispatcher::resumeOnMainThread()).
     *
     * The wait is a deadline as well: a window system that never reports a window as visible must not keep the boot from
     * running (see GuiApplication::startupStart()).
     *
     * @return A task that completes once what this shows is on screen; the framework awaits it.
     */
    virtual vn::async::Task<void> startupStart();

    /**
     * @brief Phase two of the boot: loads the plugins, runs the host's startup work, and returns when both are through.
     *
     * The framework's part is the plugin load (AppConfig::load_plugins), which runs on the main thread because a plugin's
     * load() builds windows, views and render surfaces; it is the first thing the phase does, so the commands, services
     * and UI the plugins register are there before anything the host adds.
     *
     * This is also the phase a host with startup work of its own overrides: it does its own part and then awaits
     * Application::startup(), so the plugin load keeps coming first. Heavy work goes to another thread - vn::async::run()
     * is what the async library offers for a blocking callable - and `co_await`ing it is what keeps the loop turning
     * (progress reaches the frame, the windows keep painting, the host's own input stays answered) while it runs; a host
     * that blocks here instead stops the loop for as long as its work takes. Whatever is done on another thread must not
     * touch what belongs to the main thread - windows, widgets, the render surface, the Qt application object - and
     * reaches it through MainThreadDispatcher::postToMainThread() where it has to.
     *
     * Nothing here reports progress of its own: the plugin load is reported by PluginManager ("正在加载插件").
     *
     * A failure here - an exception out of a leaf's own part, or out of the host's startup work - is fatal: the boot is
     * over and the application exits with a non-zero code. A plugin that merely fails to load is not: the application
     * starts without it.
     *
     * @return A task that completes once the boot's own work is done.
     */
    virtual vn::async::Task<void> startup();

    /**
     * @brief Phase three of the boot: puts the boot's face away and lets what it was hiding come up.
     *
     * The framework's *end of the boot* is not here: the startup progress sink is destroyed right after this returns (so
     * the presenters go back to following whatever runs next once the last phase is through). GuiApplication takes the
     * startup frame down and shows the main window for the first time; a headless application has nothing to put away.
     *
     * This is deliberately not the mirror image of startupStart(): that one waits for something only the loop can
     * deliver, so it has to suspend; this one is over as soon as it is done, and would be a plain call if the three
     * phases were not one shape.
     *
     * A host that drives its own boot calls this from its own class (it is protected: ending the boot is the framework's
     * move, and a host does not reach into a running boot from outside) - that is also the call that brings the windows
     * up for a host that never runs the loop.
     *
     * It is a lazy task, so it has to be awaited: Task::result() drives it to completion on the calling thread, which only
     * works while it does not need the event loop.
     *
     * Idempotent by contract, because that host may call it when the boot has already ended (the framework calls it once
     * per boot): the second call does nothing - GuiApplication remembers the boot ended, so the frame is not taken down
     * twice and the main window is not shown again.
     *
     * @return A task that completes once the boot's face is away.
     */
    virtual vn::async::Task<void> startupEnd();

  private:
    /// Walks the boot's three phases (startupStart() -> startup() -> startupEnd()); the order lives in this one place
    /// and it is flat: the three phases, each followed by a return to the application thread, read as a sequence under
    /// a single `try`.
    ///
    /// The three cannot be three bare calls: a phase returning is not a phase being done (the first one waits for what
    /// the boot puts up to be really on screen), and in an ordinary function "the next statement" means "after the
    /// previous one returned", so a flat sequence of phases that suspend is not expressible. The coroutine straightens
    /// "wait for the next phase" into `co_await`: the loop keeps turning while the boot waits, which is what "wait for
    /// it without holding the message loop" means.
    ///
    /// Each phase is followed by a `resumeOnMainThread()`: a phase may finish on another thread (a timer, I/O, the
    /// host's own work posted to the pool), and both the next phase and the end of this boot belong on the application
    /// thread. There it is a no-op, so the fast path pays nothing.
    ///
    /// An exception out of the boot is not swallowed: a failure in any phase (including one the host's own startup work
    /// carries up) tears the progress sink down and stops the main loop with a non-zero code (see failStartup()) - a
    /// failed boot must not leave a half-started application running. That teardown belongs on the application thread
    /// as well, and `co_await` cannot be written inside a catch handler, so the failing path is padded first and the two
    /// paths rejoin after the `try`.
    ///
    /// `DetachedTask` is not a return value in the usual sense: it is the marker that says "this is a coroutine" (C++
    /// requires a coroutine function's return type to be a coroutine type) and holds nothing - no value, nothing to
    /// await. Calling it starts it (eager start) and the frame destroys itself past the last line, which is why it is
    /// used instead of `Task`: this boot has no owner, so nobody wants its result and nobody may await or destroy it.
    /// (An exception thrown outside this function's `try` would reach the handler installed with
    /// vn::async::setDetachedExceptionHandler(), or terminate the process.)
    vn::async::DetachedTask startupSequence();

    /// The boot failed: tears the progress sink down (so nobody keeps reporting into a dead boot) and stops the main
    /// loop with a non-zero code - run() returns at once, the process exits with that code, and shutdown() still runs
    /// to the end. **startupEnd() is deliberately skipped**: the boot did not happen, and putting a half-built main
    /// window on screen would be worse.
    void failStartup();
    /// Whether this boot was asked to stop (a host calling StartupProgress::requestCancel(), or a cancel affordance it
    /// draws itself - the startup frame has none: it refuses to be closed).
    ///
    /// Cancellation is not a failure: the framework looks at it after every phase and before it instantiates each
    /// plugin, and takes cancelStartup() when it is set.
    ///
    /// @return true when this boot should stop.
    bool startupCancelled() const;

    /// Cancels this boot: logs it, unloads the plugins that were already loaded, tears the progress sink down and stops
    /// the main loop with 0 (cancelling is not failing). The main window never goes up - this is not startupEnd()'s
    /// path - and nothing that was loaded stays in the process.
    void cancelStartup();
    /// Creates the boot's progress sink (idempotent: an existing one is returned). For the startup sequence only - the
    /// sink's lifetime is the startup phase's, and a host does not create one ahead of time (that would make `isBusy()`
    /// true before the boot, which refuses every top-level command); report from inside the three phases instead.
    ///
    /// @return The application's progress sink.
    StartupProgress* beginStartupProgress();

    /// Tears the boot's progress sink down (`complete()` and then destroy). Destroying it rather than only completing
    /// it is deliberate: a live host would sit on the foreground stack and shadow the progress of every command that
    /// runs afterwards. Idempotent (nothing happens without a sink), and on a boot that succeeded it runs **after**
    /// startupEnd().
    void endStartupProgress();
};

VN_APPFW_NS_END
