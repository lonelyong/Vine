// HeadlessBootTest.cpp
//
// 无头宿主（一个窗口都不建）也要报告自己的启动，而且报告的是同一份东西：启动期"在做什么、做到哪了"由框架上报，
// 呈现者是恰好谁在听——启动框、状态栏进度条、无头时的控制台（ConsoleUserIO 挂的 ConsoleProgressReporter）。
// 也就是说**启动框只是呈现方式之一，不是上报的前提**：以前上报口是在 GuiApplication 建启动框时顺手建的，
// 于是"没开启动框"和"没有窗口"这两种宿主的启动期都是一片沉默，尽管控制台呈现器一直挂在 ProgressHost 上等着。
//
// 现在上报口与插件加载都属于**启动阶段**，而且整个启动阶段跑在**循环里**（run() 先起循环，再推一个启动步）：
// 启动步走三拍——startupStart()（建上报口、上屏并等首帧）、startup()（插件在宿主的活之前加载）、
// startupEnd()（收上报口）；构造本身两样都不做 —— 这条同样是契约：从不跑启动的进程（测试、工具）不该因为
// "构造过应用"就多出一个活的前台上报口（isBusy() 会因此为真，顶层命令全被拒），也不该顺手扫一遍插件目录。
// "跑在循环里"不是风格问题：窗口系统那一半（X11 的 expose）只能由循环派发，而启动工作要能 postToMain、
// 能用 exit() 提前收场（循环没起来时 exit() 什么也不做）。用例把这几头都钉住。
//
// 宿主的启动工作由宿主自己丢到别的线程上（`vn::async::run`，见用例里的 WorkingHostApplication；框架的插件加载仍在
// 主线程：插件在 load() 里建窗口与视图）：因此这个用例还钉两条——工作不在主线程上、以及工作跑着的时候主线程**仍在
// 派发事件**（定时器一直在跳）。旧形状里工作是同步跑在主线程上的，那两秒的"消息不更新"就是它。
//
// See .ai/design/appfw-startup-phases.md（三拍与工作线程）、.ai/design/appfw-startup-splash.md、
// .ai/design/appfw-progress.md。

#include <gtest/gtest.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <functional>
#include <memory>
#include <stdexcept>
#include <string>
#include <system_error>
#include <thread>
#include <vector>

#include <QCoreApplication>
#include <QElapsedTimer>
#include <QMetaObject>
#include <QStandardPaths>
#include <QTimer>

#include <vine/async/Sleep.hpp>
#include <vine/async/ThreadPoolScheduler.hpp>

#include <vine/appfw/Application.hpp>
#include <vine/appfw/ConfigManager.hpp>
#include <vine/appfw/EventBus.hpp>
#include <vine/appfw/MainThreadDispatcher.hpp>
#include <vine/appfw/PluginManager.hpp>
#include <vine/appfw/StartupProgress.hpp>
#include <vine/appfw/UserIO.hpp>

#include <vine/logging/Log.hpp>
#include <vine/logging/LogSink.hpp>

namespace
{

/// 把夹具插件摆进一个空目录，返回那个目录：`loadAll()` 只会扫到它。
///
/// @return The directory holding nothing but the fixture plugin.
std::filesystem::path bootPluginDirectory()
{
    const std::filesystem::path fixture = VINE_HEADLESS_BOOT_PLUGIN;
    const std::filesystem::path dir     = std::filesystem::temp_directory_path() / "vine_test_appfw_plugins";

    std::error_code ec;
    std::filesystem::remove_all(dir, ec);
    std::filesystem::create_directories(dir, ec);
    std::filesystem::copy_file(fixture, dir / ("headless_boot" + fixture.extension().string()),
                               std::filesystem::copy_options::overwrite_existing, ec);
    EXPECT_FALSE(ec) << "夹具插件没拷进 '" << dir.string() << "': " << ec.message();
    return dir;
}

/// 把两份夹具插件（**同一个 uuid**、两个名字）摆进一个空目录，返回那个目录。
///
/// @return The directory holding the two fixtures that claim one identity.
std::filesystem::path twinPluginDirectory()
{
    const std::filesystem::path dir = std::filesystem::temp_directory_path() / "vine_test_appfw_twins";

    std::error_code ec;
    std::filesystem::remove_all(dir, ec);
    std::filesystem::create_directories(dir, ec);
    for (const char* const fixture : { VINE_HEADLESS_BOOT_PLUGIN, VINE_TWIN_IDENTITY_PLUGIN }) {
        const std::filesystem::path library = fixture;
        std::filesystem::copy_file(library, dir / library.filename(), std::filesystem::copy_options::overwrite_existing, ec);
    }
    EXPECT_FALSE(ec) << "夹具插件没拷进 '" << dir.string() << "': " << ec.message();
    return dir;
}

/// 抓住 logger 上的 warning 行。
///
/// 行容器由 sink 的 lambda 持有（shared_ptr），所以用例结束后 sink 再被调用也不会悬垂。
struct WarningCapture {
    std::shared_ptr<std::vector<std::string>> lines = std::make_shared<std::vector<std::string>>();

