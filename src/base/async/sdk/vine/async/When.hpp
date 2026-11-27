#pragma once

#include "async_global.hpp"

#include <atomic>
#include <cassert>
#include <coroutine>
#include <exception>
#include <memory>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <stop_token>
#include <tuple>
#include <type_traits>
#include <utility>
#include <vector>

#include <vine/CancellationToken.hpp>

#include "Cancellation.hpp"
#include "Sleep.hpp"
#include "Task.hpp"

VN_ASYNC_NS_BEGIN

namespace detail
{

/**
 * @brief Whether a composition waits for every child or the first child.
 */
enum class WhenMode
{
    All, // Wait for every child (whenAll).
    Any  // Wait for the first child (whenAny).
};

/**
 * @brief One-slot hand-off from a completing child to the composition driver.
 *
 * @details
 * - The driver has exactly one waiter - itself - so one slot is the whole mechanism: while the
 *   driver is parked its handle sits in `parked_`, and a child that completes exchanges it out and
 *   *returns* it, so the child's `final_suspend::await_suspend` can symmetric-transfer to the
 *   driver instead of resuming it.
 * - That ordering is the point. A driver that finishes is destroyed together with its children,
 *   and destroying a coroutine is only defined while it is suspended: during `await_suspend` it is
 *   not, so resuming the driver there destroys a frame that is still executing (undefined
 *   behaviour, invisible to sanitizers because nothing is written to the frame afterwards).
 *   Handing the handle back makes the child suspended first.
 * - A completion that arrives while nobody is parked sets `pending_`, and the next park consumes
 *   it without suspending. That is the "the event is already set" rule, with no re-arm window: a
 *   manual-reset flag had to be cleared before each wait, and a completion landing in that window
 *   would be lost.
 * - The slot is a *registration*, not a write into shared state: the driver withdraws its handle in
 *   `~WhenPark` (unpark), so a wake-up posted before the driver died finds an empty slot and reports
 *   `noop_coroutine()` instead of handing back a freed frame (see WhenPark).
 */
class WhenSignal {
  public:
    WhenSignal() noexcept = default;

    WhenSignal(const WhenSignal&)            = delete;
    WhenSignal& operator=(const WhenSignal&) = delete;

    /**
     * @brief Parks the driver, or reports that a completion is already pending.
     *
     * @param self Driver coroutine to hand to the next child that completes.
     * @return true to suspend; false when a completion arrived before the park and was consumed.
     */
    bool park(std::coroutine_handle<> self) noexcept
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (pending_) {
            pending_ = false;
            return false;
        }
        parked_ = self;
        return true;
    }

    /**
     * @brief Claims the parked driver, or records a completion nobody is waiting for yet.
     *
     * @return The driver's handle, to be transferred to rather than resumed; std::noop_coroutine()
     *         when it was not parked, in which case the completion is remembered for the next
     *         park().
     */
    std::coroutine_handle<> wake() noexcept
    {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (auto parked = std::exchange(parked_, {})) {
                return parked;
            }
            pending_ = true;
        }
        return std::noop_coroutine();
    }

    /**
     * @brief Withdraws a parked driver that is going away, so no later wake can reach it.
     *
     * @details
     * - The mirror of park(): the driver's frame is about to be destroyed, while a wake-up already
     *   posted to the timer thread may still run afterwards. Clearing the slot here makes that
     *   wake-up find nothing.
     * - Only the handle being withdrawn is cleared, so a stale awaiter cannot cancel a park that
     *   replaced it.
     *
     * @param driver Handle to withdraw; ignored when the slot no longer holds it.
     */
    void unpark(std::coroutine_handle<> driver) noexcept
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (parked_ == driver) {
            parked_ = {};
        }
    }

  private:
    std::mutex              mutex_;
    /// The driver's handle while it is parked; empty otherwise.
    std::coroutine_handle<> parked_{};
    /// true when a completion arrived while no driver was parked.
    bool                    pending_{ false };
};

/**
 * @brief Awaits a composition's signal: suspends until a child reports.
 *
 * The awaiting half of WhenSignal, spelled as an awaiter so a driver reads
 * `co_await WhenPark<State>{ state }` where it used to await an event.
 *
 * @tparam State Composition state type holding a `signal` member.
 */
template <typename State>
class WhenPark {
  public:
    /**
     * @brief Binds the composition state whose signal is awaited.
     *
     * @param state State to park on; kept alive for the duration of the await.
     */
    explicit WhenPark(std::shared_ptr<State> state) noexcept
      : state_(std::move(state))
    {}

    WhenPark(const WhenPark&)            = delete;
    WhenPark& operator=(const WhenPark&) = delete;

    /**
     * @brief Withdraws the park when this awaiter dies while the driver is still parked.
     *
     * @details
     * - The driver frame is destroyed while suspended here when a `Destroy` parent, a `Scope`
     *   teardown or a thread-pool shutdown abandons the composition. A cancellation wake-up already
     *   posted to the timer thread keeps the state alive and runs afterwards, so the slot has to be
     *   empty by then - otherwise it hands back a freed frame and resumes it.
     * - The await that completes normally clears the handle in await_resume(), so this destructor
     *   only takes the signal's lock on the abandoned path.
     */
    ~WhenPark() noexcept
    {
        if (driver_) {
            state_->signal.unpark(driver_);
        }
    }

    [[nodiscard]]
    bool await_ready() const noexcept
    {
        return false;
    }

    /**
     * @brief Parks this coroutine, unless a completion is already pending.
     *
     * @param h The driver; handed to the next child that completes.
     * @return false to resume inline when a completion was already pending.
     */
    bool await_suspend(std::coroutine_handle<> h) noexcept
    {
        driver_ = h;
        return state_->signal.park(h);
    }

    void await_resume() noexcept
    {
        driver_ = {}; // The await completed; nothing is parked any more.
    }

  private:
    std::shared_ptr<State>  state_;
    /// The driver parked in `state_->signal`, non-null only while the await is suspended.
    std::coroutine_handle<> driver_{};
};

