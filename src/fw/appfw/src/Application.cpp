#include <algorithm>
#include <atomic>
#include <filesystem>
#include <system_error>
#include <utility>

#include <QCoreApplication>
#include <QMetaObject>
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

#include <vine/appfw/ProgressHost.hpp>
#include <vine/appfw/StartupProgress.hpp>

#include <vine/async/DetachedTask.hpp>

#include "ApplicationData.hpp"
#include "ConsoleUserIO.hpp"

VN_APPFW_NS_BEGIN

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
    return name.isEmpty() ? QString::fromUtf8(s_fallback_app_name) : name;
}

/// 启动失败时进程的退出码：启动期的异常是致命的（见 startupSequence()），run() 带着它返回，
/// 进程随之退出。
constexpr int s_startup_failure_exit_code = 1;

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
    // framework's public headers.
    dptr()->app = new QCoreApplication(dptr()->argc, dptr()->argv);
    initialize(config);
}

Application::Application(int argc, char** argv)
  : Application(AppConfig{}, argc, argv)
{}

Application::Application(ApplicationData* data, const AppConfig& config, int argc, char** argv)
  : d(data)
{
    if (s_current_app.load(std::memory_order_acquire) != nullptr) {
        throw Exception(-1);
    }

    s_current_app.store(this, std::memory_order_release);

    dptr()->argc = argc;
    dptr()->argv = argv;

    // The process identity comes first, because everything below reads it: the per-user data and config paths
    // (QStandardPaths), the configuration file, and the startup frame, which resolves an empty title to the application
    // name. The framework assumes an organization so those paths are well formed even when the host only names the
    // application; an identity the host set before this constructor is kept.
    if (!config.organization.empty()) {
        QCoreApplication::setOrganizationName(QString::fromStdString(config.organization));
    } else if (QCoreApplication::organizationName().isEmpty()) {
        QCoreApplication::setOrganizationName(toQString(Application::defaultOrganizationName()));
    }
    if (!config.name.empty()) {
        QCoreApplication::setApplicationName(QString::fromStdString(config.name));
    }

    dptr()->load_plugins = config.load_plugins;

    dptr()->plugin_manager  = std::make_unique<PluginManager>();
    dptr()->service_manager = std::make_unique<ServiceManager>();
    dptr()->command_manager = std::make_unique<CommandManager>(this);
    dptr()->config_manager  = std::make_unique<ConfigManager>();
    dptr()->config_registry = std::make_unique<ConfigRegistry>();
    dptr()->main_dispatcher = std::make_unique<MainThreadDispatcher>();
    // The marshaller is injected and outlives the bus (ApplicationData declares it
    // before the bus, so it is destroyed after it).
    dptr()->event_bus = std::make_unique<EventBus>(dptr()->main_dispatcher.get());
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
        return;
    }
    if (config.persist_config) {
        static_cast<void>(setConfigFile(defaultConfigFile()));
    }
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
    if (dptr()->startup_progress == nullptr) {
        dptr()->startup_progress = std::make_unique<StartupProgress>();
    }
    return dptr()->startup_progress.get();
}

StartupProgress* Application::startupProgress() const
{
    return dptr()->startup_progress.get();
}

UserIO* Application::createUserIO()
{
    return new ConsoleUserIO;
}

vn::async::Task<void> Application::startupStart()
{
    co_return; // 默认什么都不摆：无头宿主没有"启动期的脸"，叶子（GUI）在这里上屏、等首帧。
}

