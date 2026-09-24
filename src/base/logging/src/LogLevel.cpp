#include <vine/logging/LogLevel.hpp>

#include <cstddef>

VN_LOGGING_NS_BEGIN

std::string_view levelName(LogLevel level) noexcept
{
    switch (level) {
    case LogLevel::Trace:    return "trace";
    case LogLevel::Debug:    return "debug";
    case LogLevel::Info:     return "info";
    case LogLevel::Warn:     return "warn";
    case LogLevel::Error:    return "error";
    case LogLevel::Critical: return "critical";
    case LogLevel::Off:      return "off";
    }
    return "info";
}

namespace
{

bool equalsIgnoreCase(std::string_view a, std::string_view b) noexcept
{
    if (a.size() != b.size()) {
        return false;
    }

    for (std::size_t i = 0; i < a.size(); ++i) {
        char ca = a[i];
        char cb = b[i];
        if (ca >= 'A' && ca <= 'Z') {
            ca = static_cast<char>(ca + 32);
        }
        if (cb >= 'A' && cb <= 'Z') {
            cb = static_cast<char>(cb + 32);
        }
        if (ca != cb) {
            return false;
        }
    }
    return true;
}

} // namespace

LogLevel parseLevel(std::string_view name) noexcept
{
    for (int i = static_cast<int>(LogLevel::Trace); i <= static_cast<int>(LogLevel::Off); ++i) {
        const auto level = static_cast<LogLevel>(i);
        if (equalsIgnoreCase(name, levelName(level))) {
            return level;
        }
    }
    return LogLevel::Info;
}

VN_LOGGING_NS_END
