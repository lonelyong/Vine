#pragma once

#include "async_global.hpp"

#include <coroutine>
#include <exception>
#include <optional>
#include <stdexcept>
#include <type_traits>
#include <utility>

#include "Concepts.hpp"

V_ASYNC_NS_BEGIN

/**
 * @brief Lazy, move-only asynchronous operation producing a value (or void).
 *
 * The result type must be storable (see StorableValue): references, arrays and
 * function types are rejected here rather than inside std::optional.
 *
 * @tparam T Result type of the asynchronous operation; void for no result.
 */
template<StorableValue T>
class Task;

namespace detail {

/**
 * @brief State shared by every Task promise: continuation and exception.
 *
 * continuation links this task to the coroutine that awaits it: awaiting a
 * Task stores the waiter's handle here, symmetric-transfers into the task
 * body, and TaskFinalAwaiter resumes continuation when the task completes.
 * This forms the await chain that unwinds control back to the caller on
 * suspension.
 */
struct TaskPromiseBase
{
    /// Non-owning handle of the coroutine awaiting this task; resumed on completion.
    std::coroutine_handle<> continuation{};

    /// Exception captured by unhandled_exception, rethrown from await_resume.
    std::exception_ptr exception{};
};

/**
 * @brief Promise mix-in that turns co_return into the task's result.
 *
 * Holds the result slot for non-void tasks and provides return_void for void
 * tasks, so a promise type can inherit one mix-in regardless of T. The slot is
 * read by the Task's awaiter (and by syncWait's helper promise) after the body
 * has finished.
 */
template<typename T>
struct TaskPromiseReturn
{
    /// Result slot; empty until the body co_returns.
    std::optional<T> value{};

    void return_value(T value)
    {
        this->value.emplace(std::move(value));
    }
};

template<>
struct TaskPromiseReturn<void>
{
    void return_void() noexcept {}
};

/**
 * @brief The final suspend point's control-transfer rule: jump to the waiter.
 *
 * final_suspend() returns this awaiter, so co_await on it decides where
 * control goes after co_return. The key is await_suspend's return type — it
 * returns a std::coroutine_handle<>, which is the "transfer rule" that tells
 * the compiler "symmetric-transfer to this coroutine" (as opposed to void,
 * which just hands control back to the caller). That is exactly how "the
 *   child finishes and wakes up the parent": control jumps to continuation.
 *
 * await_ready() is always false, so the coroutine always suspends once at
 * completion and never resumes into a destroyed frame. If nobody awaited the
 * task, continuation is null and it transfers to std::noop_coroutine(); the
 * frame is then freed by handle_.destroy().
 */
template<typename Promise>
struct TaskFinalAwaiter
{
    [[nodiscard]]
    bool await_ready() const noexcept
    {
        return false; // Always suspend: never resume into a destroyed frame.
    }

    /**
     * @brief The child finishes here and wakes up whoever awaited it.
     *
     * Called at co_return. It reads continuation — the parent coroutine
     * that Awaiter::await_suspend registered earlier — and symmetric-
     * transfers back to it: "I'm done, go resume the parent that is
     * waiting for my result." If nobody awaited the task, it transfers to
     * std::noop_coroutine() instead.
     */
    std::coroutine_handle<> await_suspend(std::coroutine_handle<Promise> h) noexcept
    {
        auto continuation = h.promise().continuation;
        return continuation ? continuation : std::noop_coroutine();
    }

    void await_resume() const noexcept {}
};

} // namespace detail

/**
 * @brief Lazy, move-only, single-consumer asynchronous operation.
 *
 * A Task wraps the compiler-generated coroutine frame of a co_await/co_return
 * function and exposes it as an awaitable handle. The body does not run until
 * the task is awaited or passed to syncWait: initial_suspend() returns
 * suspend_always, so control returns to the caller immediately.
 *
 * Execution flow:
 * 1. The coroutine function allocates its frame and returns this Task.
 * 2. co_await on the Task symmetric-transfers into the body (Awaiter).
 * 3. The body runs until it co_awaits something that suspends, unwinding the
 *    current thread back to its caller.
 * 4. When resumed, execution continues right after the suspension point.
 * 5. At co_return the final awaiter resumes the awaiting coroutine and the
 *    frame is destroyed.
 *
 * A task can be awaited exactly once, after which it is empty. Destroying an
 * un-awaited task destroys the frame without running the body.
 *
 * @tparam T Result type of the asynchronous operation; void for no result.
 */
