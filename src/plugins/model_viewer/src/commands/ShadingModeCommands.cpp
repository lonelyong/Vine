#include "ShadingModeCommands.hpp"

#include <vine/appfw/Application.hpp>
#include <vine/appfw/DocumentManager.hpp>
#include <vine/appfw/gui/MainWindow.hpp>
#include <vine/appfw/gui/RenderControl.hpp>
#include <vine/graphics/RenderPipeline.hpp>
#include <vine/graphics/SceneView.hpp>

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
    // 改的是这个控件的视图：一块区只画一幅画（`primaryRenderControl()` 回答"窗口的 3D 区在哪块控件上"，
    // 见 ShadingModeCommands.hpp 的说明）。
    auto* view = control != nullptr ? control->view() : nullptr;
    if (view == nullptr) {
        co_return vn::appfw::CommandResult(vn::appfw::CommandStatus::NotApplicable,
                                           vn::String(u8"还没有活动的 3D 视图：把这份模型显示出来再试"));
    }

    // 模式是这条链**建好之后**还能改的那一项（见 Pipeline::setShadingMode()）。视图就是控件自己的视图时交给
    // 控件：它知道链是不是它建的 —— 能直接改就是零重建，链表达不了就带着新档重建一次（延迟链没有可改的几何趟，
    // 重建时按实面那条前置链搭，见 RenderPipelineBuilder::effectivePath）。
    if (view == control->view()) {
        if (!control->setShadingMode(mode())) {
            co_return vn::appfw::CommandResult(vn::appfw::CommandStatus::Failed,
                                               vn::String(u8"这一档没能生效：图形层拒绝了这次重建（见诊断）"));
        }
        co_return vn::appfw::CommandResult(vn::appfw::CommandStatus::Success, description());
    }

    // 视图是**宿主自己搭的**（别的插件把自己的 Pipeline 采纳到了它上面）：直接问那条链，本命令不替别人决定
    // 怎么重建 —— 它说表达不了，这里就是 NotApplicable 并说清原因（不静默）。
    auto* pipeline = view->pipeline();
    if (pipeline == nullptr) {
        co_return vn::appfw::CommandResult(vn::appfw::CommandStatus::NotApplicable,
                                           vn::String(u8"这个视图还没有链：它还没有画过，或绘制被宿主拿走了"));
    }
    if (!pipeline->setShadingMode(mode())) {
        co_return vn::appfw::CommandResult(vn::appfw::CommandStatus::NotApplicable,
                                           vn::String(u8"这条链表达不了这一档：延迟链上只有实面可改"));
    }

    co_return vn::appfw::CommandResult(vn::appfw::CommandStatus::Success, description());
}

} // namespace vn::model_viewer
