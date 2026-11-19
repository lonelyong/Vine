#include <gtest/gtest.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <thread>

#include <vine/appfw/ProgressHost.hpp>
#include <vine/progress/ProgressRange.hpp>
#include <vine/progress/ProgressScope.hpp>

using vn::appfw::ProgressHost;
using vn::progress::ProgressRange;
using vn::progress::ProgressScope;

namespace
{

TEST(ProgressHostTest, NoHostByDefault)
{
    EXPECT_EQ(ProgressHost::current(), nullptr);
    EXPECT_FALSE(ProgressHost::isActive());
}

TEST(ProgressHostTest, HostBecomesActiveAndClears)
{
    EXPECT_EQ(ProgressHost::current(), nullptr);
    {
        // 默认是后台宿主；提升为前台后 current() 才能看到它。
        ProgressHost host;
        EXPECT_EQ(ProgressHost::current(), nullptr);
        host.setForeground(true);
        EXPECT_EQ(ProgressHost::current(), &host);
        EXPECT_TRUE(ProgressHost::isActive());
        EXPECT_TRUE(host.isForeground());
    }
    EXPECT_EQ(ProgressHost::current(), nullptr);
    EXPECT_FALSE(ProgressHost::isActive());
}

// 观察者拿到的是值：宿主在别的线程销毁后，快照仍然可读，其中的取消状态也仍然可用。
TEST(ProgressHostTest, SnapshotIsAValueThatOutlivesItsHost)
{
    ProgressHost::Snapshot kept;
    std::uint64_t          first_id = 0;
    {
        ProgressHost host;
        host.setForeground(true);
        host.setLabel(u8"导出");

        const auto current = ProgressHost::currentSnapshot();
        ASSERT_TRUE(current.has_value());
        kept     = *current;
        first_id = kept.id;

        EXPECT_EQ(kept.label, vn::String(u8"导出"));
        EXPECT_EQ(ProgressHost::foregroundSnapshots().size(), 1u);
        EXPECT_EQ(ProgressHost::activeHostCount(), 1u);
    }

    // 宿主已销毁：快照仍是快照，前台空了，而快照里的取消状态依然有效（取消按钮按下的正是它）。
    EXPECT_EQ(kept.label, vn::String(u8"导出"));
    EXPECT_FALSE(ProgressHost::currentSnapshot().has_value());
    EXPECT_EQ(ProgressHost::activeHostCount(), 0u);
    kept.cancelSource.request_stop();
    EXPECT_TRUE(kept.cancelSource.stop_requested());

    // 标识不重复：两次操作的快照不会被当成同一个宿主。
    ProgressHost second;
    second.setForeground(true);
    const auto other = ProgressHost::currentSnapshot();
    ASSERT_TRUE(other.has_value());
    EXPECT_NE(other->id, first_id);
}

// 宿主在一条线程上反复创建/销毁，观察者从另一条线程反复取样：观察者拿到的只能是值。
// 旧实现给的是裸指针，观察者随后读它，而宿主正是这时候析构的（F-3）；这条用例在 TSAN 下才有分量，
// 平时它也钉住"取样不依赖宿主活着"这一条。
TEST(ProgressHostTest, SnapshotsCanBeTakenWhileHostsComeAndGo)
{
    std::atomic<bool> stop{ false };
    std::atomic<int>  taken{ 0 };

    std::thread observer([&] {
        while (!stop.load(std::memory_order_relaxed)) {
            const auto foreground = ProgressHost::foregroundSnapshots();

            // 另一个入口也一起取样：两个调用各取各的，之间的时间差里宿主可能已经没了，
            // 所以这里不断言两者的关系，只看"取到了什么"。
            static_cast<void>(ProgressHost::activeHostCount());

            for (const auto& snapshot : foreground) {
                // 拷贝出来的字段可读，取消状态也还能用（取消按钮按的就是它）。
                static_cast<void>(snapshot.label);
                static_cast<void>(snapshot.cancelSource.stop_requested());
            }
            taken.fetch_add(1, std::memory_order_relaxed);
        }
    });

    for (int i = 0; i < 200; ++i) {
        ProgressHost host;
        host.setForeground(true);
        host.setLabel(u8"并发");
    }

    stop.store(true, std::memory_order_relaxed);
    observer.join();

    EXPECT_GT(taken.load(), 0);
    EXPECT_EQ(ProgressHost::activeHostCount(), 0u);
}

TEST(ProgressHostTest, ReportsProgressThroughScope)
{
    ProgressHost  host;
    constexpr int n = 10;
    {
        ProgressScope scope(host.range(), "Exporting", n);
        // Half of the steps advance the indicator to the middle of the scale.
        for (int i = 0; i < 5; ++i) {
            scope.next(1);
        }
        EXPECT_NEAR(host.indicator().position(), 0.5, 1e-9);
    }
    // Completing the scope drives the indicator to the end of the scale.
    EXPECT_NEAR(host.indicator().position(), 1.0, 1e-9);
}

TEST(ProgressHostTest, CancelRequestsStop)
{
    ProgressHost  host;
    ProgressScope scope(host.range(), "Cancel", 1);
    EXPECT_FALSE(scope.isCancelled());

    host.cancelSource().request_stop();
    EXPECT_TRUE(scope.isCancelled());
    EXPECT_TRUE(host.indicator().isCancelled());
    EXPECT_TRUE(host.indicator().token().stop_requested());
}

TEST(ProgressHostTest, ScopeHelperReportsProgress)
{
    ProgressHost host;

    // 便捷方法：直接建顶层 scope，避免直接碰 one-shot 的 ProgressRange。
    {
        ProgressScope scope = host.scope(u8"Exporting", 10);
        EXPECT_TRUE(scope.isActive());
        for (int i = 0; i < 5; ++i) {
            scope.next(1);
        }
        EXPECT_NEAR(host.indicator().position(), 0.5, 1e-9);
    }
    // 顶层 scope 销毁后推进到结尾。
    EXPECT_NEAR(host.indicator().position(), 1.0, 1e-9);
}

TEST(ProgressHostTest, RangeIsActiveOnceOnly)
{
    ProgressHost host;

    ProgressRange r = host.range();
    EXPECT_TRUE(r.isActive());

    // A root feeds exactly one top-level scope: the scope consumes the move-only range.
    ProgressScope s1(std::move(r), "a", 10);
    EXPECT_TRUE(s1.isActive());
    EXPECT_FALSE(r.isActive()) << "the consumed range must be empty";

    // Reusing it cannot silently take the root a second time.
    ProgressScope s2(std::move(r), "b", 10);
    EXPECT_FALSE(s2.isActive());

    // Several stages are nested: one top-level scope, sub-ranges allocated with next().
    ProgressScope root = host.scope(u8"whole", 10);
    ProgressRange part = root.next(4);
    ProgressScope stage(std::move(part), "stage", 4);
    EXPECT_TRUE(stage.isActive());
}

TEST(ProgressHostTest, LabelRoundTrip)
{
    ProgressHost host;
    EXPECT_TRUE(host.label().empty());
    host.setLabel(u8"Importing mesh");
    EXPECT_EQ(host.label(), u8"Importing mesh");
}

TEST(ProgressHostTest, NestedHostDoesNotClobberForeground)
{
    ProgressHost outer;
    outer.setForeground(true);
    EXPECT_EQ(ProgressHost::current(), &outer);
    {
        // 嵌套/后台宿主不能抢占前台槽位。
        ProgressHost inner;
        EXPECT_EQ(ProgressHost::current(), &outer);
        EXPECT_FALSE(inner.isForeground());
        EXPECT_TRUE(inner.isActive()); // 内部宿主仍可用（有独立 indicator）
    }
    // 后台宿主销毁不会清掉前台宿主。
    EXPECT_EQ(ProgressHost::current(), &outer);
    EXPECT_TRUE(outer.isForeground());
}

TEST(ProgressHostTest, ConcurrentHostsCoexist)
{
    ProgressHost fg;
    fg.setForeground(true);
    ProgressHost bg1;
    ProgressHost bg2;

    EXPECT_TRUE(fg.isForeground());
    EXPECT_FALSE(bg1.isForeground());
    EXPECT_FALSE(bg2.isForeground());
    EXPECT_EQ(ProgressHost::current(), &fg);
    // 只有 fg 进入前台栈；bg1/bg2 是后台宿主。
    EXPECT_EQ(ProgressHost::foregroundStack().size(), 1u);

    // 注册表包含全部活跃宿主（前台栈 + 后台）。
    const auto hosts = ProgressHost::activeHosts();
    EXPECT_EQ(hosts.size(), 3u);
    EXPECT_TRUE(std::find(hosts.begin(), hosts.end(), &fg) != hosts.end());
    EXPECT_TRUE(std::find(hosts.begin(), hosts.end(), &bg1) != hosts.end());
    EXPECT_TRUE(std::find(hosts.begin(), hosts.end(), &bg2) != hosts.end());
}

TEST(ProgressHostTest, ForegroundStackPushRestoresOnDestroy)
{
    ProgressHost parent;
    parent.setForeground(true);
    EXPECT_EQ(ProgressHost::current(), &parent);

    {
        // 子宿主压栈顶替父宿主。
        ProgressHost child;
        child.setForeground(true);
        EXPECT_EQ(ProgressHost::current(), &child);
        EXPECT_TRUE(child.isForeground());
        // 父宿主仍在链内（非后台），只是不再是栈顶。
        EXPECT_TRUE(parent.isForeground());
        EXPECT_EQ(ProgressHost::foregroundStack().size(), 2u);

        // 后台宿主不进前台栈。
        ProgressHost bg;
        EXPECT_FALSE(bg.isForeground());
        EXPECT_EQ(ProgressHost::foregroundStack().size(), 2u);
        EXPECT_EQ(ProgressHost::activeHosts().size(), 3u);
    }

    // 子宿主析构 → 自动恢复父宿主为栈顶。
    EXPECT_EQ(ProgressHost::current(), &parent);
    EXPECT_TRUE(parent.isForeground());

    // 显式降级：从链中移除。
    parent.setForeground(false);
    EXPECT_EQ(ProgressHost::current(), nullptr);
    EXPECT_FALSE(parent.isForeground());
}

TEST(ProgressHostTest, PositionIsReadableFromOtherThread)
{
    ProgressHost      host;
    std::atomic<bool> done{ false };

    std::thread worker([&] {
        ProgressScope scope(host.range(), "Worker", 100);
        for (int i = 0; i < 100; ++i) {
            scope.next(1);
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        done.store(true);
    });

    double last = 0.0;
    while (!done.load()) {
        last = host.indicator().position();
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    worker.join();

    EXPECT_NEAR(host.indicator().position(), 1.0, 1e-9);
    EXPECT_GE(last, 0.0);
}

TEST(ProgressHostTest, ChangedFiresOnRegistrationLabelAndForeground)
{
    int  events  = 0;
    auto handler = ProgressHost::changed().connect([&events] { ++events; });

    ProgressHost host;
    EXPECT_EQ(events, 1); // 注册

    host.setLabel(u8"导入");
    EXPECT_EQ(events, 2);
    host.setLabel(u8"导入");
    EXPECT_EQ(events, 2); // 标签没变：不打扰观察者

    host.setForeground(true);
    EXPECT_EQ(events, 3);
    host.setForeground(true);
    EXPECT_EQ(events, 3); // 已经在前台栈里：不重复通知

    host.setForeground(false);
    EXPECT_EQ(events, 4);
}

TEST(ProgressHostTest, ChangedFiresWhenTheLastHostIsGone)
{
    int  events  = 0;
    auto handler = ProgressHost::changed().connect([&events] { ++events; });

    {
        ProgressHost host;
        host.setForeground(true);
        EXPECT_EQ(events, 2);
    }

    // 宿主析构要发通知，否则呈现者永远停在"还在跑"。
    EXPECT_EQ(events, 3);
    EXPECT_EQ(ProgressHost::current(), nullptr);
}

TEST(ProgressHostTest, ChangedIsCoalescedToPercentSteps)
{
    ProgressHost host;

    int  events  = 0;
    auto handler = ProgressHost::changed().connect([&events] { ++events; });

    constexpr int kItems = 10'000;
    {
        ProgressScope scope = host.scope(u8"coalesce", kItems);
        for (int i = 0; i < kItems; ++i) {
            scope.next(1);
        }
    }

    // 一万个条目只该通知约 100 次（每个百分点一次），不是一万次。
    EXPECT_GE(events, 100);
    EXPECT_LE(events, 103);
}

TEST(ProgressHostTest, ChangedIsSilentAfterTheHandleIsGone)
{
    int events = 0;
    {
        auto handler = ProgressHost::changed().connect([&events] { ++events; });
        {
            ProgressHost host;
            EXPECT_EQ(events, 1); // 注册
        }
        EXPECT_EQ(events, 2); // 注销，句柄还在
    }

    // 句柄析构即取消订阅：之后的变更不再打扰它。
    ProgressHost other;
    EXPECT_EQ(events, 2);
}

} // namespace