    /**
     * @brief Reports whether one captured warning contains @p text.
     *
     * @param text Substring to look for.
     * @return true when some captured line contains it.
     */
    [[nodiscard]] bool contains(const std::string& text) const
    {
        return std::any_of(lines->begin(), lines->end(), [&text](const std::string& line) { return line.find(text) != std::string::npos; });
    }
};

/// @brief Attaches a warning capture to the logger the VN_LOG* macros write to.
/// @return The capture, to assert on.
[[nodiscard]] WarningCapture captureWarnings()
{
    WarningCapture capture;
    vn::logging::defaultLogger().addSink(vn::logging::LogSink::function(
        [lines = capture.lines](vn::logging::LogLevel level, const std::string& line) {
            if (level == vn::logging::LogLevel::Warn) {
                lines->push_back(line);
            }
        }));
    return capture;
}

/// 有自己启动工作的无头宿主：第二拍先让框架把插件加载完（`co_await Application::startup()`），再把重活丢到线程池上
/// 并等它——框架不再替宿主拥有线程（`run(work)` 那个重载已删），宿主的写法就是这一条：`vn::async::run()` 把同步的
/// 重活送到别的线程，`co_await` 它挂起这一拍，主循环因此一直是自由的。
class WorkingHostApplication : public vn::appfw::Application {
  public:
    using Application::Application;

    /// 收尾是 protected（只有 `run()` 那条路径调它）：用例要能直接调，才能量"线程/时机不对会怎样"。
    using Application::shutdown;

    /// 第二拍：框架的插件加载照旧先来，然后是宿主的活。
    vn::async::Task<void> startup() override
    {
        co_await Application::startup();
        if (work) {
            co_await vn::async::run(std::move(work));
        }
    }