/**
 * @brief Posts a composition wake-up to the timer thread.
 *
 * What a `std::stop_callback` above a composition uses: it may not resume the driver itself (the
 * driver unwinds and destroys that callback while its own callback is still running on this
 * thread), so the wake-up is queued and the timer thread runs it - the same rule detail::deferWake
 * follows for a Scope, with the composition's own signal instead of its event.
 *
 * @tparam State Composition state type holding a `signal` member.
 * @param state State to wake; kept alive until the wake-up has run.
 */
template <typename State>
void deferWhenWake(const std::shared_ptr<State>& state) noexcept
{
    // wake() hands back either the parked driver or noop_coroutine(); resuming noop is a no-op.
    TimerService::instance().postWake(state, [](void* p) noexcept { static_cast<State*>(p)->signal.wake().resume(); });
}

/**
 * @brief Shared completion state of a whenAll/whenAny composition.
 *
 * Lives in the composition's coroutine frame; the children are owned by the
 * composition too, so the state can never be accessed after the frame dies
 * (structured concurrency).
 *
 * Synchronization: the non-atomic payloads (first_exception, and the typed
 * results) are published before the child ticks the seq_cst counters below.
 * The final decrement synchronizes-with every earlier one, and `signal.wake()`
 * hands the parked composition back to the child that finished, which resumes it
 * on its own thread by symmetric transfer - see WhenSignal for why it is handed
 * back rather than resumed.
 * Cancellation is the one path that does not: its wake-up is posted to the timer thread, so
 * the composition never unwinds inside the cancelling thread's stop_callback (see
 * detail::deferWhenWake).
 * The cancellation path never reads those payloads (it throws first), so no
 * data race exists.
 *
 * policy and source are what a completed composition does about its children: source is the
 * environment every child is handed (the same mechanism Scope::add uses, see TaskEnvironment), and
 * policy decides whether being done means "ask them to stop and wait" or "destroy them now". A
 * Destroy composition never calls source.request_stop(), so the token its children see stays inert.
 */
struct WhenState {
    std::atomic<std::size_t> remaining{ 0 };
    std::atomic<bool>        exception_recorded{ false };
    std::atomic<bool>        any_finished{ false };
    std::atomic<bool>        cancelled{ false };
    WhenMode                 mode{ WhenMode::All };
    CancelPolicy             policy{ CancelPolicy::Destroy };
    std::exception_ptr       first_exception{};
    std::stop_source         source{};
    /// The driver's park slot; see WhenSignal.
    WhenSignal signal;
};

/**
 * @brief Waits until every child of a StopAndWait composition has reported.
 *
 * Asks the children to stop first: that request is what a cooperative child needs in order to wind
 * down, and it is idempotent, so asking again on a path that already asked is free.
 *
 * The counter decides; the signal only says "something changed". A child that finishes between the
 * count being read and the driver parking leaves a pending wake-up behind, so the park returns
 * without suspending and the loop re-reads the count - there is no re-arm window a completion could
 * fall into, which is why this loop is shorter than the manual-reset version it replaced.
 *
 * @tparam State Composition state type (WhenState, or the typed whenAny state).
 * @param state Shared state whose remaining counter is drained.
 * @return A task completing once every child has finished.
 */
template <typename State>
Task<void> drainChildren(std::shared_ptr<State> state)
{
    state->source.request_stop();

    while (state->remaining.load() != 0) {
        co_await WhenPark<State>{ state };
    }
}

/**
 * @brief A child coroutine of a composition that drives one sub-task.
 *
 * The composition owns the child's frame (WhenChild), so destroying the
 * composition destroys every child first — children can never outlive the
 * composition. On completion the child ticks the shared state and stays
 * suspended at final_suspend until the composition destroys it.
 */
class WhenChild {
  public:
    struct promise_type {
        std::shared_ptr<WhenState> state;

        /// true while this child's final `await_suspend` is waking the driver; see ~WhenChild.
        bool in_await_suspend{ false };

        [[nodiscard]]
        WhenChild get_return_object() noexcept
        {
            return WhenChild{ std::coroutine_handle<promise_type>::from_promise(*this) };
        }

        std::suspend_always initial_suspend() noexcept
        {
            return {};
        }

        struct FinalAwaiter {
            [[nodiscard]]
            bool await_ready() const noexcept
            {
                return false;
            }

            std::coroutine_handle<> await_suspend(std::coroutine_handle<promise_type> h) noexcept
            {
                // A thread-stack copy keeps the state alive through the transfer below: the driver
                // may complete and be destroyed inside it, taking this frame with it.
                auto state  = h.promise().state;
                bool failed = h.promise().exception != nullptr;
                bool finish = false;

                if (state->mode == WhenMode::All) {
                    // Record the first failure among all children, in any order.
                    const bool first_failure = failed && !state->exception_recorded.exchange(true);
                    if (first_failure) {
                        state->first_exception = std::move(h.promise().exception);
                    }
                    const bool last = (--state->remaining == 0);
                    // The last child always ends the wait. A failure wakes the composition early
                    // only under StopAndWait, where it is the cue to ask the survivors to stop and
                    // then keep waiting; under Destroy waking early would make the composition
                    // abandon and destroy them, which is the behaviour that policy exists to keep.
                    finish          = last || (first_failure && state->policy == CancelPolicy::StopAndWait);
                }
                else {
                    // Only the first child to finish publishes its failure.
                    const bool first = !state->any_finished.exchange(true);
                    if (first && failed) {
                        state->first_exception = std::move(h.promise().exception);
                    }
                    const bool last = (--state->remaining == 0);
                    // Under StopAndWait the losers are still on their way out after the first one
                    // won, so the composition has to be woken again when the last of them reports.
                    finish          = first || (last && state->policy == CancelPolicy::StopAndWait);
                }

                if (finish) {
                    // Hand the driver back instead of resuming it. This coroutine is not suspended
                    // until await_suspend returns, and a driver that completes is destroyed
                    // together with its children - including this one - so resuming it here would
                    // destroy a frame that is still executing.
                    h.promise().in_await_suspend = true;
                    std::coroutine_handle<> next = state->signal.wake();
                    h.promise().in_await_suspend = false;
                    return next;
                }

                // Stay suspended: the composition destroys this frame.
                return std::noop_coroutine();
            }

