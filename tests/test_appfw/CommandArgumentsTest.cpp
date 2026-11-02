// CommandArgumentsTest.cpp
//
// The mechanism only: one tokenizer, one carrier, and the tokens reaching the command through every
// entry point. What an argument means is the command's business, so nothing here asserts typed
// parsing, defaults or validation - those belong to the command until the framework grows a
// parameter declaration.

#include <gtest/gtest.h>

#include <cstddef>
#include <initializer_list>
#include <memory>
#include <vector>

#include <QStandardPaths>

#include <vine/appfw/AppConfig.hpp>
#include <vine/appfw/Application.hpp>
#include <vine/appfw/Command.hpp>
#include <vine/appfw/CommandArguments.hpp>
#include <vine/appfw/CommandManager.hpp>
#include <vine/async/Task.hpp>

namespace
{

using vn::String;
using vn::appfw::CommandArguments;

/// Splits a line and returns the tokens, so each assertion reads as "this line means these tokens".
///
/// @param line Text to split.
/// @return The tokens.
std::vector<String> split(const char8_t* line)
{
    return vn::appfw::tokenizeCommandLine(String(line));
}

/// Asserts that the canonical text of the tokens splits back into exactly those tokens.
///
/// @param original Tokens to round-trip.
void expectRoundTrip(const std::vector<String>& original)
{
    const CommandArguments    args(original);
    const String              text   = args.toString();
    const std::vector<String> replay = vn::appfw::tokenizeCommandLine(text);

    ASSERT_EQ(replay.size(), original.size()) << "canonical text was: " << text.as_std_str();
    for (std::size_t i = 0; i < original.size(); ++i) {
        EXPECT_EQ(replay[i], original[i]) << "token " << i << " of: " << text.as_std_str();
    }
}

/// Records what the execution carried, so the entry points can be compared.
class ArgsProbeCommand : public vn::appfw::Command {
    VN_OBJECT_META_DECL;

  public:
    vn::String name() const override
    {
        return u8"args_probe";
    }

    vn::String group() const override
    {
        return u8"Test";
    }

    vn::appfw::CommandFlags flags() const override
    {
        return vn::appfw::CommandFlags::None;
    }

    vn::async::Task<vn::appfw::CommandResult> execute(vn::appfw::CommandExecutionContext* context) override
    {
        s_record = context->arguments().toString();
        s_count  = context->arguments().size();
        co_return vn::appfw::CommandResult(vn::appfw::CommandStatus::Success);
    }

    /// Canonical text the last execution carried.
    inline static String s_record;

    /// Number of tokens the last execution carried.
    inline static std::size_t s_count = 0;
};

} // namespace

VN_OBJECT_META_IMPL(ArgsProbeCommand, vn::appfw::Command)

TEST(CommandArgumentsTest, SplitsOnWhitespace)
{
    const std::vector<String> tokens = split(u8"  createbox\t10\r\n20  30 ");
    ASSERT_EQ(tokens.size(), 4u);
    EXPECT_EQ(tokens[0], String(u8"createbox"));
    EXPECT_EQ(tokens[1], String(u8"10"));
    EXPECT_EQ(tokens[2], String(u8"20"));
    EXPECT_EQ(tokens[3], String(u8"30"));
}

TEST(CommandArgumentsTest, EmptyLineHasNoTokens)
{
    EXPECT_TRUE(split(u8"").empty());
    EXPECT_TRUE(split(u8"   \t\r\n ").empty());
}

TEST(CommandArgumentsTest, QuotesGroupWhitespaceAndAnEmptyQuotedTokenIsAToken)
{
    const std::vector<String> tokens = split(u8"open \"a b.txt\" \"\" c");
    ASSERT_EQ(tokens.size(), 4u);
    EXPECT_EQ(tokens[0], String(u8"open"));
    EXPECT_EQ(tokens[1], String(u8"a b.txt"));
    EXPECT_EQ(tokens[2], String(u8""));
    EXPECT_EQ(tokens[3], String(u8"c"));
}

TEST(CommandArgumentsTest, BackslashesAreLiteralUnlessTheyEscapeAQuote)
{
    // A path is not an escape sequence: only a run directly before a quote means anything.
    const std::vector<String> tokens = split(u8"open C:\\dir\\file.txt \"C:\\Program Files\\x.stl\"");
    ASSERT_EQ(tokens.size(), 3u);
    EXPECT_EQ(tokens[1], String(u8"C:\\dir\\file.txt"));
    EXPECT_EQ(tokens[2], String(u8"C:\\Program Files\\x.stl"));
}

TEST(CommandArgumentsTest, EscapedQuoteStaysInsideTheToken)
{
    const std::vector<String> tokens = split(u8"say \"a\\\"b\"");
    ASSERT_EQ(tokens.size(), 2u);
    EXPECT_EQ(tokens[1], String(u8"a\"b"));
}

TEST(CommandArgumentsTest, UnterminatedQuoteTakesTheRestOfTheLine)
{
    const std::vector<String> tokens = split(u8"open \"a b");
    ASSERT_EQ(tokens.size(), 2u);
    EXPECT_EQ(tokens[1], String(u8"a b"));
}

TEST(CommandArgumentsTest, ToStringRoundTripsEveryToken)
{
    expectRoundTrip({});
    expectRoundTrip({ String(u8"plain") });
    expectRoundTrip({ String(u8"a b") });
    expectRoundTrip({ String(u8"") });
    expectRoundTrip({ String(u8"a\"b") });
    expectRoundTrip({ String(u8"trailing\\") });
    expectRoundTrip({ String(u8"one"), String(u8"two words"), String(u8""), String(u8"--x=1") });
    expectRoundTrip({ String(u8"C:\\Program Files\\x.stl"), String(u8"a\\\"b") });
}

