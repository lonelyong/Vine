#pragma once

#include <vine/appfw/Command.hpp>
#include <vine/appfw/Translation.hpp>
#include <vine/appfw/command_export.hpp>

VN_APPFW_NS_BEGIN

/**
 * @brief Shows the currently loaded plugins and their library paths.
 */
class ShowPluginsCommand : public Command {
    VN_OBJECT_META_DECL;
    VN_DECLARE_COMMAND(ShowPluginsCommand, u8"show_plugins")

  public:
    String group() const override
    {
        return vn::appfw::translate("app_shell", "Plugins");
    }

    String description() const override
    {
        return vn::appfw::translate("app_shell", "Show the loaded plugins");
    }

    CommandFlags flags() const override
    {
        return CommandFlags::None;
    }

    vn::async::Task<CommandResult> execute(CommandExecutionContext* context) override;
};

VN_APPFW_NS_END