            void await_resume() const noexcept
            {}
        };

        FinalAwaiter final_suspend() noexcept
        {
            return {};
        }

        void return_void() noexcept
        {}

        void unhandled_exception() noexcept
        {
            exception = std::current_exception();
        }

        std::exception_ptr exception{};
    };

    using handle_type = std::coroutine_handle<promise_type>;

    WhenChild() noexcept = default;

    explicit WhenChild(handle_type h) noexcept
      : handle_(h)
    {}

    WhenChild(WhenChild&& other) noexcept
      : handle_(std::exchange(other.handle_, {}))
    {}

    WhenChild& operator=(WhenChild&& other) noexcept
    {
        if (this != &other) {
            if (handle_) {
                handle_.destroy();
            }
            handle_ = std::exchange(other.handle_, {});
        }
        return *this;
    }

    ~WhenChild()
    {
        if (handle_) {
            // Debug check for the lifetime rule this class exists to keep: the composition owns
            // this frame and destroys it when it completes, so the frame must already be suspended
            // by then. A wake path that resumes the driver from inside await_suspend gets here with
            // the flag still set. See WhenSignal.
            assert(!handle_.promise().in_await_suspend && "a composition child was destroyed from inside its own final await_suspend");
            handle_.destroy();
        }
    }

    WhenChild(const WhenChild&)            = delete;
    WhenChild& operator=(const WhenChild&) = delete;

    /// Binds the shared state and starts driving the sub-task.
    void start(std::shared_ptr<WhenState> state) noexcept
    {
        assert(handle_);
        handle_.promise().state = std::move(state);
        handle_.resume();
    }

  private:
    handle_type handle_{};
};

/**
 * @brief Child coroutine that drives one erased task.
 *
 * The task is handed the composition's environment before it is awaited: it is still lazy here, so
 * this is the last moment at which it can be told what environment it lives in (see
 * currentStopToken(); Scope::add() does the same injection on the scope side).
 *
 * @param task Task to drive; must be non-empty.
 * @param token Environment token handed to the task.
 * @return The child to start.
 */
inline WhenChild composeChild(AnyTask task, std::stop_token token)
{
    TaskEnvironment::setToken(task, token);
    co_await std::move(task);
}

/**
 * @brief Drives a whenAll/whenAny over void tasks.
 *
 * Both modes share the whole lifecycle: pre-check the token, build the shared
 * state, own and start every child, hand them the composition's environment, wake on completion
 * or cancellation, cancel per policy, then rethrow the recorded outcome. The completion rule and
 * the cancellation policy are the two parameters this driver takes.
 *
 * @param mode Completion rule: All waits for every child, Any for the first.
 * @param tasks Tasks to await; each element must be non-empty.
 * @param token Token checked before any child is started.
 * @param policy What to do with the children the composition is done with.
 * @return A task completing per mode, rethrowing the first child failure.
 */
[[nodiscard]]
Task<void> whenRace(WhenMode mode, std::vector<AnyTask> tasks, CancellationToken token, CancelPolicy policy)
{
    throwIfCancelled(token);

    const std::size_t count = tasks.size();
    if (count == 0) {
        co_return;
    }

    // Heap-allocated because three parties hold it independently: the composition frame, the
    // stack copy a completing child takes in its final await_suspend, and a posted cancellation
    // wake-up. The frame dies first when the composition completes, and the other two must still
    // be able to touch the state safely.
    auto state       = std::make_shared<WhenState>();
    state->mode      = mode;
    state->policy    = policy;
    state->remaining = count;

    // Wakes the composition on cancellation; flag-only, no coroutine resume race. The wake-up
    // is handed to the timer thread instead of being resumed here: this callback runs inside
    // request_stop(), and the composition it would resume unwinds and destroys this very
    // stop_callback while its own callback is still on the stack - the same-thread destruction
    // the standard leaves undefined (see detail::deferWhenWake).
    std::stop_callback cancellation{ token, [state]() noexcept {
                                        state->cancelled = true;
                                        detail::deferWhenWake(state);
                                    } };

    std::vector<WhenChild> children;
    children.reserve(count);
    const std::stop_token environment = state->source.get_token();
    for (auto& task : tasks) {
        children.push_back(composeChild(std::move(task), environment));
    }
    for (auto& child : children) {
        child.start(state);
    }

    co_await WhenPark<WhenState>{ state };

    if (state->policy == CancelPolicy::StopAndWait) {
        co_await drainChildren(state);
    }

    if (state->cancelled) {
        throw TaskCancelledException{};
    }
    if (state->first_exception) {
        std::rethrow_exception(state->first_exception);
    }
}

/**
 * @brief Result container a whenAll(std::vector<Task<T>>) produces.
 *
 * The void case never reaches a coroutine body - the overload's constraint rejects it - and it is not
 * here for the result, it is here for the declaration: substituting T=void into it must stay
 * well-formed. std::vector<void> is a hard error rather than a substitution failure, and a compiler
 * may form the return type before it looks at the requires-clause (MSVC does), which would break the
 * overload set of a call it is not even meant to win - whenAll(std::vector<AnyTask>), where AnyTask
 * is Task<void> and the plain non-template overload is the one that should be chosen.
 */
template <typename T, bool IsVoid = std::is_void_v<T>>
struct VectorResult {
    using type = std::vector<T>;
};

template <typename T>
struct VectorResult<T, true> {
    using type = void;
};

/**
 * @brief Placeholder for a void slot of the tuple a variadic whenAll produces.
 *
 * Never observed - the overloads producing that tuple reject void argument types - and here for the
 * same reason as the void case of VectorResult: it keeps their declarations substitutable for
 * Task<void> arguments instead of failing on std::tuple<void> while forming them.
 */
struct VoidResult {};

