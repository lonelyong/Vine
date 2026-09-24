#include "TestCreateBoxCommand.hpp"

#include <string>

#include <vine/appfw/Application.hpp>
#include <vine/appfw/UserIO.hpp>

VN_APPFW_NS_BEGIN

namespace
{

String numToString(double value)
{
    const std::string s = std::to_string(value);
    return String::fromLocal8Bit(s.c_str());
}

} // namespace

VN_OBJECT_META_IMPL(TestCreateBoxCommand, Command)

vn::async::Task<CommandResult> TestCreateBoxCommand::execute(CommandExecutionContext* context)
{
    auto* app = context ? context->application() : nullptr;
    auto* io  = app ? app->userIO() : nullptr;
    if (!io) {
        co_return CommandResult(CommandStatus::Failed, String(u8"用户 I/O 未就绪"));
    }

    const auto length = co_await io->getDoubleAsync(u8"长度: ");
    if (!length) {
        co_return CommandResult(CommandStatus::Cancelled);
    }

    const auto width = co_await io->getDoubleAsync(u8"宽度: ");
    if (!width) {
        co_return CommandResult(CommandStatus::Cancelled);
    }

    const auto height = co_await io->getDoubleAsync(u8"高度: ");
    if (!height) {
        co_return CommandResult(CommandStatus::Cancelled);
    }

    io->putString(String(u8"长: ") + numToString(*length));
    io->putString(String(u8"宽: ") + numToString(*width));
    io->putString(String(u8"高: ") + numToString(*height));

    co_return CommandResult(CommandStatus::Success);
}

VN_APPFW_NS_END
