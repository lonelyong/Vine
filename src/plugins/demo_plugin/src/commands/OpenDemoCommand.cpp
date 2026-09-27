#include "OpenDemoCommand.hpp"

#include <vine/String.hpp>
#include <vine/appfw/Application.hpp>
#include <vine/appfw/DocumentManager.hpp>

#include "DemoDocument.hpp"

namespace vn::demo
{

VN_OBJECT_META_IMPL(OpenDemoCommand, vn::appfw::Command)

vn::async::Task<vn::appfw::CommandResult> OpenDemoCommand::execute(vn::appfw::CommandExecutionContext* context)
{
    auto* app       = context != nullptr ? context->application() : nullptr;
    auto* documents = app != nullptr ? app->documentManager() : nullptr;
    if (documents == nullptr) {
        co_return vn::appfw::CommandResult(vn::appfw::CommandStatus::Failed, vn::String(u8"应用未就绪"));
    }

    // 只能存在一份：已经开着就切过去（"打开"对用户是幂等的，就像点一次已经开着的窗口）。
    for (vn::appfw::Document* document : documents->documents()) {
        if (document->typeId() == vn::String(DemoDocument::kTypeId)) {
            documents->setCurrent(document);
            co_return vn::appfw::CommandResult(vn::appfw::CommandStatus::Success, vn::String(u8"演示文档已经打开，已切换过去"));
        }
    }

    vn::appfw::Document* document = documents->create(vn::String(DemoDocument::kTypeId));
    if (document == nullptr) {
        co_return vn::appfw::CommandResult(vn::appfw::CommandStatus::Failed, vn::String(u8"创建演示文档失败（类型没登记，或没有 create 工厂）"));
    }

    // "打开之后就让它成为当前"是宿主的策略，不是框架的。
    documents->setCurrent(document);
    co_return vn::appfw::CommandResult(vn::appfw::CommandStatus::Success);
}

} // namespace vn::demo
