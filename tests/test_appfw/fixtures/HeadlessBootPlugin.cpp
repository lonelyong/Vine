// 测试夹具：一个什么都不做的插件，用来钉住"加载插件是框架的内置启动动作"。
// 用例把它拷进一个空目录当配置文件里的 built_in_plugin_dir，于是 loadAll() 只会扫描到它。
// 只有 test_appfw 构建它（与 test_gui 的 vine_plugin_fixture_* 同一套做法），不影响别的扫描。
//
// 它还带一条**只在用例设了配置时才走**的启动写法：与界面无关的长活丢到池上，回来时回应用线程再继续
// （`plugins.headless_boot.pool_ms`）。插件在池上那段里循环空着，应用线程谁也占不到它。
#include <chrono>
#include <stdexcept>
#include <thread>

#include <vine/appfw/Application.hpp>
#include <vine/appfw/ConfigManager.hpp>
#include <vine/appfw/MainThreadDispatcher.hpp>
#include <vine/appfw/Plugin.hpp>
#include <vine/appfw/PluginLoadContext.hpp>
#include <vine/appfw/StartupProgress.hpp>
#include <vine/appfw/plugin_export.hpp>

#include <vine/async/ThreadPoolScheduler.hpp>

VN_APPFW_NS_BEGIN

class HeadlessBootPlugin : public Plugin {
    VN_OBJECT_META_DECL;

  public:
    /**
     * @brief Loads the plugin, optionally exercising the pool pattern the test nails.
     *
     * The plugin always asserts the thread property it depends on (the hook runs on the application thread); the
     * configuration key "plugins.headless_boot.pool_ms" - a test hook, absent in a normal run - additionally makes it
     * send a stretch to the thread pool and come back to the application thread afterwards, which is what UI-free
     * startup work does.
     *
     * @param context Load context exposing host capabilities.
     * @return A task that completes when the plugin is loaded.
     */
    vn::async::Task<void> load(PluginLoadContext* context) override;
};

VN_OBJECT_META_IMPL(HeadlessBootPlugin, Plugin)

namespace
{

/// 应用线程上真占住 CPU 一段（留给“这里必须在这条线程上”的活；当前没有用例用它，保留是为了下一条钉子有地方挂）。
[[maybe_unused]] void busyOnApplicationThread(int milliseconds)
{
    const auto until = std::chrono::steady_clock::now() + std::chrono::milliseconds(milliseconds);
    while (std::chrono::steady_clock::now() < until) {
    }
}

} // namespace

vn::async::Task<void> HeadlessBootPlugin::load(PluginLoadContext* context)
{
    raw_ptr<Application> app = context != nullptr ? context->application() : nullptr;

    // 钩子必须在应用线程上跑（插件正是在这里建窗口、建图形对象）。夹具自己断言这条，用例就量得到它：
    // 不在应用线程上就把加载变成失败。
    if (MainThreadDispatcher::hasEventLoop() && !MainThreadDispatcher::isMainThread()) {
        throw std::runtime_error("headless_boot_plugin: load() ran off the application thread");
    }

    ConfigManager* config = app != nullptr ? app->configManager() : nullptr;
    const int      pool_ms = config != nullptr ? config->getInt(u8"plugins.headless_boot.pool_ms", 0) : 0;

    // 测试钩子：这一拍里就请求取消（模拟用户按了启动框上的关闭——它就是往这个 token 上写），并且**记下**
    // "这次启动的 token 从这个口能不能看到"——钩子里抛异常是看不见的（管理器会把它收成 loadAllAsync() 返回 false，
    // 而取消本身也让它返回 false），所以结果必须落到用例读得到的地方。
    if (config != nullptr && config->getBool(u8"plugins.headless_boot.cancel_in_load", false)) {
        if (StartupProgress* const progress = StartupProgress::current(); progress != nullptr) {
            progress->requestCancel();
        }
        config->setBool(u8"plugins.headless_boot.saw_token",
                        context != nullptr && context->stopToken().stop_requested());
    }

    // 模式：与界面无关的活丢到池上，回来时回应用线程再继续。池上那一段里循环空着，谁也占不到它。
    if (pool_ms > 0) {
        co_await vn::async::run([pool_ms] { std::this_thread::sleep_for(std::chrono::milliseconds(pool_ms)); });
        co_await MainThreadDispatcher::resumeOnMainThread();
        if (MainThreadDispatcher::hasEventLoop() && !MainThreadDispatcher::isMainThread()) {
            throw std::runtime_error("headless_boot_plugin: did not come back to the application thread");
        }
    }

    co_await Plugin::load(context);
}

VN_DECLARE_PLUGIN(HeadlessBootPlugin, u8"6a1b6d0e-0000-4000-8000-0000000000f1", u8"headless_boot_plugin", u8"Headless boot fixture",
                  u8"0.1.0", u8"Test fixture the framework loads during the startup phase", u8"Vine", u8"", u8"", u8"", {})

VN_APPFW_NS_END
