#pragma once

#include "logging_global.hpp"

#include <cstddef>
#include <filesystem>
#include <functional>
#include <iosfwd>
#include <memory>
#include <string>

#include "LogLevel.hpp"

VN_LOGGING_NS_BEGIN

class Logger;

/**
 * @brief Opaque handle to a log destination (an spdlog sink).
 *
 * LogSink is a lightweight value type created through the static factory
 * functions below. It never exposes any spdlog type; the concrete sink is held
 * behind a private implementation. Sinks are shared on copy, so the same sink
 * may be attached to several loggers.
 */
class VN_LOGGING_API LogSink
{
  public:
    /**
     * @brief Returns a colored console sink writing to standard error.
     *
     * @return The console sink.
     */
    static LogSink console();

    /**
     * @brief Returns a sink writing to the given output stream.
     *
     * The stream must outlive the logger using this sink.
     *
     * @param stream Output stream.
     * @return The stream sink.
     */
    static LogSink stream(std::ostream& stream);

    /**
     * @brief Returns a sink appending to the given file.
     *
     * @param path Log file path.
     * @return The file sink.
     */
    static LogSink file(const std::filesystem::path& path);

    /**
     * @brief Returns a sink writing to a size-rotating set of files.
     *
     * @param path Base log file path.
     * @param max_size Maximum size in bytes of a single file.
     * @param max_files Maximum number of rotated files to keep.
     * @return The rotating file sink.
     */
    static LogSink rotatingFile(const std::filesystem::path& path, std::size_t max_size, std::size_t max_files);

    /**
     * @brief Returns a sink writing to a daily-rotating log file.
     *
     * The file name is derived from base_path by inserting the date before the
     * extension, e.g. "vine_2026-08-30.log" for base "vine.log". A new file is
     * started every day at midnight; once max_files dated files exist the old
     * ones are pruned (files from previous runs are left untouched).
     *
     * @param base_path Base log file path; its containing directory is created
     *                  on first use when it does not exist.
     * @param max_files Maximum number of dated files to keep; 0 keeps all.
     * @return The daily file sink.
     */
    static LogSink dailyFile(const std::filesystem::path& base_path, std::size_t max_files = 0);

    /**
     * @brief Returns a custom sink forwarding each formatted line to a callback.
     *
     * The callback receives the log level and the fully formatted single line
     * (without a trailing newline).
     *
     * @param fn Callback invoked for each record.
     * @return The custom sink.
     */
    static LogSink function(std::function<void(LogLevel level, const std::string& message)> fn);

  private:
    struct Impl;
    std::shared_ptr<Impl> d;

    friend class Logger;
};

VN_LOGGING_NS_END
