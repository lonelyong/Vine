#include <vine/ThreadPool.hpp>

#include <condition_variable>
#include <functional>
#include <mutex>
#include <queue>
#include <thread>
#include <utility>
#include <vector>

VN_CORE_NS_BEGIN

struct ThreadPool::Impl {
    std::vector<std::thread>          workers;
    std::queue<std::function<void()>> tasks;
    std::mutex                        mutex;
    std::condition_variable           cv;
    bool                              stop  = true;
    std::size_t                       count = 1;
};

ThreadPool::ThreadPool()
  : ThreadPool(std::thread::hardware_concurrency())
{}

ThreadPool::ThreadPool(std::size_t thread_count)
  : d(new Impl)
{
    d->count = thread_count == 0 ? 1 : thread_count;
    start();
}

ThreadPool::~ThreadPool()
{
    stop();
}

ThreadPool& ThreadPool::defaultPool()
{
    static ThreadPool s_pool;
    return s_pool;
}

void ThreadPool::start()
{
    std::lock_guard<std::mutex> lock(d->mutex);

    if (!d->stop) {
        return;
    }

    d->workers.clear();
    d->stop = false;

    for (std::size_t i = 0; i < d->count; ++i) {
        d->workers.emplace_back([this]() {
            for (;;) {
                std::function<void()> task;
                {
                    std::unique_lock<std::mutex> lock(d->mutex);
                    d->cv.wait(lock, [this]() { return d->stop || !d->tasks.empty(); });
                    if (d->stop && d->tasks.empty()) {
                        return;
                    }
                    task = std::move(d->tasks.front());
                    d->tasks.pop();
                }
                task();
            }
        });
    }
}

void ThreadPool::stop()
{
    {
        std::lock_guard<std::mutex> lock(d->mutex);
        d->stop = true;
    }
    d->cv.notify_all();

    for (auto& worker : d->workers) {
        if (worker.joinable()) {
            worker.join();
        }
    }
    d->workers.clear();
}

void ThreadPool::enqueueTask(std::function<void()> task)
{
    {
        std::lock_guard<std::mutex> lock(d->mutex);
        if (d->stop) {
            return;
        }
        d->tasks.push(std::move(task));
    }
    d->cv.notify_one();
}

std::size_t ThreadPool::threadCount() const
{
    return d->count;
}

VN_CORE_NS_END
