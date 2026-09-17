#pragma once

#include "async_global.hpp"

#include <atomic>
#include <cassert>
#include <coroutine>
#include <exception>
#include <memory>
#include <optional>
#include <stdexcept>
#include <stop_token>
#include <tuple>
#include <type_traits>
#include <utility>
#include <vector>

#include <vine/CancellationToken.hpp>

#include "AsyncEvent.hpp"
#include "Cancellation.hpp"
#include "Task.hpp"

V_ASYNC_NS_BEGIN

namespace detail {

/**
 * @brief Whether a composition waits for every child or the first child.
 */
enum class WhenMode
{
    All,  ///< Wait for every child (whenAll).
    Any   ///< Wait for the first child (whenAny).
};

/**
 * @brief Shared completion state of a whenAll/whenAny composition.
 *
 * Lives in the composition's coroutine frame; the children are owned by the
 * composition too, so the state can never be accessed after the frame dies
 * (structured concurrency).
 *
 * Synchronization: the non-atomic payloads (first_exception, and the typed
 * results) are published before the child ticks the seq_cst counters below.
 * The final decrement synchronizes-with every earlier one, and
 * AsyncEvent::set() resumes the composition inline on the completer's thread.
 * The cancellation path never reads those payloads (it throws first), so no
 * data race exists.
 */
struct WhenState
{
    std::atomic<std::size_t> remaining{ 0 };
    std::atomic<bool>        exception_recorded{ false };
    std::atomic<bool>        any_finished{ false };
    std::atomic<bool>        cancelled{ false };
    WhenMode                 mode{ WhenMode::All };
    std::exception_ptr       first_exception{};
    AsyncEvent               done;
};

/**
 * @brief A child coroutine of a composition that drives one sub-task.
 *
 * The composition owns the child's frame (WhenChild), so destroying the
 * composition destroys every child first — children can never outlive the
 * composition. On completion the child ticks the shared state and stays
 * suspended at final_suspend until the composition destroys it.
 */
class WhenChild
{
  public:
    struct promise_type
    {
        std::shared_ptr<WhenState> state;

        [[nodiscard]]
        WhenChild get_return_object() noexcept
        {
            return WhenChild{ std::coroutine_handle<promise_type>::from_promise(*this) };
        }

        std::suspend_always initial_suspend() noexcept { return {}; }

        struct FinalAwaiter
        {
            [[nodiscard]]
            bool await_ready() const noexcept
            {
                return false;
            }

            std::coroutine_handle<> await_suspend(std::coroutine_handle<promise_type> h) noexcept
            {
                // A thread-stack copy keeps the state alive through done.set(),
                // which resumes the composition to completion and destroys the
                // frames (children included) that also hold references.
                auto  state = h.promise().state;
                bool  finish = false;

                if (state->mode == WhenMode::All)
                {
                    // Record the first failure among all children, in any order.
                    if (h.promise().exception && !state->exception_recorded.exchange(true))
                    {
                        state->first_exception = std::move(h.promise().exception);
                    }
                    finish = (--state->remaining == 0);
                }
                else
                {
                    // Only the first child to finish publishes its failure.
                    finish = !state->any_finished.exchange(true);
                    if (finish && h.promise().exception)
                    {
                        state->first_exception = std::move(h.promise().exception);
                    }
                }

                if (finish)
                {
                    state->done.set();
                }

                // Stay suspended: the composition destroys this frame.
                return std::noop_coroutine();
            }

            void await_resume() const noexcept {}
        };

        FinalAwaiter final_suspend() noexcept { return {}; }

        void return_void() noexcept {}

        void unhandled_exception() noexcept { exception = std::current_exception(); }

        std::exception_ptr exception{};
    };

    using handle_type = std::coroutine_handle<promise_type>;

    WhenChild() noexcept = default;

    explicit WhenChild(handle_type h) noexcept : handle_(h) {}

    WhenChild(WhenChild&& other) noexcept : handle_(std::exchange(other.handle_, {})) {}

    WhenChild& operator=(WhenChild&& other) noexcept
    {
        if (this != &other)
        {
            if (handle_)
            {
                handle_.destroy();
            }
            handle_ = std::exchange(other.handle_, {});
        }
        return *this;
    }

    ~WhenChild()
    {
        if (handle_)
        {
            handle_.destroy();
        }
    }