/**
 * @brief One slot of that tuple: the task's result type, or the placeholder for a void task.
 */
template <typename T, bool IsVoid = std::is_void_v<T>>
struct TupleResult {
    using type = T;
};

template <typename T>
struct TupleResult<T, true> {
    using type = VoidResult;
};

/**
 * @brief Tuple a variadic whenAll produces, substitutable for void argument types.
 */
template <typename... Ts>
using TupleResults = std::tuple<typename TupleResult<Ts>::type...>;

} // namespace detail

/**
 * @brief Awaits all tasks concurrently; completes when every task finishes.
 *
 * Every task is started immediately and handed the composition's environment token. If a task
 * throws, the first exception observed by completion order is rethrown after every task has
 * finished, and the survivors are asked to stop first (the default policy, StopAndWait). If the
 * token is already cancelled, TaskCancelledException is thrown without starting any task;
 * cancellation while waiting asks the children to stop and then waits for them, and the exception
 * is thrown once the last of them has reported. CancelPolicy::Destroy restores the older behaviour
 * of abandoning the unfinished children instead of waiting for them. Structured concurrency: the
 * children are owned by the composition, so destroying the returned task also destroys them. The
 * container is consumed.
 *
 * @param tasks Tasks to await; each element must be non-empty.
 * @param token Optional cancellation token.
 * @param policy What to do with children the composition is done with; defaults to waiting.
 * @return A task that completes when every input task completes.
 */
[[nodiscard]]
Task<void> whenAll(std::vector<AnyTask> tasks, CancellationToken token = {}, CancelPolicy policy = CancelPolicy::StopAndWait)
{
    // Forwarded, not wrapped in another coroutine: the driver is already lazy and
    // owns the children, so an extra frame would buy nothing.
    return detail::whenRace(detail::WhenMode::All, std::move(tasks), std::move(token), policy);
}

/**
 * @brief Awaits until the first task completes.
 *
 * Every task is started immediately and handed the composition's environment token. The returned
 * task completes as soon as one task finishes; under the default policy the remaining tasks are
 * then destroyed together with the composition (structured concurrency), and CancelPolicy::
 * StopAndWait asks them to stop and waits for them instead. If the first task to finish threw,
 * that exception is rethrown. If the token is already cancelled, TaskCancelledException is thrown
 * without starting any task; cancellation while waiting destroys the children (or, under
 * StopAndWait, asks them to stop and waits). The container is consumed.
 *
 * @param tasks Tasks to await; each element must be non-empty.
 * @param token Optional cancellation token.
 * @param policy What to do with children the composition is done with; defaults to destroying.
 * @return A task that completes when the first input task completes.
 */
[[nodiscard]]
Task<void> whenAny(std::vector<AnyTask> tasks, CancellationToken token = {}, CancelPolicy policy = CancelPolicy::Destroy)
{
    return detail::whenRace(detail::WhenMode::Any, std::move(tasks), std::move(token), policy);
}

namespace detail
{

/**
 * @brief Child coroutine that drives one typed task and stores its result.
 *
 * The slot is owned by the composition frame, so it outlives the child
 * (structured concurrency). On success the result is stored before the child
 * ticks the shared state; on failure the exception propagates to the child's
 * unhandled_exception and the slot stays empty.
 *
 * @tparam T Result type of the task.
 * @param task Task to run.
 * @param slot Where to store the result on success.
 * @param token Environment token handed to the task before it starts.
 */
template <typename T>
WhenChild composeChildResult(Task<T> task, std::optional<T>* slot, std::stop_token token)
{
    TaskEnvironment::setToken(task, token);

    if constexpr (std::is_void_v<T>) {
        co_await std::move(task);
        co_return;
    }
    else {
        slot->emplace(co_await std::move(task));
        co_return;
    }
}

/**
 * @brief Implements the variadic whenAll.
 *
 * @tparam Ts Result types of the tasks (non-void).
 * @param token Cancellation token.
 * @param policy What to do with children the composition is done with.
 * @param tasks Tasks to await.
 * @return A task producing the tuple of all results.
 */
template <typename... Ts>
[[nodiscard]]
Task<std::tuple<Ts...>> whenAllImpl(CancellationToken token, CancelPolicy policy, Task<Ts>... tasks)
{
    throwIfCancelled(token);

    auto state       = std::make_shared<detail::WhenState>();
    state->mode      = detail::WhenMode::All;
    state->policy    = policy;
    state->remaining = sizeof...(Ts);

    // Cancellation is handed to the timer thread; see whenRace() for why.
    std::stop_callback cancellation{ token, [state]() noexcept {
                                        state->cancelled = true;
                                        detail::deferWhenWake(state);
                                    } };

    std::tuple<std::optional<Ts>...> results;
    const std::stop_token            environment = state->source.get_token();
    auto                             children =
        std::apply([&tasks..., environment](auto&... slot) { return std::make_tuple(detail::composeChildResult(std::move(tasks), &slot, environment)...); },
                   results);

    std::apply([&state](auto&... child) { (child.start(state), ...); }, children);

    co_await WhenPark<WhenState>{ state };

    if (state->policy == CancelPolicy::StopAndWait) {
        co_await drainChildren(state);
    }

    if (state->cancelled) {
        throw TaskCancelledException{};
    }
    if (state->first_exception) {
        std::rethrow_exception(state->first_exception);
    }

    assert(std::apply([](const auto&... slot) { return (... && slot.has_value()); }, results));
    co_return std::apply([](auto&... slot) { return std::tuple<Ts...>{ std::move(*slot)... }; }, results);
}

/**
 * @brief Shared completion state of a result-returning whenAny.
 *
 * policy and source mean what they mean in WhenState: the children are handed source's token, and
 * policy decides whether the composition waits for the losers (StopAndWait) or destroys them
 * (Destroy). Only StopAndWait reads remaining, which counts the children that have not reported.
 */
template <typename T>
struct WhenAnyState {
    std::atomic<std::size_t> remaining{ 0 };
    std::atomic<bool>        first_done{ false };
    std::atomic<bool>        cancelled{ false };
    CancelPolicy             policy{ CancelPolicy::Destroy };
    std::exception_ptr       exception{};
    std::optional<T>         result{};
    std::stop_source         source{};
    /// The driver's park slot; see WhenSignal.
    WhenSignal signal;
};

/**
 * @brief A child of a result-returning whenAny; publishes its own outcome.
 *
 * Only the first child to finish publishes (its result or its exception) into
 * the shared state; later children stay suspended until the composition
 * destroys them. The result is stored in the child's promise by return_value.
 */
template <typename T>
class WhenAnyChild {
  public:
    struct promise_type {
        std::shared_ptr<WhenAnyState<T>> state{};
        std::exception_ptr               exception{};
        std::optional<T>                 result{};

