#pragma once
#include "core_global.hpp"

#include <cstddef>
#include <functional>
#include <future>
#include <memory>
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
 * A stopped pool refuses work instead of dropping it silently: enqueue() reports the refusal
 * through its future (broken_promise) and tryEnqueue() reports it directly. A caller with nothing
 * to collect must use tryEnqueue() - its future-less form has nothing else to report it with.
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
     * @brief Queues a callable for execution on a worker thread.
     *
     * @details
     * - A pool that is stopped (see stop()) refuses the task: it is never run, and the returned
     *   future reports std::future_error with broken_promise rather than blocking forever.
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
     * - The form for a caller with no result to collect: a scheduler hop, an event post.
     * - Returns false when the pool is stopped. The caller decides what that means; silently
     *   dropping the request is the one answer this API does not give.
     *
     * @param task Callable to run on a worker thread; never run when the pool refuses it.
     * @return true when the callable was queued, false when the pool is stopped.
     */
    [[nodiscard]]
    bool tryEnqueue(std::function<void()> task);

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
    std::unique_ptr<Impl> d;
};

template <typename F, typename... Args>
auto ThreadPool::enqueue(F&& f, Args&&... args) -> std::future<std::invoke_result_t<F, Args...>>
{
    using ResultType = std::invoke_result_t<F, Args...>;
    auto task        = std::make_shared<std::packaged_task<ResultType()>>(std::bind(std::forward<F>(f), std::forward<Args>(args)...));
    auto result      = task->get_future();
    // A refused task is abandoned right here: the packaged_task loses its last reference and leaves
    // broken_promise in the shared state, so result.get() throws instead of waiting forever.
    (void)tryEnqueue([task]() { (*task)(); });
    return result;
}

VN_CORE_NS_END
