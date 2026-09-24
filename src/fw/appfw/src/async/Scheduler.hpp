#pragma once

#include <coroutine>

#include <QObject>

#include <vine/appfw/appfw_global.hpp>

namespace vn {
namespace appfw {
namespace async {

/**
 * @brief Scheduler that resumes coroutines on the thread of this QObject.
 *
 * Create the scheduler on the thread whose event loop should drive the
 * coroutines. The scheduler must outlive every coroutine scheduled on it, and
 * the owning thread must run an event loop.
 */
class VN_APPFW_API Scheduler final : public QObject
{
  public:
    explicit Scheduler(QObject* parent = nullptr);

    ~Scheduler() override;

    class ScheduleAwaiter
    {
      public:
        explicit ScheduleAwaiter(Scheduler* scheduler) noexcept : scheduler_(scheduler) {}

        [[nodiscard]]
        bool await_ready() const noexcept
        {
            return false;
        }

        void await_suspend(std::coroutine_handle<> h) noexcept
        {
            scheduler_->postResume(h);
        }

        void await_resume() const noexcept {}

      private:
        Scheduler* scheduler_;
    };

    [[nodiscard]]
    ScheduleAwaiter schedule() noexcept
    {
        return ScheduleAwaiter{ this };
    }

  private:
    void postResume(std::coroutine_handle<> h);
};

} // namespace async
} // namespace appfw
} // namespace vine
