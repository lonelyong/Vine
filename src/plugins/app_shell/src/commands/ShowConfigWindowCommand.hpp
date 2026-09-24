#pragma once

#include <vine/appfw/Command.hpp>
#include <vine/appfw/command_export.hpp>

VN_APPFW_NS_BEGIN

/**
 * @brief Opens the configuration window (ConfigWindow, hosted by this plugin).
 */
class ShowConfigWindowCommand : public Command {
    VN_OBJECT_META_DECL;
    VN_DECLARE_COMMAND(ShowConfigWindowCommand, u8"show_config")

  public:
    String group() const override { return u8"插件"; }
    String description() const override { return u8"打开配置窗口"; }
    CommandFlags flags() const override { return CommandFlags::None; }
    vn::async::Task<CommandResult> execute(CommandExecutionContext* context) override;
};

VN_APPFW_NS_END