vn::async::Task<void> Application::startup()
{
    auto* const d = dptr();

    // 框架自己的启动工作先来：插件是宿主的活最容易依赖的东西（命令、服务、界面都是它们注册的），而且
    // "加载插件"每个应用都要做一遍，不该由每个 main 重写（AppConfig::load_plugins 关掉就能自管）。
    // 时机是契约：这一段跑在启动画面画出首帧之后（由 startupStart() 那一拍等出来）——插件里建窗口、建渲染
    // 表面时，窗口系统已经能派发通知了（X11 上不派发就只是一个空窗口）。它也必须在主线程：插件正是在
    // load() 里建窗口与视图。
    if (d->load_plugins && d->plugin_manager != nullptr) {
        // 异步门（loadAllAsync()）：插件生命周期必须跑在应用线程上（插件正是在 load() 里建窗口与视图），所以
        // "耗时操作不阻塞事件循环"只能靠切：插件把与界面无关的那一半丢到池上（`vn::async::run`），回来之前
        // 事件循环照跑——启动框的进度也正是这样从池上那一半报上来的。
        // 进度是按**相位**更新的：管理器在每段之前报一次阶段名、每个插件换一次标签（查找/创建/加载/收尾），
        // 启动框每收到一次上报就同步重画一次（见 BootSplash::onStartupChanged()）——所以相位边界一定看得见，
        // 而相位**内部**那段连续占用（例如会话 init() 的两百多毫秒）期间画面不动，这是有意接受的：
        // 不为此在启动期回转事件循环（会把别人的定时器也跑起来，见 PluginManager::loadAllAsync() 的注释）。
        const bool loaded = co_await d->plugin_manager->loadAllAsync();
        if (!loaded && !startupCancelled()) {
            // 取消不是失败：取消时管理器提前收工也返回 false，别拿"插件没装上"去吓人。
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

void Application::cancelStartup()
{
    // 用户（或宿主）不想等这次启动了：这不是失败，所以不走 failStartup() 那条非零退出的路，但也不能装作启动
    // 成功——主窗永远不上屏，已经装上的插件按依赖反序卸掉（插件写在 load() 里的东西不能留在进程里），上报口收起，
    // 主循环以 0 停掉：run() 随即返回，shutdown() 照常跑完（那时候插件表已经是空的）。
    VN_LOGI("startup cancelled: the application stops without finishing the boot");

    if (auto* const plugins = dptr()->plugin_manager.get(); plugins != nullptr) {
        static_cast<void>(plugins->unloadAll());
    }

    endStartupProgress();
    exit(0);
}

void Application::failStartup()
{
    VN_LOGE("startup failed: the application exits with code {}", s_startup_failure_exit_code);

    // 上报口先收：不让任何人继续往一个死掉的启动里报（presenter 也随之回到"跟下一步干什么"）。
    endStartupProgress();

    // 以非零码停主循环：run() 随即返回，进程带着这个码退出，shutdown() 照常跑完（命令、插件、配置都干净收尾）。
    exit(s_startup_failure_exit_code);
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
    co_return; // 默认没有"脸"要收：叶子（GUI）在这里收框、把主窗第一次上屏。
}

int Application::run()
{
    auto* const d = dptr();

    // 计时从“被要求运行”算起，给启动期的诊断一个统一起点（无头宿主没有读者，但多算一次也不花钱）。
    d->startup_requested_at.start();

    // 先让循环跑起来，启动阶段再从队列里推：窗口系统那一半（X11 的 expose、Windows 的 WM_PAINT）本来就靠循环派发，
    // 循环起来之前 show() 出来的窗口一个像素都没有；而启动阶段的第一道门就是“启动画面真的在屏上”。
    // 两个好处：无头与有窗口的启动阶段走同一条时间轴（同一个事件队列），差别只剩“上不上屏”；
    // 宿主的活跑在池里，循环不必为它停。
    QMetaObject::invokeMethod(d->app, [this] { static_cast<void>(startupSequence()); }, Qt::QueuedConnection);

    // 循环跑着的时候收尾是用法错误（见 shutdown()）：这一位就是那句话的判据。
    d->loop_running = true;
    const int code = d->app->exec();
    d->loop_running = false;

    shutdown();
    return code;
}

vn::async::DetachedTask Application::startupSequence()
{
    // 三拍平铺（startupStart() → startup() → startupEnd()），像顺序代码一样读。
    //
    // 三拍不能是三个裸调用：一拍返回不等于它做完了（GUI 的第一拍要等启动框真的画出首帧）。协程把“等下一拍”
    // 拉直成 `co_await`：挂起期间控制权回到事件循环——这就是“等它，但不占住消息循环”。
    //
    // 每拍之后垫一次 `resumeOnMainThread()`：一拍可能在别的线程上结束（定时器、IO、宿主自己丢到池上的活），
    // 而下一拍与这次启动的收尾都必须在应用线程上。已经在主线程时它是空操作。
    beginStartupProgress()->stage("正在启动");

    bool ok = true;
    try {
        co_await startupStart();
        co_await dptr()->main_dispatcher->resumeOnMainThread();
        if (startupCancelled()) {
            cancelStartup();
            co_return;
        }
        co_await startup();
        co_await dptr()->main_dispatcher->resumeOnMainThread();
        if (startupCancelled()) {
            cancelStartup();
            co_return;
        }
        co_await startupEnd();
        co_await dptr()->main_dispatcher->resumeOnMainThread();
    }
    catch (const std::exception& error) {
        VN_LOGE("the startup phase failed: {}", error.what());
        ok = false;
    }
    catch (...) {
        VN_LOGE("the startup phase failed with an unknown exception");
        ok = false;
    }

    // 启动期异常**不吞**：任一拍（含宿主启动工作带上来的异常）失败就收起上报口、以非零码停掉主循环（见
    // failStartup()）——启动失败不该留下一个半启动的应用在跑。取消走另一条路（cancelStartup()：没失败，也没
    // 启动成功）。收尾也要在应用线程上，而 `co_await` 不能写在 catch
    // 处理块里（失败那一拍也可能是在别的线程上抛的）⇒ 两条路都在 try 之后合并。
    if (!ok) {
        co_await dptr()->main_dispatcher->resumeOnMainThread();
        failStartup();
        co_return;
    }

    // 三拍都过了：这次启动结束，上报口最后收（presenter 随之回到“跟下一步干什么”）。
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
    if (auto* dispatcher = dptr()->main_dispatcher.get(); dispatcher != nullptr && !dispatcher->isMainThread()) {
        VN_LOGW("Application::shutdown() was called from a thread that is not the application thread: plugin unload() runs "
                "here, and a plugin takes its windows and graphics objects down there");
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
        const String path(dptr()->config_file.u8string());
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
    // 退出请求只能由主线程下：别的线程上的调用方（宿主自己丢到池上的启动工作就是一条）不必知道这条规矩。
    if (auto* dispatcher = dptr()->main_dispatcher.get(); dispatcher != nullptr && !dispatcher->isMainThread()) {
        static_cast<void>(dispatcher->postToMain([code] { QCoreApplication::exit(code); }));
        return;
    }

    QCoreApplication::exit(code);
}

bool Application::isBusy() const
{
    // Only the foreground operation blocks new commands; background hosts run
    // in parallel and do not make the application busy.
    return vn::appfw::ProgressHost::current() != nullptr;
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

VN_APPFW_NS_END
