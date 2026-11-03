#pragma once

#include <vine/appfw/Command.hpp>
#include <vine/appfw/command_export.hpp>

VN_APPFW_NS_BEGIN

/**
 * @brief Coroutine test command with two parameter variants: box by dimensions, or box by two opposite corners.
 *
 * @details
 * - The variant is an explicit discriminator token, never inferred from how many values there are:
 *   `test_createbox size <l> <w> <h>` or `test_createbox corners <x,y,z> <x,y,z>`, plus the one
 *   unambiguous shorthand `test_createbox <l> <w> <h>`.
 * - Everything the command line carried is used as typed and only the missing values are asked
 *   for, so `test_createbox corners 0,0,0 10,20,30` runs without a single prompt while a bare
 *   `test_createbox` first asks which variant and then for that variant's values.
 * - A token that does not fit the chosen variant is a caller error and fails the command; it is
 *   never replaced by a prompt, because asking again would hide the mistake.
 * - Both variants converge on the same box: nothing downstream can tell which one was used.
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
        return u8"测试协程命令：按长宽高或两个对角点建盒子，缺的再询问";
    }

    CommandFlags flags() const override
    {
        return CommandFlags::None;
    }

    vn::async::Task<CommandResult> execute(CommandExecutionContext* context) override;
};

VN_APPFW_NS_END
