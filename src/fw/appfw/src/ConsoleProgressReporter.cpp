#include <vine/appfw/ConsoleProgressReporter.hpp>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <mutex>
#include <optional>
#include <string>
#include <utility>

#include <vine/Signal.hpp>
#include <vine/async/DetachedTask.hpp>
#include <vine/async/Sleep.hpp>

#include <vine/appfw/ProgressHost.hpp>
#include <vine/appfw/Translation.hpp>

VN_APPFW_NS_BEGIN

namespace
{

/// Duration type used for the wakeup delays.
using WakeupDelay = std::chrono::milliseconds;

/**
 * @brief Builds the UTF-8 line a progress report is printed as.
 *
 * The label arrives as an SDK String (UTF-8), and so does the translated prefix; the percentage is the one piece
 * formatted with std::to_string. Concatenating Strings keeps the same allocation count as the std::string version.
 *
 * @param percent Overall progress in percent, already rounded.
 * @param label Stage label, may be empty.
 * @return The line to hand to the sink.
 */
String lineFor(int percent, const String& label)
{
    String text = translate("appfw", "[Progress]");
    text += String(u8" ");
    text += String::fromUtf8(std::to_string(percent));
    text += String(u8"%");
    if (!label.empty()) {
        text += String(u8" ");
        text += label;
    }
    return text;
}

} // namespace

struct ConsoleProgressReporter::Impl : public std::enable_shared_from_this<ConsoleProgressReporter::Impl> {
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
    static vn::async::DetachedTask wakeupAfter(std::shared_ptr<Impl> impl, std::uint64_t generation, WakeupDelay delay)
    {
        co_await vn::async::sleepFor(delay);

        impl->afterWakeup(generation);
    }

    /// Handles one change notification; runs on whichever thread changed the state.
    void onChanged()
    {
        Refresh update;
        {
            std::lock_guard lock(mutex_);
            if (stopped_) {
                return;
            }
            update = refresh(std::chrono::steady_clock::now());
        }

        // Both happen outside the state lock: the sink is user code and may be slow.
        write(update.line);
        arm(update.delay);
    }

    /// Picks up after a wakeup; runs on the shared timer thread.
    ///
    /// The state is re-checked rather than trusted: the wakeup may have been armed before the
    /// operation ended, or belongs to a reporter that has been stopped since.
    ///
    /// @param generation Generation the wakeup was armed for.
    void afterWakeup(std::uint64_t generation)
    {
        Refresh update;
        {
            std::lock_guard lock(mutex_);
            if (stopped_ || generation != generation_) {
                return;
            }
            sleeping_ = false;
            update    = refresh(std::chrono::steady_clock::now());
        }

        write(update.line);
        arm(update.delay);
    }

    /// What one reconciliation found: the line that is due, and when to look again.
    struct Refresh {
        /// Line to write, if one is due; empty when the printed state is current.
        std::optional<String> line;
        /// How long to wait before another line may be due, or nothing when the state has nothing
        /// pending and can be left alone until something changes.
        std::optional<WakeupDelay> delay;
    };