        /// true while this child's final `await_suspend` is waking the driver; see ~WhenAnyChild.
        bool in_await_suspend{ false };

        [[nodiscard]]
        WhenAnyChild get_return_object() noexcept
        {
            return WhenAnyChild{ std::coroutine_handle<promise_type>::from_promise(*this) };
        }

        std::suspend_always initial_suspend() noexcept
        {
            return {};
        }

        struct FinalAwaiter {
            [[nodiscard]]
            bool await_ready() const noexcept
            {
                return false;
            }

            std::coroutine_handle<> await_suspend(std::coroutine_handle<promise_type> h) noexcept
            {
                auto& p    = h.promise();
                // Thread-stack copy keeps the state alive through the transfer below.
                auto state = p.state;

                // Only the first child to finish publishes its outcome.
                const bool first = !state->first_done.exchange(true);
                if (first) {
                    if (p.exception) {
                        state->exception = std::move(p.exception);
                    }
                    else {
                        assert(p.result.has_value());
                        // A result whose move constructor throws must not escape this noexcept
                        // await_suspend: nothing here could catch it, so the process would
                        // terminate. It becomes this child's failure instead, and the
                        // composition rethrows it like any other.
                        try {
                            state->result.emplace(std::move(p.result).value());
                        }
                        catch (...) {
                            state->exception = std::current_exception();
                        }
                    }
                }

                // StopAndWait keeps waiting after the winner has published, so the composition has
                // to be woken again when the last of the losers reports.
                const bool last = (--state->remaining == 0);
                if (first || (last && state->policy == CancelPolicy::StopAndWait)) {
                    // Hand the driver back instead of resuming it: this coroutine is not suspended
                    // until await_suspend returns, and a driver that completes is destroyed
                    // together with its children - including this one.
                    p.in_await_suspend = true;
                    std::coroutine_handle<> next = state->signal.wake();
                    p.in_await_suspend = false;
                    return next;
                }

                // Stay suspended; the composition destroys this frame.
                return std::noop_coroutine();
            }

            void await_resume() const noexcept
            {}
        };

        FinalAwaiter final_suspend() noexcept
        {
            return {};
        }

        /// A value that cannot be moved into the frame is reported as this child's failure.
        void return_value(T value)
        {
            try {
                result.emplace(std::move(value));
            }
            catch (...) {
                exception = std::current_exception();
            }
        }

        void unhandled_exception() noexcept
        {
            exception = std::current_exception();
        }
    };

    using handle_type = std::coroutine_handle<promise_type>;

    WhenAnyChild() noexcept = default;

    explicit WhenAnyChild(handle_type h) noexcept
      : handle_(h)
    {}

    WhenAnyChild(WhenAnyChild&& other) noexcept
      : handle_(std::exchange(other.handle_, {}))
    {}

    WhenAnyChild& operator=(WhenAnyChild&& other) noexcept
    {
        if (this != &other) {
            if (handle_) {
                handle_.destroy();
            }
            handle_ = std::exchange(other.handle_, {});
        }
        return *this;
    }

    ~WhenAnyChild()
    {
        if (handle_) {
            // Debug check for the same rule WhenChild keeps: the composition destroys this frame
            // when it completes, so the frame must already be suspended by then.
            assert(!handle_.promise().in_await_suspend && "a composition child was destroyed from inside its own final await_suspend");
            handle_.destroy();
        }
    }

    WhenAnyChild(const WhenAnyChild&)            = delete;
    WhenAnyChild& operator=(const WhenAnyChild&) = delete;

    /// Binds the shared state and starts driving the sub-task.
    void start(std::shared_ptr<WhenAnyState<T>> state) noexcept
    {
        handle_.promise().state = std::move(state);
        handle_.resume();
    }

  private:
    handle_type handle_{};
};

/**
 * @brief Composes a typed task into a result-returning whenAny child.
 *
 * @tparam T Result type of the task.
 * @param task Task to drive; must be non-empty.
 * @param token Environment token handed to the task before it starts.
 * @return The child to start.
 */
template <typename T>
WhenAnyChild<T> composeAnyChild(Task<T> task, std::stop_token token)
{
    TaskEnvironment::setToken(task, token);

    if constexpr (std::is_void_v<T>) {
        co_await std::move(task);
        co_return;
    }
    else {
        co_return co_await std::move(task);
    }
}

/**
 * @brief Composes a void task pack into the container the shared driver takes.
 *
 * The void variadic overloads have no result to return, so they reuse the
 * already-tested vector driver rather than duplicating it over tuples.
 *
 * @tparam Ts Task types; every one of them must be void.
 * @param tasks Tasks to move into the container.
 * @return The tasks in argument order.
 */
template <typename... Ts>
requires(sizeof...(Ts) > 0) && (std::is_void_v<Ts> && ...)
std::vector<AnyTask> toAnyTasks(Task<Ts>... tasks)
{
    std::vector<AnyTask> all;
    all.reserve(sizeof...(tasks));
    (all.push_back(std::move(tasks)), ...);
    return all;
}

} // namespace detail

