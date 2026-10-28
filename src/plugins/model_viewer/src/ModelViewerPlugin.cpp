#include "ModelViewerPlugin.hpp"

#include <vine/appfw/Application.hpp>
#include <vine/appfw/CommandManager.hpp>
#include <vine/appfw/DocumentManager.hpp>
#include <vine/appfw/PluginLoadContext.hpp>
#include <vine/appfw/StartupProgress.hpp>
#include <vine/appfw/gui/DockPanel.hpp>
#include <vine/appfw/gui/DockPanelManager.hpp>
#include <vine/appfw/gui/DocumentViewRegistry.hpp>
#include <vine/appfw/gui/GuiApplication.hpp>
#include <vine/appfw/gui/MainWindow.hpp>
#include <vine/appfw/gui/RibbonBar.hpp>
#include <vine/appfw/gui/RibbonButton.hpp>
#include <vine/appfw/gui/RibbonGroup.hpp>
#include <vine/appfw/gui/RibbonTab.hpp>
#include <vine/appfw/plugin_export.hpp>
#include <vine/logging/Log.hpp>

#include "MeshFileOpener.hpp"
#include "ModelDocument.hpp"
#include "ModelInfoPanel.hpp"
#include "ModelPayload.hpp"
#include "ModelRenderView.hpp"
#include "commands/ShadingModeCommands.hpp"

namespace vn::model_viewer
{

VN_OBJECT_META_IMPL(ModelViewerPlugin, vn::appfw::Plugin)

ModelViewerPlugin::ModelViewerPlugin() = default;

vn::async::Task<void> ModelViewerPlugin::load(vn::appfw::PluginLoadContext* context)
{
    auto* app = context != nullptr ? context->application() : nullptr;
    if (app == nullptr) {
        co_return;
    }

    registerDocumentSurface(*app);
    registerView(*app);

    if (vn::appfw::StartupProgress* progress = vn::appfw::StartupProgress::current(); progress != nullptr) {
        progress->setLabel(u8"正在准备模型查看器");
    }
    installInfoPanel(*app);

    co_return;
}

void ModelViewerPlugin::registerView(vn::appfw::Application& app)
{
    // 视图注册表只有 GUI 宿主才有（视图就是 UIElement）：无头运行里根本没有"中央区显示什么"这件事。
    auto* gui_app = dynamic_cast<vn::appfw::gui::GuiApplication*>(&app);
    if (gui_app == nullptr) {
        return;
    }

    auto* views = gui_app->viewRegistry();
    if (views == nullptr) {
        return;
    }

    // 一类型一份视图：`"model"` 的**渲染方式**登记在这里（视图就是渲染器，见 ModelRenderView），
    // 谁来登记不受限制（可以是拥有这个类型的插件，也可以不是）。
    if (!views->registerView(ModelDocument::kTypeId, &createRenderView)) {
        VN_LOGW("model_viewer: the view for '{}' was not registered (taken, or empty id)", "model");
    }
}

void ModelViewerPlugin::registerDocumentSurface(vn::appfw::Application& app)
{
    auto* documents = app.documentManager();
    if (documents == nullptr) {
        return;
    }

    vn::appfw::DocumentTypeRegistration registration;
    registration.type_id      = ModelDocument::kTypeId;
    registration.display_name = u8"模型";
    registration.description  = u8"模型查看器文档：一份解析好的网格（只读）";
    // 没有 create 工厂是**合法**的：模型只能从载荷打开（can_create == false，面板会如实说"否（只能打开）"）。
    documents->registerType(registration);

    documents->registerOpener<MeshPayload>({
      .type_id = ModelDocument::kTypeId,
      .open    = [](const MeshPayload& payload) -> vn::appfw::Document* {
          if (payload.mesh == nullptr) {
              return nullptr; // 没有网格的载荷我不接（别的打开器还能收拾它）。
          }
          return new ModelDocument(payload.name, payload.mesh);
      },
      .source_scheme = u8"memory",
    });

    documents->registerOpener<MeshFilePayload>({
      .type_id = ModelDocument::kTypeId,
      .open    = [](const MeshFilePayload& payload) -> vn::appfw::Document* {
          // 怎么读、用什么选项读，都在这里（header-only，所以用例可以直接拿同一段代码验）。
          return openMeshFilePayload(payload);
      },
      .source_scheme = u8"file",
    });
}

vn::async::Task<void> ModelViewerPlugin::postLoad(vn::appfw::PluginLoadContext* context)
{
    auto* app = context != nullptr ? context->application() : nullptr;
    if (app != nullptr) {
        installShadingModeRibbon(*app);
    }
    co_return;
}

namespace
{

/// 登记一条"显示模式"命令：名字被占时不说难听话 —— 插件重载会走到这里第二次，那不是错误。
template <typename CommandT>
void registerShadingCommand(vn::appfw::CommandManager& commands, const char8_t* id)
{
    const vn::String name(id);
    if (!commands.registerCommand<CommandT>(name)) {
        VN_LOGW("model_viewer: the command '{}' was not registered (the name is taken)", reinterpret_cast<const char*>(id));
    }
}

} // namespace

void ModelViewerPlugin::installShadingModeRibbon(vn::appfw::Application& app)
{
    // 登记命令：一处说得清"这五条属于本插件"（不用 VN_DECLARE_COMMAND 的静态自动登记，见 ShadingModeCommands.hpp
    // 的说明）。
    if (auto* commands = app.commandManager(); commands != nullptr) {
        registerShadingCommand<SetShadingLitCommand>(*commands, kShadingModeLitCommand);
        registerShadingCommand<SetShadingFlatCommand>(*commands, kShadingModeFlatCommand);
        registerShadingCommand<SetShadingUnlitCommand>(*commands, kShadingModeUnlitCommand);
        registerShadingCommand<SetShadingNormalsCommand>(*commands, kShadingModeNormalsCommand);
        registerShadingCommand<SetShadingWireframeCommand>(*commands, kShadingModeWireframeCommand);
    }

    auto* wnd = vn::appfw::gui::MainWindow::current();
    if (wnd == nullptr) {
        return; // 无头运行：没有 ribbon，命令本身照样能执行。
    }
    auto* bar = wnd->ribbonBar();
    if (bar == nullptr) {
        return;
    }

    // 幂等：插件重载会再走一遍 postLoad()；tab 的 id 是它的身份（标题只用于显示）。
    const vn::String tab_id(u8"model_viewer.tab.model");
    if (bar->findTab(tab_id) != nullptr) {
        return;
    }

    // 自己的 tab（外壳那几个 tab 是它自己的事）：插件给自家功能挂 UI，与"面板自己建自己挂"同一条规矩。
    auto* tab = new vn::appfw::gui::RibbonTab();
    tab->setId(tab_id);
    tab->setTitle(u8"模型");

    // 帮助是终止 tab：本 tab 放到它前面，而不是追加到它后面。
    const auto placed = bar->addTab(tab, vn::appfw::gui::RibbonPlacement::before(vn::appfw::gui::RibbonTabRole::Help));
    if (placed.status != vn::appfw::gui::RibbonPlaceStatus::Placed) {
        VN_LOGW("model_viewer: the '模型' tab was placed at a fallback position");
    }

    auto* group = new vn::appfw::gui::RibbonGroup();
    group->setId(u8"model_viewer.group.shading");
    group->setTitle(u8"显示");
    tab->addGroup(group);

    // 五档各一个按钮，勾选态由"这块 3D 区现在画的是哪一档"回答（provider 读派发目标，见 isCurrentShadingMode）：
    // 于是切文档、命令执行之后按钮都会自己跟上，而不是按钮自己记一个可能过期的档位。
    const struct {
        const char8_t*            label;
        const char8_t*            command;
        vn::graphics::ShadingMode mode;
    } entries[] = {
        { u8"实面",   kShadingModeLitCommand,       vn::graphics::ShadingMode::Lit       },
        { u8"平面",   kShadingModeFlatCommand,      vn::graphics::ShadingMode::Flat      },
        { u8"无光照", kShadingModeUnlitCommand,     vn::graphics::ShadingMode::Unlit     },
        { u8"法线",   kShadingModeNormalsCommand,   vn::graphics::ShadingMode::Normals   },
        { u8"线框",   kShadingModeWireframeCommand, vn::graphics::ShadingMode::Wireframe },
    };
    for (const auto& item : entries) {
        auto* button = new vn::appfw::gui::RibbonButton();
        button->setText(vn::String(item.label));
        button->setButtonSize(vn::appfw::gui::RibbonItemSize::Large);
        button->setCommand(vn::String(item.command));
        button->setCheckedProvider([mode = item.mode] { return isCurrentShadingMode(mode); });
        group->addButton(button);
    }

    VN_LOGI("model_viewer: the '显示' ribbon group is up (5 shading-mode buttons)");
}

void ModelViewerPlugin::installInfoPanel(vn::appfw::Application& app)
{
    auto* wnd = vn::appfw::gui::MainWindow::current();
    if (wnd == nullptr) {
        // 无头运行（测试、工具）：没有窗口就没有面板；插件其余部分照常工作。
        return;
    }

    auto* docks = wnd->dockPanelManager();
    if (docks == nullptr) {
        VN_LOGW("model_viewer: the main window has no dock panel manager; the info panel is not installed");
        return;
    }

    auto* panel = new ModelInfoPanel();
    auto* dock  = docks->createDockPanel(u8"模型信息", panel, vn::appfw::gui::DockAreas::Right);
    if (dock == nullptr) {
        // 没挂上就自己删：dock 面板接管内容所有权（DockPanel::setContent），没接管就不能留一个没人拥有的控件。
        delete panel;
        return;
    }
    dock->setId(u8"dock_model_info");

    // 挂上之后再接线：面板跟着**当前文档**走，它自己听事件（"面板自己的内容自己听"，见 §12）。
    if (auto* documents = app.documentManager(); documents != nullptr) {
        panel->followDocumentManager(*documents);
    }
}

} // namespace vn::model_viewer

