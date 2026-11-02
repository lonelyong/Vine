#include "ShowConfigWindowCommand.hpp"

#include <vine/appfw/Translation.hpp>
#include <vine/appfw/gui/ConfigWindow.hpp>

#include <vine/appfw/Application.hpp>

VN_APPFW_NS_BEGIN

VN_OBJECT_META_IMPL(ShowConfigWindowCommand, Command)

vn::async::Task<CommandResult> ShowConfigWindowCommand::execute(CommandExecutionContext* context)
{
    auto* app = context ? context->application() : nullptr;
    if (!app) {
        co_return CommandResult(CommandStatus::Failed, vn::appfw::translate("app_shell", "The application is not ready"));
    }
    auto* win = new gui::ConfigWindow(app->configRegistry(), app->configManager());
    win->setWindowTitle(vn::appfw::translate("app_shell", "Settings"));
    win->resize(480, 360);
    win->exec();
    delete win;
    co_return CommandResult(CommandStatus::Success);
}

VN_APPFW_NS_END
