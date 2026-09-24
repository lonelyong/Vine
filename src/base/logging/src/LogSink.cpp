#include <vine/logging/LogSink.hpp>

#include <cstdint>
#include <memory>
#include <mutex>
#include <utility>

#include <spdlog/sinks/base_sink.h>
#include <spdlog/sinks/basic_file_sink.h>
#include <spdlog/sinks/daily_file_sink.h>
#include <spdlog/sinks/ostream_sink.h>
#include <spdlog/sinks/rotating_file_sink.h>
#include <spdlog/sinks/stdout_color_sinks.h>

#include "SpdlogInternal.hpp"

VN_LOGGING_NS_BEGIN

namespace
{

/**
 * @brief Adapts a user callback to the spdlog sink interface.
 */
class FunctionSink final : public spdlog::sinks::base_sink<std::mutex>
{
  public:
    explicit FunctionSink(std::function<void(LogLevel, const std::string&)> fn)
      : fn_(std::move(fn))
    {}

  protected:
    void sink_it_(const spdlog::details::log_msg& msg) override
    {
        if (!fn_) {
            return;
        }

        spdlog::memory_buf_t formatted;
        this->formatter_->format(msg, formatted);

        std::string line(formatted.data(), formatted.size());
        while (!line.empty() && (line.back() == '\n' || line.back() == '\r')) {
            line.pop_back();
        }
        fn_(toLogLevel(msg.level), line);
    }

    void flush_() override {}

  private:
    std::function<void(LogLevel, const std::string&)> fn_;
};

} // namespace

LogSink LogSink::console()
{
    LogSink result;
    result.d       = std::make_shared<Impl>();
    result.d->sink = std::make_shared<spdlog::sinks::stderr_color_sink_mt>();
    return result;
}

LogSink LogSink::stream(std::ostream& stream)
{
    LogSink result;
    result.d       = std::make_shared<Impl>();
    result.d->sink = std::make_shared<spdlog::sinks::ostream_sink_mt>(stream);
    return result;
}

LogSink LogSink::file(const std::filesystem::path& path)
{
    LogSink result;
    result.d       = std::make_shared<Impl>();
    result.d->sink = std::make_shared<spdlog::sinks::basic_file_sink_mt>(path.string());
    return result;
}

LogSink LogSink::rotatingFile(const std::filesystem::path& path, std::size_t max_size, std::size_t max_files)
{
    LogSink result;
    result.d       = std::make_shared<Impl>();
    result.d->sink = std::make_shared<spdlog::sinks::rotating_file_sink_mt>(path.string(), max_size, max_files);
    return result;
}

LogSink LogSink::dailyFile(const std::filesystem::path& base_path, std::size_t max_files)
{
    LogSink result;
    result.d       = std::make_shared<Impl>();
    result.d->sink = std::make_shared<spdlog::sinks::daily_file_sink_mt>(
        base_path.string(), 0, 0, false, static_cast<std::uint16_t>(max_files));
    return result;
}

LogSink LogSink::function(std::function<void(LogLevel, const std::string&)> fn)
{
    LogSink result;
    result.d       = std::make_shared<Impl>();
    result.d->sink = std::make_shared<FunctionSink>(std::move(fn));
    return result;
}

VN_LOGGING_NS_END
