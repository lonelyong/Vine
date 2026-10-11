#include "ShadingModeCommands.hpp"

#include <vine/appfw/Application.hpp>
#include <vine/appfw/DocumentManager.hpp>
#include <vine/appfw/gui/MainWindow.hpp>
#include <vine/appfw/gui/RenderControl.hpp>

#include "../ModelDocument.hpp"

namespace vn::model_viewer
{

VN_OBJECT_META_IMPL(SetShadingLitCommand, vn::appfw::Command)
VN_OBJECT_META_IMPL(SetShadingFlatCommand, vn::appfw::Command)
VN_OBJECT_META_IMPL(SetShadingUnlitCommand, vn::appfw::Command)
VN_OBJECT_META_IMPL(SetShadingNormalsCommand, vn::appfw::Command)
VN_OBJECT_META_IMPL(SetShadingWireframeCommand, vn::appfw::Command)

vn::async::Task<vn::appfw::CommandResult> ShadingModeCommand::execute(vn::appfw::CommandExecutionContext* context)
{
    auto* app       = context != nullptr ? context->application() : nullptr;
    auto* documents = app != nullptr ? app->documentManager() : nullptr;
    if (documents == nullptr) {
        co_return vn::appfw::CommandResult(vn::appfw::CommandStatus::Failed, vn::String(u8"应用未就绪"));
    }

    // 守卫一：演示文档也有视图，但那是演示的视图 —— 模型查看器的命令不该改它（appfw-document-model.md：一个文档
    // 一个视图，谁的东西谁改）。
    const vn::appfw::Document* current = documents->current();
    if (current == nullptr || current->typeId() != vn::String(ModelDocument::kTypeId)) {
        co_return vn::appfw::CommandResult(vn::appfw::CommandStatus::NotApplicable,
                                           vn::String(u8"当前文档不是模型文档：先打开（或切到）一份模型"));
    }

    // 守卫二：模式是**视图**的设置，而视图只在真的被显示过之后才存在/才算数。
    auto* window  = vn::appfw::gui::MainWindow::current();
    auto* control = window != nullptr ? window->primaryRenderControl() : nullptr;
    // 改的是**焦点视图**：用户最后按下过的那一格（分屏就是多条呈现里被他点中的那一条）。没有任何输入时控制器
    // 的回答是自己的视图，所以单视图那条路一字未变；`primaryRenderControl()` 只回答"窗口的 3D 区在哪块控件上"，
    // 不再决定改哪个视图（见 ShadingModeCommands.hpp 的说明）。
    auto* view = control != nullptr ? control->focusedView() : nullptr;
    if (view == nullptr) {
        co_return vn::appfw::CommandResult(vn::appfw::CommandStatus::NotApplicable,
                                           vn::String(u8"还没有活动的 3D 视图：把这份模型显示出来再试"));
    }

    // 读-改-写：`setOptions()` 收的是整个配方，只送一个 mode 会顺手把帧率读数、坐标轴那些关掉。
    vn::graphics::SceneView::Options options = view->options();
    options.mode                             = mode();
    view->setOptions(options);

    co_return vn::appfw::CommandResult(vn::appfw::CommandStatus::Success, description());
}

} // namespace vn::model_viewer
