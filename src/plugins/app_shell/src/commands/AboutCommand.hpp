#pragma once

#include <vine/appfw/Command.hpp>
#include <vine/appfw/Translation.hpp>
#include <vine/appfw/command_export.hpp>

VN_APPFW_NS_BEGIN

/**
 * @brief Shows the about dialog with this application's content.
 */
class AboutCommand : public Command {
    VN_OBJECT_META_DECL;
    VN_DECLARE_COMMAND(AboutCommand, u8"about")

  public:
    String group() const override
    {
        return vn::appfw::translate("app_shell", "Help");
    }

    String description() const override
    {
        return vn::appfw::translate("app_shell", "Show the about information");
    }

    CommandFlags flags() const override
    {
        return CommandFlags::None;
    }

    vn::async::Task<CommandResult> execute(CommandExecutionContext* context) override;
};

VN_APPFW_NS_END
