#include "ListCommandsCommand.hpp"

#include <vine/appfw/Application.hpp>
#include <vine/appfw/CommandManager.hpp>
#include <vine/appfw/UserIO.hpp>

V_APPFW_NS_BEGIN

V_OBJECT_META_IMPL(ListCommandsCommand, Command)

vine::async::Task<CommandResult> ListCommandsCommand::execute(CommandExecutionContext* context)
{
    auto* app = context ? context->application() : nullptr;
    auto* cm  = app ? app->commandManager() : nullptr;
    auto* io  = app ? app->userIO() : nullptr;
    if (!cm) {
        co_return CommandResult(CommandStatus::Failed, String(u8"命令管理器未就绪"));
    }

    if (io) {
        // 每行输出：完整命令名 [别名] 描述；commandInfos() 已按命令名字母序排列。
        for (const auto& info : cm->commandInfos()) {
            String line = info.name;

            if (!info.aliases.empty()) {
                line += String(u8" (");
                bool first = true;
                for (const auto& alias : info.aliases) {
                    if (!first) {
                        line += String(u8", ");
                    }
                    line += alias;
                    first = false;
                }
                line += String(u8")");
            }

            if (!info.description.empty()) {
                line += String(u8"  ") + info.description;
            }

            // Disabled commands stay registered and listed; the marker explains why
            // they cannot be started.
            if (!info.enabled) {
                line += String(u8"  [已禁用]");
            }

            io->putString(line);
        }
    }
    co_return CommandResult(CommandStatus::Success);
}

V_APPFW_NS_END
