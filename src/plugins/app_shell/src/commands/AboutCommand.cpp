#include "AboutCommand.hpp"

#include <vine/appfw/gui/AboutDialog.hpp>
#include <vine/appfw/gui/Backstage.hpp>
#include <vine/appfw/gui/MainWindow.hpp>

#include <vine/appfw/Application.hpp>

VN_APPFW_NS_BEGIN

VN_OBJECT_META_IMPL(AboutCommand, Command)

vn::async::Task<CommandResult> AboutCommand::execute(CommandExecutionContext* context)
{
    auto* app = context ? context->application() : nullptr;
    if (!app) {
        co_return CommandResult(CommandStatus::Failed, String(u8"应用未就绪"));
    }

    // “关于”是 Backstage 的一页：命令只是把它打开，内容由外壳在装配时填进 AboutPanel。
    if (auto* window = gui::MainWindow::current(); window != nullptr) {
        if (auto* backstage = window->backstage(); backstage != nullptr) {
            if (auto* page = backstage->findPage(gui::BackstagePageRole::About); page != nullptr) {
                backstage->setCurrentPage(page);
                backstage->open();
                co_return CommandResult(CommandStatus::Success);
            }
        }
    }

    // 没有 Backstage 的宿主（或没装那页）：退回对话框；它读的也是 Application::appInfo()，与页面同一份内容。
    auto* dlg = new gui::AboutDialog();
    dlg->resize(460, 360);
    dlg->exec();
    delete dlg;
    co_return CommandResult(CommandStatus::Success);
}

VN_APPFW_NS_END