    WhenChild(const WhenChild&) = delete;
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

inline WhenChild composeChild(AnyTask task)
{
    co_await std::move(task);
}

/**
 * @brief Drives a whenAll/whenAny over void tasks.
 *
 * Both modes share the whole lifecycle: pre-check the token, build the shared
 * state, own and start every child, wake on completion or cancellation, then
 * rethrow the recorded outcome. Only the completion rule differs, so it is the
 * single parameter this driver takes.
 *
 * @param mode Completion rule: All waits for every child, Any for the first.
 * @param tasks Tasks to await; each element must be non-empty.
 * @param token Token checked before any child is started.
 * @return A task completing per mode, rethrowing the first child failure.
 */
[[nodiscard]]
Task<void> whenRace(WhenMode mode, std::vector<AnyTask> tasks, CancellationToken token)
{
    throwIfCancelled(token);

    const std::size_t count = tasks.size();
    if (count == 0)
    {
        co_return;
    }

    // Heap-allocated so the completion event outlives the composition frame:
    // done.set() resumes this coroutine to completion, and destroying the frame
    // (which owns the event) while set() runs would be a use-after-free. The
    // shared state stays alive through set() via thread-stack copies held by
    // the stop callback and by the completing child's FinalAwaiter.
    auto state = std::make_shared<WhenState>();
    state->mode      = mode;
    state->remaining = count;

    // Wakes the composition on cancellation; flag-only, no coroutine resume race.
    std::stop_callback cancellation{ token, [state]() noexcept {
        auto s = state; // Thread-stack copy keeps the state alive through set().
        s->cancelled = true;
        s->done.set();
    } };

    std::vector<WhenChild> children;
    children.reserve(count);
    for (auto& task : tasks)
    {
        children.push_back(composeChild(std::move(task)));
    }
    for (auto& child : children)
    {
        child.start(state);
    }

    co_await state->done;

    if (state->cancelled)
    {
        throw TaskCancelledException{};
    }
    if (state->first_exception)
    {
        std::rethrow_exception(state->first_exception);
    }
}

} // namespace detail

/**
 * @brief Awaits all tasks concurrently; completes when every task finishes.
 *
 * Every task is started immediately. If any task throws, the first exception
 * observed by completion order is rethrown after all tasks finish. If the
 * token is already cancelled, TaskCancelledException is thrown without
 * starting any task; cancellation while waiting abandons (destroys) the
 * not-yet-finished children rather than cancelling them cooperatively.
 * Structured concurrency: the children are owned by the composition, so
 * destroying the returned task also destroys them. The container is consumed.
 *
 * @param tasks Tasks to await; each element must be non-empty.
 * @param token Optional cancellation token.
 * @return A task that completes when every input task completes.
 */
[[nodiscard]]
Task<void> whenAll(std::vector<AnyTask> tasks, CancellationToken token = {})
{
    // Forwarded, not wrapped in another coroutine: the driver is already lazy and
    // owns the children, so an extra frame would buy nothing.
    return detail::whenRace(detail::WhenMode::All, std::move(tasks), std::move(token));
}

/**
 * @brief Awaits until the first task completes; the rest are destroyed.
 *
 * Every task is started immediately. The returned task completes as soon as
 * one task finishes; the remaining tasks are then destroyed together with the
 * composition (structured concurrency). If the first task to finish threw,
 * that exception is rethrown. If the token is already cancelled,
 * TaskCancelledException is thrown without starting any task; cancellation
 * while waiting abandons (destroys) the children. The container is consumed.
 *
 * @param tasks Tasks to await; each element must be non-empty.
 * @param token Optional cancellation token.
 * @return A task that completes when the first input task completes.
 */
[[nodiscard]]
Task<void> whenAny(std::vector<AnyTask> tasks, CancellationToken token = {})
{
    return detail::whenRace(detail::WhenMode::Any, std::move(tasks), std::move(token));
}

namespace detail {

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
 */
template<typename T>
WhenChild composeChildResult(Task<T> task, std::optional<T>* slot)
{
    if constexpr (std::is_void_v<T>)
    {
        co_await std::move(task);
        co_return;
    }
    else
    {
        slot->emplace(co_await std::move(task));
        co_return;
    }
}

/**
 * @brief Implements the variadic whenAll.
 *
 * @tparam Ts Result types of the tasks (non-void).
 * @param token Cancellation token.
 * @param tasks Tasks to await.
 * @return A task producing the tuple of all results.
 */
template<typename... Ts>
[[nodiscard]]
Task<std::tuple<Ts...>> whenAllImpl(CancellationToken token, Task<Ts>... tasks)
{
    throwIfCancelled(token);

    auto state = std::make_shared<detail::WhenState>();
    state->mode      = detail::WhenMode::All;
    state->remaining = sizeof...(Ts);

    std::stop_callback cancellation{ token, [state]() noexcept {
        auto s = state; // Thread-stack copy keeps the state alive through set().
        s->cancelled = true;
        s->done.set();
    } };

    std::tuple<std::optional<Ts>...> results;
    auto children = std::apply(
        [&tasks...](auto&... slot) {
            return std::make_tuple(detail::composeChildResult(std::move(tasks), &slot)...);
        },
        results);

    std::apply([&state](auto&... child) { (child.start(state), ...); }, children);

    co_await state->done;

    if (state->cancelled)
    {
        throw TaskCancelledException{};
    }
    if (state->first_exception)
    {
        std::rethrow_exception(state->first_exception);
    }

    assert(std::apply([](const auto&... slot) { return (... && slot.has_value()); }, results));
    co_return std::apply([](auto&... slot) { return std::tuple<Ts...>{ std::move(*slot)... }; }, results);
}

/**
 * @brief Shared completion state of a result-returning whenAny.
 */
template<typename T>
struct WhenAnyState
{
    std::atomic<bool>  first_done{ false };
    std::atomic<bool>  cancelled{ false };
    std::exception_ptr exception{};
    std::optional<T>   result{};
    AsyncEvent         done;
};

/**
 * @brief A child of a result-returning whenAny; publishes its own outcome.
 *
 * Only the first child to finish publishes (its result or its exception) into
 * the shared state; later children stay suspended until the composition
 * destroys them. The result is stored in the child's promise by return_value.
 */
template<typename T>
class WhenAnyChild
{
  public:
    struct promise_type
    {
        std::shared_ptr<WhenAnyState<T>> state{};
        std::exception_ptr exception{};
        std::optional<T> result{};