/**
 * @brief Awaits heterogeneous tasks concurrently and returns all results.
 *
 * Equivalent to whenAll(std::vector<AnyTask>) but preserves each result. The
 * returned task produces a std::tuple holding every result in argument order, and every task is
 * handed the composition's environment token. If a task throws, the first exception observed by
 * completion order is rethrown after every task has finished, the survivors having been asked to
 * stop first (the default policy, StopAndWait); CancelPolicy::Destroy abandons them instead. If
 * the token is already cancelled, TaskCancelledException is thrown without starting any task.
 * Structured concurrency: children are owned by the composition. Tasks must be non-void; when
 * every task is a Task<void> the void overload below is selected instead.
 *
 * @tparam Ts Result types of the tasks.
 * @param tasks Tasks to await; each must be non-empty.
 * @param token Optional cancellation token; when supplied it is the first
 *        argument.
 * @return A task producing the tuple of all results.
 */
template <typename... Ts>
requires(sizeof...(Ts) > 0) && (std::conjunction_v<std::negation<std::is_void<Ts>>...>)
[[nodiscard]]
Task<detail::TupleResults<Ts...>> whenAll(Task<Ts>... tasks)
{
    co_return co_await detail::whenAllImpl(CancellationToken{}, CancelPolicy::StopAndWait, std::move(tasks)...);
}

template <typename... Ts>
requires(sizeof...(Ts) > 0) && (std::conjunction_v<std::negation<std::is_void<Ts>>...>)
[[nodiscard]]
Task<detail::TupleResults<Ts...>> whenAll(CancellationToken token, Task<Ts>... tasks)
{
    co_return co_await detail::whenAllImpl(std::move(token), CancelPolicy::StopAndWait, std::move(tasks)...);
}

/**
 * @brief Awaits heterogeneous tasks concurrently and returns all results, with an explicit policy.
 *
 * The explicit-policy counterpart of the forms above: CancelPolicy::Destroy is the one to reach for
 * when a task ignores its environment token, since such a task parks a StopAndWait composition
 * until it finishes on its own.
 *
 * @tparam Ts Result types of the tasks.
 * @param policy What to do with the children the composition is done with.
 * @param tasks Tasks to await; each must be non-empty.
 * @return A task producing the tuple of all results.
 */
template <typename... Ts>
requires(sizeof...(Ts) > 0) && (std::conjunction_v<std::negation<std::is_void<Ts>>...>)
[[nodiscard]]
Task<detail::TupleResults<Ts...>> whenAll(CancelPolicy policy, Task<Ts>... tasks)
{
    co_return co_await detail::whenAllImpl(CancellationToken{}, policy, std::move(tasks)...);
}

/**
 * @brief Awaits heterogeneous tasks concurrently and returns all results, with an explicit policy and token.
 *
 * @tparam Ts Result types of the tasks.
 * @param policy What to do with the children the composition is done with.
 * @param token Cancellation token checked before any task is started.
 * @param tasks Tasks to await; each must be non-empty.
 * @return A task producing the tuple of all results.
 */
template <typename... Ts>
requires(sizeof...(Ts) > 0) && (std::conjunction_v<std::negation<std::is_void<Ts>>...>)
[[nodiscard]]
Task<detail::TupleResults<Ts...>> whenAll(CancelPolicy policy, CancellationToken token, Task<Ts>... tasks)
{
    co_return co_await detail::whenAllImpl(std::move(token), policy, std::move(tasks)...);
}

/**
 * @brief Awaits void tasks concurrently; variadic form.
 *
 * The void counterpart of the tuple form above and the direct form of
 * whenAll(std::vector<AnyTask>): pass Task<void> values without building a container. Semantics
 * match the container form - every task is started and handed the composition's environment token,
 * the first failure by completion order is rethrown after every task has finished (the survivors
 * having been asked to stop), and an already-cancelled token throws without starting any task.
 *
 * @tparam Ts Task types; every one of them must be void.
 * @param tasks Tasks to await; each must be non-empty.
 * @param token Optional cancellation token; when supplied it is the first
 *        argument.
 * @return A task completing when every input task completes.
 */
template <typename... Ts>
requires(sizeof...(Ts) > 0) && (std::is_void_v<Ts> && ...)
[[nodiscard]]
Task<void> whenAll(Task<Ts>... tasks)
{
    co_await detail::whenRace(detail::WhenMode::All, detail::toAnyTasks(std::move(tasks)...), CancellationToken{}, CancelPolicy::StopAndWait);
}

template <typename... Ts>
requires(sizeof...(Ts) > 0) && (std::is_void_v<Ts> && ...)
[[nodiscard]]
Task<void> whenAll(CancellationToken token, Task<Ts>... tasks)
{
    co_await detail::whenRace(detail::WhenMode::All, detail::toAnyTasks(std::move(tasks)...), std::move(token), CancelPolicy::StopAndWait);
}

/**
 * @brief Awaits void tasks concurrently with an explicit policy; variadic form.
 *
 * The CancelPolicy::Destroy counterpart of the forms above, for a task that ignores its
 * environment token and would therefore park a StopAndWait composition.
 *
 * @tparam Ts Task types; every one of them must be void.
 * @param policy What to do with the children the composition is done with.
 * @param tasks Tasks to await; each must be non-empty.
 * @return A task completing when every input task completes.
 */
template <typename... Ts>
requires(sizeof...(Ts) > 0) && (std::is_void_v<Ts> && ...)
[[nodiscard]]
Task<void> whenAll(CancelPolicy policy, Task<Ts>... tasks)
{
    co_await detail::whenRace(detail::WhenMode::All, detail::toAnyTasks(std::move(tasks)...), CancellationToken{}, policy);
}

/**
 * @brief Awaits void tasks concurrently with an explicit policy and token; variadic form.
 *
 * @tparam Ts Task types; every one of them must be void.
 * @param policy What to do with the children the composition is done with.
 * @param token Cancellation token checked before any task is started.
 * @param tasks Tasks to await; each must be non-empty.
 * @return A task completing when every input task completes.
 */
template <typename... Ts>
requires(sizeof...(Ts) > 0) && (std::is_void_v<Ts> && ...)
[[nodiscard]]
Task<void> whenAll(CancelPolicy policy, CancellationToken token, Task<Ts>... tasks)
{
    co_await detail::whenRace(detail::WhenMode::All, detail::toAnyTasks(std::move(tasks)...), std::move(token), policy);
}