    /// 宿主的启动工作：在池里的一条工作线程上跑（这里就是测试要观察的那一段）。
    std::function<void()> work;
};

TEST(HeadlessBootTest, TheBootReportsItsProgressAndLoadsThePluginsWithoutAWindow)
{
    // 用户数据/配置位置重定向到 Qt 测试目录：用例既不读也不写开发机上的真实配置。
    QStandardPaths::setTestModeEnabled(true);

    static char  arg0[] = "test_appfw";
    static char* argv[] = { arg0, nullptr };

    vn::appfw::AppConfig config;
    config.name           = u8"test_appfw";
    config.persist_config = false;
    config.load_plugins   = true; // 默认值，写明"加载是框架的内置动作"

    // 只有夹具插件：这个叶子不是 `createApplication()` 建的，所以构建器替宿主做的那一个副作用（把内置插件目录指过去，
    // 见 AppBuilder.cpp）要自己写，否则框架会顺手扫真正的内置插件目录（那里是 GUI 插件）。
    vn::appfw::PluginManager::setBuiltInPluginDirectory(bootPluginDirectory());

    auto app = std::make_unique<WorkingHostApplication>(config, 1, argv);
    ASSERT_NE(app, nullptr);

    // 构造即完成（见待办 1）：Qt 应用对象与 UserIO 已经在，但**还没有启动阶段**——上报口与插件加载
    // 都不属于构造。
    EXPECT_NE(QCoreApplication::instance(), nullptr);
    EXPECT_NE(app->userIO(), nullptr);
    EXPECT_EQ(vn::appfw::StartupProgress::current(), nullptr);
    EXPECT_FALSE(app->pluginManager()->isLoaded(u8"headless_boot_plugin"));
    EXPECT_FALSE(app->isBusy()); // 没在启动：命令可以跑

    bool reported      = false; // 启动阶段里上报口在（无头宿主报得出东西）
    bool busy          = false; // 启动阶段里前台被启动上报占着
    bool labeled       = false; // 上报口能用：阶段与状态文字都写得进去
    bool advanced      = false;
    bool plugins_first = false; // 宿主的活开跑之前，插件已经被框架加载完了
    bool probe_delivered = false; // 由下面 post 出去的回调写：只有循环派发了它才会为真
    bool boot_in_loop    = false; // 工作里快照：跑工作的那一刻，那个回调是不是已经到过
    bool delegated_ran   = false; // 同步委托（invokeOnMainThread）跑完了才返回
    bool delegated_on_main = false; // 而且是在应用线程上跑的
    qint64 delegated_ms  = 0; // 委托那一段在被调用方身上花了多久（用来量"同步等待"）
    bool delegated_when_returned = false; // 返回的那一刻它已经跑过了

    // 主线程身份 + 主线程上的一串定时器：宿主的活跑在别的线程上，而这串定时器在工作期间要一直在跳——
    // 这就是"启动期没把消息循环占住"的直视断言（旧形状里工作同步跑在主线程上，它一跳都不跳）。
    const std::thread::id main_thread = std::this_thread::get_id();
    std::thread::id       work_thread{};
    std::atomic<int>      ticks{ 0 };
    QTimer                ticker;
    ticker.setInterval(10);
    QObject::connect(&ticker, &QTimer::timeout, qApp, [&ticks] { ticks.fetch_add(1); });
    ticker.start();

    // 循环起来之前 post 的一帧回调：它排在 run() 推的启动步之前，只有"启动阶段真在循环里派发事件"时
    // 才会先到达（post 事件 FIFO）；旧形状（工作直接跑在 exec() 之前）里它在工作之后才被派发。
    // 注意快照必须在工作里取：循环起来之后它一定会被派发，出循环再读就永远是真的。
    QMetaObject::invokeMethod(qApp, [&] { probe_delivered = true; }, Qt::QueuedConnection);

    app->work = [&] {
        work_thread = std::this_thread::get_id();

        auto* boot    = vn::appfw::StartupProgress::current();
        reported      = boot != nullptr && vn::appfw::StartupProgress::current() == boot;
        busy          = app->isBusy();
        plugins_first = app->pluginManager()->isLoaded(u8"headless_boot_plugin");
        boot_in_loop  = probe_delivered;

        if (boot != nullptr) {
            boot->stage(u8"正在加载插件", 2);
            boot->setDone(1);
            labeled  = boot->label() == u8"正在加载插件";
            advanced = boot->fraction().has_value() && *boot->fraction() > 0.0;
        }

        // 睡够几拍定时器的时间：工作期间主线程应该一直在派发（ticks 会涨）。
        std::this_thread::sleep_for(std::chrono::milliseconds(120));

        // 工作线程上的"同步委托"：把一段只属于应用线程的初始化交给它，并**等它跑完**才继续——
        // 这就是 invokeOnMainThread()（不是 resumeOnMainThread()：那个是把整条尾巴搬过去）。
        // 委托那一段故意睡 60 ms：调用方量到的时长才能证明它真的在等（不等就一定远小于 60 ms）。
        QElapsedTimer delegated_timer;
        delegated_timer.start();
        delegated_ran = vn::appfw::MainThreadDispatcher::invokeOnMainThread([&] {
            delegated_on_main = std::this_thread::get_id() == main_thread;
            std::this_thread::sleep_for(std::chrono::milliseconds(60));
        });
        delegated_ms = delegated_timer.elapsed();
        delegated_when_returned = delegated_on_main;

        // 宿主的活干完了：回主线程收尾——让框架把启动收完（startupEnd() 那一拍），再让循环退出。
        static_cast<void>(vn::appfw::MainThreadDispatcher::postToMainThread([] {
            QTimer::singleShot(50, qApp, [] { QCoreApplication::exit(0); });
        }));
    };

    const int code = app->run();
    ticker.stop();

    EXPECT_EQ(code, 0);
    EXPECT_NE(work_thread, main_thread) << "宿主的启动工作必须跑在别的线程上（框架的插件加载仍在主线程），否则循环被占住、消息不更新";
    EXPECT_GE(ticks.load(), 5) << "工作跑着的时候主线程必须还在派发事件（消息循环没被占住）";
    EXPECT_TRUE(reported) << "无头启动阶段必须有上报口，否则无头宿主什么都报不出来";
    EXPECT_TRUE(busy);
    EXPECT_TRUE(labeled);
    EXPECT_TRUE(advanced);
    EXPECT_TRUE(plugins_first) << "插件加载是框架的内置启动动作，而且要跑在宿主自己的工作之前";
    EXPECT_TRUE(boot_in_loop) << "启动阶段必须跑在事件循环里（run() 先起循环，再推启动步），否则 post 和 exit() 都不算数";
    EXPECT_TRUE(delegated_ran) << "工作线程上的同步委托必须等应用线程跑完才返回";
    EXPECT_TRUE(delegated_on_main) << "同步委托的那段代码必须在应用线程上执行（它就是为了这个才存在）";
    EXPECT_GE(delegated_ms, 50) << "同步委托要真的等那一段跑完（只排队、不等的话返回值会立刻回来）";
    EXPECT_TRUE(delegated_when_returned) << "同步委托返回时那段代码必须已经跑过了";

    // 已经在应用线程时，同步委托是**内联执行**：排队等自己会把调用线程锁死。
    bool inline_ran = false;
    EXPECT_TRUE(app->mainThreadDispatcher()->invokeOnMainThread([&] { inline_ran = true; }));
    EXPECT_TRUE(inline_ran);

    // 启动阶段结束：上报口销毁，前台栈清空（之后的命令进度不会被启动期遮住）。
    EXPECT_EQ(vn::appfw::StartupProgress::current(), nullptr);
    EXPECT_FALSE(app->isBusy());

    // run() 的收尾是 shutdown()：插件在那里按依赖反序卸载（框架加载，也框架卸），
    // 所以循环退出后它已经不在了 —— 与构造/启动之前的"尚未加载"对称。
    EXPECT_FALSE(app->pluginManager()->isLoaded(u8"headless_boot_plugin"));
}

/// 宿主的启动工作拗异常：启动期异常是**致命的** —— 收起上报口，以非零码停掉主循环，run() 带着它返回。
///
/// （插件的 `loadAll()` 返回 false 那种"某个插件没装上"不在此列：应用照样启动，只记一条告警。）
TEST(HeadlessBootTest, AFailingStartupWorkEndsTheProcess)
{
    QStandardPaths::setTestModeEnabled(true);

    static char  arg0[] = "test_appfw";
    static char* argv[] = { arg0, nullptr };

    vn::appfw::AppConfig config;
    config.name           = u8"test_appfw";
    config.persist_config = false;
    config.load_plugins   = false; // 这条用例只量"工作抛异常"那一头，插件不是主角
    auto app              = std::make_unique<WorkingHostApplication>(config, 1, argv);
    ASSERT_NE(app, nullptr);

    // 宿主那一拍里的异常（这里从池上的工作重抛回来）是致命的：启动序列记日志、收上报口、以非零码停掉主循环。
    app->work = [] { throw std::runtime_error("the host's startup work failed"); };

    // 不需要额外的定时器让循环退出：启动失败自己就把它停了。
    const int code = app->run();

    EXPECT_EQ(code, 1) << "启动期的异常是致命的：进程以非零码退出";
    EXPECT_EQ(vn::appfw::StartupProgress::current(), nullptr) << "上报口照样要收掉（不该继续往一个死掉的启动里报）";
    EXPECT_FALSE(app->isBusy());
}

/// 叶子自己收场的宿主：取消的启动**不该**跑到最后一拍（那一拍是"撤掉启动的脸、把主窗端出来"）。
class CancellingHostApplication : public vn::appfw::Application {
  public:
    using Application::Application;