    /// Reconciles the printed line with the registry.
    ///
    /// Produces the line instead of writing it: the caller writes it after releasing the state lock, so a
    /// slow sink cannot hold up a progress report or a stop().
    ///
    /// @param now Current time.
    /// @return The line that is due, if any, and when to look again.
    Refresh refresh(std::chrono::steady_clock::time_point now)
    {
        const auto current = ProgressHost::currentSnapshot();

        if (!current) {
            if (shown_) {
                // The operation ended (or was cancelled): close its reporting with one line.
                shown_   = false;
                host_id_ = 0;
                return Refresh{ translate("appfw", "[Progress] finished"), std::nullopt };
            }
            return Refresh{};
        }

        if (current->id != host_id_) {
            // A new operation - or a nested one taking the bar over from its parent: report it on
            // its own, with its own show delay.
            host_id_      = current->id;
            shown_        = false;
            started_      = now;
            last_percent_ = -1;
            last_label_.clear();
        }

        const int percent = static_cast<int>(current->position * 100.0 + 0.5);

        if (!shown_) {
            const auto due = started_ + options_.show_delay;
            if (now < due) {
                // Too early: an operation that finishes quickly should print nothing.
                return Refresh{ std::nullopt, std::chrono::duration_cast<WakeupDelay>(due - now) };
            }
            shown_ = true;
        }
        else {
            const bool worth_a_line =
                current->label != last_label_ || percent >= 100 || percent >= last_percent_ + options_.step_percent;
            if (!worth_a_line) {
                return Refresh{};
            }

            // The first line of a stage is gated by the show delay alone; later ones are also
            // gated by the shortest gap between lines, so a fast loop cannot flood the stream.
            if (last_written_ != std::chrono::steady_clock::time_point{} && now - last_written_ < options_.interval) {
                return Refresh{ std::nullopt, std::chrono::duration_cast<WakeupDelay>(last_written_ + options_.interval - now) };
            }
        }

        const String& label = current->label;
        last_percent_       = percent;
        last_label_         = label;
        last_written_       = now;
        return Refresh{ lineFor(percent, label), std::nullopt };
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
            sleeping_  = true;
            generation = generation_;
        }

        wakeupAfter(shared_from_this(), generation, *delay);
    }

    /// Writes one line through the sink, outside every state lock.
    ///
    /// The write gate is what keeps "nothing is written once stop() returns" true now that the sink runs
    /// outside the state lock: stop() takes the same gate after it flipped the flag, so a write that is
    /// already running finishes before stop() returns, and one that starts later sees the flag.
    ///
    /// @param line Line to write; nothing when no line is due.
    void write(const std::optional<String>& line)
    {
        if (!line) {
            return;
        }

        std::lock_guard lock(write_mutex_);
        if (stopped_) {
            // stop() won the race: dropping the line is what stopping means.
            return;
        }
        if (sink_) {
            sink_(*line);
        }
    }

    Sink                   sink_;
    ConsoleProgressOptions options_;

    /// Guards the state below; released around every call to the sink.
    std::mutex     mutex_;
    /// Serializes sink calls and is what stop() waits on, so the barrier outlives the sink running
    /// outside the state lock.
    std::mutex     write_mutex_;
    vn::Connection subscription_; // Installed by start(), removed by stop().
    /// Read by write() without the state lock, hence atomic.
    std::atomic<bool> stopped_{ false };
    std::uint64_t     generation_{ 0 };   // Bumped by stop(): stale wakeups bail.
    bool              sleeping_{ false }; // A wakeup is already in flight.
    std::uint64_t     host_id_{ 0 };      // Operation being reported on, if any.
    std::chrono::steady_clock::time_point started_{};
    std::chrono::steady_clock::time_point last_written_{};
    bool                                  shown_{ false }; // A line was written for host_id_.
    int                                   last_percent_{ -1 };
    String                                last_label_;
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
    ConsoleProgressReporter::Impl::Refresh update;
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

        update = d->refresh(std::chrono::steady_clock::now());
    }

    d->write(update.line);
    d->arm(update.delay);
}

void ConsoleProgressReporter::stop()
{
    {
        std::lock_guard lock(d->mutex_);

        // Every notification or wakeup that arrives from here on sees the new generation and stays silent.
        // The operation, not the reporter, owns the closing line, so stopping mid-operation prints nothing.
        d->stopped_ = true;
        ++d->generation_;
        d->subscription_.disconnect();
    }

    // Barrier: a line that is being written finishes before this returns, and one that comes later is
    // dropped. Taken after the state lock is released, so the two are never held together.
    std::lock_guard write_lock(d->write_mutex_);
}

void ConsoleProgressReporter::poll()
{
    Impl::Refresh update;
    {
        std::lock_guard lock(d->mutex_);
        if (d->stopped_) {
            return;
        }
        update = d->refresh(std::chrono::steady_clock::now());
    }

    d->write(update.line);
    d->arm(update.delay);
}

VN_APPFW_NS_END
