#include "AppShellPlugin.hpp"

#include <vine/appfw/gui/MainWindow.hpp>
#include <vine/appfw/plugin_export.hpp>

#include "AppShellUi.hpp"
#include "ConsoleLogRouter.hpp"

V_APPFW_NS_BEGIN

V_OBJECT_META_IMPL(AppShellPlugin, Plugin)

AppShellPlugin::AppShellPlugin() = default;

void AppShellPlugin::load(PluginLoadContext* context)
{
    auto* wnd = gui::MainWindow::current();
    if (!wnd) {
        return;
    }

    // UI：Ribbon 选项卡 + 工作区停靠布局（见 AppShellUi）。
    buildAppShellRibbon(wnd);
    auto dock = buildAppShellDock(wnd);

    // 控制台日志路由：配置项注册、配置同步与 sink 安装（见 ConsoleLogRouter）。
    installConsoleLogSink(dock.console_panel, context);
}

void AppShellPlugin::unload(PluginLoadContext* context)
{
    (void)context;
    uninstallConsoleLogSink();
}

/// 插件图标（内联 SVG）。含逗号的 SVG 要放在命名常量里：V_DECLARE_PLUGIN 是宏，
/// 圆括号外层的逗号会把参数切开。
constexpr const char8_t* s_plugin_icon = u8R"SVG(<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 24 24"><g fill="none" stroke="#4c9a2a" stroke-width="1.8" stroke-linecap="round" stroke-linejoin="round"><rect x="3" y="4.5" width="18" height="15" rx="2"/><path d="M3 9h18"/><path d="M7.5 12.5h4.5"/><path d="M7.5 16h7" stroke-dasharray="3,2"/></g></svg>)SVG";

V_DECLARE_PLUGIN(AppShellPlugin, u8"759ff6a5-21b2-43a3-bf0d-67b215e4a8f8", u8"app_shell", u8"AppShell", u8"1.0.0", u8"应用外壳：基础界面（插件信息与配置管理）", u8"Vine",
                 u8"dev@vine.example", u8"https://github.com/vine/app_shell", s_plugin_icon, {})

V_APPFW_NS_END
