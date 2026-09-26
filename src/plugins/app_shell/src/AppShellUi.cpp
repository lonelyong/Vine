#include "AppShellUi.hpp"

#include <memory>

#include <vine/appfw/Application.hpp>
#include <vine/appfw/gui/ConsolePanel.hpp>
#include <vine/appfw/gui/DockPanel.hpp>
#include <vine/appfw/gui/DockPanelManager.hpp>
#include <vine/appfw/gui/GuiApplication.hpp>
#include <vine/appfw/gui/Icon.hpp>
#include <vine/appfw/gui/MainWindow.hpp>
#include <vine/appfw/gui/RenderControl.hpp>
#include <vine/appfw/gui/RibbonBar.hpp>
#include <vine/appfw/gui/RibbonButton.hpp>
#include <vine/appfw/gui/RibbonGroup.hpp>
#include <vine/appfw/gui/RibbonTab.hpp>
#include <vine/appfw/gui/VisualUserIO.hpp>

#include <vine/logging/Log.hpp>

#include "AppShellDemo.hpp"

VN_APPFW_NS_BEGIN

namespace
{

/**
 * @brief Adds a large Ribbon button executing the given command by name.
 *
 * @param group Target group.
 * @param text Button label.
 * @param icon Icon resource path.
 * @param command Registered command name.
 * @return The created button (owned by the group).
 */
gui::RibbonButton* addCommandButton(gui::RibbonGroup* group, const String& text, const String& icon, const String& command)
{
    auto* button = new gui::RibbonButton();
    button->setText(text);
    button->setIcon(gui::Icon(icon));
    button->setButtonSize(gui::RibbonItemSize::Large);
    button->setCommand(command);
    group->addButton(button);
    return button;
}

} // namespace

void buildAppShellRibbon(gui::MainWindow* wnd)
{
    auto* bar = wnd->ribbonBar();

    auto* plugin_tab = new gui::RibbonTab();
    plugin_tab->setTitle(u8"插件");
    bar->addTab(plugin_tab);
    auto* plugin_group = new gui::RibbonGroup();
    plugin_group->setTitle(u8"插件管理");
    plugin_tab->addGroup(plugin_group);

    auto* help_tab = new gui::RibbonTab();
    help_tab->setTitle(u8"帮助");
    bar->addTab(help_tab);
    auto* help_group = new gui::RibbonGroup();
    help_group->setTitle(u8"帮助");
    help_tab->addGroup(help_group);

    addCommandButton(plugin_group, u8"插件信息", u8":/icons/show_plugins.svg", u8"show_plugins");
    addCommandButton(plugin_group, u8"渲染后端", u8":/icons/show_plugins.svg", u8"show_render_backends");
    addCommandButton(plugin_group, u8"配置管理", u8":/icons/show_config.svg", u8"show_config");
    addCommandButton(help_group, u8"命令管理器", u8":/icons/show_commands.svg", u8"show_commands");
    addCommandButton(help_group, u8"帮助", u8":/icons/show_help.svg", u8"show_help");
    addCommandButton(help_group, u8"关于", u8":/icons/about.svg", u8"about");
}

AppShellDock buildAppShellDock(gui::MainWindow* wnd)
{
    AppShellDock result;

    auto* manager = wnd->dockPanelManager();

    auto* left_panel = manager->createDockPanel(u8"项目", gui::DockAreas::Left);
    left_panel->setId(u8"dock_project");

    // Render view in the central client area. The control goes into the window first (its surface starts at the
    // degenerate size a fresh QWindow has and the layout gives it the real one), then init() brings the backend up
    // synchronously - device and pipelines built inside load(), instead of in the first turn of the event loop, which is
    // the window between "the startup frame closes" and "the window can be painted". The size the surface has right now
    // does not matter: the resize path rebuilds the swapchain at the real size once the layout has run.
    // The control attaches when it is asked to and not before (see RenderControl: the host owns the timing, the
    // control only maintains an established session), so this line is the one that decides when the device and
    // the pipelines are built. The rest of the lifecycle - and its timings - are in RenderControl's log lines and
    // its state_changed.
    auto* render_control = new gui::RenderControl();
    manager->setCentralWidget(render_control);

    // The default demo (content, overlays, diagnostic passes, pipeline) lives in AppShellDemo; the shell only hands it
    // the render control. install() builds its SKELETON (scenes, camera, passes, pipeline) and must run before the
    // control is initialized (see initAppShellRenderControl()), which is what creates the orbit manipulator that takes
    // its home vantage from the camera the demo positioned.
    auto demo = std::make_shared<AppShellDemo>(render_control);
    demo->install();

    // 内容（两张 cube map）的重活**现在就出去**：池上并行读/解码/降采样，和随后的会话 init()（设备与管线，~220 ms）
    // 重叠。等会话建好时它常常已经回来了，所以主窗上屏时场景基本是完整的（实测：两者相差 ~30 ms 以内）。
    // 界面本身不阻塞：这一步只是把活丢出去（见 AppShellDemo::assembleDemoContentLater()）。
    assembleDemoContentLater(demo);

    result.render_control = render_control;

    auto* right_panel = manager->createDockPanel(u8"属性", gui::DockAreas::Right);
    right_panel->setId(u8"dock_properties");

    auto* console_panel = new gui::ConsolePanel();
    auto* console_dock  = manager->createDockPanel(u8"控制台", console_panel, gui::DockAreas::Bottom);
    console_dock->setId(u8"dock_console");

    // The panel is bound on the visual user I/O itself: that is where the output and the prompts go, and it is the panel
    // that decides whether the interactive console exists at all (a host whose UserIO does not take one has no console).
    if (auto* io = ::vn::obj_cast<gui::VisualUserIO>(Application::current() ? Application::current()->userIO() : nullptr)) {
        io->setConsolePanel(console_panel);
    }
    else {
        VN_LOGW("app_shell: no visual user I/O to bind the console panel to; the console will show nothing");
    }

    result.console_panel = console_panel;
    return result;
}

vn::async::Task<void> initAppShellRenderControl(gui::MainWindow* wnd, const AppShellDock& dock)
{
    if (wnd == nullptr || dock.render_control == nullptr) {
        co_return;
    }

    // 会话（attach + 设备/管线）建在这里，也就是骨架建好之后、主窗上屏之前：管线由 install() 注册的 pass 建，而且
    // 实测这一句必须贴着主窗上屏（旧序里两者只差 50 ms；提前到插件 load() 开头时，渲染面的原生窗口一直不被 map，
    // 门禁读到 not viewable）。之所以单独成一个协程：它是一段自己在等的活——设备与管线那一段只要句柄、不要应用线程，
    // initAsync() 把它丢到池上，应用线程于是回到循环（启动框能重画、消息能到），而取句柄、状态、尺寸、warm-up 这些
    // 仍然在应用线程上。
    const bool attached = co_await dock.render_control->initAsync();
    if (!attached) {
        // 没建上不是致命：控件保持隐藏，宿主/用户改尺寸或重试时它会自己再试（见 RenderControl::init()）。
        VN_LOGW("the render session could not be attached yet; the 3D view stays empty until it can");
    }

    // Register the 3D view so other plugins (tests/editors) can reach the render engine/scene without depending on app
    // shell internals.
    wnd->setPrimaryRenderControl(dock.render_control);
}

VN_APPFW_NS_END
