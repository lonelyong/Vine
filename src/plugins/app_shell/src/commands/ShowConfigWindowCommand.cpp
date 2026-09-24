#include "ShowConfigWindowCommand.hpp"

#include <vine/appfw/gui/ConfigWindow.hpp>

#include <vine/appfw/Application.hpp>

VN_APPFW_NS_BEGIN

VN_OBJECT_META_IMPL(ShowConfigWindowCommand, Command)

vn::async::Task<CommandResult> ShowConfigWindowCommand::execute(CommandExecutionContext* context)
{
    auto* app = context ? context->application() : nullptr;
    if (!app) {
        co_return CommandResult(CommandStatus::Failed, String(u8"应用未就绪"));
    }
    auto* win = new gui::ConfigWindow(app->configRegistry(), app->configManager());
    win->setWindowTitle(u8"设置");
    win->resize(480, 360);
    win->exec();
    delete win;
    co_return CommandResult(CommandStatus::Success);
}

VN_APPFW_NS_END
