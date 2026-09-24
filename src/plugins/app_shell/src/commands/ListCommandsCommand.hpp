#pragma once

#include <vine/appfw/Command.hpp>
#include <vine/appfw/command_export.hpp>

VN_APPFW_NS_BEGIN

/**
 * @brief Lists every registered command (command name: list_commands, alias gcm).
 *
 * Outputs the command names through the application UserIO (console in GUI,
 * stdout headless).
 */
class ListCommandsCommand : public Command {
    VN_OBJECT_META_DECL;
    VN_DECLARE_COMMAND(ListCommandsCommand, u8"list_commands")
    VN_DECLARE_COMMAND_ALIAS(u8"gcm", u8"list_commands")

  public:
    String group() const override { return u8"帮助"; }
    String description() const override { return u8"列出所有命令及其别名"; }
    CommandFlags flags() const override { return CommandFlags::None; }
    vn::async::Task<CommandResult> execute(CommandExecutionContext* context) override;
};

VN_APPFW_NS_END
