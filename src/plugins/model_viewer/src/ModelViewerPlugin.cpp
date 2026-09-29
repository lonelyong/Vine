#include "ModelViewerPlugin.hpp"

#include <vine/appfw/Application.hpp>
#include <vine/appfw/DocumentManager.hpp>
#include <vine/appfw/PluginLoadContext.hpp>
#include <vine/appfw/StartupProgress.hpp>
#include <vine/appfw/gui/DockPanel.hpp>
#include <vine/appfw/gui/DockPanelManager.hpp>
#include <vine/appfw/gui/DocumentViewRegistry.hpp>
#include <vine/appfw/gui/GuiApplication.hpp>
#include <vine/appfw/gui/MainWindow.hpp>
#include <vine/appfw/plugin_export.hpp>
#include <vine/logging/Log.hpp>

#include "MeshFileOpener.hpp"
#include "ModelDocument.hpp"
#include "ModelInfoPanel.hpp"
#include "ModelPayload.hpp"
#include "ModelRenderView.hpp"

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
constexpr const char8_t* kPluginIcon = u8R"SVG(<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 24 24"><g fill="none" stroke="#2a7ac9" stroke-width="1.8" stroke-linecap="round" stroke-linejoin="round"><path d="M12 3.2 20 7.6v8.8L12 20.8 4 16.4V7.6z"/><path d="M4 16.4 12 12l8 4.4"/><path d="M12 3.2V12"/></g></svg>)SVG";

// 宏在**全局命名空间**展开（VN_DEFINE_MODULE_COMMAND_QUEUE() 在 vn::appfw::detail 里定义函数，要求展开位置必须
// 包着那个命名空间），所以类名写成全限定名 —— 同 gfx_backend_vsg 与 demo_plugin。
VN_DECLARE_PLUGIN(vn::model_viewer::ModelViewerPlugin, u8"6d1f3b90-7c4e-4a2f-9d38-5e2b8a4c1f77", u8"model_viewer",
                 u8"模型查看器", u8"1.0.0", u8"模型查看器：一种文档类型 + 模型信息面板（顶点数 / 三角形数）", u8"Vine",
                 u8"dev@vine.example", u8"https://github.com/vine/model_viewer", kPluginIcon, {})
