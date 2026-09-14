#include "AppShellUi.hpp"

#include <QTimer>

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

#include "AppShellDemo.hpp"

V_APPFW_NS_BEGIN

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

    // Render view in the central client area; init() picks the first
    // registered render backend (e.g. "vsg") by default.
    auto* render_control = new gui::RenderControl();
    manager->setCentralWidget(render_control);

    // The default demo (content, overlays, diagnostic passes, pipeline) lives in
    // AppShellDemo; the shell only hands it the render control.
    AppShellDemo demo(render_control);
    demo.install();

    // Register the 3D view so other plugins (tests/editors) can reach the
    // render engine/scene without depending on app shell internals.
    wnd->setPrimaryRenderControl(render_control);
    result.render_control = render_control;

    // Initialize once the window is shown and the central widget is laid out:
    // the backend attaches to the realized native surface. Deferred so the
    // first layout pass has happened by the time init() runs.
    QTimer::singleShot(100, [render_control] { render_control->init(); });

    auto* right_panel = manager->createDockPanel(u8"属性", gui::DockAreas::Right);
    right_panel->setId(u8"dock_properties");

    auto* console_panel = new gui::ConsolePanel();
    auto* console_dock  = manager->createDockPanel(u8"控制台", console_panel, gui::DockAreas::Bottom);
    console_dock->setId(u8"dock_console");

    if (auto* app = ::vine::obj_cast<gui::GuiApplication>(Application::current())) {
        app->setConsolePanel(console_panel);
    }

    result.console_panel = console_panel;
    return result;
}

V_APPFW_NS_END
