#include "ConsoleUserIO.hpp"

#include <cmath>
#include <iostream>
#include <string>
#include <thread>

#include <vine/logging/Log.hpp>

V_APPFW_NS_BEGIN

V_OBJECT_META_IMPL(ConsoleUserIO, UserIO)

namespace
{

String toVineString(const std::string& s)
{
    return String(reinterpret_cast<const char8_t*>(s.data()), s.size());
}

} // namespace

/**
 * @brief The background reader this IO owns, plus everything it shares with it.
 *
 * std::getline blocks until a line arrives and cannot be interrupted, so it must not
 * run on the thread that awaits a read: a command parked on user input has to stay
 * cancellable, or Application::shutdown() cannot unblock it (see
 * UserIO::cancelPendingInput()). The thread lives as long as the process and only
 * ever touches this state, never the ConsoleUserIO that started it.
 */
struct ConsoleUserIO::StdinReader {
    std::mutex              mutex;
    std::deque<String>      lines;                 ///< Lines typed while nothing waited.
    bool                    started     = false;   ///< Guards against a second thread.
    bool                    stopping    = false;   ///< Set by the destructor.
    bool                    eof         = false;
    bool                    failed      = false;
    bool                    cancelled   = false;   ///< The waiting read was cancelled.
    vine::async::AsyncEvent line_ready;            ///< A line arrived, or the stream ended.

    /// Moves the outcome of one blocking read into this state and wakes the reader.
    void push(std::string line, bool at_eof, bool broken)
    {
        {
            std::lock_guard lock(mutex);
            if (at_eof || broken) {
                eof    = at_eof;
                failed = broken;
            }
            else {
                lines.push_back(toVineString(line));
            }
        }
        line_ready.set();
    }
};

ConsoleUserIO::ConsoleUserIO()
  : reader_(std::make_shared<StdinReader>())
{}

ConsoleUserIO::~ConsoleUserIO()
{
    // Nothing to join: the thread may sit in a blocking read until the process ends.
    // It stops after the next line - at once when it is between lines - and it never
    // touches this object, only the state it shares with us.
    std::lock_guard lock(reader_->mutex);
    reader_->stopping = true;
}

void ConsoleUserIO::putString(const String& str)
{
    // stdout is shared: a command writes from whatever thread it resumed on.
    std::lock_guard lock(output_mutex_);
    std::cout << str.stdstr() << std::endl;
}

void ConsoleUserIO::clear()
{
    std::lock_guard lock(output_mutex_);
    // ANSI: clear the screen and move the cursor home.
    std::cout << "\033[2J\033[1;1H" << std::flush;
}

void ConsoleUserIO::cancelPendingInput()
{
    bool waiting = false;
    {
        std::lock_guard state_lock(state_mutex_);
        waiting = slot_busy_;
        if (waiting) {
            std::lock_guard reader_lock(reader_->mutex);
            reader_->cancelled = true;
        }
    }

    if (waiting) {
        // Outside both locks: set() resumes the awaiting read on this thread, and
        // that read takes state_mutex_ again to release the slot. A line that is
        // already typed stays buffered for the next read.
        reader_->line_ready.set();
    }
}

bool ConsoleUserIO::beginRead()
{
    std::lock_guard state_lock(state_mutex_);
    if (slot_busy_) {
        V_LOGW("A user-input read is already waiting; refusing the new one");
        return false;
    }
    slot_busy_ = true;

    {
        std::lock_guard reader_lock(reader_->mutex);
        reader_->cancelled = false;
        if (!reader_->started) {
            reader_->started = true;
            auto reader      = reader_;
            std::thread([reader] {
                while (true) {
                    std::string line;
                    if (!std::getline(std::cin, line)) {
                        reader->push(std::string(), std::cin.eof(), !std::cin.eof());
                        return;
                    }

                    reader->push(std::move(line), false, false);

                    std::lock_guard stop_lock(reader->mutex);
                    if (reader->stopping) {
                        return;
                    }
                }
            }).detach();
        }
    }
    return true;
}

void ConsoleUserIO::endRead() noexcept
{
    std::lock_guard state_lock(state_mutex_);
    slot_busy_ = false;
}

vine::async::Task<std::optional<String>> ConsoleUserIO::readLineAsync(const String& prompt)
{
    if (!beginRead()) {
        co_return std::nullopt;
    }
    const ReadScope scope{ this }; // frees the interaction slot however this ends

    if (!prompt.empty()) {
        putString(prompt);
    }

    auto reader = reader_;
    while (true) {
        // Re-arm before looking at the state, so a line arriving in between is not
        // lost: set() sticks until the next reset().
        reader->line_ready.reset();

        {
            std::lock_guard lock(reader->mutex);
            if (reader->cancelled) {
                co_return std::nullopt;
            }
            if (!reader->lines.empty()) {
                String line = std::move(reader->lines.front());
                reader->lines.pop_front();
                co_return line;
            }
            if (reader->eof || reader->failed) {
                co_return std::nullopt;
            }
        }

        co_await reader->line_ready;
    }
}

vine::async::Task<std::optional<String>> ConsoleUserIO::getStringAsync(const String& prompt)
{
    co_return co_await readLineAsync(prompt);
}

vine::async::Task<std::optional<int>> ConsoleUserIO::getIntAsync(const String& prompt)
{
    const auto line = co_await readLineAsync(prompt);
    if (!line.has_value()) {
        co_return std::nullopt;
    }

    int value = 0;
    co_return parseInt(*line, value) ? std::optional<int>(value) : std::nullopt;
}

vine::async::Task<std::optional<double>> ConsoleUserIO::getDoubleAsync(const String& prompt)
{
    const auto line = co_await readLineAsync(prompt);
    if (!line.has_value()) {
        co_return std::nullopt;
    }

    bool         ok    = false;
    const double value = line->trimmed().toDouble(&ok);
    co_return ok && std::isfinite(value) ? std::optional<double>(value) : std::nullopt;
}

vine::async::Task<std::optional<math::Point3d>> ConsoleUserIO::getPoint3dAsync(const String& prompt)
{
    const auto line = co_await readLineAsync(prompt);
    if (!line.has_value()) {
        co_return std::nullopt;
    }

    const auto parts = line->split(u8',');
    if (parts.size() != 3) {
        co_return std::nullopt;
    }

    bool   x_ok = false;
    bool   y_ok = false;
    bool   z_ok = false;
    double x    = parts[0].trimmed().toDouble(&x_ok);
    double y    = parts[1].trimmed().toDouble(&y_ok);
    double z    = parts[2].trimmed().toDouble(&z_ok);
    if (!x_ok || !y_ok || !z_ok) {
        co_return std::nullopt;
    }

    math::Point3d point;
    point.x = x;
    point.y = y;
    point.z = z;
    co_return point;
}

V_APPFW_NS_END
