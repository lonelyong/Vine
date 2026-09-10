#include <vine/logging/Logger.hpp>

#include <atomic>
#include <cstdio>
#include <memory>
#include <utility>

#include <spdlog/pattern_formatter.h>
#include <spdlog/spdlog.h>

#include "SpdlogInternal.hpp"

V_LOGGING_NS_BEGIN

struct Logger::Impl
{
    std::string                     name;
    std::string                     pattern;
    std::shared_ptr<spdlog::logger> logger;
};

Logger::Logger()
  : d(std::make_shared<Impl>())
{}

Logger::Logger(std::string name, LogLevel level)
  : Logger(std::move(name), level, { LogSink::console() }, {})
{}

Logger::Logger(std::string name, LogLevel level, std::vector<LogSink> sinks, std::string pattern)
  : d(std::make_shared<Impl>())
{
    d->name = std::move(name);

    std::vector<spdlog::sink_ptr> spd_sinks;
    for (const auto& sink : sinks) {
        if (sink.d && sink.d->sink) {
            spd_sinks.push_back(sink.d->sink);
        }
    }

    d->logger = std::make_shared<spdlog::logger>(d->name, spd_sinks.begin(), spd_sinks.end());
    d->logger->set_level(toSpdlogLevel(level));
    if (!pattern.empty()) {
        d->logger->set_pattern(pattern);
    }
    d->pattern = std::move(pattern);
}

const std::string& Logger::name() const
{
    return d->name;
}

LogLevel Logger::level() const
{
    return d->logger ? toLogLevel(d->logger->level()) : LogLevel::Off;
}

void Logger::setLevel(LogLevel level)
{
    if (d->logger) {
        d->logger->set_level(toSpdlogLevel(level));
    }
}

void Logger::setPattern(const std::string& pattern)
{
    if (d->logger) {
        d->logger->set_pattern(pattern);
    }
    d->pattern = pattern;
}

void Logger::addSink(LogSink sink)
{
    if (!d->logger || !sink.d || !sink.d->sink) {
        return;
    }

    auto spd_sink = sink.d->sink;

    // Give the new sink the same formatter as the logger's existing sinks.
    // An empty pattern means the spdlog default ("%+"), mirroring the logger
    // constructor which leaves the default formatter in place.
    spd_sink->set_formatter(std::make_unique<spdlog::pattern_formatter>(d->pattern.empty() ? std::string("%+") : d->pattern));

    d->logger->sinks().push_back(std::move(spd_sink));
}

void Logger::flush()
{
    if (d->logger) {
        d->logger->flush();
    }
}

bool Logger::isEnabled(LogLevel level) const
{
    return d->logger && d->logger->should_log(toSpdlogLevel(level));
}

void Logger::log(LogLevel level, std::string message, const std::source_location& loc) noexcept
{
    if (!d->logger || !d->logger->should_log(toSpdlogLevel(level))) {
        return;
    }

    // spdlog routes std::exception sink failures through its error handler, but it
    // rethrows anything that is not a std::exception. Swallow both, together with
    // whatever formatting throws: logging is a diagnostic and must never decide
    // what the caller does next.
    try {
        d->logger->log(spdlog::source_loc{loc.file_name(), static_cast<int>(loc.line()), loc.function_name()}, toSpdlogLevel(level), message);
    }
    catch (...) {
        reportLoggingFailure();
    }
}

void reportLoggingFailure() noexcept
{
    static std::atomic<bool> reported{ false };
    if (reported.exchange(true, std::memory_order_relaxed)) {
        return;
    }

    // fputs does not throw, and stderr is used instead of the logging machinery
    // because the logger is what failed here.
    try {
        std::fputs("[vine::logging] a logging operation failed; further logging failures are dropped\n", stderr);
    }
    catch (...) {
    }
}

V_LOGGING_NS_END
