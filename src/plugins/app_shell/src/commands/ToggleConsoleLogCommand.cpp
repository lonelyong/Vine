#include "ToggleConsoleLogCommand.hpp"

#include "ConsoleLogRouter.hpp"

#include <vine/appfw/Application.hpp>
#include <vine/appfw/ConfigManager.hpp>
#include <vine/appfw/Translation.hpp>
#include <vine/appfw/UserIO.hpp>

VN_APPFW_NS_BEGIN

VN_OBJECT_META_IMPL(ToggleConsoleLogCommand, Command)

vn::async::Task<CommandResult> ToggleConsoleLogCommand::execute(CommandExecutionContext* context)
{
    auto* app = context ? context->application() : nullptr;
    auto* io  = app ? app->userIO() : nullptr;
    auto* cfg = app ? app->configManager() : nullptr;

    if (!cfg) {
        if (io) {
            io->putString(vn::appfw::translate("app_shell", "The configuration manager is unavailable"));
        }
        co_return CommandResult(CommandStatus::Failed, vn::appfw::translate("app_shell", "The configuration manager is unavailable"));
    }

    const String key     = consoleLogConfigKey();
    const bool   enabled = !cfg->getBool(key, true);
    cfg->setBool(key, enabled); // 触发 changed，AppShellPlugin 同步 sink 的原子开关

    if (io) {
        io->putString(enabled ? vn::appfw::translate("app_shell", "Logging to the console: on")
                              : vn::appfw::translate("app_shell", "Logging to the console: off"));
    }

    co_return CommandResult(CommandStatus::Success);
}

VN_APPFW_NS_END
