#include <gtest/gtest.h>

#include <ctime>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <sstream>
#include <string>

#include <vine/logging/Log.hpp>
#include <vine/logging/LogLevel.hpp>
#include <vine/logging/LogSink.hpp>
#include <vine/logging/Logger.hpp>

using vine::logging::LogConfig;
using vine::logging::LogLevel;
using vine::logging::LogSink;
using vine::logging::Logger;
using vine::logging::defaultLogger;
using vine::logging::flushDefault;
using vine::logging::initDefault;
using vine::logging::levelName;
using vine::logging::parseLevel;

namespace
{

TEST(LogLevelTest, LevelNameIsLowerCase)
{
    EXPECT_EQ(levelName(LogLevel::Trace), "trace");
    EXPECT_EQ(levelName(LogLevel::Debug), "debug");
    EXPECT_EQ(levelName(LogLevel::Info), "info");
    EXPECT_EQ(levelName(LogLevel::Warn), "warn");
    EXPECT_EQ(levelName(LogLevel::Error), "error");
    EXPECT_EQ(levelName(LogLevel::Critical), "critical");
    EXPECT_EQ(levelName(LogLevel::Off), "off");
}

TEST(LogLevelTest, ParseLevelIsCaseInsensitive)
{
    EXPECT_EQ(parseLevel("trace"), LogLevel::Trace);
    EXPECT_EQ(parseLevel("DEBUG"), LogLevel::Debug);
    EXPECT_EQ(parseLevel("Info"), LogLevel::Info);
    EXPECT_EQ(parseLevel("wArN"), LogLevel::Warn);
    EXPECT_EQ(parseLevel("ERROR"), LogLevel::Error);
    EXPECT_EQ(parseLevel("critical"), LogLevel::Critical);
    EXPECT_EQ(parseLevel("off"), LogLevel::Off);
}

TEST(LogLevelTest, ParseLevelUnknownMapsToInfo)
{
    EXPECT_EQ(parseLevel("verbose"), LogLevel::Info);
    EXPECT_EQ(parseLevel(""), LogLevel::Info);
}

struct Capture
{
    LogLevel     level{ LogLevel::Off };
    std::string  line;
};

TEST(LoggerTest, NameAndLevel)
{
    Logger logger("unit", LogLevel::Warn);
    EXPECT_EQ(logger.name(), "unit");
    EXPECT_EQ(logger.level(), LogLevel::Warn);
}

TEST(LoggerTest, SetLevelRoundTrips)
{
    Logger logger("unit");
    logger.setLevel(LogLevel::Error);
    EXPECT_EQ(logger.level(), LogLevel::Error);
}

TEST(LoggerTest, IsEnabledReflectsLevel)
{
    Logger logger("unit", LogLevel::Warn);
    EXPECT_TRUE(logger.isEnabled(LogLevel::Warn));
    EXPECT_TRUE(logger.isEnabled(LogLevel::Error));
    EXPECT_FALSE(logger.isEnabled(LogLevel::Info));
    EXPECT_FALSE(logger.isEnabled(LogLevel::Debug));
}

TEST(LoggerTest, DefaultLoggerProducesNoOutput)
{
    Logger logger;
    EXPECT_EQ(logger.name(), "");
    EXPECT_NO_THROW(logger.info("hello"));
}

TEST(LoggerTest, LogSendsFormattedMessageToSink)
{
    Capture capture;
    auto sink = LogSink::function(
        [&capture](LogLevel level, const std::string& line) {
            capture.level = level;
            capture.line  = line;
        });
    Logger logger("unit", LogLevel::Trace, { sink }, "%v");
    logger.error("boom {}", 42);
    EXPECT_EQ(capture.level, LogLevel::Error);
    EXPECT_EQ(capture.line, "boom 42");
}

TEST(LoggerTest, LevelFilteringSuppressesBelowThreshold)
{
    Capture capture;
    auto sink = LogSink::function(
        [&capture](LogLevel, const std::string& line) { capture.line = line; });
    Logger logger("unit", LogLevel::Error, { sink }, "%v");
    logger.warn("should not appear");
    EXPECT_TRUE(capture.line.empty());
    logger.error("appears");
    EXPECT_EQ(capture.line, "appears");
}

TEST(LoggerTest, VariadicLevelMethodsForwardMessage)
{
    Capture capture;
    auto sink = LogSink::function(
        [&capture](LogLevel level, const std::string& line) {
            capture.level = level;
            capture.line  = line;
        });
    Logger logger("unit", LogLevel::Trace, { sink }, "%v");

    logger.trace("t {}", 1);
    EXPECT_EQ(capture.level, LogLevel::Trace);
    EXPECT_EQ(capture.line, "t 1");

    logger.debug("d {}", 2);
    EXPECT_EQ(capture.level, LogLevel::Debug);

    logger.info("i {}", 3);
    EXPECT_EQ(capture.level, LogLevel::Info);

    logger.warn("w {}", 4);
    EXPECT_EQ(capture.level, LogLevel::Warn);

    logger.error("e {}", 5);
    EXPECT_EQ(capture.level, LogLevel::Error);

    logger.critical("c {}", 6);
    EXPECT_EQ(capture.level, LogLevel::Critical);
}

TEST(LoggerTest, LogWithExplicitLevelAndMessage)
{
    Capture capture;
    auto sink = LogSink::function(
        [&capture](LogLevel level, const std::string& line) {
            capture.level = level;
            capture.line  = line;
        });
    Logger logger("unit", LogLevel::Trace, { sink }, "%v");
    logger.log(LogLevel::Warn, "raw message");
    EXPECT_EQ(capture.level, LogLevel::Warn);
    EXPECT_EQ(capture.line, "raw message");
}

TEST(LoggerTest, PatternControlsFormatting)
{
    Capture capture;
    auto sink = LogSink::function(
        [&capture](LogLevel, const std::string& line) { capture.line = line; });
    Logger logger("unit", LogLevel::Info, { sink }, "[%n] %v");
    logger.info("hello");
    EXPECT_EQ(capture.line, "[unit] hello");
}

TEST(LoggerTest, SetPatternChangesOutput)
{
    Capture capture;
    auto sink = LogSink::function(
        [&capture](LogLevel, const std::string& line) { capture.line = line; });
    Logger logger("unit", LogLevel::Info, { sink }, "%v");
    logger.setPattern("%n:%v");
    logger.info("msg");
    EXPECT_EQ(capture.line, "unit:msg");
}

TEST(LoggerTest, FlushDoesNotThrow)
{
    Logger logger("unit", LogLevel::Info);
    EXPECT_NO_THROW(logger.flush());
}

TEST(LogSinkTest, StreamSinkWritesToStream)
{
    std::ostringstream out;
    auto sink = LogSink::stream(out);
    Logger logger("unit", LogLevel::Info, { sink }, "%v");
    logger.info("to stream");

    // spdlog appends the platform EOL (\r\n on Windows, \n elsewhere), so
    // compare the payload ignoring the trailing line terminator.
    std::string line = out.str();
    while (!line.empty() && (line.back() == '\r' || line.back() == '\n')) {
        line.pop_back();
    }
    EXPECT_EQ(line, "to stream");
}

TEST(LogSinkTest, FileSinkWritesToFile)
{
    const auto path = std::filesystem::temp_directory_path() / "vine_test_log.txt";
    std::filesystem::remove(path);

    // Scope the logger/sink so the file handle is released before removal;
    // Windows cannot delete a file that is still open.
    {
        auto sink = LogSink::file(path);
        Logger logger("unit", LogLevel::Info, { sink }, "%v");
        logger.info("file line");
        logger.flush();
    }

    std::ifstream in(path);
    ASSERT_TRUE(in.good());
    std::string line;
    std::getline(in, line);
    EXPECT_EQ(line, "file line");
    in.close();

    std::filesystem::remove(path);
}

TEST(LogSinkTest, DailyFileSinkWritesToDatedFile)
{
    const auto base = std::filesystem::temp_directory_path() / "vine_test_daily.log";

    // The sink inserts the local date before the extension:
    // base "vine_test_daily.log" -> "vine_test_daily_YYYY-MM-DD.log".
    const auto now = std::time(nullptr);
    const auto tm  = *std::localtime(&now);
    std::ostringstream name;
    name << "vine_test_daily_" << std::put_time(&tm, "%Y-%m-%d") << ".log";
    const auto path = std::filesystem::temp_directory_path() / name.str();
    std::filesystem::remove(path);

    // Scope the logger/sink so the file handle is released before removal;
    // Windows cannot delete a file that is still open.
    {
        auto sink = LogSink::dailyFile(base);
        Logger logger("unit", LogLevel::Info, { sink }, "%v");
        logger.info("daily line");
        logger.flush();
    }

    std::ifstream in(path);
    ASSERT_TRUE(in.good());
    std::string line;
    std::getline(in, line);
    EXPECT_EQ(line, "daily line");
    in.close();

    std::filesystem::remove(path);
}

TEST(LogTest, DefaultLoggerHasVineName)
{
    EXPECT_EQ(defaultLogger().name(), "vine");
}

TEST(LogTest, InitDefaultGuaranteesConsoleSink)
{
    initDefault();  // empty config -> a console sink is added
    EXPECT_NO_THROW(V_LOGI("default console log"));
    EXPECT_EQ(defaultLogger().name(), "vine");
}

TEST(LogTest, InitWithFunctionSinkCapturesMacro)
{
    std::string line;
    auto sink = LogSink::function(
        [&line](LogLevel, const std::string& l) { line = l; });
    initDefault(LogConfig{ .level = LogLevel::Info, .pattern = "%v", .sinks = { sink } });
    V_LOGI("hello {} from macro", 42);
    EXPECT_EQ(line, "hello 42 from macro");
}

TEST(LogTest, SetLevelControlsDefaultLogger)
{
    std::string line;
    auto sink = LogSink::function(
        [&line](LogLevel, const std::string& l) { line = l; });
    initDefault(LogConfig{ .level = LogLevel::Error, .pattern = "%v", .sinks = { sink } });
    V_LOGI("suppressed");
    EXPECT_TRUE(line.empty());
    V_LOGE("visible");
    EXPECT_EQ(line, "visible");
}

TEST(LogTest, InitWithFileSinkWritesToFile)
{
    const auto path = std::filesystem::temp_directory_path() / "vine_test_log_sink.txt";
    std::filesystem::remove(path);

    initDefault(LogConfig{ .pattern = "%v", .sinks = { LogSink::file(path) } });
    V_LOGI("config file line");
    flushDefault();
    initDefault();  // reset default logger, releasing the file sink handle

    std::ifstream in(path);
    ASSERT_TRUE(in.good());
    std::string line;
    std::getline(in, line);
    EXPECT_EQ(line, "config file line");
    in.close();

    std::filesystem::remove(path);
}

// 日志路径的异常安全契约：宏、级别函数、log() 与 defaultLogger() 都不抛。
// 这几个 static_assert 让契约在编译期守住：一旦有人把 noexcept 去掉，这里先编译失败。
static_assert(noexcept(defaultLogger()));
static_assert(noexcept(std::declval<Logger&>().log(LogLevel::Info, std::string{})));
static_assert(noexcept(std::declval<Logger&>().info("{}", 1)));
static_assert(noexcept(std::declval<Logger&>().error(std::source_location::current(), "{}", 1)));
static_assert(noexcept(V_LOGE("{}", 1)));

// sink 抛出非 std::exception 时 spdlog 会重新抛出（它只对 std::exception 走错误处理器）。
// 这种情况必须被日志层吞掉：否则在 catch 块/析构路径上调日志就会 terminate。
TEST(LoggerTest, SinkThrowingNonStandardExceptionDoesNotPropagate)
{
    Logger logger("throwing",
                  LogLevel::Info,
                  { LogSink::function([](LogLevel, const std::string&) { throw 42; }) },
                  "%v");

    EXPECT_NO_THROW(logger.info("a sink that throws an int"));
    EXPECT_NO_THROW(logger.log(LogLevel::Error, "a sink that throws an int"));
    EXPECT_NO_THROW(logger.warn("a sink that throws an int"));
}

// 格式化失败（参数与格式串不匹配）也不得抛。
TEST(LoggerTest, FormatMismatchDoesNotPropagate)
{
    std::string seen;
    Logger      logger("mismatch",
                       LogLevel::Info,
                       { LogSink::function([&seen](LogLevel, const std::string& line) { seen = line; }) },
                       "%v");

    EXPECT_NO_THROW(logger.info("{} {}", 1));   // 少一个参数 ⇒ std::format_error
    logger.info("ok {}", 7);
    EXPECT_EQ(seen, "ok 7");
}

} // namespace