/**
 * @brief Awaits same-typed tasks concurrently and returns all results.
 *
 * Equivalent to whenAll(std::vector<AnyTask>) but preserves each result. The
 * returned task produces a std::vector with results in input order, and every task is handed the
 * composition's environment token. If a task throws, the first exception observed by completion
 * order is rethrown after every task has finished, and the survivors are asked to stop first (the
 * default policy, StopAndWait). If the token is already cancelled, TaskCancelledException is
 * thrown without starting any task; cancellation while waiting asks the children to stop and then
 * waits for them, and the exception is thrown once the last of them has reported.
 * CancelPolicy::Destroy restores the older behaviour of abandoning the unfinished children. The
 * container is consumed.
 *
 * @tparam T Result type of the tasks (non-void).
 * @param tasks Tasks to await; each must be non-empty.
 * @param token Optional cancellation token.
 * @param policy What to do with children the composition is done with; defaults to waiting.
 * @return A task producing the vector of all results.
 */
template <typename T>
requires(!std::is_void_v<T>)
[[nodiscard]]
Task<typename detail::VectorResult<T>::type> whenAll(std::vector<Task<T>> tasks, CancellationToken token = {}, CancelPolicy policy = CancelPolicy::StopAndWait)
{
    throwIfCancelled(token);

    const std::size_t count = tasks.size();
    if (count == 0) {
        co_return {};
    }

    auto state       = std::make_shared<detail::WhenState>();
    state->mode      = detail::WhenMode::All;
    state->policy    = policy;
    state->remaining = count;

    // Cancellation is handed to the timer thread; see whenRace() for why.
    std::stop_callback cancellation{ token, [state]() noexcept {
                                        state->cancelled = true;
                                        detail::deferWhenWake(state);
                                    } };

    std::vector<std::optional<T>>  results(count);
    std::vector<detail::WhenChild> children;
    children.reserve(count);
    const std::stop_token environment = state->source.get_token();
    for (std::size_t i = 0; i < count; ++i) {
        children.push_back(detail::composeChildResult(std::move(tasks[i]), &results[i], environment));
    }
    for (auto& child : children) {
        child.start(state);
    }

    co_await detail::WhenPark<detail::WhenState>{ state };

    if (state->policy == CancelPolicy::StopAndWait) {
        co_await detail::drainChildren(state);
    }

    if (state->cancelled) {
        throw TaskCancelledException{};
    }
    if (state->first_exception) {
        std::rethrow_exception(state->first_exception);
    }

    std::vector<T> out;
    out.reserve(count);
    for (auto& slot : results) {
        out.push_back(std::move(*slot));
    }
    co_return out;
}

/**
 * @brief Awaits same-typed tasks and returns the first result.
 *
 * Every task is started immediately and handed the composition's environment token; the returned
 * task completes with the result of whichever task finishes first. If that first task threw, its
 * exception is rethrown; under the default policy the remaining tasks are destroyed together with
 * the composition (structured concurrency), and CancelPolicy::StopAndWait asks them to stop and
 * waits for them instead. If the token is already cancelled, TaskCancelledException is thrown
 * without starting any task; cancellation while waiting destroys the children (or, under
 * StopAndWait, asks them to stop and waits). The container is consumed.
 *
 * @tparam T Result type of the tasks (non-void).
 * @param tasks Tasks to await; must not be empty.
 * @param token Optional cancellation token.
 * @param policy What to do with children the composition is done with; defaults to destroying.
 * @return A task producing the first task's result.
 */
template <typename T>
requires(!std::is_void_v<T>)
[[nodiscard]]
Task<T> whenAny(std::vector<Task<T>> tasks, CancellationToken token = {}, CancelPolicy policy = CancelPolicy::Destroy)
{
    throwIfCancelled(token);

    const std::size_t count = tasks.size();
    if (count == 0) {
        throw std::invalid_argument("async::whenAny: empty task list");
    }

    auto state       = std::make_shared<detail::WhenAnyState<T>>();
    state->policy    = policy;
    state->remaining = count;

    // Cancellation is handed to the timer thread; see whenRace() for why.
    std::stop_callback cancellation{ token, [state]() noexcept {
                                        state->cancelled = true;
                                        detail::deferWhenWake(state);
                                    } };

    std::vector<detail::WhenAnyChild<T>> children;
    children.reserve(count);
    const std::stop_token environment = state->source.get_token();
    for (auto& task : tasks) {
        children.push_back(detail::composeAnyChild(std::move(task), environment));
    }
    for (auto& child : children) {
        child.start(state);
    }

    co_await detail::WhenPark<detail::WhenAnyState<T>>{ state };

    if (state->policy == CancelPolicy::StopAndWait) {
        co_await detail::drainChildren(state);
    }

    if (state->cancelled) {
        throw TaskCancelledException{};
    }
    if (state->exception) {
        std::rethrow_exception(state->exception);
    }

    co_return std::move(state->result).value();
}

/**
 * @brief Awaits same-typed tasks and returns the first result; variadic form.
 *
 * Shorthand for whenAny(std::vector<Task<T>>): pass tasks directly without
 * building a container. All tasks must share the same non-void result type.
 * Semantics match the vector form: every task is handed the composition's environment token, the
 * returned task completes with the first finisher's result (or rethrows its exception), and the
 * remaining tasks are destroyed with the composition (or, under CancelPolicy::StopAndWait, asked to
 * stop and waited for). An already-cancelled token throws TaskCancelledException without starting
 * any task.
 *
 * @tparam T Result type shared by every task.
 * @tparam Ts Remaining task types (same as T).
 * @param first First task.
 * @param rest Remaining tasks.
 * @param token Optional cancellation token; when supplied it is the first
 *        argument.
 * @return A task producing the first task's result.
 */
