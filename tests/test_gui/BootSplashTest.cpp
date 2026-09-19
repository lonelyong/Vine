#include <gtest/gtest.h>

#include <vine/appfw/AppBuilder.hpp>
#include <vine/appfw/Application.hpp>
#include <vine/appfw/ProgressHost.hpp>
#include <vine/appfw/StartupProgress.hpp>
#include <vine/appfw/gui/BootSplash.hpp>
#include <vine/appfw/gui/GuiApplication.hpp>
#include <vine/appfw/gui/MainWindow.hpp>

using vine::appfw::ProgressHost;
using vine::appfw::SplashConfig;
using vine::appfw::StartupProgress;
using vine::appfw::gui::BootSplash;
using vine::appfw::gui::GuiApplication;

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

TEST(BootSplashTest, ShowsReportedStageAndProgress)
{
    StartupProgress boot;
    SplashConfig    config;
    config.title    = "Vine";
    config.subtitle = "test";

    BootSplash splash(config);
    // 还没上报任何阶段：状态文字为空，进度条显示为"进行中"。
    EXPECT_TRUE(splash.statusText().empty());
    EXPECT_TRUE(splash.isIndeterminate());

    boot.stage("正在查找插件");
    EXPECT_TRUE(splash.statusText() == u8"正在查找插件");
    EXPECT_TRUE(splash.isIndeterminate());

    boot.stage("正在加载插件", 4);
    EXPECT_FALSE(splash.isIndeterminate());
    EXPECT_NEAR(splash.progressFraction(), 0.0, 1e-9);

    boot.setLabel("正在加载插件 app_shell (2/4)");
    boot.advance(2);
    EXPECT_TRUE(splash.statusText() == u8"正在加载插件 app_shell (2/4)");
    EXPECT_NEAR(splash.progressFraction(), 0.5, 1e-9);

    boot.complete();
    EXPECT_NEAR(splash.progressFraction(), 1.0, 1e-9);
}

TEST(BootSplashTest, MissingLogoIsNotFatal)
{
    StartupProgress boot;
    SplashConfig    config;
    config.logo = "no_such_logo_file.svg";

    // 读不到的装饰按"没有 logo"处理：启动框不会因为一张图而让整个启动失败。
    BootSplash splash(config);
    EXPECT_TRUE(splash.isIndeterminate());
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
    EXPECT_FALSE(splash->isIndeterminate());
    EXPECT_NEAR(splash->progressFraction(), 0.5, 1e-9);
}

TEST(BootSplashTest, DisabledByDefaultInTheTestApplication)
{
    auto* app = vine::obj_cast<GuiApplication>(vine::appfw::Application::current());
    ASSERT_NE(app, nullptr);

    // test_gui 共享的 GuiApplication 用默认 AppConfig 创建：不开启动框就没有启动进度口，主窗口直接可见。
    EXPECT_EQ(app->startupProgress(), nullptr);
    EXPECT_EQ(app->bootSplash(), nullptr);
    ASSERT_NE(app->mainWindow(), nullptr);

    // finishStartup() 幂等：没有启动框时它只是保证主窗口处于可见状态。
    app->finishStartup();
    app->finishStartup();
    EXPECT_TRUE(app->mainWindow()->visible());
}

} // namespace
