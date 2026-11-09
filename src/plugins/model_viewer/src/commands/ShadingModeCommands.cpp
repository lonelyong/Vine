#include "ShadingModeCommands.hpp"

#include <vine/appfw/Application.hpp>
#include <vine/appfw/Translation.hpp>
#include <vine/appfw/gui/CommandTarget.hpp>
#include <vine/appfw/gui/RenderControl.hpp>

#include "../ModelDocument.hpp"

namespace vn::model_viewer
{

VN_OBJECT_META_IMPL(SetShadingLitCommand, vn::appfw::Command)
VN_OBJECT_META_IMPL(SetShadingFlatCommand, vn::appfw::Command)
VN_OBJECT_META_IMPL(SetShadingUnlitCommand, vn::appfw::Command)
VN_OBJECT_META_IMPL(SetShadingNormalsCommand, vn::appfw::Command)
VN_OBJECT_META_IMPL(SetShadingWireframeCommand, vn::appfw::Command)

bool isCurrentShadingMode(vn::graphics::ShadingMode mode)
{
    auto* window  = vn::appfw::gui::MainWindow::current();
    auto* view    = vn::appfw::gui::targetView(vn::appfw::gui::resolveCommandTarget(window));
    auto* control = view != nullptr ? view->renderControl() : nullptr;
    // The REQUEST, not the built chain: it is the area's own record of what it was asked to show, it survives a
    // surface that has not attached yet, and Viewer::setShadingMode() only changes it when the request succeeded.
    return control != nullptr && control->viewerOptions().mode == mode;
}

vn::async::Task<vn::appfw::CommandResult> ShadingModeCommand::execute(vn::appfw::CommandExecutionContext* context)
{
    if (context == nullptr) {
        co_return vn::appfw::CommandResult(vn::appfw::CommandStatus::Failed, vn::appfw::translate("model_viewer", "There is no execution context"));
    }

    // 目标就是派发解析出来的那一块：文档 + 它的视图。框架已经用 `NeedsView` 在命令体之前拒绝了“没有 3D 区”的
    // 情形，所以这里不写那个空链判断 —— 剩下的只是“它是不是该改的东西”。
    const vn::appfw::CommandTarget& target = context->target();

    // 守卫：演示文档也有视图，但那是演示的视图 —— 模型查看器的命令不该改它（appfw-document-model.md：一个文档
    // 一个视图，谁的东西谁改）。
    if (target.document == nullptr || target.document->typeId() != vn::String(ModelDocument::kTypeId)) {
        co_return vn::appfw::CommandResult(vn::appfw::CommandStatus::NotApplicable,
                                           vn::appfw::translate("model_viewer", "The current document is not a model: open (or switch to) a model first"));
    }

    // 视图与它那块控件：目标是数据，所以拿不到时仍要说清楚，而不是解引用空指针（正常派发走不到这里）。
    auto* view    = vn::appfw::gui::targetView(target);
    auto* control = view != nullptr ? view->renderControl() : nullptr;
    if (control == nullptr) {
        co_return vn::appfw::CommandResult(vn::appfw::CommandStatus::NotApplicable,
                                           vn::appfw::translate("model_viewer", "The target view has no 3D area: show this model, then try again"));
    }

    // 模式是这条链**建好之后**还能改的那一项（见 PassChain::setShadingMode()）。交给控件：它知道链是不是它建的
    // —— 能直接改就是零重建，链表达不了就带着新档重建一次（延迟链没有可改的几何趟，重建时按实面那条前置链搭，
    // 见 PassChain::effectivePath）。“怎么重建”是这块区的主人的事，本命令不替它做决定。
    if (!control->setShadingMode(mode())) {
        co_return vn::appfw::CommandResult(
            vn::appfw::CommandStatus::Failed,
            vn::appfw::translate("model_viewer", "This mode did not take effect: the graphics layer refused the rebuild (see the diagnostics)"));
    }

    // 按需出帧：换档只改链上的状态，不会自己产生一帧 —— 不在这里要一帧，画面要等下一次输入或缩放才变。
    // 这是**请求**而不是立即画（RenderControl::requestFrame()）：帧由下一次显示同步的那一拍画出，同一拍里的多次
    // 请求合成一帧，窗口没上屏就不画 —— 命令不必卡在这帧的耗时上。控件文档里的分工也是如此（见
    // RenderControl::setShadingMode："takes effect on the next frame"）：谁改状态谁要帧。还没 attach 时这一请求
    // 画不出东西（没有会话），attach 那一帧本来就会画出新档。
    control->requestFrame();

    co_return vn::appfw::CommandResult(vn::appfw::CommandStatus::Success, description());
}

} // namespace vn::model_viewer
