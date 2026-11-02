#pragma once

#include <vine/appfw/Command.hpp>
#include <vine/appfw/command_export.hpp>

VN_APPFW_NS_BEGIN

/**
 * @brief Coroutine test command: takes length/width/height from the command line, asking UserIO only for the missing ones, and prints them.
 *
 * @details
 * - `test_createbox 10 20 30` runs without a single prompt: a command line that carries every
 *   parameter is executed as typed.
 * - `test_createbox` asks for all three, and a partial line lets the command ask for the rest, so
 *   the same command works from a macro and from a person typing.
 * - A token that is not a number is a caller error and fails the command; it is never replaced by
 *   a prompt, because asking again would hide the typo.
 */
class TestCreateBoxCommand : public Command {
    VN_OBJECT_META_DECL;
    VN_DECLARE_COMMAND(TestCreateBoxCommand, u8"test_createbox")

  public:
    String group() const override
    {
        return u8"测试";
    }

    String description() const override
    {
        return u8"测试协程命令：命令行给长宽高，缺的再询问，然后打印";
    }

    CommandFlags flags() const override
    {
        return CommandFlags::None;
    }

    vn::async::Task<CommandResult> execute(CommandExecutionContext* context) override;
};

VN_APPFW_NS_END