        [[nodiscard]]
        WhenAnyChild get_return_object() noexcept
        {
            return WhenAnyChild{ std::coroutine_handle<promise_type>::from_promise(*this) };
        }

        std::suspend_always initial_suspend() noexcept { return {}; }

        struct FinalAwaiter
        {
            [[nodiscard]]
            bool await_ready() const noexcept
            {
                return false;
            }

            std::coroutine_handle<> await_suspend(std::coroutine_handle<promise_type> h) noexcept
            {
                auto& p = h.promise();
                // Thread-stack copy keeps the state alive through done.set().
                auto  state = p.state;

                // Only the first child to finish publishes its outcome.
                if (!state->first_done.exchange(true))
                {
                    if (p.exception)
                    {
                        state->exception = std::move(p.exception);
                    }
                    else
                    {
                        assert(p.result.has_value());
                        state->result.emplace(std::move(p.result).value());
                    }
                    state->done.set();
                }

                // Stay suspended; the composition destroys this frame.
                return std::noop_coroutine();
            }

            void await_resume() const noexcept {}
        };

        FinalAwaiter final_suspend() noexcept { return {}; }

        void return_value(T value)
        {
            result.emplace(std::move(value));
        }

        void unhandled_exception() noexcept { exception = std::current_exception(); }
    };

    using handle_type = std::coroutine_handle<promise_type>;

    WhenAnyChild() noexcept = default;

    explicit WhenAnyChild(handle_type h) noexcept : handle_(h) {}

    WhenAnyChild(WhenAnyChild&& other) noexcept : handle_(std::exchange(other.handle_, {})) {}

    WhenAnyChild& operator=(WhenAnyChild&& other) noexcept
    {
        if (this != &other)
        {
            if (handle_)
            {
                handle_.destroy();
            }
            handle_ = std::exchange(other.handle_, {});
        }
        return *this;
    }

    ~WhenAnyChild()
    {
        if (handle_)
        {
            handle_.destroy();
        }
    }

