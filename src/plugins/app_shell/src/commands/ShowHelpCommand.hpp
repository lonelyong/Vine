#pragma once

#include <vine/appfw/Command.hpp>
#include <vine/appfw/command_export.hpp>

VN_APPFW_NS_BEGIN

/**
 * @brief Shows a short help text through the application UserIO.
 */
class ShowHelpCommand : public Command {
    VN_OBJECT_META_DECL;
    VN_DECLARE_COMMAND(ShowHelpCommand, u8"show_help")

  public:
    String group() const override { return u8"帮助"; }
    String description() const override { return u8"显示帮助信息"; }
    CommandFlags flags() const override { return CommandFlags::None; }
    vn::async::Task<CommandResult> execute(CommandExecutionContext* context) override;
};

VN_APPFW_NS_END
