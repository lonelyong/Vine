#pragma once

#include <vine/appfw/Command.hpp>
#include <vine/appfw/command_export.hpp>

VN_APPFW_NS_BEGIN

/**
 * @brief Toggles routing of the default logger to the console panel.
 */
class ToggleConsoleLogCommand : public Command {
    VN_OBJECT_META_DECL;
    VN_DECLARE_COMMAND(ToggleConsoleLogCommand, u8"log_to_console")

  public:
    String group() const override { return u8"控制台"; }
    String description() const override { return u8"开启/关闭日志输出到控制台"; }
    CommandFlags flags() const override { return CommandFlags::None; }
    vn::async::Task<CommandResult> execute(CommandExecutionContext* context) override;
};

VN_APPFW_NS_END
