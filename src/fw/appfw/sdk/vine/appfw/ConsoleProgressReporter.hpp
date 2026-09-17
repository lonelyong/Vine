#pragma once

#include "appfw_global.hpp"

#include <chrono>
#include <functional>
#include <memory>

#include <vine/String.hpp>

V_APPFW_NS_BEGIN

/**
 * @brief Tuning of the throttle ConsoleProgressReporter writes with.
 *
 * A plain value type at namespace scope, like CommandManager's CommandInfo: callers construct it
 * and pass it, and the reporter keeps a copy.
 */
struct ConsoleProgressOptions
{
    /// Shortest gap between two consecutive lines.
    std::chrono::milliseconds interval{ 200 };
    /// How long an operation runs before its first line is printed.
    std::chrono::milliseconds show_delay{ 500 };
    /// Least progress change, in percent, that justifies another line.
    int step_percent{ 5 };
};

/**
 * @brief Prints the progress of a running operation as throttled console lines.
 *
 * The headless counterpart of gui::ProgressPresenter. An operation reports progress through the
 * ambient vine::appfw::ProgressHost (the CommandManager creates one for a LongRunning command),
 * and this reporter subscribes to ProgressHost::changed() and writes single-line updates through
 * the sink it was given. ConsoleUserIO hands it its own output path, which is what makes the
 * progress of a LongRunning command visible in a host without a GUI at all.
 *
 * Nothing is polled: a line is written when the state actually moved. Lines are still deliberately
 * coarse, because the operation's own output shares the same stream: nothing is written for the
 * first Options::show_delay, then one line per Options::step_percent of progress or whenever the
 * stage label changes, at most one line per Options::interval, and one closing line when the
 * operation ends. A terminal therefore gets a readable trickle of lines rather than a redrawn one,
 * and a non-interactive stdout (a log file, a CI transcript) stays meaningful.
 *
 * Only the show delay needs the clock rather than an event - an operation that has been running
 * for a while is worth a line even if it reports no progress - and that is the single wakeup the
 * reporter arms, on the shared async timer service: an idle reporter holds no timer at all. poll()
 * is public so a host that runs its own loop can drive the reporter from there, and so tests can
 * step it deterministically.
 */
class V_APPFW_API ConsoleProgressReporter
{
  public:
    /// Tuning of the printed lines; see ConsoleProgressOptions.
    using Options = ConsoleProgressOptions;

    /** @brief Receives one formatted line. */
    using Sink = std::function<void(const String&)>;

    /**
     * @brief Constructs a reporter that is not listening yet.
     *
     * @param sink Receives every line; must not be empty.
     * @param options Tuning of the printed lines.
     */
    explicit ConsoleProgressReporter(Sink sink, ConsoleProgressOptions options = {});

    /**
     * @brief Destroys the reporter, stopping it if start() had started it.
     *
     * No line is written once this returns: stop() waits for a write that is in flight, and a
     * notification or wakeup that comes later finds the reporter stopped.
     */
    ~ConsoleProgressReporter();

  public:
    /**
     * @brief Subscribes to the progress registry and reports the state as it is now; idempotent.
     */
    void start();

    /**
     * @brief Unsubscribes and stops every pending wakeup; idempotent.
     */
    void stop();

    /**
     * @brief Reconciles the printed line with the registry and writes one when it is due.
     *
     * Writes nothing while no foreground operation is running, apart from the closing line of an
     * operation that had been reported on. Independent of start(): a host that runs its own loop can
     * create the reporter and drive it from there, without a subscription, and tests step it this
     * way. Nothing is written once stop() was called.
     */
    void poll();

  private:
    struct Impl;

    /// State of the reporter.
    ///
    /// Shared on purpose, unlike the unique_ptr the other appfw PImpl classes use: a wakeup keeps
    /// a reference of its own, so the state stays alive while the reporter is being destroyed and
    /// that wakeup is still on its way back.
    std::shared_ptr<Impl> d;
};

V_APPFW_NS_END
