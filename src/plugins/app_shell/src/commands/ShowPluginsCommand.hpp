#pragma once

#include <vine/appfw/Command.hpp>
#include <vine/appfw/command_export.hpp>

VN_APPFW_NS_BEGIN

/**
 * @brief Shows the currently loaded plugins and their library paths.
 */
class ShowPluginsCommand : public Command {
    VN_OBJECT_META_DECL;
    VN_DECLARE_COMMAND(ShowPluginsCommand, u8"show_plugins")

  public:
    String group() const override { return u8"插件"; }
    String description() const override { return u8"显示已加载插件"; }
    CommandFlags flags() const override { return CommandFlags::None; }
    vn::async::Task<CommandResult> execute(CommandExecutionContext* context) override;
};

VN_APPFW_NS_END
