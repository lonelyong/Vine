#include <gtest/gtest.h>

#include <atomic>
#include <memory>
#include <thread>
#include <utility>
#include <vector>

#include <vine/progress/ProgressIndicator.hpp>
#include <vine/progress/ProgressRange.hpp>
#include <vine/progress/ProgressScope.hpp>

using vn::progress::ProgressIndicator;
using vn::progress::ProgressRange;
using vn::progress::ProgressScope;

namespace
{

TEST(ProgressIndicatorTest, StartsAtZeroAndReportsThroughScope)
{
    ProgressIndicator indicator;
    EXPECT_DOUBLE_EQ(indicator.position(), 0.0);

    constexpr int n = 10;
    {
        ProgressScope scope(indicator.start(), "Exporting", n);
        // Half of the steps advance the indicator to the middle of the scale.
        for (int i = 0; i < 5; ++i) {
            scope.next(1);
        }
        EXPECT_NEAR(indicator.position(), 0.5, 1e-9);
    }

    // Completing the scope drives the indicator to the end of the scale.
    EXPECT_NEAR(indicator.position(), 1.0, 1e-9);
}

TEST(ProgressIndicatorTest, CancelTracksTheExternalToken)
{
    std::stop_source  source;
    ProgressIndicator indicator(source.get_token());

    ProgressScope scope(indicator.start(), "Cancel", 1);
    EXPECT_FALSE(scope.isCancelled());

    source.request_stop();
    EXPECT_TRUE(scope.isCancelled());
    EXPECT_TRUE(indicator.isCancelled());
    EXPECT_TRUE(indicator.token().stop_requested());
}

TEST(ProgressIndicatorTest, RangeIsActiveOnceOnly)
{
    ProgressIndicator indicator;

    ProgressRange r = indicator.start();
    EXPECT_TRUE(r.isActive());

    // A root feeds exactly one top-level scope: the scope consumes the move-only range.
    ProgressScope s1(std::move(r), "a", 10);
    EXPECT_TRUE(s1.isActive());
    EXPECT_FALSE(r.isActive()) << "the consumed range must be empty";

    // Reusing it cannot silently take the root a second time.
    ProgressScope s2(std::move(r), "b", 10);
    EXPECT_FALSE(s2.isActive());
}

TEST(ProgressIndicatorTest, MoveTransfersTheReportingObligation)
{
    ProgressIndicator indicator;

    ProgressScope  root = ProgressScope(indicator.start(), "root", 10);
    ProgressRange  a    = root.next(4);
    EXPECT_TRUE(a.isActive());

    ProgressRange b = std::move(a);
    EXPECT_FALSE(a.isActive()) << "the moved-from range must be empty";
    EXPECT_TRUE(b.isActive());

    b.complete();
    EXPECT_NEAR(indicator.position(), 0.4, 1e-9);
    EXPECT_FALSE(b.isActive());
}

TEST(ProgressIndicatorTest, MoveAssignmentCompletesThePendingPortionFirst)
{
    ProgressIndicator indicator;

    ProgressScope root   = ProgressScope(indicator.start(), "root", 10);
    ProgressRange first  = root.next(4); // 0.4, not reported yet
    ProgressRange second = root.next(2); // 0.2

    // Replacing a live range must report what it owed, not drop it.
    first = std::move(second);
    EXPECT_NEAR(indicator.position(), 0.4, 1e-9);
    EXPECT_FALSE(second.isActive());
    EXPECT_TRUE(first.isActive());

    first.complete();
    EXPECT_NEAR(indicator.position(), 0.6, 1e-9);
}

TEST(ProgressIndicatorTest, NestedScopesMapOntoTheParentRange)
{
    ProgressIndicator indicator;

    ProgressScope root = ProgressScope(indicator.start(), "whole", 10);
    ProgressRange part = root.next(4);
    ProgressScope stage(std::move(part), "stage", 4);
    EXPECT_TRUE(stage.isActive());

    stage.next(2); // 子阶段的一半 = 全局的 20%
    EXPECT_NEAR(indicator.position(), 0.2, 1e-9);

    stage.next(2); // 子阶段结束 = 全局的 40%
    EXPECT_NEAR(indicator.position(), 0.4, 1e-9);
}

TEST(ProgressIndicatorTest, PositionCallbackIsCoalescedToPercentSteps)
{
    ProgressIndicator indicator;

    int events = 0;
    indicator.setPositionCallback([&events] { ++events; });

    constexpr int kItems = 10'000;
    {
        ProgressScope scope(indicator.start(), "coalesce", kItems);
        for (int i = 0; i < kItems; ++i) {
            scope.next(1);
        }
    }

    // 一万次 increment 只该回调约 100 次（每个百分点一次）。
    EXPECT_GE(events, 100);
    EXPECT_LE(events, 103);
}

TEST(ProgressIndicatorTest, ReachingTheEndIsAlwaysAnnounced)
{
    ProgressIndicator indicator;

    int events = 0;
    indicator.setPositionCallback([&events] { ++events; });

    // 最后一步远小于一个百分点：到终点仍必须通知，否则进度条永远差最后一格。
    ProgressScope scope(indicator.start(), "tiny", 1000);
    for (int i = 0; i < 999; ++i) {
        scope.next(1);
    }
    const int before_end = events;
    scope.next(1); // position 到 1.0
    EXPECT_GT(events, before_end);
}

TEST(ProgressIndicatorTest, RestartingAnnouncesAgain)
{
    ProgressIndicator indicator;

    int events = 0;
    indicator.setPositionCallback([&events] { ++events; });

    {
        ProgressScope scope(indicator.start(), "first run", 4);
        for (int i = 0; i < 4; ++i) {
            scope.next(1);
        }
    }
    const int after_first_run = events;
    EXPECT_GE(after_first_run, 2);

    // 新的一轮：位置重置是可见变化，必须再通知一次。
    indicator.start();
    EXPECT_GT(events, after_first_run);
}

TEST(ProgressIndicatorTest, PositionIsReadableFromOtherThread)
{
    ProgressIndicator indicator;
    std::atomic<bool> done{ false };

    std::thread worker([&] {
        ProgressScope scope(indicator.start(), "Worker", 100);
        for (int i = 0; i < 100; ++i) {
            scope.next(1);
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        done.store(true);
    });

    double first = 1.0;
    while (!done.load()) {
        first = indicator.position();
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    worker.join();

    EXPECT_NEAR(indicator.position(), 1.0, 1e-9);
    EXPECT_GE(first, 0.0);
}

TEST(ProgressIndicatorTest, ReportingWithoutACallbackIsHarmless)
{
    // 绝大多数调用者只关心位置，不装回调：increment 不能因为空回调出问题。
    ProgressIndicator indicator;
    ProgressScope     scope(indicator.start(), "silent", 5);
    for (int i = 0; i < 5; ++i) {
        scope.next(1);
    }
    EXPECT_NEAR(indicator.position(), 1.0, 1e-9);
}

TEST(ProgressIndicatorTest, ConcurrentRangesReportWithoutLosingAStep)
{
    ProgressIndicator indicator;

    constexpr int kWorkers   = 8;
    constexpr int kPerWorker = 4;
    constexpr int kRanges    = kWorkers * kPerWorker;

    // Allocation belongs to one thread: every range is carved, and every worker is handed its four ranges,
    // before the threads start. Reporting a pre-allocated range is the part the module supports concurrently.
    ProgressScope root = ProgressScope(indicator.start(), "concurrent", kRanges);

    std::vector<ProgressRange> ranges;
    ranges.reserve(kRanges);
    for (int i = 0; i < kRanges; ++i) {
        ranges.push_back(root.next(1));
    }

    std::atomic<bool> stop{ false };
    std::atomic<bool> out_of_range{ false };

    std::thread reader([&] {
        while (!stop.load(std::memory_order_acquire)) {
            const double position = indicator.position();
            if (position < 0.0 || position > 1.0) {
                out_of_range.store(true, std::memory_order_relaxed);
            }
            std::this_thread::yield();
        }
    });

    std::vector<std::thread> workers;
    workers.reserve(kWorkers);
    for (int worker = 0; worker < kWorkers; ++worker) {
        workers.emplace_back([&ranges, worker] {
            for (int i = worker * kPerWorker; i < (worker + 1) * kPerWorker; ++i) {
                ranges[static_cast<std::size_t>(i)].complete();
            }
        });
    }
    for (auto& thread : workers) {
        thread.join();
    }
    stop.store(true, std::memory_order_release);
    reader.join();

    // The 32 ranges are 1/32 each: a compare-exchange lost to another worker would leave the bar short.
    EXPECT_NEAR(indicator.position(), 1.0, 1e-9);
    EXPECT_FALSE(out_of_range.load());
    for (const auto& range : ranges) {
        EXPECT_FALSE(range.isActive());
    }
}

TEST(ProgressIndicatorTest, ConcurrentScopesReportTheirOwnPortions)
{
    ProgressIndicator indicator;

    constexpr int kStages        = 8;
    constexpr int kStepsPerStage = 100;

    ProgressScope root = ProgressScope(indicator.start(), "stages", kStages);

    // One stage per worker. They are carved while the thread count is still one; ProgressScope is not movable,
    // so the stages are held behind unique_ptr.
    std::vector<std::unique_ptr<ProgressScope>> stages;
    stages.reserve(kStages);
    for (int i = 0; i < kStages; ++i) {
        stages.push_back(std::make_unique<ProgressScope>(root.next(1), "stage", kStepsPerStage));
    }

    std::vector<std::thread> workers;
    workers.reserve(kStages);
    for (int stage = 0; stage < kStages; ++stage) {
        workers.emplace_back([&stages, stage] {
            for (int step = 0; step < kStepsPerStage; ++step) {
                stages[static_cast<std::size_t>(stage)]->next(1);
            }
        });
    }
    for (auto& thread : workers) {
        thread.join();
    }

    // Destroying a stage reports its (zero) remainder; the eight stages together are the whole scale.
    stages.clear();
    EXPECT_NEAR(indicator.position(), 1.0, 1e-9);
}

TEST(ProgressIndicatorTest, PositionCallbackIsSafeFromConcurrentReporters)
{
    ProgressIndicator indicator;

    std::atomic<int> calls{ 0 };
    indicator.setPositionCallback([&calls] { calls.fetch_add(1, std::memory_order_relaxed); });

    constexpr int kWorkers = 8;
    constexpr int kRanges  = 64;

    ProgressScope root = ProgressScope(indicator.start(), "callback", kRanges);

    std::vector<ProgressRange> ranges;
    ranges.reserve(kRanges);
    for (int i = 0; i < kRanges; ++i) {
        ranges.push_back(root.next(1));
    }

    std::vector<std::thread> workers;
    workers.reserve(kWorkers);
    for (int worker = 0; worker < kWorkers; ++worker) {
        workers.emplace_back([&ranges, worker] {
            for (int i = worker; i < kRanges; i += kWorkers) {
                ranges[static_cast<std::size_t>(i)].complete();
            }
        });
    }
    for (auto& thread : workers) {
        thread.join();
    }

    // The mark is a coalescing hint, not a lock, so two reporters may announce the same position. What must
    // hold is that the run finishes and that the callback is callable from several threads at once.
    EXPECT_NEAR(indicator.position(), 1.0, 1e-9);
    EXPECT_GE(calls.load(), kRanges);
}

} // namespace
