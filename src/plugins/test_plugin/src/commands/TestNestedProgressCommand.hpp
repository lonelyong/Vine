#pragma once

#include <vine/appfw/Command.hpp>
#include <vine/appfw/command_export.hpp>

VN_APPFW_NS_BEGIN

/**
 * @brief Simulates a long-running parent command that runs a child via
 *        context->executeChild().
 *
 * Each long-running command owns its own foreground progress host. The parent
 * advances to 40%, the child is pushed on top (taking over the bar), then on
 * child exit the parent resumes to 100%. The presenter shows a breadcrumb
 * while the chain is deeper than one.
 */
class TestNestedProgressCommand : public Command {
    VN_OBJECT_META_DECL;
    VN_DECLARE_COMMAND(TestNestedProgressCommand, u8"test_nested_progress")

  public:
    String group() const override { return u8"测试"; }
    String description() const override { return u8"模拟耗时父命令并嵌套调用耗时子命令"; }
    CommandFlags flags() const override { return CommandFlags::LongRunning; }
    vn::async::Task<CommandResult> execute(CommandExecutionContext* context) override;
};

VN_APPFW_NS_END