template<StorableValue T>
class Task
{
  public:
    /**
     * @brief The coroutine's control room, living inside the frame.
     *
     * The compiler requires a coroutine's return-object type to expose a
     * nested `promise_type`. This is the one object that actually lives
     * inside the coroutine frame, and every lifecycle hook the compiler
     * calls goes through it:
     * - get_return_object(): hands the caller the Task that reaches the frame.
     * - initial_suspend(): decides whether the body runs immediately.
     * - final_suspend(): decides what happens when the body co_returns.
     * - unhandled_exception(): catches whatever the body throws.
     * It also inherits the shared state (continuation / exception / value)
     * from TaskPromiseBase and TaskPromiseReturn, so the Task's awaiter and
     * final awaiter can find everything in one place.
     */
    struct promise_type : detail::TaskPromiseBase,
                          detail::TaskPromiseReturn<T>
    {
        /**
         * @brief Hands back the Task the caller receives.
         *
         * Called once, when the coroutine is first created. from_promise()
         * turns the promise's own address into a coroutine_handle, so the
         * returned Task can reach the frame with no extra allocation.
         */
        [[nodiscard]]
        Task<T> get_return_object() noexcept
        {
            return Task<T>{ std::coroutine_handle<promise_type>::from_promise(*this) };
        }

        /**
         * @brief Suspends before the first line: calling the coroutine does
         * not run its body.
         *
         * Example: calling a coroutine function only allocates the frame and
         * stops here, so the body has not executed a single line yet:
         *
         *     Task<int> t = compute();   // stops here; compute's body has not run
         *     int x = co_await t;        // the body starts running now
         *
         * This is what makes a Task lazy: calling the function just "places
         * the order", the work starts only when someone awaits the task. Here
         * we only need "suspend and return", with nobody to resume, so the
         * standard std::suspend_always (whose await_ready() is always false)
         * is enough.
         *
         * @return An awaiter that always suspends first, deferring the body
         * until the task is awaited.
         */
        std::suspend_always initial_suspend() noexcept { return {}; }

        /**
         * @brief Suspends at final_suspend and transfers control to the
         * awaiting coroutine.
         *
         * Example:
         *
         *     Task<int> compute()
         *     {
         *         co_return 42;   // stores the result, then reaches final_suspend()
         *     }
         *
         *     Task<int> caller()
         *     {
         *         int x = co_await compute();   // resumes here with x == 42
         *         co_return x;
         *     }
         *
         * When the coroutine reaches final_suspend, its result is already
         * stored in the promise. The coroutine frame must remain alive until
         * the awaiting coroutine has resumed and collected the result.
         *
         * TaskFinalAwaiter::await_suspend() performs a symmetric transfer to
         * the awaiting coroutine stored in continuation. The parent then
         * resumes at the suspension point of co_await and obtains the result
         * through await_resume().
         *
         * A custom final awaiter is required because await_suspend() may
         * return:
         *
         * - void: suspend the current coroutine and return control to its
         *   caller;
         * - bool: suspend or continue based on the returned value;
         * - std::coroutine_handle<>: suspend and transfer control directly to
         *   another coroutine.
         *
         * std::suspend_always uses the void form, so it cannot perform the
         * required symmetric transfer to continuation. Task therefore uses a
         * custom TaskFinalAwaiter.
         *
         * @return An awaiter that suspends the completed coroutine and
         * transfers control to its awaiting coroutine.
         */
        detail::TaskFinalAwaiter<promise_type> final_suspend() noexcept { return {}; }

        /// If the body throws, stash the exception so await_resume can rethrow it.
        void unhandled_exception() noexcept { exception = std::current_exception(); }
    };

    using handle_type = std::coroutine_handle<promise_type>;

    /// Result type of the task; useful for generic combinators.
    using value_type = T;

  public:
    Task() noexcept = default;

    explicit Task(handle_type h) noexcept : handle_(h) {}

    Task(Task&& other) noexcept : handle_(std::exchange(other.handle_, {})) {}

    Task& operator=(Task&& other) noexcept
    {
        if (this != &other)
        {
            destroy();
            handle_ = std::exchange(other.handle_, {});
        }
        return *this;
    }

    ~Task()
    {
        destroy();
    }

    Task(const Task&) = delete;
    Task& operator=(const Task&) = delete;

    /**
     * @brief Returns whether the task owns a coroutine frame.
     *
     * @return true if the task is non-empty.
     */
    [[nodiscard]]
    explicit operator bool() const noexcept
    {
        return static_cast<bool>(handle_);
    }

  public:
    /**
     * @brief Awaitable that starts the task and yields its result.
     *
     * await_suspend performs symmetric transfer: it records the waiting
     * coroutine as this task's continuation and returns this task's handle, so
     * the thread jumps straight into the task body instead of first returning
     * to the waiting coroutine. await_resume yields the stored value or
     * rethrows the exception captured during execution.
     */
    class Awaiter
    {
      public:
        /// Takes ownership of the task's frame handle (moved in by operator co_await).
        explicit Awaiter(handle_type h) noexcept : handle_(h) {}

        Awaiter(const Awaiter&) = delete;
        Awaiter& operator=(const Awaiter&) = delete;

