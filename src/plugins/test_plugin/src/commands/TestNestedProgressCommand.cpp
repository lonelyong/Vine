#include "TestNestedProgressCommand.hpp"

#include <chrono>

#include <vine/async/Sleep.hpp>
#include <vine/appfw/ProgressHost.hpp>
#include <vine/progress/ProgressRange.hpp>
#include <vine/progress/ProgressScope.hpp>

V_APPFW_NS_BEGIN

V_OBJECT_META_IMPL(TestNestedProgressCommand, Command)

vine::async::Task<CommandResult> TestNestedProgressCommand::execute(CommandExecutionContext* context)
{
    auto* host = vine::appfw::ProgressHost::current();
    if (!host) {
        co_return CommandResult(CommandStatus::Failed, String(u8"无进度宿主"));
    }
    if (!context) {
        co_return CommandResult(CommandStatus::Failed, String(u8"无执行上下文"));
    }

    // 父命令驱动整体进度（0→40% → 子命令顶替 → 40→100%）。
    // 每步 50ms：前段约 2s 走到 40%，子命令约 1.5s 顶替驱动整条进度条，
    // 结束后恢复父命令，后段约 3s 从 40% 走完到 100%。
    host->setLabel("父:导出");
    vine::progress::ProgressScope root = host->scope("父:导出", 100);
    for (int i = 0; i < 40; ++i) {
        if (root.isCancelled()) {
            co_return CommandResult(CommandStatus::Cancelled);
        }
        root.next(1);
        co_await vine::async::sleepFor(std::chrono::milliseconds(50));
    }

    // 嵌套运行耗时子命令：按名字创建，子命令有独立前台宿主（顶替父命令），
    // 共享取消 token，绕过串联门。子命令退出后自动恢复父命令前台。
    const auto child_result = co_await context->executeChild(u8"test_nested_child");
    if (!child_result.succeeded()) {
        co_return child_result;
    }

    for (int i = 0; i < 60; ++i) {
        if (root.isCancelled()) {
            co_return CommandResult(CommandStatus::Cancelled);
        }
        root.next(1);
        co_await vine::async::sleepFor(std::chrono::milliseconds(50));
    }

    co_return CommandResult(CommandStatus::Success);
}

V_APPFW_NS_END
