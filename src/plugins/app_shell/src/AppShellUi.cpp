#include "AppShellUi.hpp"

#include <cstring>
#include <memory>

#include <vine/appfw/Application.hpp>
#include <vine/appfw/Document.hpp>
#include <vine/appfw/DocumentManager.hpp>
#include <vine/appfw/gui/ConsolePanel.hpp>
#include <vine/appfw/gui/DockPanel.hpp>
#include <vine/appfw/gui/DockPanelManager.hpp>
#include <vine/appfw/gui/GuiApplication.hpp>
#include <vine/appfw/gui/Icon.hpp>
#include <vine/appfw/gui/MainWindow.hpp>
#include <vine/appfw/gui/RibbonBar.hpp>
#include <vine/appfw/gui/RibbonButton.hpp>
#include <vine/appfw/gui/RibbonGroup.hpp>
#include <vine/appfw/gui/RibbonTab.hpp>
#include <vine/appfw/gui/VisualUserIO.hpp>

#include <vine/logging/Log.hpp>

#include <vine/String.hpp>

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
    addCommandButton(help_group, u8"文档管理器", u8":/icons/show_documents.svg", u8"show_document_types");
    addCommandButton(help_group, u8"帮助", u8":/icons/show_help.svg", u8"show_help");
    addCommandButton(help_group, u8"关于", u8":/icons/about.svg", u8"about");
}

AppShellDock buildAppShellDock(gui::MainWindow* wnd)
{
    AppShellDock result;

    auto* manager = wnd->dockPanelManager();

    auto* left_panel = manager->createDockPanel(u8"项目", gui::DockAreas::Left);
    left_panel->setId(u8"dock_project");

    // 中央客户区**空着**：3D 内容现在是文档（一份文档一个视图、一块自己的渲染面），宿主的文档容器会在有当前文档时
    // 接管这一格，没文档时它就是窗口自己的背景。外壳不再在这里建 RenderControl / 发布它 —— 那是视图自己的事
    // （见 DemoView / appfw-document-model.md §7/§9）。

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

void openDocumentFromCommandLine(gui::MainWindow* wnd)
{
    (void)wnd; // 打开走文档管理器，不经过窗口；参数留着是因为调用点就在外壳里（将来要把它显示出来时就有地方）。

    const char* const* argv = Application::current() != nullptr ? Application::current()->argv() : nullptr;    if (argv == nullptr) {
        return;
    }

    // `--open <type-id>`：找第一个出现在参数里的那个（值在下一个参数里）。值缺失时什么都不做。
    String type_id;
    for (const char* const* it = argv; *it != nullptr; ++it) {
        if (std::strcmp(*it, "--open") == 0) {
            const char* value = *(it + 1);
            if (value != nullptr) {
                type_id = String::fromUtf8(value);
            }
            break;
        }
    }
    if (type_id.empty()) {
        return;
    }

    auto* documents = Application::current() != nullptr ? Application::current()->documentManager() : nullptr;
    if (documents == nullptr) {
        return;
    }

    // 和用户自己的命令走**同一条**路：能不能建由文档类型自己说了算（没有 create 工厂的类型就是打不开）。
    Document* document = documents->create(type_id);
    if (document == nullptr) {
        // 值可能是任意用户输入，直接当成一段字节打出来（不做任何解释）：它是命令行的回显，不是标识符解析。
        const std::string_view requested{ reinterpret_cast<const char*>(type_id.data()), type_id.size() };
        VN_LOGW("app_shell: --open {} could not be opened (no such type, or it cannot be created)", requested);
        return;
    }
    documents->setCurrent(document);
}

VN_APPFW_NS_END