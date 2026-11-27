#pragma once
#include "core_global.hpp"

#include <cstddef>
#include <functional>
#include <future>
#include <memory>
#include <stdexcept>
#include <type_traits>
#include <utility>

VN_CORE_NS_BEGIN

/**
 * @brief A fixed-size thread pool that executes queued tasks concurrently.
 *
 * Tasks are submitted with enqueue() and may return a result through a
 * std::future. The pool starts its workers in the constructor, stops and
 * drains them in stop() or the destructor, and can be restarted with start().
 * The pool is not copyable or movable.
 *
 * A stopped pool refuses work instead of dropping it silently, and the refusal surfaces WHERE the request is made:
 * enqueue() throws std::logic_error, tryEnqueue() returns false. Neither defers it to the result, so a task either runs
 * or the caller learns at once that it will not. tryEnqueue() is the form for a caller with no result to collect (a
 * scheduler hop, an event post); enqueue() is the same call that also hands back a future.
 *
 * Qt-free; relies only on the C++ standard library.
 */
class VN_CORE_API ThreadPool {

  public:
    /**
     * @brief Creates a thread pool sized to the hardware concurrency.
     */
    ThreadPool();

    /**
     * @brief Creates a thread pool with the given number of worker threads.
     *
     * @param thread_count Number of worker threads; clamped to at least 1.
     */
    explicit ThreadPool(std::size_t thread_count);

    /**
     * @brief Destroys the pool, stops accepting tasks and joins all workers.
     */
    ~ThreadPool();

    ThreadPool(const ThreadPool&)            = delete;
    ThreadPool& operator=(const ThreadPool&) = delete;
    ThreadPool(ThreadPool&&)                 = delete;
    ThreadPool& operator=(ThreadPool&&)      = delete;

  public:
    /**
     * @brief Queues a callable, handing back a future for its result.
     *
     * @details
     * - The throwing sibling of tryEnqueue(): the same callable, the same queue and the same refusal rule - the
     *   difference is that this form hands back the result, and reports a refusal as an exception rather than a bool.
     * - The callable and its arguments are copied into a std::packaged_task, so a move-only argument is passed with
     *   std::move at the call.
     *
     * @throws std::logic_error when the pool is stopped (see stop()); the callable never runs, and no future is handed
     * back to read a deferred answer from.
     * @throws std::bad_alloc when the task cannot be allocated, and whatever copying the callable and its arguments
     * throws.
     *
     * @tparam F Callable type.
     * @tparam Args Argument types.
     * @param f Callable to invoke on a worker thread.
     * @param args Arguments forwarded to f.
     * @return A future that yields the result of invoking f.
     */
    template <typename F, typename... Args>
    auto enqueue(F&& f, Args&&... args) -> std::future<std::invoke_result_t<F, Args...>>;

    /**
     * @brief Queues a fire-and-forget callable, reporting whether the pool took it.
     *
     * @details
     * - The form for a caller with no result to collect: a scheduler hop, an event post. It builds no packaged_task and
     *   returns no future, so "did it get in" is answered by the return value rather than by something to collect.
     * - The try* sibling of enqueue() in the sense the rest of the module uses: false where enqueue() throws
     *   std::logic_error, for the same pool state (see TaskCompletionSource::setResult / trySetResult).
     * - Returns false when the pool is stopped. The caller decides what that means; silently dropping the request is
     *   the one answer this API does not give.
     *
     * @note
     * - try* promises "a refusal is the return value", not "nothing throws": copying the callable into the queue can
     *   still throw std::bad_alloc, exactly as enqueue() can.
     *
     * @tparam F Callable type; invoked with no arguments.
     * @param task Callable to run on a worker thread; never run when the pool refuses it.
     * @return true when the callable was queued, false when the pool is stopped.
     */
    template <typename F>
    [[nodiscard]]
    bool tryEnqueue(F&& task);

    /**
     * @brief Returns the number of worker threads.
     *
     * @return The worker thread count.
     */
    std::size_t threadCount() const;

    /**
     * @brief Starts the worker threads if the pool is stopped.
     */
    void start();

    /**
     * @brief Stops accepting tasks, drains the queue and joins all workers.
     */
    void stop();

    /**
     * @brief Returns the process-wide default thread pool.
     *
     * @return The default pool singleton.
     */
    static ThreadPool& defaultPool();

  private:
    struct Impl;

    /// The one type-erased entry point both public forms funnel through.
    bool pushTask(std::function<void()> task);

    std::unique_ptr<Impl> d;
};

template <typename F>
bool ThreadPool::tryEnqueue(F&& task)
{
    static_assert(std::is_invocable_v<std::decay_t<F>&>, "ThreadPool::tryEnqueue() takes a callable with no arguments");
    return pushTask(std::function<void()>(std::forward<F>(task)));
}

template <typename F, typename... Args>
auto ThreadPool::enqueue(F&& f, Args&&... args) -> std::future<std::invoke_result_t<F, Args...>>
{
    using ResultType = std::invoke_result_t<F, Args...>;
    auto task        = std::make_shared<std::packaged_task<ResultType()>>(std::bind(std::forward<F>(f), std::forward<Args>(args)...));
    auto result      = task->get_future();
    // Refused: the caller is handed no future at all, so the refusal cannot be left for get() to find.
    if (!tryEnqueue([task]() { (*task)(); })) {
        throw std::logic_error("vn::ThreadPool: the pool is stopped");
    }
    return result;
}

VN_CORE_NS_END
