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
 * A stopped pool refuses work instead of dropping it silently, and the refusal surfaces WHERE the request is made: one
 * entry point, one rule - enqueue() throws std::logic_error. A caller with no result to collect discards the returned
 * future (the callable still runs); a caller that must survive a refusal catches it.
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
     * @brief Queues a callable for execution on a worker thread, handing back a future for its result.
     *
     * @details
     * - The pool's only entry point: a caller with no result to collect discards the returned future, and the callable
     *   still runs.
     * - The callable and its arguments are copied into a std::packaged_task, so a move-only argument is passed with
     *   std::move at the call.
     * - A refusal is an exception here, not something the future carries: a caller holding a future holds a task that
     *   will run, so nothing has to be decoded from a broken_promise later.
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

    /// The queue push enqueue() reports a refusal from; true when the pool took the task.
    bool pushTask(std::function<void()> task);

    std::unique_ptr<Impl> d;
};

template <typename F, typename... Args>
auto ThreadPool::enqueue(F&& f, Args&&... args) -> std::future<std::invoke_result_t<F, Args...>>
{
    using ResultType = std::invoke_result_t<F, Args...>;
    auto task        = std::make_shared<std::packaged_task<ResultType()>>(std::bind(std::forward<F>(f), std::forward<Args>(args)...));
    auto result      = task->get_future();
    // Refused: the caller is handed no future at all, so the refusal cannot be left for get() to find.
    if (!pushTask([task]() { (*task)(); })) {
        throw std::logic_error("vn::ThreadPool: the pool is stopped");
    }
    return result;
}

VN_CORE_NS_END
