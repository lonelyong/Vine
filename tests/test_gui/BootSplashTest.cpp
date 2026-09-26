#include <gtest/gtest.h>

#include <chrono>
#include <memory>
#include <thread>

#include <QCloseEvent>
#include <QCoreApplication>
#include <QElapsedTimer>
#include <QProgressBar>
#include <QPushButton>
#include <QThread>

#include <vine/async/SyncWait.hpp>

#include <vine/appfw/AppBuilder.hpp>
#include <vine/appfw/Application.hpp>
#include <vine/appfw/ProgressHost.hpp>
#include <vine/appfw/StartupProgress.hpp>
#include <vine/appfw/gui/BootSplash.hpp>
#include <vine/appfw/gui/GuiApplication.hpp>
#include <vine/appfw/gui/MainWindow.hpp>

#include "fixtures/TestGuiApplication.hpp"

using vn::appfw::ProgressHost;
using vn::appfw::SplashConfig;
using vn::appfw::StartupProgress;
using vn::appfw::gui::BootSplash;
using vn::appfw::gui::GuiApplication;

namespace
{

TEST(StartupProgressTest, NoSinkByDefault)
{
    EXPECT_EQ(StartupProgress::current(), nullptr);
    EXPECT_EQ(ProgressHost::current(), nullptr);
}
TEST(StartupProgressTest, SinkIsForegroundAndProcessWide)
{
    {
        StartupProgress boot;
        EXPECT_EQ(StartupProgress::current(), &boot);
        // 呈现层（启动框、状态栏）看的是前台宿主，所以启动口必须把自己提升到前台栈上。
        EXPECT_TRUE(ProgressHost::isActive());
        EXPECT_NE(ProgressHost::current(), nullptr);
    }
    EXPECT_EQ(StartupProgress::current(), nullptr);
    EXPECT_EQ(ProgressHost::current(), nullptr);
    EXPECT_FALSE(ProgressHost::isActive());
}

TEST(StartupProgressTest, IndeterminateStageReportsLabelOnly)
{
    StartupProgress boot;
    boot.stage("正在查找插件");
    EXPECT_EQ(boot.label(), "正在查找插件");
    EXPECT_FALSE(boot.isCounted());
}

TEST(StartupProgressTest, CountedStageReportsFraction)
{
    StartupProgress boot;
    boot.stage("正在加载插件", 4);
    EXPECT_TRUE(boot.isCounted());
    EXPECT_EQ(boot.label(), "正在加载插件");
    EXPECT_NEAR(boot.fraction(), 0.0, 1e-9);

    boot.advance(1);
    EXPECT_NEAR(boot.fraction(), 0.25, 1e-9);

    // 绝对值语义：报到 3 就是 3/4，而不是在 1/4 之上再加 3。
    boot.advance(3);
    EXPECT_NEAR(boot.fraction(), 0.75, 1e-9);

    // 倒退被忽略：进度条不回退。
    boot.advance(2);
    EXPECT_NEAR(boot.fraction(), 0.75, 1e-9);

    // 状态文字可以细到插件一级，而进度仍按整个阶段走。
    boot.setLabel("正在加载插件 app_shell (4/4)");
    EXPECT_EQ(boot.label(), "正在加载插件 app_shell (4/4)");
    EXPECT_NEAR(boot.fraction(), 0.75, 1e-9);

    boot.complete();
    EXPECT_FALSE(boot.isCounted());
    EXPECT_NEAR(boot.fraction(), 1.0, 1e-9);
}

TEST(StartupProgressTest, EveryCountedStageStartsOver)
{
    StartupProgress boot;
    boot.stage("阶段一", 2);
    boot.advance(2);
    EXPECT_NEAR(boot.fraction(), 1.0, 1e-9);

    // 可计数阶段各自从零开始：进度条读的是"本阶段的比例"，不是整次启动的剩余。
    boot.stage("阶段二", 10);
    EXPECT_NEAR(boot.fraction(), 0.0, 1e-9);
}

TEST(StartupProgressTest, StageWithoutTotalIsIndeterminate)
{
    StartupProgress boot;
    boot.stage("无总量的阶段", 0);
    EXPECT_FALSE(boot.isCounted());
}

TEST(BootSplashTest, ShowsReportedStageAndKeepsItsIndicatorTurning)
{
    StartupProgress boot;
    SplashConfig    config;
    config.title    = "Vine";
    config.subtitle = "test";

    BootSplash splash(config);
    auto* const frame = splash.impl<QWidget>();
    ASSERT_NE(frame, nullptr);

    // 还没上报任何阶段：状态文字为空。
    EXPECT_TRUE(splash.statusText().empty());

    // **没有进度条、没有百分比**（2026-09-26 决定）：启动没有已知总量，条只能骗人。框只说“在干什么”与“在动”。
    EXPECT_EQ(frame->findChild<QProgressBar*>(), nullptr) << "启动框不该有进度条";
    EXPECT_TRUE(splash.isBusyIndicatorRunning()) << "指示器必须是一直在转的那个东西";

    boot.stage("正在查找插件");
    EXPECT_TRUE(splash.statusText() == u8"正在查找插件");

    // 计数阶段也一样：文字照跟，但框上没有任何总量可看（条不存在，所以也没有“走到哪”这回事）。
    boot.stage("正在加载插件", 4);
    boot.setLabel("正在加载插件 app_shell (2/4)");
    boot.advance(2);
    EXPECT_TRUE(splash.statusText() == u8"正在加载插件 app_shell (2/4)");
    EXPECT_TRUE(splash.isBusyIndicatorRunning());

    // 指示器真的在动：两次抓图在事件循环跑过之后必须不一样（定时器停了就是静止的图）。
    const QPixmap before = frame->grab();
    const auto    deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(250);
    while (std::chrono::steady_clock::now() < deadline) {
        QCoreApplication::processEvents();
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    QCoreApplication::processEvents();
    EXPECT_NE(before.toImage(), frame->grab().toImage()) << "指示器必须在转：两次抓图一模一样就说明它是一张静止的图";
}

TEST(BootSplashTest, TheFrameRefusesToCloseWhileTheBootRuns)
{
    SplashConfig config;
    config.title = "Vine";

    StartupProgress boot;
    BootSplash      splash(config);

    // 启动期不允许关闭（2026-09-26 决定）：框上没有取消按钮，窗口系统送来的关闭请求也被拒——启动的脸是框架撤的
    // （startupEnd() 析构它），用户提前把它关掉只会得到一段空屏幕。取消的机器仍在，只是不接到界面上。
    auto* const frame = splash.impl<QWidget>();
    ASSERT_NE(frame, nullptr);
    EXPECT_EQ(frame->findChild<QPushButton*>(), nullptr) << "启动期不给关闭入口：框上不该有取消按钮";

    QCloseEvent request;
    QCoreApplication::sendEvent(frame, &request);
    EXPECT_FALSE(request.isAccepted()) << "关闭请求必须被拒（否则用户可以中途把启动框撤走）";
}

TEST(BootSplashTest, MissingLogoIsNotFatal)
{
    StartupProgress boot;
    SplashConfig    config;
    config.logo = "no_such_logo_file.svg";

    // 读不到的装饰按"没有 logo"处理：启动框不会因为一张图而让整个启动失败。
    BootSplash splash(config);
    EXPECT_TRUE(splash.isBusyIndicatorRunning());
}

TEST(BootSplashTest, FrameSurvivesTheEndOfTheBoot)
{
    SplashConfig config;
    config.title = "Vine";

    std::unique_ptr<BootSplash> splash;
    {
        StartupProgress boot;
        splash = std::make_unique<BootSplash>(config);
        boot.stage("正在加载插件", 2);
        boot.advance(1);
    }

    // 上报口先于启动框销毁（宿主先关框），此时启动框保留最后一帧而不是读已销毁的上报口。
    EXPECT_TRUE(splash->statusText() == u8"正在加载插件");
    EXPECT_TRUE(splash->isBusyIndicatorRunning());
}

TEST(BootSplashTest, AnEmptyTitleShowsTheApplicationName)
{
    // 身份先于窗口应用（构造函数里先设 name/组织，再建窗口），所以空标题由启动框自己解析成应用名。
    // GUI builder 里那句 `splash.title = config.name` 的补丁就是因此删掉的。
    SplashConfig config;
    BootSplash   splash(config);

    EXPECT_EQ(QString::fromStdString(splash.windowTitle().as_std_str()), QCoreApplication::applicationName());
    EXPECT_FALSE(splash.windowTitle().empty());
}

TEST(BootSplashTest, TheFramePaintsOnceTheWindowSystemHasShownIt)
{
    SplashConfig config;
    config.title = "Vine";

    BootSplash splash(config);

    // 还没 show：没有窗口，也就没有任何绘制。
    EXPECT_FALSE(splash.hasPainted());

    splash.show();

    // show() 只把窗口交给窗口系统：“现在可见了”的通知（X11 是 expose 事件）要派发事件队列才会到窗口。
    // 宿主因此要在 show() 之后派发到首帧（GUI 的上屏路径：`GuiApplication::startupStart()` 里等首帧），
    // 否则窗口里一个像素都没有：WSLg 实测整个启动期全透明，十六次上报都没能把它画上屏。
    QElapsedTimer timer;
    timer.start();
    while (!splash.hasPainted() && timer.elapsed() < 2000) {
        QCoreApplication::processEvents();
        QThread::msleep(5);
    }

    EXPECT_TRUE(splash.hasPainted());
}

TEST(BootSplashTest, DisabledByDefaultInTheTestApplication)
{
    auto* app = static_cast<TestGuiApplication*>(vn::appfw::Application::current());
    ASSERT_NE(app, nullptr);

    // test_gui 共享的 GuiApplication 用默认 AppConfig 创建：不开启动框就没有启动进度口，主窗口直接可见。
    EXPECT_EQ(app->startupProgress(), nullptr);
    EXPECT_EQ(app->bootSplash(), nullptr);
    ASSERT_NE(app->mainWindow(), nullptr);

    // startupEnd() 幂等：没有启动框时它只是保证主窗口处于可见状态。
    // 钩子是受保护的（收尾是框架的动作），而且是一个**懒**任务 - 只有 await 它才会跑：本进程不跑循环，
    // 用例用 syncWait 在调用线程上把它驱动完（见 fixtures/TestGuiApplication.hpp）。
    vn::async::syncWait(app->startupEnd());
    vn::async::syncWait(app->startupEnd());
    EXPECT_TRUE(app->mainWindow()->visible());
}

TEST(GuiApplicationConstructionTest, TheBuilderReturnsAFinishedApplication)
{
    // 构造函数即完成全部初始化：builder 只把 AppConfig 交进来，进程身份、Qt 应用对象、UserIO 与配置文件都在
    // 构造里办完 —— 没有 "先构造、再 init()" 的第二步（Application::init() 与 setSplashConfig() 已删）。
    // 因此把 AppConfig 从构造路径上拿掉（例如重新变成构造后单独调用）就会红。
    auto* app = vn::obj_cast<GuiApplication>(vn::appfw::Application::current());
    ASSERT_NE(app, nullptr);

    EXPECT_NE(QCoreApplication::instance(), nullptr);
    EXPECT_EQ(QCoreApplication::applicationName(), QStringLiteral("test_gui"));
    // 身份在构造里先应用（AppConfig::organization 缺省 ⇒ 回落框架默认组织），路径都读它。
    EXPECT_EQ(QCoreApplication::organizationName(), QStringLiteral("Vine"));
    EXPECT_NE(app->userIO(), nullptr);
    ASSERT_NE(app->mainWindow(), nullptr);

    // persist_config 默认打开：构造函数已经把默认配置文件装上（<数据目录>/config/test_gui.json）。
    EXPECT_EQ(app->configFile(), app->defaultConfigFile());
}

} // namespace
