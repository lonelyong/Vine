#pragma once

#include <vine/appfw/Command.hpp>
#include <vine/appfw/command_export.hpp>

VN_APPFW_NS_BEGIN

/**
 * @brief Simulates a long-running child command, run nested via executeChild().
 *
 * It shares the parent chain's cancellation token and progress host and does
 * not report its own progress (the parent drives the shared bar).
 */
class TestNestedChildCommand : public Command {
    VN_OBJECT_META_DECL;
    VN_DECLARE_COMMAND(TestNestedChildCommand, u8"test_nested_child")

  public:
    String group() const override { return u8"测试"; }
    String description() const override { return u8"模拟耗时子命令（嵌套）"; }
    CommandFlags flags() const override { return CommandFlags::LongRunning; }
    vn::async::Task<CommandResult> execute(CommandExecutionContext* context) override;
};

VN_APPFW_NS_END
