#include "TestCreateBoxCommand.hpp"

#include <cmath>
#include <cstddef>
#include <optional>
#include <span>

#include <vine/Format.hpp>
#include <vine/math/Point3.hpp>

#include <vine/appfw/Application.hpp>
#include <vine/appfw/UserIO.hpp>

VN_APPFW_NS_BEGIN

namespace
{

/// The two ways to state the same box: by its three dimensions, or by two opposite corners.
enum class BoxMode
{
    Size,
    Corners,
};

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

/// Parses a mode keyword.
///
/// The comparison ignores case because the same keyword is typed by a person at the prompt and
/// written by a script in a command line; refusing "Size" in one place and accepting it in the
/// other would be a second, unwritten rule.
///
/// @param token Token to read.
/// @return The mode, or nothing when the token is not a mode keyword.
std::optional<BoxMode> parseMode(const String& token)
{
    const String lowered = token.trimmed().toLower();
    if (lowered == u8"size") {
        return BoxMode::Size;
    }
    if (lowered == u8"corners") {
        return BoxMode::Corners;
    }
    return std::nullopt;
}

/// Parses one command-line token as a 3D point in the form UserIO's point prompt accepts: x,y,z.
///
/// The syntax is shared with the prompt on purpose: a point typed into a macro and a point picked
/// at a prompt are the same three comma-separated finite numbers.
///
/// @param token Token to read.
/// @param point Receives the point when the token is well formed.
/// @return true when the token holds exactly three finite numbers.
bool parsePoint(const String& token, math::Point3d& point)
{
    const std::vector<String> parts = token.split(u8',');
    if (parts.size() != 3) {
        return false;
    }

    double coordinates[3]{};
    for (std::size_t i = 0; i < 3; ++i) {
        if (!parseDimension(parts[i], coordinates[i])) {
            return false;
        }
    }

    point = math::Point3d(coordinates[0], coordinates[1], coordinates[2]);
    return true;
}

/// Parses the size mode: three numbers, taken from the command line and asked for when missing.
///
/// @param io         User I/O used for the missing values.
/// @param tokens     Tokens after the mode keyword.
/// @param index      Index of the first token this mode owns.
/// @param dimensions Receives the three dimensions.
/// @return The result to report when the input is wrong or cancelled, or nothing on success.
vn::async::Task<std::optional<CommandResult>> readSize(UserIO& io, std::span<const String> tokens, std::size_t index, double (&dimensions)[3])
{
    static constexpr const char8_t* kPrompts[3] = { u8"长度: ", u8"宽度: ", u8"高度: " };
    const String                    usage       = String(u8"用法: test_createbox [size] <长> <宽> <高>");

    std::size_t taken = 0;
    for (; index < tokens.size(); ++index, ++taken) {
        if (taken == 3) {
            co_return CommandResult(CommandStatus::Failed, usage);
        }
        if (!parseDimension(tokens[index], dimensions[taken])) {
            co_return CommandResult(CommandStatus::Failed, vn::format(u8"第 {} 个参数不是数值: {}", taken + 1, tokens[index]));
        }
    }
    for (; taken < 3; ++taken) {
        const auto answer = co_await io.getDoubleAsync(kPrompts[taken]);
        if (!answer) {
            co_return CommandResult(CommandStatus::Cancelled);
        }
        dimensions[taken] = *answer;
    }
    for (const double dimension : dimensions) {
        if (dimension <= 0.0) {
            co_return CommandResult(CommandStatus::Failed, String(u8"长宽高必须为正"));
        }
    }
    co_return std::nullopt;
}

/// Parses the corners mode: two points, taken from the command line and asked for when missing.
///
/// @param io     User I/O used for the missing points.
/// @param tokens Tokens after the mode keyword.
/// @param index  Index of the first token this mode owns.
/// @param points Receives the two opposite corners.
/// @return The result to report when the input is wrong or cancelled, or nothing on success.
vn::async::Task<std::optional<CommandResult>> readCorners(UserIO& io, std::span<const String> tokens, std::size_t index, math::Point3d (&points)[2])
{
    static constexpr const char8_t* kPrompts[2] = { u8"第一个对角点 (x,y,z): ", u8"第二个对角点 (x,y,z): " };
    const String                    usage       = String(u8"用法: test_createbox corners <x1,y1,z1> <x2,y2,z2>");

    std::size_t taken = 0;
    for (; index < tokens.size(); ++index, ++taken) {
        if (taken == 2) {
            co_return CommandResult(CommandStatus::Failed, usage);
        }
        if (!parsePoint(tokens[index], points[taken])) {
            co_return CommandResult(CommandStatus::Failed, vn::format(u8"第 {} 个对角点不是坐标: {}", taken + 1, tokens[index]));
        }
    }
    for (; taken < 2; ++taken) {
        const auto answer = co_await io.getPoint3dAsync(kPrompts[taken]);
        if (!answer) {
            co_return CommandResult(CommandStatus::Cancelled);
        }
        points[taken] = *answer;
    }

    // Two corners that share a coordinate on an axis describe a flat box: refuse instead of
    // producing a degenerate one, which is the same rule the size mode applies to its numbers.
    if (std::abs(points[1].x - points[0].x) <= 0.0 || std::abs(points[1].y - points[0].y) <= 0.0 || std::abs(points[1].z - points[0].z) <= 0.0) {
        co_return CommandResult(CommandStatus::Failed, String(u8"两个对角点不能在任何一轴上重合"));
    }
    co_return std::nullopt;
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

    // The mode is data, never inferred from how many values happen to be there: an explicit
    // keyword, or the one shorthand (three bare numbers mean "size"). Everything the command
    // line carried is used as typed; only the missing part is asked for.
    const std::span<const String> tokens = context->arguments().tokens();

    BoxMode     mode{ BoxMode::Size };
    std::size_t index = 0;

    if (tokens.empty()) {
        const auto answer = co_await io->getStringAsync(u8"指定方式（size 长宽高 / corners 对角点）: ");
        if (!answer) {
            co_return CommandResult(CommandStatus::Cancelled);
        }
        const auto parsed = parseMode(*answer);
        if (!parsed) {
            co_return CommandResult(CommandStatus::Failed, vn::format(u8"未知的指定方式: {}（可用: size, corners）", *answer));
        }
        mode = *parsed;
    }
    else if (const auto parsed = parseMode(tokens[0])) {
        mode  = *parsed;
        index = 1;
    }
    else {
        double first = 0.0;
        if (!parseDimension(tokens[0], first)) {
            co_return CommandResult(CommandStatus::Failed, vn::format(u8"未知的指定方式: {}（可用: size, corners）", tokens[0]));
        }
        mode = BoxMode::Size;
    }

    // A second mode keyword is the caller saying two different things at once; with the
    // discriminator first, this is the only way both can appear.
    for (std::size_t i = index; i < tokens.size(); ++i) {
        if (parseMode(tokens[i])) {
            co_return CommandResult(CommandStatus::Failed, String(u8"两种指定方式不能同时使用"));
        }
    }

    math::Point3d first;
    math::Point3d second;

    if (mode == BoxMode::Size) {
        double dimensions[3]{};
        if (auto refused = co_await readSize(*io, tokens, index, dimensions)) {
            co_return *refused;
        }
        first  = math::Point3d(0.0, 0.0, 0.0);
        second = math::Point3d(dimensions[0], dimensions[1], dimensions[2]);
    }
    else {
        math::Point3d points[2];
        if (auto refused = co_await readCorners(*io, tokens, index, points)) {
            co_return *refused;
        }
        first  = points[0];
        second = points[1];
        io->putString(vn::format(u8"对角点: ({}, {}, {}) - ({}, {}, {})", first.x, first.y, first.z, second.x, second.y, second.z));
    }

    io->putString(vn::format(u8"长: {} 宽: {} 高: {}", std::abs(second.x - first.x), std::abs(second.y - first.y), std::abs(second.z - first.z)));

    co_return CommandResult(CommandStatus::Success);
}

VN_APPFW_NS_END
