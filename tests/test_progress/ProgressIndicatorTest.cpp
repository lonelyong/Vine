#include <gtest/gtest.h>

#include <atomic>
#include <thread>

#include <vine/progress/ProgressIndicator.hpp>
#include <vine/progress/ProgressRange.hpp>
#include <vine/progress/ProgressScope.hpp>

using vine::progress::ProgressIndicator;
using vine::progress::ProgressRange;
using vine::progress::ProgressScope;

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

    // indicator.start() 返回的 root 是 active 的（C++17 拷贝省略 / move-on-copy 都保证接收方 active）。
    ProgressRange r = indicator.start();
    EXPECT_TRUE(r.isActive());

    // 一个 root 只能喂给一个顶层 scope：ProgressRange 是 move-on-copy，
    // 第一个 scope 构造后 r 失效，复用会静默变成空 scope。
    ProgressScope s1(r, "a", 10);
    EXPECT_TRUE(s1.isActive());
    ProgressScope s2(r, "b", 10);
    EXPECT_FALSE(s2.isActive());
}

TEST(ProgressIndicatorTest, NestedScopesMapOntoTheParentRange)
{
    ProgressIndicator indicator;

    ProgressScope root = ProgressScope(indicator.start(), "whole", 10);
    ProgressRange part = root.next(4);
    ProgressScope stage(part, "stage", 4);
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

} // namespace
