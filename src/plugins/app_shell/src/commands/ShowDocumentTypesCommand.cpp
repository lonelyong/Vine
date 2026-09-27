#include "ShowDocumentTypesCommand.hpp"

#include <vine/appfw/gui/DocumentManagerDialog.hpp>

#include <vine/appfw/Application.hpp>
#include <vine/appfw/DocumentManager.hpp>

VN_APPFW_NS_BEGIN

VN_OBJECT_META_IMPL(ShowDocumentTypesCommand, Command)

vn::async::Task<CommandResult> ShowDocumentTypesCommand::execute(CommandExecutionContext* context)
{
    auto* app = context ? context->application() : nullptr;
    auto* dm  = app ? app->documentManager() : nullptr;
    if (!dm) {
        co_return CommandResult(CommandStatus::Failed, String(u8"应用未就绪"));
    }

    auto* dlg = new gui::DocumentManagerDialog(dm);
    dlg->setWindowTitle(u8"文档管理器");
    dlg->resize(820, 520);
    dlg->exec();
    delete dlg;
    co_return CommandResult(CommandStatus::Success);
}

VN_APPFW_NS_END