        /// Transfers the frame handle, leaving the source awaiter empty.
        Awaiter(Awaiter&& other) noexcept : handle_(std::exchange(other.handle_, {})) {}
        Awaiter& operator=(Awaiter&&) = delete;

        /**
         * @brief Destroys the frame if the task never completed.
         *
         * Runs when the awaiting coroutine is destroyed while this task is
         * still suspended (the await is abandoned), so destroying the frame
         * cancels the task. After a normal completion await_resume has already
         * destroyed the frame and this is a no-op.
         */
        ~Awaiter()
        {
            destroy();
        }

        /**
         * @brief Called first by the compiler at `co_await`.
         *
         * @return true when the task has already finished, so the coroutine
         * skips suspension and proceeds straight to await_resume().
         * @return false when the task is still running, so the coroutine
         * suspends and starts the task body via await_suspend().
         */
        [[nodiscard]]
        bool await_ready() const noexcept
        {
            return !handle_ || handle_.done();
        }

        /**
         * @brief The parent registers itself as the "wake-up" target, then
         * jumps straight into the child.
         *
         * Called by the compiler right after await_ready() returned false,
         * when the parent coroutine reaches this co_await. The parent is
         * about to suspend, so it does two things:
         * 1. Saves its own handle as the task's continuation — "I'm going
         *    to sleep, wake me up when you're done" — so the task knows
         *    whom to resume on completion.
         * 2. Returns the task's handle for symmetric transfer, so the thread
         *    jumps straight into the task body instead of first returning to
         *    the parent.
         *
         * @param waiter The parent coroutine awaiting this task; stored as
         * the continuation the task resumes on completion.
         * @return The task's handle, transferred to instead of returning.
         */
        std::coroutine_handle<> await_suspend(std::coroutine_handle<> waiter) noexcept
        {
            // Save the parent's handle: "I'm sleeping, wake me up when done."
            handle_.promise().continuation = waiter;
            // Jump straight into the child body instead of returning to the parent.
            return handle_;
        }

        /**
         * @brief The parent wakes up here to collect the child's result.
         *
         * Called by the compiler inside the parent's own resume function, so
         * no waiter handle is needed — the parent is already running. Its
         * return value becomes the value of `co_await task`: the stored
         * result on success, or a rethrown exception on failure. It then
         * destroys the child's frame.
         *
         * @return The value of `co_await task` (or void for Task<void>).
         */
        decltype(auto) await_resume()
        {
            if (!handle_)
            {
                throw std::logic_error("async::Task: awaiting an empty task");
            }

            auto& promise = handle_.promise();
            if (promise.exception)
            {
                std::exception_ptr ex = std::move(promise.exception);
                destroy();
                std::rethrow_exception(ex);
            }

            if constexpr (!std::is_void_v<T>)
            {
                T value = std::move(promise.value).value();
                destroy();
                return value;
            }
            else
            {
                destroy();
            }
        }

      private:
        /// Destroys the owned frame, leaving the awaiter empty.
        void destroy() noexcept
        {
            if (handle_)
            {
                handle_.destroy();
                handle_ = {};
            }
        }

        /// The task's coroutine frame; owned until completion or abandonment.
        handle_type handle_{};
    };

    /**
     * @brief Begins awaiting this task; the task must be an rvalue.
     *
     * Transfers exclusive ownership of the coroutine frame to the returned
     * Awaiter. The rvalue-only overload moves the handle out of the task,
     * enforcing the single-consumer rule: after co_await the task is empty and
     * cannot be awaited again.
     *
     * @return An awaiter that runs the task and yields its result.
     */
    [[nodiscard]]
    Awaiter operator co_await() && noexcept
    {
        // std::exchange returns the old handle and resets handle_ to null:
        // the Awaiter takes sole ownership, so this Task no longer owns the
        // frame (avoids a second destroy) and cannot be awaited again.
        return Awaiter{ std::exchange(handle_, {}) };
    }

  private:
    /// Destroys the owned frame, leaving the task empty.
    void destroy() noexcept
    {
        if (handle_)
        {
            handle_.destroy();
            handle_ = {};
        }
    }

    /// Owns the coroutine frame.
    handle_type handle_{};
};

/**
 * @brief Task with no result value; the C#-style non-generic Task.
 *
 * Alias of Task<void>; use it as the common container element so any Task<T>
 * can be stored after its result is discarded with discard().
 */
using AnyTask = Task<void>;

/**
 * @brief Converts any task to a Task<void>, discarding its result.
 *
 * The returned task runs the source task to completion; the result is
 * discarded while exceptions still propagate. This lets heterogeneous tasks
 * be stored together, for example in a std::vector<Task<void>>.
 *
 * @tparam T Result type of the source task.
 * @param task Task to run; its result is ignored.
 * @return A task that completes when task completes.
 */
template<StorableValue T>
[[nodiscard]]
Task<void> discard(Task<T> task)
{
    co_await std::move(task);
}

V_ASYNC_NS_END
