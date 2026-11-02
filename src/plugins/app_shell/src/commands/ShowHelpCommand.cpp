#include "ShowHelpCommand.hpp"

#include <vine/appfw/Application.hpp>
#include <vine/appfw/Translation.hpp>
#include <vine/appfw/UserIO.hpp>

VN_APPFW_NS_BEGIN

VN_OBJECT_META_IMPL(ShowHelpCommand, Command)

vn::async::Task<CommandResult> ShowHelpCommand::execute(CommandExecutionContext* context)
{
    auto* app = context ? context->application() : nullptr;
    auto* io  = app ? app->userIO() : nullptr;

    if (io) {
        io->putString(vn::appfw::translate("app_shell", "Vine 帮助"));
        io->putString(vn::appfw::translate("app_shell", "  输入 list_commands 查看所有命令"));
        io->putString(vn::appfw::translate("app_shell", "  输入 clear 清空控制台"));
        io->putString(vn::appfw::translate("app_shell", "  在控制台输入命令名并按回车即可执行"));
    }
    co_return CommandResult(CommandStatus::Success);
}

VN_APPFW_NS_END
