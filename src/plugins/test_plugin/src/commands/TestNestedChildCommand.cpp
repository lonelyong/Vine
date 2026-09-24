#include "TestNestedChildCommand.hpp"

#include <chrono>

#include <vine/async/Sleep.hpp>
#include <vine/appfw/ProgressHost.hpp>
#include <vine/progress/ProgressRange.hpp>
#include <vine/progress/ProgressScope.hpp>

VN_APPFW_NS_BEGIN

VN_OBJECT_META_IMPL(TestNestedChildCommand, Command)

vn::async::Task<CommandResult> TestNestedChildCommand::execute(CommandExecutionContext* context)
{
    // 子命令是独立宿主（前台栈顶），上报自己的进度；父命令进度冻结在栈中。
    // 每步 50ms，30 步约 1.5s，足够看清子命令顶替父命令驱动整条进度条。
    if (auto* host = vn::appfw::ProgressHost::current()) {
        host->setLabel("子:压缩");
        vn::progress::ProgressScope scope = host->scope("子:压缩", 30);
        for (int i = 0; i < 30; ++i) {
            if (context && context->isCancelled()) {
                co_return CommandResult(CommandStatus::Cancelled);
            }
            scope.next(1);
            co_await vn::async::sleepFor(std::chrono::milliseconds(50));
        }
    }
    else {
        // 无宿主（父命令非 LongRunning）：仅做耗时 + 取消检查。
        for (int i = 0; i < 30; ++i) {
            if (context && context->isCancelled()) {
                co_return CommandResult(CommandStatus::Cancelled);
            }
            co_await vn::async::sleepFor(std::chrono::milliseconds(50));
        }
    }
    co_return CommandResult(CommandStatus::Success);
}

VN_APPFW_NS_END