/// 插件图标（内联 SVG）。含逗号的 SVG 要放在命名常量里：VN_DECLARE_PLUGIN 是宏，圆括号外层的逗号会把参数切开。
constexpr const char8_t* kPluginIcon =
    u8R"SVG(<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 24 24"><g fill="none" stroke="#2a7ac9" stroke-width="1.8" stroke-linecap="round" stroke-linejoin="round"><path d="M12 3.2 20 7.6v8.8L12 20.8 4 16.4V7.6z"/><path d="M4 16.4 12 12l8 4.4"/><path d="M12 3.2V12"/></g></svg>)SVG";

// 宏在**全局命名空间**展开（VN_DEFINE_MODULE_COMMAND_QUEUE() 在 vn::appfw::detail 里定义函数，要求展开位置必须
// 包着那个命名空间），所以类名写成全限定名 —— 同 gfx_backend_vsg 与 demo_plugin。
VN_DECLARE_PLUGIN(vn::model_viewer::ModelViewerPlugin,
                  u8"6d1f3b90-7c4e-4a2f-9d38-5e2b8a4c1f77",
                  u8"model_viewer",
                  u8"模型查看器",
                  u8"1.0.0",
                  u8"模型查看器：一种文档类型 + 模型信息面板（顶点数 / 三角形数）",
                  u8"Vine",
                  u8"dev@vine.example",
                  u8"https://github.com/vine/model_viewer",
                  kPluginIcon,
                  {})
