#include <vine/appfw/ConsoleProgressReporter.hpp>

#include <chrono>
#include <mutex>
#include <optional>
#include <string>
#include <utility>

#include <vine/Signal.hpp>
#include <vine/async/DetachedTask.hpp>
#include <vine/async/Sleep.hpp>

#include <vine/appfw/ProgressHost.hpp>

V_APPFW_NS_BEGIN

namespace
{

/// Duration type used for the wakeup delays.
using WakeupDelay = std::chrono::milliseconds;

/**
 * @brief Builds the UTF-8 String of a formatted progress line.
 *
 * The progress module labels are UTF-8 std::string, so this is the same reinterpretation the
 * plugin layer uses for native text (PluginManager.cpp) - a copy, not a transcode.
 *
 * @param percent Overall progress in percent, already rounded.
 * @param label Stage label, may be empty.
 * @return The line to hand to the sink.
 */
String lineFor(int percent, const std::string& label)
{
    std::string text = "[进度] ";
    text += std::to_string(percent);
    text += '%';
    if (!label.empty()) {
        text += ' ';
        text += label;
    }
    return String(std::u8string_view(reinterpret_cast<const char8_t*>(text.data()), text.size()));
}

} // namespace

struct ConsoleProgressReporter::Impl : public std::enable_shared_from_this<ConsoleProgressReporter::Impl>
{
    Impl(ConsoleProgressReporter::Sink sink, ConsoleProgressOptions options)
      : sink_(std::move(sink))
      , options_(options)
    {}

    /// Arms a wakeup at 'delay' from now, then picks up in afterWakeup().
    ///
    /// One-shot, never periodic: the reporter wakes exactly when a line is due, and an operation
    /// that reports nothing while it runs costs one wakeup per show delay and nothing else. Static
    /// because the frame outlives the caller and keeps the state alive by itself.
    ///
    /// @param impl State to wake up.
    /// @param generation Generation the state had when the wakeup was armed; a stale one is a
    ///                   no-op, which is how stop() silences a wakeup that is already in flight.
    /// @param delay How long to sleep before reporting back.
    static vine::async::DetachedTask wakeupAfter(std::shared_ptr<Impl> impl,
                                                std::uint64_t       generation,
                                                WakeupDelay         delay)
    {
        co_await vine::async::sleepFor(delay);

        impl->afterWakeup(generation);
    }

    /// Handles one change notification; runs on whichever thread changed the state.
    void onChanged()
    {
        std::optional<WakeupDelay> delay;
        {
            std::lock_guard lock(mutex_);
            if (stopped_) {
                return;
            }
            delay = refresh(std::chrono::steady_clock::now());
        }

        arm(delay);
    }

    /// Picks up after a wakeup; runs on the shared timer thread.
    ///
    /// The state is re-checked rather than trusted: the wakeup may have been armed before the
    /// operation ended, or belongs to a reporter that has been stopped since.
    ///
    /// @param generation Generation the wakeup was armed for.
    void afterWakeup(std::uint64_t generation)
    {
        std::optional<WakeupDelay> delay;
        {
            std::lock_guard lock(mutex_);
            if (stopped_ || generation != generation_) {
                return;
            }
            sleeping_ = false;
            delay     = refresh(std::chrono::steady_clock::now());
        }

        arm(delay);
    }

    /// Reconciles the printed line with the registry.
    ///
    /// @param now Current time.
    /// @return How long to wait before another line may be due, or nothing when the state has
    ///         nothing pending and can be left alone until something changes.
    std::optional<WakeupDelay> refresh(std::chrono::steady_clock::time_point now)
    {
        ProgressHost* const current = ProgressHost::current();

        if (current == nullptr) {
            if (shown_) {
                // The operation ended (or was cancelled): close its reporting with one line.
                shown_ = false;
                host_  = nullptr;
                write(String(u8"[进度] 已结束"));
            }
            return std::nullopt;
        }

        if (current != host_) {
            // A new operation - or a nested one taking the bar over from its parent: report it on
            // its own, with its own show delay.
            host_         = current;
            shown_        = false;
            started_      = now;
            last_percent_ = -1;
            last_label_.clear();
        }

        const int         percent = static_cast<int>(current->indicator().position() * 100.0 + 0.5);
        const std::string label   = current->label();

        if (!shown_) {
            const auto due = started_ + options_.show_delay;
            if (now < due) {
                // Too early: an operation that finishes quickly should print nothing.
                return std::chrono::duration_cast<WakeupDelay>(due - now);
            }
            shown_ = true;
        }
        else {
            const bool worth_a_line =
                label != last_label_ || percent >= 100 || percent >= last_percent_ + options_.step_percent;
            if (!worth_a_line) {
                return std::nullopt;
            }

            // The first line of a stage is gated by the show delay alone; later ones are also
            // gated by the shortest gap between lines, so a fast loop cannot flood the stream.
            if (last_written_ != std::chrono::steady_clock::time_point{}
                && now - last_written_ < options_.interval) {
                return std::chrono::duration_cast<WakeupDelay>(last_written_ + options_.interval - now);
            }
        }

        last_percent_ = percent;
        last_label_   = label;
        last_written_ = now;
        write(lineFor(percent, label));
        return std::nullopt;
    }