    WhenAnyChild(const WhenAnyChild&) = delete;
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
 */
template<typename T>
WhenAnyChild<T> composeAnyChild(Task<T> task)
{
    if constexpr (std::is_void_v<T>)
    {
        co_await std::move(task);
        co_return;
    }
    else
    {
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
template<typename... Ts>
    requires (sizeof...(Ts) > 0) && (std::is_void_v<Ts> && ...)
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
 * returned task produces a std::tuple holding every result in argument order.
 * If any task throws, the first exception observed by completion order is
 * rethrown after all tasks finish; if the token is already cancelled,
 * TaskCancelledException is thrown without starting any task, and cancellation
 * while waiting abandons (destroys) the children. Structured concurrency:
 * children are owned by the composition. Tasks must be non-void; when every
 * task is a Task<void> the void overload below is selected instead.
 *
 * @tparam Ts Result types of the tasks.
 * @param tasks Tasks to await; each must be non-empty.
 * @param token Optional cancellation token; when supplied it is the first
 *        argument.
 * @return A task producing the tuple of all results.
 */
template<typename... Ts>
    requires (sizeof...(Ts) > 0) && (std::conjunction_v<std::negation<std::is_void<Ts>>...>)
[[nodiscard]]
Task<std::tuple<Ts...>> whenAll(Task<Ts>... tasks)
{
    co_return co_await detail::whenAllImpl(CancellationToken{}, std::move(tasks)...);
}

template<typename... Ts>
    requires (sizeof...(Ts) > 0) && (std::conjunction_v<std::negation<std::is_void<Ts>>...>)
[[nodiscard]]
Task<std::tuple<Ts...>> whenAll(CancellationToken token, Task<Ts>... tasks)
{
    co_return co_await detail::whenAllImpl(std::move(token), std::move(tasks)...);
}

/**
 * @brief Awaits void tasks concurrently; variadic form.
 *
 * The void counterpart of the tuple form above and the direct form of
 * whenAll(std::vector<AnyTask>): pass Task<void> values without building a
 * container. Semantics match the container form - every task is started, the
 * first failure by completion order is rethrown after all tasks finish, and
 * an already-cancelled token throws without starting any task.
 *
 * @tparam Ts Task types; every one of them must be void.
 * @param tasks Tasks to await; each must be non-empty.
 * @param token Optional cancellation token; when supplied it is the first
 *        argument.
 * @return A task completing when every input task completes.
 */
template<typename... Ts>
    requires (sizeof...(Ts) > 0) && (std::is_void_v<Ts> && ...)
[[nodiscard]]
Task<void> whenAll(Task<Ts>... tasks)
{
    co_await detail::whenRace(detail::WhenMode::All,
                              detail::toAnyTasks(std::move(tasks)...),
                              CancellationToken{});
}

template<typename... Ts>
    requires (sizeof...(Ts) > 0) && (std::is_void_v<Ts> && ...)
[[nodiscard]]
Task<void> whenAll(CancellationToken token, Task<Ts>... tasks)
{
    co_await detail::whenRace(
        detail::WhenMode::All, detail::toAnyTasks(std::move(tasks)...), std::move(token));
}

/**
 * @brief Awaits same-typed tasks concurrently and returns all results.
 *
 * Equivalent to whenAll(std::vector<AnyTask>) but preserves each result. The
 * returned task produces a std::vector with results in input order. If any
 * task throws, the first exception observed by completion order is rethrown
 * after all tasks finish; if the token is already cancelled, TaskCancelledException
 * is thrown without starting any task, and cancellation while waiting abandons
 * (destroys) the not-yet-finished children. The container is consumed.
 *
 * @tparam T Result type of the tasks (non-void).
 * @param tasks Tasks to await; each must be non-empty.
 * @param token Optional cancellation token.
 * @return A task producing the vector of all results.
 */
template<typename T>
    requires (!std::is_void_v<T>)
[[nodiscard]]
Task<std::vector<T>> whenAll(std::vector<Task<T>> tasks, CancellationToken token = {})
{
    throwIfCancelled(token);

    const std::size_t count = tasks.size();
    if (count == 0)
    {
        co_return {};
    }

    auto state = std::make_shared<detail::WhenState>();
    state->mode      = detail::WhenMode::All;
    state->remaining = count;

    std::stop_callback cancellation{ token, [state]() noexcept {
        auto s = state; // Thread-stack copy keeps the state alive through set().
        s->cancelled = true;
        s->done.set();
    } };

    std::vector<std::optional<T>> results(count);
    std::vector<detail::WhenChild> children;
    children.reserve(count);
    for (std::size_t i = 0; i < count; ++i)
    {
        children.push_back(detail::composeChildResult(std::move(tasks[i]), &results[i]));
    }
    for (auto& child : children)
    {
        child.start(state);
    }

    co_await state->done;

    if (state->cancelled)
    {
        throw TaskCancelledException{};
    }
    if (state->first_exception)
    {
        std::rethrow_exception(state->first_exception);
    }

    std::vector<T> out;
    out.reserve(count);
    for (auto& slot : results)
    {
        out.push_back(std::move(*slot));
    }
    co_return out;
}

/**
 * @brief Awaits same-typed tasks and returns the first result.
 *
 * Every task is started immediately; the returned task completes with the
 * result of whichever task finishes first. If that first task threw, its
 * exception is rethrown; the remaining tasks are destroyed together with the
 * composition (structured concurrency). If the token is already cancelled,
 * TaskCancelledException is thrown without starting any task; cancellation
 * while waiting abandons (destroys) the children. The container is consumed.
 *
 * @tparam T Result type of the tasks (non-void).
 * @param tasks Tasks to await; must not be empty.
 * @param token Optional cancellation token.
 * @return A task producing the first task's result.
 */
template<typename T>
    requires (!std::is_void_v<T>)
[[nodiscard]]
Task<T> whenAny(std::vector<Task<T>> tasks, CancellationToken token = {})
{
    throwIfCancelled(token);

    const std::size_t count = tasks.size();
    if (count == 0)
    {
        throw std::invalid_argument("async::whenAny: empty task list");
    }

    auto state = std::make_shared<detail::WhenAnyState<T>>();

    std::stop_callback cancellation{ token, [state]() noexcept {
        auto s = state; // Thread-stack copy keeps the state alive through set().
        s->cancelled = true;
        s->done.set();
    } };

    std::vector<detail::WhenAnyChild<T>> children;
    children.reserve(count);
    for (auto& task : tasks)
    {
        children.push_back(detail::composeAnyChild(std::move(task)));
    }
    for (auto& child : children)
    {
        child.start(state);
    }

    co_await state->done;

    if (state->cancelled)
    {
        throw TaskCancelledException{};
    }
    if (state->exception)
    {
        std::rethrow_exception(state->exception);
    }

    co_return std::move(state->result).value();
}

/**
 * @brief Awaits same-typed tasks and returns the first result; variadic form.
 *
 * Shorthand for whenAny(std::vector<Task<T>>): pass tasks directly without
 * building a container. All tasks must share the same non-void result type.
 * Semantics match the vector form: the returned task completes with the first
 * finisher's result (or rethrows its exception); the remaining tasks are
 * destroyed with the composition. An already-cancelled token throws
 * TaskCancelledException without starting any task.
 *
 * @tparam T Result type shared by every task.
 * @tparam Ts Remaining task types (same as T).
 * @param first First task.
 * @param rest Remaining tasks.
 * @param token Optional cancellation token; when supplied it is the first
 *        argument.
 * @return A task producing the first task's result.
 */
template<typename T, typename... Ts>
    requires (!std::is_void_v<T>) && (std::is_same_v<T, Ts> && ...)
[[nodiscard]]
Task<T> whenAny(Task<T> first, Task<Ts>... rest)
{
    std::vector<Task<T>> tasks;
    tasks.reserve(1 + sizeof...(Ts));
    tasks.push_back(std::move(first));
    (tasks.push_back(std::move(rest)), ...);
    co_return co_await whenAny(std::move(tasks));
}

template<typename T, typename... Ts>
    requires (!std::is_void_v<T>) && (std::is_same_v<T, Ts> && ...)
[[nodiscard]]
Task<T> whenAny(CancellationToken token, Task<T> first, Task<Ts>... rest)
{
    throwIfCancelled(token);

    std::vector<Task<T>> tasks;
    tasks.reserve(1 + sizeof...(Ts));
    tasks.push_back(std::move(first));
    (tasks.push_back(std::move(rest)), ...);
    co_return co_await whenAny(std::move(tasks), std::move(token));
}

/**
 * @brief Awaits void tasks and completes with the first of them; variadic form.
 *
 * The void counterpart of the form above and the direct form of
 * whenAny(std::vector<AnyTask>): pass Task<void> values without building a
 * container. Semantics match the container form - the returned task completes
 * when the first task finishes, the remaining tasks are destroyed with the
 * composition, and an already-cancelled token throws without starting any task.
 *
 * @tparam Ts Task types; every one of them must be void.
 * @param tasks Tasks to await; each must be non-empty.
 * @param token Optional cancellation token; when supplied it is the first
 *        argument.
 * @return A task completing when the first input task completes.
 */
template<typename... Ts>
    requires (sizeof...(Ts) > 0) && (std::is_void_v<Ts> && ...)
[[nodiscard]]
Task<void> whenAny(Task<Ts>... tasks)
{
    co_await detail::whenRace(detail::WhenMode::Any,
                              detail::toAnyTasks(std::move(tasks)...),
                              CancellationToken{});
}

template<typename... Ts>
    requires (sizeof...(Ts) > 0) && (std::is_void_v<Ts> && ...)
[[nodiscard]]
Task<void> whenAny(CancellationToken token, Task<Ts>... tasks)
{
    co_await detail::whenRace(
        detail::WhenMode::Any, detail::toAnyTasks(std::move(tasks)...), std::move(token));
}

V_ASYNC_NS_END
