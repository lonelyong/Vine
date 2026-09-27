#include "AppShellPlugin.hpp"

#include <vine/appfw/MainThreadDispatcher.hpp>
#include <vine/appfw/StartupProgress.hpp>
#include <vine/appfw/gui/MainWindow.hpp>
#include <vine/appfw/plugin_export.hpp>

#include "AppShellUi.hpp"
#include "ConsoleLogRouter.hpp"

namespace
{

/// 上报当前相位。启动期没有上报口（无头启动、或启动已经结束）时是空操作，所以调用处不必判断。
void reportPhase(const char8_t* text)
{
    if (vn::appfw::StartupProgress* progress = vn::appfw::StartupProgress::current(); progress != nullptr) {
        progress->setLabel(text);
    }
}

} // namespace

VN_APPFW_NS_BEGIN

VN_OBJECT_META_IMPL(AppShellPlugin, Plugin)

AppShellPlugin::AppShellPlugin() = default;

vn::async::Task<void> AppShellPlugin::load(PluginLoadContext* context)
{
    auto* wnd = gui::MainWindow::current();
    if (!wnd) {
        co_return;
    }

    // 这一拍的三段各自报一次相位：进度按相位跳（每个相位内不再动），所以每一段开始前把“在干什么”说清楚。
    reportPhase(u8"正在准备功能栏");
    buildAppShellRibbon(wnd);

    reportPhase(u8"正在准备面板");
    dock_ = buildAppShellDock(wnd);

    // 控制台日志路由：配置项注册、配置同步与 sink 安装（见 ConsoleLogRouter）。
    reportPhase(u8"正在接通控制台");
    installConsoleLogSink(dock_.console_panel, context);
    co_return;
}

vn::async::Task<void> AppShellPlugin::postLoad(PluginLoadContext* context)
{
    (void)context;

    auto* wnd = gui::MainWindow::current();
    if (!wnd) {
        co_return;
    }

    // 启动参数指定的那份文档**在启动里**打开（不是启动后第一拍）：打开会建会话，而**在窗口上屏之前**建会话，
    // 那第一帧才是屏外的预热帧、由线程池渲染（`initAsync()` 的契约）。排到窗口上屏之后，第一帧就由应用线程
    // 渲染，启动占用判据直接超（实测 204 ms / 限 150 ms）—— 这正是"宿主给时机"要早给的原因。
    openDocumentFromCommandLine(wnd);
    co_return;
}

void AppShellPlugin::unload(PluginLoadContext* context)
{
    (void)context;
    uninstallConsoleLogSink();
}

/// 插件图标（内联 SVG）。含逗号的 SVG 要放在命名常量里：VN_DECLARE_PLUGIN 是宏，
/// 圆括号外层的逗号会把参数切开。
constexpr const char8_t* kPluginIcon = u8R"SVG(<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 24 24"><g fill="none" stroke="#4c9a2a" stroke-width="1.8" stroke-linecap="round" stroke-linejoin="round"><rect x="3" y="4.5" width="18" height="15" rx="2"/><path d="M3 9h18"/><path d="M7.5 12.5h4.5"/><path d="M7.5 16h7" stroke-dasharray="3,2"/></g></svg>)SVG";

VN_DECLARE_PLUGIN(AppShellPlugin, u8"759ff6a5-21b2-43a3-bf0d-67b215e4a8f8", u8"app_shell", u8"AppShell", u8"1.0.0", u8"应用外壳：基础界面（插件信息与配置管理）", u8"Vine",
                 u8"dev@vine.example", u8"https://github.com/vine/app_shell", kPluginIcon, {})

VN_APPFW_NS_END
