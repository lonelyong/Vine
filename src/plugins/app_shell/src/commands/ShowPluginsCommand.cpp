#include "ShowPluginsCommand.hpp"

#include <vine/appfw/gui/PluginManagerDialog.hpp>

#include <vine/appfw/Application.hpp>
#include <vine/appfw/PluginManager.hpp>

VN_APPFW_NS_BEGIN

VN_OBJECT_META_IMPL(ShowPluginsCommand, Command)

vn::async::Task<CommandResult> ShowPluginsCommand::execute(CommandExecutionContext* context)
{
    auto* app = context ? context->application() : nullptr;
    auto* pm  = app ? app->pluginManager() : nullptr;
    if (!pm) {
        co_return CommandResult(CommandStatus::Failed, String(u8"应用未就绪"));
    }

    auto* dlg = new gui::PluginManagerDialog(pm);
    dlg->setWindowTitle(u8"插件管理器");
    dlg->resize(780, 500);
    dlg->exec();
    delete dlg;
    co_return CommandResult(CommandStatus::Success);
}

VN_APPFW_NS_END
