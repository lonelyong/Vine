#include <vine/logging/Log.hpp>

#include <utility>

V_LOGGING_NS_BEGIN

Logger& defaultLogger() noexcept
{
    // Built on first use, so another translation unit's static initializer can
    // safely log. A logger that cannot even be built (no memory, an unusable
    // console) falls back to the silent logger: this path is noexcept, and the
    // failure is reported once instead of propagating.
    static Logger s_logger = []() noexcept {
        try {
            return Logger("vine");
        }
        catch (...) {
            reportLoggingFailure();
            return Logger{};
        }
    }();
    return s_logger;
}

void initDefault(LogConfig config)
{
    if (config.sinks.empty()) {
        config.sinks.push_back(LogSink::console());
    }
    defaultLogger() = Logger("vine", config.level, std::move(config.sinks), std::move(config.pattern));
}

void flushDefault()
{
    defaultLogger().flush();
}

V_LOGGING_NS_END