    /// 第三拍是否跑过（由测试读）。
    bool end_phase_ran = false;

    vn::async::Task<void> startupEnd() override
    {
        end_phase_ran = true;
        co_await Application::startupEnd();
    }
};

/// 用户/宿主请求取消启动：**不是失败**（进程以 0 退出，不是启动失败那个 1），但也不是启动成功——
/// 已经装上的插件要卸掉（插件在 load() 里建的东西不能留在进程里），上报口收起，最后一拍不跑。
TEST(HeadlessBootTest, ACancelledBootEndsCleanlyWithoutFinishingTheBoot)
{
    QStandardPaths::setTestModeEnabled(true);

    static char  arg0[] = "test_appfw";
    static char* argv[] = { arg0, nullptr };

    vn::appfw::AppConfig config;
    config.name           = u8"test_appfw";
    config.persist_config = false;
    config.load_plugins   = true;

    vn::appfw::PluginManager::setBuiltInPluginDirectory(bootPluginDirectory());

    auto app = std::make_unique<CancellingHostApplication>(config, 1, argv);
    ASSERT_NE(app, nullptr);

    // 触发器是夹具插件自己：它在 load() 里往这次启动的 token 上写（就是启动框那个关闭按钮做的事，
    // 只是这里没有窗口可点）。
    app->configManager()->setBool(u8"plugins.headless_boot.cancel_in_load", true);

    // 兜底：取消这条路自己会停主循环；万一它没起作用（回归），这里让用例以失败收场而不是挂死
    // （挂死的红比失败的红难诊断得多——见 .ai/memory 的钉子规矩）。
    QTimer::singleShot(3000, qApp, [] { QCoreApplication::exit(0); });

    const int code = app->run();

    EXPECT_EQ(code, 0) << "取消不是失败：退出码是 0";
    EXPECT_FALSE(app->end_phase_ran) << "取消的启动不走最后一拍，主窗不该上屏";
    EXPECT_EQ(vn::appfw::StartupProgress::current(), nullptr) << "上报口要收掉";
    EXPECT_FALSE(app->isBusy());
    EXPECT_FALSE(app->pluginManager()->isLoaded(u8"headless_boot_plugin"))
        << "取消时已经装上的插件必须卸掉（插件在 load() 里建的东西不能留在进程里）";
    EXPECT_TRUE(app->configManager()->getBool(u8"plugins.headless_boot.saw_token", false))
        << "插件必须能从这个口看到这次启动的 token（PluginLoadContext::stopToken()），而不是去摸全局上报口";
}

/// 叶子自己的启动拍可以异步：`co_await` 一个定时器，启动照常往前走。
class AwaitingPhaseApplication : public vn::appfw::Application {  public:
    using Application::Application;