    /// Starts a wakeup for 'delay' unless one is already in flight.
    ///
    /// The task is created after the lock is released: a wakeup whose delay is already up may run
    /// on the timer thread at once, and it needs the same lock to look at the state.
    ///
    /// @param delay How long to wait, or nothing when no wakeup is needed.
    void arm(const std::optional<WakeupDelay>& delay)
    {
        if (!delay) {
            return;
        }

        std::uint64_t generation = 0;
        {
            std::lock_guard lock(mutex_);
            if (stopped_ || sleeping_) {
                return;
            }
            sleeping_   = true;
            generation = generation_;
        }

        wakeupAfter(shared_from_this(), generation, *delay);
    }

    /// Writes one line through the sink; the caller holds mutex_, so that a line and the stop
    /// barrier cannot cross.
    ///
    /// @param line Line to write.
    void write(const String& line)
    {
        if (sink_) {
            sink_(line);
        }
    }

    Sink                   sink_;
    ConsoleProgressOptions options_;

    std::mutex                        mutex_;        ///< Guards everything below; the stop barrier.
    vine::Connection                  subscription_; ///< Installed by start(), removed by stop().
    bool                              stopped_{ false };
    std::uint64_t                     generation_{ 0 }; ///< Bumped by stop(): stale wakeups bail.
    bool                              sleeping_{ false }; ///< A wakeup is already in flight.
    ProgressHost*                     host_{ nullptr };  ///< Operation being reported on, if any.
    std::chrono::steady_clock::time_point started_{};
    std::chrono::steady_clock::time_point last_written_{};
    bool                                  shown_{ false }; ///< A line was written for host_.
    int                                   last_percent_{ -1 };
    std::string                           last_label_;
};

ConsoleProgressReporter::ConsoleProgressReporter(Sink sink, ConsoleProgressOptions options)
  : d(new Impl(std::move(sink), options))
{}

ConsoleProgressReporter::~ConsoleProgressReporter()
{
    stop();
}

void ConsoleProgressReporter::start()
{
    std::optional<WakeupDelay> delay;
    {
        std::lock_guard lock(d->mutex_);
        if (d->subscription_.isActive() && !d->stopped_) {
            return;
        }

        d->stopped_ = false;

        if (!d->subscription_.isActive()) {
            d->subscription_ = ProgressHost::changed().connect([weak = std::weak_ptr<Impl>(d)] {
                // The state is kept alive by the reporter and by any wakeup in flight, so a
                // notification that arrives after both are gone finds nothing to do.
                if (auto impl = weak.lock()) {
                    impl->onChanged();
                }
            });
        }

        delay = d->refresh(std::chrono::steady_clock::now());
    }

    d->arm(delay);
}

void ConsoleProgressReporter::stop()
{
    std::lock_guard lock(d->mutex_);

    // Barrier: a write that is already running finishes here, and every notification or wakeup
    // that arrives later sees the new generation and writes nothing. The operation, not the
    // reporter, owns the closing line, so stopping mid-operation prints nothing here.
    d->stopped_ = true;
    ++d->generation_;
    d->subscription_.disconnect();
}

void ConsoleProgressReporter::poll()
{
    std::optional<WakeupDelay> delay;
    {
        std::lock_guard lock(d->mutex_);
        if (d->stopped_) {
            return;
        }
        delay = d->refresh(std::chrono::steady_clock::now());
    }

    d->arm(delay);
}

V_APPFW_NS_END
