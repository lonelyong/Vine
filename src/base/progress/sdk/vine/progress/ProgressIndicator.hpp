#pragma once

#include "progress_global.hpp"

#include <atomic>
#include <functional>
#include <stop_token>

V_PROGRESS_NS_BEGIN

class ProgressRange;
class ProgressScope;

/**
 * @brief Concrete progress indicator.
 *
 * Tracks the global progress position in [0, 1] and observes a cancellation
 * state supplied from outside as a std::stop_token. It performs no
 * presentation of its own; callers poll position() and observe cancellation
 * through isCancelled() or token(). Cancellation is requested through the
 * external std::stop_source that produced the token. position() and progress
 * increments are thread-safe; the scope/range graph is single-threaded.
 *
 * Position changes are published through the callback installed with
 * setPositionCallback(), coalesced to whole percent steps: this is the path a
 * reporting loop hits once per item, so the notification is what has to be cheap,
 * not the update. The indicator itself has no opinion about who listens - the
 * ambient host is the one caller that cares, and it turns the callback into its
 * change signal.
 */
class V_PROGRESS_API ProgressIndicator
{
  public:
    friend class ProgressRange;
    friend class ProgressScope;

  public:
    /**
     * @brief Constructs an indicator observing an externally-owned token.
     *
     * The indicator never requests cancellation itself; request_stop() is
     * called on the external std::stop_source that produced the token. An
     * empty token (the default) is never cancelled.
     *
     * @param token Token observing the cancellation state.
     */
    explicit ProgressIndicator(std::stop_token token = {});

    ~ProgressIndicator();

    /**
     * @brief Resets the progress and returns the root range covering the whole
     *        scale.
     *
     * The returned range may be completed to advance the indicator to its end
     * or passed to a ProgressScope to carve out sub-stages. The indicator and
     * its root scope are reset on every call, so start() may be called again
     * to begin a new run and always returns a fresh range covering the whole
     * [0, 1] scale. Cancellation is owned externally and is not reset here;
     * use a fresh source/token for a new operation when a clean cancellation
     * state is required.
     *
     * @return The root range covering the whole [0, 1] scale.
     */
    ProgressRange start();

    /**
     * @brief Returns the current global progress position.
     *
     * @return Overall progress in [0, 1].
     */
    double position() const;

    /**
     * @brief Returns whether cancellation has been requested.
     *
     * @return true if the operation should stop.
     */
    bool isCancelled() const;

    /**
     * @brief Returns the cancellation token.
     *
     * The token observes the same cancellation state as isCancelled(); it can
     * be polled from other threads or used to register std::stop_callbacks.
     *
     * @return The std token backing this indicator's cancellation state.
     */
    std::stop_token token() const;

    /**
     * @brief Installs the callback called when the reported position moves on.
     *
     * Called at most once per percent of the scale, when the position reaches its
     * end, and when it is reset behind the announcer's back (a restarted
     * indicator). Coalescing is what makes this affordable on the reporting path:
     * the callback is one indirect call per percent step, not one per item.
     *
     * Called on the thread that reports the progress, with no lock held, so the
     * callback must be safe to run concurrently with itself - the ambient host
     * publishes an event from it.
     *
     * Install it before reporting starts (a host does it in its constructor); the
     * reporting path reads it without a lock.
     *
     * @param callback Callback to install; empty to remove it.
     */
    void setPositionCallback(std::function<void()> callback);

  private:
    void increment(double step);

    /// Calls the installed position callback, if there is one.
    void announce();

    /**
     * @brief Announces the new position to observers when it is worth seeing.
     *
     * Coalescing lives here because increment() is the single writer of the
     * position, and it is called once per reported item: a whole percent step, the
     * end of the scale and a step backwards (a restarted indicator) are announced,
     * and everything in between is not.
     *
     * @param new_position Position just stored.
     */
    void announceIfSignificant(double new_position);

    std::atomic<double>     position_{0.0};

    /// Position last announced through the position callback.
    std::atomic<double>     announced_{0.0};

    /// Installed by setPositionCallback(); read by the reporting path.
    std::function<void()>   on_position_;

    ProgressScope*          root_scope_{nullptr};

    std::stop_token         token_;
};

V_PROGRESS_NS_END
