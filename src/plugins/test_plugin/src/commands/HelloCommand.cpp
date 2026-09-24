#include "HelloCommand.hpp"

#include <QMessageBox>

VN_APPFW_NS_BEGIN

VN_OBJECT_META_IMPL(HelloCommand, Command)

vn::async::Task<CommandResult> HelloCommand::execute(CommandExecutionContext* context)
{
    (void)context;
    QMessageBox::information(nullptr, QStringLiteral("测试插件"), QStringLiteral("Hello from test_plugin!"));
    co_return CommandResult(CommandStatus::Success);
}

VN_APPFW_NS_END
