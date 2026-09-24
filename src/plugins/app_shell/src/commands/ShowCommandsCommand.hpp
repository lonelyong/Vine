#pragma once

#include <vine/appfw/Command.hpp>
#include <vine/appfw/command_export.hpp>

VN_APPFW_NS_BEGIN

/**
 * @brief Opens the command manager dialog (view and manage registered commands).
 */
class ShowCommandsCommand : public Command {
    VN_OBJECT_META_DECL;
    VN_DECLARE_COMMAND(ShowCommandsCommand, u8"show_commands")

  public:
    String group() const override { return u8"帮助"; }
    String description() const override { return u8"查看与管理已注册命令"; }
    CommandFlags flags() const override { return CommandFlags::None; }
    vn::async::Task<CommandResult> execute(CommandExecutionContext* context) override;
};

VN_APPFW_NS_END
