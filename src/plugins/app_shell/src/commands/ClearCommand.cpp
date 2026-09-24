#include "ClearCommand.hpp"

#include <vine/appfw/Application.hpp>
#include <vine/appfw/UserIO.hpp>

VN_APPFW_NS_BEGIN

VN_OBJECT_META_IMPL(ClearCommand, Command)

vn::async::Task<CommandResult> ClearCommand::execute(CommandExecutionContext* context)
{
    auto* app = context ? context->application() : nullptr;
    auto* io  = app ? app->userIO() : nullptr;
    if (!io) {
        co_return CommandResult(CommandStatus::Failed, String(u8"用户 I/O 未就绪"));
    }

    io->clear();
    co_return CommandResult(CommandStatus::Success);
}

VN_APPFW_NS_END