TEST(CommandArgumentsTest, ToStringQuotesOnlyWhatNeedsIt)
{
    EXPECT_EQ(CommandArguments({ String(u8"a"), String(u8"b") }).toString(), String(u8"a b"));
    EXPECT_EQ(CommandArguments({ String(u8"a b") }).toString(), String(u8"\"a b\""));
    EXPECT_EQ(CommandArguments({ String(u8"") }).toString(), String(u8"\"\""));
    EXPECT_TRUE(CommandArguments{}.toString().empty());
}

class CommandArgumentsManagerTest : public ::testing::Test {
  protected:
    void SetUp() override
    {
        // Redirect the user data/config directories into the Qt test locations: the case must not
        // read or write the developer's real configuration.
        QStandardPaths::setTestModeEnabled(true);

        static char  arg0[] = "test_appfw";
        static char* argv[] = { arg0, nullptr };

        vn::appfw::AppConfig config;
        config.name           = u8"test_appfw_command_arguments";
        config.persist_config = false;
        config.load_plugins   = false;

        app     = std::make_unique<vn::appfw::Application>(config, 1, argv);
        manager = app->commandManager();
        ASSERT_NE(manager, nullptr);
        ASSERT_TRUE(manager->registerCommand<ArgsProbeCommand>(u8"args_probe"));

        ArgsProbeCommand::s_record = String{};
        ArgsProbeCommand::s_count  = 0;
    }

    void TearDown() override
    {
        app.reset();
    }

    std::unique_ptr<vn::appfw::Application> app;
    vn::appfw::CommandManager*              manager = nullptr;
};

TEST_F(CommandArgumentsManagerTest, ArgumentsReachTheCommandThroughTheNameEntry)
{
    const auto result = manager->executeCommandAndWait(u8"args_probe", CommandArguments(vn::appfw::tokenizeCommandLine(u8"10 20 30")));

    ASSERT_EQ(result.status(), vn::appfw::CommandStatus::Success);
    EXPECT_EQ(ArgsProbeCommand::s_count, 3u);
    EXPECT_EQ(ArgsProbeCommand::s_record, String(u8"10 20 30"));
}

TEST_F(CommandArgumentsManagerTest, TypedLineIsSplitOnceAndTheRestIsCarriedUninterpreted)
{
    const auto result = manager->executeCommandLineAsync(u8"args_probe 10 \"a b\" --x=1").result();

    ASSERT_EQ(result.status(), vn::appfw::CommandStatus::Success);
    EXPECT_EQ(ArgsProbeCommand::s_count, 3u);
    EXPECT_EQ(ArgsProbeCommand::s_record, String(u8"10 \"a b\" --x=1")) << "the command name is not part of the arguments";
}

TEST_F(CommandArgumentsManagerTest, InstanceEntryCarriesNoArguments)
{
    ArgsProbeCommand instance;
    const auto       result = manager->executeCommandAndWait(&instance);

    ASSERT_EQ(result.status(), vn::appfw::CommandStatus::Success);
    EXPECT_TRUE(ArgsProbeCommand::s_record.empty());
    EXPECT_EQ(ArgsProbeCommand::s_count, 0u);
}

TEST_F(CommandArgumentsManagerTest, EmptyLineIsRefusedBeforeAnythingRuns)
{
    const auto result = manager->executeCommandLineAsync(u8"   ").result();

    EXPECT_EQ(result.status(), vn::appfw::CommandStatus::Failed);
    EXPECT_FALSE(result.message().empty());
    EXPECT_EQ(manager->historyCount(), 0) << "a line with no command must not be recorded as an execution";
}

TEST_F(CommandArgumentsManagerTest, HistoryRecordsTheReplayableInvocation)
{
    ASSERT_EQ(manager->executeCommandLineAsync(u8"args_probe 10 \"a b\" --x=1").result().status(), vn::appfw::CommandStatus::Success);

    ASSERT_EQ(manager->historyCount(), 1);
    const auto entry = manager->historyAt(0);
    ASSERT_TRUE(entry.has_value());
    EXPECT_EQ(entry->name, String(u8"args_probe"));
    EXPECT_EQ(entry->args_record, String(u8"10 \"a b\" --x=1"));

    // The record is the replayable form: splitting it reproduces the tokens the command saw.
    const std::vector<String> replay = vn::appfw::tokenizeCommandLine(entry->args_record);
    ASSERT_EQ(replay.size(), 3u);
    EXPECT_EQ(replay[0], String(u8"10"));
    EXPECT_EQ(replay[1], String(u8"a b"));
    EXPECT_EQ(replay[2], String(u8"--x=1"));
}

TEST_F(CommandArgumentsManagerTest, EventsCarryTheSameArgumentsAsTheExecution)
{
    String executing_record;
    String executed_record;

    auto executing = manager->executing.connect(
        [&executing_record](vn::appfw::CommandManager&, vn::appfw::CommandExecutingEventArgs& args) { executing_record = args.arguments().toString(); });
    auto executed = manager->executed.connect(
        [&executed_record](vn::appfw::CommandManager&, vn::appfw::CommandExecutedEventArgs& args) { executed_record = args.arguments().toString(); });

    ASSERT_EQ(manager->executeCommandLineAsync(u8"args_probe x \"y z\"").result().status(), vn::appfw::CommandStatus::Success);

    executing.disconnect();
    executed.disconnect();

    EXPECT_EQ(executing_record, String(u8"x \"y z\""));
    EXPECT_EQ(executed_record, String(u8"x \"y z\""));
}
