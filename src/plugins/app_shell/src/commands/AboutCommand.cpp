#include "AboutCommand.hpp"

#include <vine/appfw/gui/AboutDialog.hpp>

#include <vine/appfw/Application.hpp>

VN_APPFW_NS_BEGIN

VN_OBJECT_META_IMPL(AboutCommand, Command)

vn::async::Task<CommandResult> AboutCommand::execute(CommandExecutionContext* context)
{
    auto* app = context ? context->application() : nullptr;
    if (!app) {
        co_return CommandResult(CommandStatus::Failed, String(u8"应用未就绪"));
    }

    auto* dlg = new gui::AboutDialog();
    dlg->setAppName(u8"Vine");
    dlg->setVersion(u8"1.0.0");
    dlg->setDescription(u8"基于 Qt 与 C++20 的模块化应用框架");
    dlg->setCopyright(u8"© 2026 Vine");
    dlg->resize(420, 260);
    dlg->exec();
    delete dlg;
    co_return CommandResult(CommandStatus::Success);
}

VN_APPFW_NS_END
