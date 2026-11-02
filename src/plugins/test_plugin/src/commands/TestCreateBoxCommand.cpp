#include "TestCreateBoxCommand.hpp"

#include <cmath>
#include <cstddef>
#include <span>

#include <vine/Format.hpp>

#include <vine/appfw/Application.hpp>
#include <vine/appfw/UserIO.hpp>

VN_APPFW_NS_BEGIN

namespace
{

/// Parses one command-line token as a box dimension.
///
/// @details
/// - The whole token has to be a finite number: a token the caller typed is either a value or an
///   error, never a reason to ask again - re-prompting would silently replace what was typed.
/// - Conversion is String::toDouble, the same rule UserIO uses for the answers it collects, so a
///   value typed on the command line and one typed at a prompt are read identically.
///
/// @param token Token to convert.
/// @param value Receives the dimension when the token is a number.
/// @return true when the token is a finite number.
bool parseDimension(const String& token, double& value)
{
    bool         ok     = false;
    const double parsed = token.trimmed().toDouble(&ok);
    if (!ok || !std::isfinite(parsed)) {
        return false;
    }
    value = parsed;
    return true;
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

    // Three positional values: length, width, height. What the command line carried is used as
    // typed and the command never interrupts the user for it; only the missing ones are asked
    // for, so `test_createbox 10 20 30` runs straight away and a bare `test_createbox` still
    // works from the console.
    constexpr std::size_t         kDimensionCount = 3;
    const std::span<const String> tokens          = context->arguments().tokens();
    if (tokens.size() > kDimensionCount) {
        co_return CommandResult(CommandStatus::Failed, String(u8"用法: test_createbox [长度 宽度 高度]"));
    }

    double dimensions[kDimensionCount]{};
    for (std::size_t i = 0; i < tokens.size(); ++i) {
        if (!parseDimension(tokens[i], dimensions[i])) {
            co_return CommandResult(CommandStatus::Failed, vn::format(u8"第 {} 个参数不是数值: {}", i + 1, tokens[i]));
        }
    }

    static constexpr const char8_t* kPrompts[kDimensionCount] = { u8"长度: ", u8"宽度: ", u8"高度: " };
    for (std::size_t i = tokens.size(); i < kDimensionCount; ++i) {
        const auto answer = co_await io->getDoubleAsync(kPrompts[i]);
        if (!answer) {
            co_return CommandResult(CommandStatus::Cancelled);
        }
        dimensions[i] = *answer;
    }

    for (const double dimension : dimensions) {
        if (dimension <= 0.0) {
            co_return CommandResult(CommandStatus::Failed, String(u8"长宽高必须为正"));
        }
    }

    io->putString(vn::format(u8"长: {} 宽: {} 高: {}", dimensions[0], dimensions[1], dimensions[2]));

    co_return CommandResult(CommandStatus::Success);
}

VN_APPFW_NS_END