template <typename T, typename... Ts>
requires(!std::is_void_v<T>) && (std::is_same_v<T, Ts> && ...)
[[nodiscard]]
Task<T> whenAny(Task<T> first, Task<Ts>... rest)
{
    std::vector<Task<T>> tasks;
    tasks.reserve(1 + sizeof...(Ts));
    tasks.push_back(std::move(first));
    (tasks.push_back(std::move(rest)), ...);
    co_return co_await whenAny(std::move(tasks), CancellationToken{}, CancelPolicy::Destroy);
}

template <typename T, typename... Ts>
requires(!std::is_void_v<T>) && (std::is_same_v<T, Ts> && ...)
[[nodiscard]]
Task<T> whenAny(CancellationToken token, Task<T> first, Task<Ts>... rest)
{
    throwIfCancelled(token);

    std::vector<Task<T>> tasks;
    tasks.reserve(1 + sizeof...(Ts));
    tasks.push_back(std::move(first));
    (tasks.push_back(std::move(rest)), ...);
    co_return co_await whenAny(std::move(tasks), std::move(token), CancelPolicy::Destroy);
}

/**
 * @brief Awaits same-typed tasks and returns the first result, with an explicit policy.
 *
 * StopAndWait is the policy to reach for when the losers have something to release: the composition
 * asks them to stop and stays parked until they have all wound down, instead of destroying them.
 *
 * @tparam T Result type shared by every task.
 * @tparam Ts Remaining task types (same as T).
 * @param policy What to do with the children the composition is done with.
 * @param first First task.
 * @param rest Remaining tasks.
 * @return A task producing the first task's result.
 */
template <typename T, typename... Ts>
requires(!std::is_void_v<T>) && (std::is_same_v<T, Ts> && ...)
[[nodiscard]]
Task<T> whenAny(CancelPolicy policy, Task<T> first, Task<Ts>... rest)
{
    std::vector<Task<T>> tasks;
    tasks.reserve(1 + sizeof...(Ts));
    tasks.push_back(std::move(first));
    (tasks.push_back(std::move(rest)), ...);
    co_return co_await whenAny(std::move(tasks), CancellationToken{}, policy);
}

/**
 * @brief Awaits same-typed tasks and returns the first result, with an explicit policy and token.
 *
 * @tparam T Result type shared by every task.
 * @tparam Ts Remaining task types (same as T).
 * @param policy What to do with the children the composition is done with.
 * @param token Cancellation token checked before any task is started.
 * @param first First task.
 * @param rest Remaining tasks.
 * @return A task producing the first task's result.
 */
template <typename T, typename... Ts>
requires(!std::is_void_v<T>) && (std::is_same_v<T, Ts> && ...)
[[nodiscard]]
Task<T> whenAny(CancelPolicy policy, CancellationToken token, Task<T> first, Task<Ts>... rest)
{
    throwIfCancelled(token);

    std::vector<Task<T>> tasks;
    tasks.reserve(1 + sizeof...(Ts));
    tasks.push_back(std::move(first));
    (tasks.push_back(std::move(rest)), ...);
    co_return co_await whenAny(std::move(tasks), std::move(token), policy);
}

/**
 * @brief Awaits void tasks and completes with the first of them; variadic form.
 *
 * The void counterpart of the form above and the direct form of
 * whenAny(std::vector<AnyTask>): pass Task<void> values without building a
 * container. Semantics match the container form - every task is started and handed the composition's
 * environment token, the returned task completes when the first task finishes, the remaining tasks
 * are destroyed with the composition (or, under CancelPolicy::StopAndWait, asked to stop and waited
 * for), and an already-cancelled token throws without starting any task.
 *
 * @tparam Ts Task types; every one of them must be void.
 * @param tasks Tasks to await; each must be non-empty.
 * @param token Optional cancellation token; when supplied it is the first
 *        argument.
 * @return A task completing when the first input task completes.
 */
template <typename... Ts>
requires(sizeof...(Ts) > 0) && (std::is_void_v<Ts> && ...)
[[nodiscard]]
Task<void> whenAny(Task<Ts>... tasks)
{
    co_await detail::whenRace(detail::WhenMode::Any, detail::toAnyTasks(std::move(tasks)...), CancellationToken{}, CancelPolicy::Destroy);
}

template <typename... Ts>
requires(sizeof...(Ts) > 0) && (std::is_void_v<Ts> && ...)
[[nodiscard]]
Task<void> whenAny(CancellationToken token, Task<Ts>... tasks)
{
    co_await detail::whenRace(detail::WhenMode::Any, detail::toAnyTasks(std::move(tasks)...), std::move(token), CancelPolicy::Destroy);
}

/**
 * @brief Awaits void tasks and completes with the first of them, with an explicit policy; variadic form.
 *
 * @tparam Ts Task types; every one of them must be void.
 * @param policy What to do with the children the composition is done with.
 * @param tasks Tasks to await; each must be non-empty.
 * @return A task completing when the first input task completes.
 */
template <typename... Ts>
requires(sizeof...(Ts) > 0) && (std::is_void_v<Ts> && ...)
[[nodiscard]]
Task<void> whenAny(CancelPolicy policy, Task<Ts>... tasks)
{
    co_await detail::whenRace(detail::WhenMode::Any, detail::toAnyTasks(std::move(tasks)...), CancellationToken{}, policy);
}

/**
 * @brief Awaits void tasks and completes with the first of them, with an explicit policy and token.
 *
 * @tparam Ts Task types; every one of them must be void.
 * @param policy What to do with the children the composition is done with.
 * @param token Cancellation token checked before any task is started.
 * @param tasks Tasks to await; each must be non-empty.
 * @return A task completing when the first input task completes.
 */
template <typename... Ts>
requires(sizeof...(Ts) > 0) && (std::is_void_v<Ts> && ...)
[[nodiscard]]
Task<void> whenAny(CancelPolicy policy, CancellationToken token, Task<Ts>... tasks)
{
    co_await detail::whenRace(detail::WhenMode::Any, detail::toAnyTasks(std::move(tasks)...), std::move(token), policy);
}

VN_ASYNC_NS_END
