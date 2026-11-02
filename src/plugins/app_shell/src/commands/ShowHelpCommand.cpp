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
        io->putString(vn::appfw::translate("app_shell", "Vine help"));
        io->putString(vn::appfw::translate("app_shell", "  Type list_commands to list every command"));
        io->putString(vn::appfw::translate("app_shell", "  Type clear to clear the console"));
        io->putString(vn::appfw::translate("app_shell", "  Type a command name in the console and press Enter to run it"));
    }
    co_return CommandResult(CommandStatus::Success);
}

VN_APPFW_NS_END
