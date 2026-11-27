#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <stdexcept>
#include <thread>
#include <vector>

#include <vine/ThreadPool.hpp>

using vn::ThreadPool;

namespace
{

TEST(ThreadPoolTest, EnqueueReturnsResult)
{
    ThreadPool pool(2);
    auto       future = pool.enqueue([](int a, int b) { return a + b; }, 20, 22);
    EXPECT_EQ(future.get(), 42);
}

TEST(ThreadPoolTest, DiscardingTheFutureStillRunsTheTask)
{
    // A caller with no result to collect takes the same entry point: the future is dropped, the callable still runs.
    ThreadPool         pool(1);
    std::promise<void> done;
    auto               future = done.get_future();
    (void)pool.enqueue([&done] { done.set_value(); });
    EXPECT_EQ(future.wait_for(std::chrono::seconds(5)), std::future_status::ready);
}

TEST(ThreadPoolTest, EnqueueOnAStoppedPoolThrows)
{
    // The refusal is thrown where the request is made, so a caller holding a future holds a task that will run.
    ThreadPool pool(2);
    pool.stop();
    EXPECT_THROW(pool.enqueue([] { return 1; }), std::logic_error) << "the caller was handed a future for a task the pool will never run";
}

TEST(ThreadPoolTest, ExecutesAllTasks)
{
    ThreadPool                     pool(4);
    std::atomic<int>               counter{ 0 };
    constexpr int                  task_count = 100;
    std::vector<std::future<void>> futures;
    futures.reserve(task_count);
    for (int i = 0; i < task_count; ++i) {
        futures.push_back(pool.enqueue([&counter]() { counter.fetch_add(1, std::memory_order_relaxed); }));
    }
    for (auto& future : futures) {
        future.get();
    }
    EXPECT_EQ(counter.load(), task_count);
}

TEST(ThreadPoolTest, RunsTasksInParallel)
{
    ThreadPool                     pool(4);
    std::atomic<int>               active{ 0 };
    std::atomic<int>               peak{ 0 };
    constexpr int                  task_count = 8;
    std::vector<std::future<void>> futures;
    futures.reserve(task_count);
    for (int i = 0; i < task_count; ++i) {
        futures.push_back(pool.enqueue([&active, &peak]() {
            const int cur  = active.fetch_add(1) + 1;
            int       prev = peak.load();
            while (prev < cur && !peak.compare_exchange_weak(prev, cur)) {}
            std::this_thread::sleep_for(std::chrono::milliseconds(30));
            active.fetch_sub(1);
        }));
    }
    for (auto& future : futures) {
        future.get();
    }
    EXPECT_GT(peak.load(), 1);
}

TEST(ThreadPoolTest, ReportsThreadCount)
{
    ThreadPool pool(3);
    EXPECT_EQ(pool.threadCount(), 3u);
}

} // namespace