    /// 第二拍进来时是否已经在应用线程上（由测试读）。
    std::atomic<bool> second_phase_on_main{ false };

    /// 第一拍：等 150 ms 再回去，模拟"叶子有自己的异步准备"（等设备、等文件、等网络）。
    /// 它**故意在定时器线程上结束、不自己回主线程**：把控制权拉回应用线程是框架的责任。
    vn::async::Task<void> startupStart() override
    {
        co_await vn::async::sleepFor(std::chrono::milliseconds(150));
    }

    /// 第二拍：进来时应该已经在应用线程上（框架在上一拍之后垫了一次 `resumeOnMainThread()`）。
    vn::async::Task<void> startup() override
    {
        second_phase_on_main.store(vn::appfw::MainThreadDispatcher::isMainThread());
        co_await Application::startup();
    }
};

/// 叶子等自己的东西时，主循环必须还在转：忙等的一拍会把定时器的最大间隔拉到那一拍的长度。
///
/// 这条同时钉住两件事：①"每拍都不许占住消息循环"（比"只有工作那一段不占"更强）；②叶子可以写出自己的
/// 异步拍（不用碰延续管道）。
TEST(HeadlessBootTest, ALeafPhaseCanWaitWithoutBlockingTheLoop)
{
    QStandardPaths::setTestModeEnabled(true);

    static char  arg0[] = "test_appfw";
    static char* argv[] = { arg0, nullptr };

    vn::appfw::AppConfig config;
    config.name           = u8"test_appfw";
    config.persist_config = false;
    config.load_plugins   = false; // 这条用例量的是"叶子那一拍在等"，插件不是主角

    auto app = std::make_unique<AwaitingPhaseApplication>(config, 1, argv);
    ASSERT_NE(app, nullptr);

    // 主线程上每 10 ms 一跳：整个启动期都应该在跳，而且没有任何一段间隔长到像被占住过。
    QElapsedTimer since_start;
    since_start.start();
    qint64 last_tick = 0;
    qint64 max_gap   = 0;
    int    ticks     = 0;
    QTimer ticker;
    ticker.setInterval(10);
    QObject::connect(&ticker, &QTimer::timeout, qApp, [&] {
        const qint64 now = since_start.elapsed();
        max_gap          = std::max(max_gap, now - last_tick);
        last_tick        = now;
        ++ticks;
    });
    ticker.start();

    QTimer::singleShot(400, qApp, [] { QCoreApplication::exit(0); });
    const int code = app->run();
    ticker.stop();

    EXPECT_EQ(code, 0);
    EXPECT_GE(ticks, 20) << "启动期主线程必须一直在派发（消息循环不被任何一拍占住）";
    EXPECT_LT(max_gap, 100) << "叶子那一拍等 150 ms 时循环也必须转：忙等会把最大间隔拉到 150 ms 以上";
    EXPECT_TRUE(app->second_phase_on_main.load()) << "叶子在别的线程上结束了上一拍，框架要把下一拍拉回应用线程";
    EXPECT_EQ(vn::appfw::StartupProgress::current(), nullptr) << "叶子自己等待的那一拍跑完后，启动要照常走完（上报口收起）";
}

/// 同步门（`loadAll()`）里插件把重活丢到池上、再回应用线程：等的时候必须派发"回应用线程"那条投递，否则死锁。
///
/// 这就是 `MainThreadDispatcher::runToCompletion()` 存在的理由：`.result()` 只阻塞、不派发，而钩子回来的那一步正是一条已投递的
/// 调用。用例没有循环在跑（它自己调同步门），所以这条投递只能由同步门派发；变异（把门改成只阻塞的 `Task::result()`）会挂死在这里。
TEST(HeadlessBootTest, TheSynchronousDoorLoadsAPluginThatComesBackToTheApplicationThread)
{
    QStandardPaths::setTestModeEnabled(true);

    static char  arg0[] = "test_appfw";
    static char* argv[] = { arg0, nullptr };

    vn::appfw::AppConfig config;
    config.name           = u8"test_appfw";
    config.persist_config = false;
    config.load_plugins   = false; // 同步门由用例自己调，不经过启动阶段

    vn::appfw::PluginManager::setBuiltInPluginDirectory(bootPluginDirectory());

    auto app = std::make_unique<WorkingHostApplication>(config, 1, argv);
    ASSERT_NE(app, nullptr);

    // 夹具：与界面无关的 60 ms 丢到池上，回来后 `resumeOnMainThread()` 回应用线程，并在回来后再断言一次线程。
    app->configManager()->setInt(u8"plugins.headless_boot.pool_ms", 60);

    EXPECT_TRUE(app->pluginManager()->loadAll()) << "同步门必须把一段\"跳池再回来\"的插件加载完";
    EXPECT_TRUE(app->pluginManager()->isLoaded(u8"headless_boot_plugin"));
    EXPECT_TRUE(app->pluginManager()->unloadAll());

    // 插件自己在那次加载里断言了两次线程（钩子在应用线程上、跳池回来还在应用线程上），所以加载成功就是那两条也过了。
    EXPECT_FALSE(app->pluginManager()->isLoaded(u8"headless_boot_plugin"));
}

/// 两个插件声明同一个身份（同 uuid、不同名字）：两个都照常被发现与加载，但日志点名。
///
/// 名字不同，所以除了 uuid 没有任何检查会发现它们是同一个插件——这正是 uuid 存在的理由。
TEST(HeadlessBootTest, TwoPluginsClaimingOneIdentityAreBothFoundAndReported)
{
    QStandardPaths::setTestModeEnabled(true);

    static char  arg0[] = "test_appfw";
    static char* argv[] = { arg0, nullptr };

    vn::appfw::AppConfig config;
    config.name           = u8"test_appfw";
    config.persist_config = false;
    config.load_plugins   = false;  // 同步门由用例自己调，不经过启动阶段

    vn::appfw::PluginManager::setBuiltInPluginDirectory(twinPluginDirectory());

    auto app = std::make_unique<WorkingHostApplication>(config, 1, argv);
    ASSERT_NE(app, nullptr);

    const WarningCapture warnings = captureWarnings();

    EXPECT_TRUE(app->pluginManager()->loadAll());
    EXPECT_TRUE(app->pluginManager()->isLoaded(u8"headless_boot_plugin"));
    EXPECT_TRUE(app->pluginManager()->isLoaded(u8"headless_boot_twin")) << "同一个身份的两个插件都要留下：人要看得见才能改";
    ASSERT_TRUE(warnings.contains("same identity")) << "身份冲突必须被报出来（uuid 就是为这个存在的）";
    EXPECT_TRUE(warnings.contains("headless_boot_twin")) << "告警要点名两边";

    EXPECT_TRUE(app->pluginManager()->unloadAll());
}

/// 注册文件里的 name/uuid 与库里的不符：身份以**库**为准 ⇒ 照常加载，日志两条警告点名注册文件。
TEST(HeadlessBootTest, ARegistrationThatDisagreesWithItsLibraryIsReportedAndTheLibraryWins)
{
    QStandardPaths::setTestModeEnabled(true);

    static char  arg0[] = "test_appfw";
    static char* argv[] = { arg0, nullptr };

    vn::appfw::AppConfig config;
    config.name           = u8"test_appfw";
    config.persist_config = false;
    config.load_plugins   = false;

    const std::filesystem::path dir = bootPluginDirectory();
    vn::appfw::PluginManager::setBuiltInPluginDirectory(dir);

    auto app = std::make_unique<WorkingHostApplication>(config, 1, argv);
    ASSERT_NE(app, nullptr);

    // 手写一份"名字与 uuid 都跟库里不符"的注册文件（安装器或人手写错就是这样）。指向库文件的注册
    // 才做这个比对：目录注册没法说清里面的每个插件是谁。
    const std::filesystem::path fixture  = VINE_HEADLESS_BOOT_PLUGIN;
    const std::filesystem::path library  = dir / ("headless_boot" + fixture.extension().string());
    const std::filesystem::path registry = app->pluginRegistrationDirectory() / "mismatch.plugin";

    std::error_code ec;
    std::filesystem::create_directories(registry.parent_path(), ec);
    {
        std::ofstream stream(registry, std::ios::binary | std::ios::trunc);
        ASSERT_TRUE(stream.good()) << "写不了注册文件 '" << registry.string() << "'";
        stream << "# 用例夹具：注册文件与它指的库不一致\n";
        stream << "path = " << library.string() << "\n";
        stream << "name = not_the_real_name\n";
        stream << "uuid = 11111111-1111-4111-8111-111111111111\n";
    }
    // 注册目录是每用户共享的：用例自己收尾，免得后面跑的用例多扫到这份文件。
    struct RemoveRegistration {
        std::filesystem::path path;
        ~RemoveRegistration()
        {
            std::error_code ec;
            std::filesystem::remove(path, ec);
        }
    } remove_registration{ registry };

    const WarningCapture warnings = captureWarnings();

    EXPECT_TRUE(app->pluginManager()->loadAll());
    EXPECT_TRUE(app->pluginManager()->isLoaded(u8"headless_boot_plugin")) << "身份以库为准：照常按库里的名字加载";
    EXPECT_FALSE(app->pluginManager()->isLoaded(u8"not_the_real_name"));
    EXPECT_TRUE(warnings.contains("names plugin")) << "注册文件写的名字与库不符要报";
    EXPECT_TRUE(warnings.contains("records uuid")) << "注册文件写的 uuid 与库不符要报";
    EXPECT_TRUE(warnings.contains("mismatch")) << "要点名是哪个注册文件";

    EXPECT_TRUE(app->pluginManager()->unloadAll());
}

/// 收尾必须发生在应用线程上：插件正是在 `unload()` 里拆自己的窗口与图形对象。用错线程要留下一条警告 ——
/// **只警告不拒绝**（调用者可能有自己的理由，而"拒绝收尾"比"不干净的收尾"更糟）。
TEST(HeadlessBootTest, ShutdownFromAnotherThreadIsReportedAndStillRuns)
{
    QStandardPaths::setTestModeEnabled(true);

    static char  arg0[] = "test_appfw";
    static char* argv[] = { arg0, nullptr };

    vn::appfw::AppConfig config;
    config.name           = u8"test_appfw";
    config.persist_config = false;
    config.load_plugins   = false;

    auto app = std::make_unique<WorkingHostApplication>(config, 1, argv);
    ASSERT_NE(app, nullptr);

    const WarningCapture warnings = captureWarnings();

    std::thread teardown([&app] { app->shutdown(); });
    teardown.join();

    EXPECT_TRUE(warnings.contains("not the application thread")) << "插件收尾在错的线程上必须被报出来";
    // 而且它**真的跑过**：总线已经是停的，第二次调用直接复述同一个结果（不是又等一遍）。
    EXPECT_TRUE(app->eventBus()->shutdownGracefully(std::chrono::milliseconds(10)));
}

} // namespace
