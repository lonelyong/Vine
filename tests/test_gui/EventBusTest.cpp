// EventBus 单元测试：Object 派生事件、多态派发、线程模式、RAII、取消与关停。
//
// 覆盖：发布/订阅、多态（订阅基类收派生事件）、RAII 自动取消、move 转移、
// 派发中取消、异常隔离、Current/Main/Auto 线程模式、一致快照、shutdown/析构边界。

#include <gtest/gtest.h>

#include <QCoreApplication>

#include <vine/appfw/Application.hpp>
#include <vine/appfw/EventBus.hpp>
#include <vine/appfw/MainThreadDispatcher.hpp>
#include <vine/Events.hpp>

#include <atomic>
#include <chrono>
#include <memory>
#include <stdexcept>
#include <thread>
#include <vector>

namespace
{

/// 进程级 marshaller，由 GuiEnv 创建的 Application 提供。
vine::appfw::MainThreadDispatcher* mainDispatcher()
{
    auto* app = vine::appfw::Application::current();
    return app != nullptr ? app->mainThreadDispatcher() : nullptr;
}

class ITagged {
    V_DECLARE_INTERFACE(ITagged)

  public:
    virtual ~ITagged() = default;
    virtual int tag() const = 0;
};

class ITaggedEx : public ITagged {
    V_DECLARE_INTERFACE(ITaggedEx, ITagged)

  public:
    virtual int extra() const = 0;
};

class BaseEvent : public vine::EventArgs {
  public:
    V_OBJECT_META_DECL;
};
V_OBJECT_META_IMPL(BaseEvent, vine::EventArgs)

class DerivedEvent : public BaseEvent {
  public:
    V_OBJECT_META_DECL;
};
V_OBJECT_META_IMPL(DerivedEvent, BaseEvent)

class PingEvent : public vine::EventArgs {
  public:
    V_OBJECT_META_DECL;
};
V_OBJECT_META_IMPL(PingEvent, vine::EventArgs)

/// 事件类实现接口（含接口继承接口）。
class TaggedEvent : public vine::EventArgs, public ITaggedEx {
  public:
    V_OBJECT_META_DECL;

    int tag() const override { return 1; }
    int extra() const override { return 2; }
};
V_OBJECT_META_IMPL(TaggedEvent, vine::EventArgs, ITaggedEx)

/// 基类已实现 ITagged，派生类元数据又列了一遍（合法但冗余：接口经多条路径可达）。
class TaggedDerivedEvent : public TaggedEvent {
  public:
    V_OBJECT_META_DECL;
};
V_OBJECT_META_IMPL(TaggedDerivedEvent, TaggedEvent, ITagged)

/// 多接口（共享父接口 ITagged）：验证访问顺序与去重。
class ILeft : public ITagged {
    V_DECLARE_INTERFACE(ILeft, ITagged)

  public:
    virtual int left() const = 0;
};

class IRight : public ITagged {
    V_DECLARE_INTERFACE(IRight, ITagged)

  public:
    virtual int right() const = 0;
};

class IBoth : public ILeft, public IRight {
    V_DECLARE_INTERFACE(IBoth, ILeft, IRight)

  public:
    virtual int both() const = 0;
};

class BothEvent : public vine::EventArgs, public IBoth {
  public:
    V_OBJECT_META_DECL;

    int tag() const override { return 1; }
    int left() const override { return 2; }
    int right() const override { return 3; }
    int both() const override { return 4; }
};
V_OBJECT_META_IMPL(BothEvent, vine::EventArgs, IBoth)

} // namespace

TEST(EventBusTest, PublishDeliversToSubscribers)
{
    vine::appfw::EventBus bus;
    int                   received = 0;
    auto                  sub      = bus.subscribe<PingEvent>([&](const PingEvent&) { ++received; });
    bus.publish(std::make_shared<PingEvent>());
    bus.publish(std::make_shared<PingEvent>());
    EXPECT_EQ(received, 2);
}

TEST(EventBusTest, DifferentTypesAreIsolated)
{
    vine::appfw::EventBus bus;
    int                   ping = 0;
    int                   base = 0;
    auto                  s1   = bus.subscribe<PingEvent>([&](const PingEvent&) { ++ping; });
    auto                  s2   = bus.subscribe<BaseEvent>([&](const BaseEvent&) { ++base; });
    bus.publish(std::make_shared<PingEvent>());
    EXPECT_EQ(ping, 1);
    EXPECT_EQ(base, 0);  // PingEvent 与 BaseEvent 无继承关系，互不派发
}

TEST(EventBusTest, PolymorphicDispatch)
{
    vine::appfw::EventBus bus;
    int                   base    = 0;
    int                   derived = 0;
    auto                  s1      = bus.subscribe<BaseEvent>([&](const BaseEvent&) { ++base; });
    auto                  s2      = bus.subscribe<DerivedEvent>([&](const DerivedEvent&) { ++derived; });

    bus.publish(std::make_shared<DerivedEvent>());
    EXPECT_EQ(derived, 1);  // 精确类型订阅者收到
    EXPECT_EQ(base, 1);     // 订阅基类也收到派生事件

    bus.publish(std::make_shared<BaseEvent>());
    EXPECT_EQ(derived, 1);  // 发布基类，派生订阅者不收
    EXPECT_EQ(base, 2);
}

TEST(EventBusTest, PublishWithNoSubscribersIsNoOp)
{
    vine::appfw::EventBus bus;
    EXPECT_NO_THROW(bus.publish(std::make_shared<PingEvent>()));
}

TEST(EventBusTest, SubscriptionUnsubscribesOnDestruction)
{
    vine::appfw::EventBus bus;
    int                   received = 0;
    {
        auto sub = bus.subscribe<PingEvent>([&](const PingEvent&) { ++received; });
        bus.publish(std::make_shared<PingEvent>());
        EXPECT_EQ(received, 1);
    }
    bus.publish(std::make_shared<PingEvent>());
    EXPECT_EQ(received, 1);  // RAII 自动退订
}

TEST(EventBusTest, MoveTransfersOwnership)
{
    vine::appfw::EventBus bus;
    int                   received = 0;
    auto                  sub      = bus.subscribe<PingEvent>([&](const PingEvent&) { ++received; });
    auto                  sub2     = std::move(sub);
    EXPECT_FALSE(sub.isActive());
    EXPECT_TRUE(sub2.isActive());
    bus.publish(std::make_shared<PingEvent>());
    EXPECT_EQ(received, 1);

    sub2.unsubscribe();
    EXPECT_FALSE(sub2.isActive());
    bus.publish(std::make_shared<PingEvent>());
    EXPECT_EQ(received, 1);
}

TEST(EventBusTest, UnsubscribeInsideHandlerCancelsNotYetVisited)
{
    vine::appfw::EventBus bus;
    int                   first  = 0;
    int                   second = 0;
    std::shared_ptr<vine::appfw::Subscription> s1;
    std::shared_ptr<vine::appfw::Subscription> s2;
    s1 = std::make_shared<vine::appfw::Subscription>(
        bus.subscribe<PingEvent>([&](const PingEvent&) {
            ++first;
            if (s2) {
                s2->unsubscribe();
            }
        }));
    s2 = std::make_shared<vine::appfw::Subscription>(
        bus.subscribe<PingEvent>([&](const PingEvent&) {
            ++second;
        }));

    ASSERT_NO_THROW(bus.publish(std::make_shared<PingEvent>()));
    // 取消立即对“尚未开始调用”的 handler 生效：本次派发就不再调用 s2。
    EXPECT_EQ(first, 1);
    EXPECT_EQ(second, 0);

    bus.publish(std::make_shared<PingEvent>());
    EXPECT_EQ(first, 2);
    EXPECT_EQ(second, 0);
}

TEST(EventBusTest, ShutdownCancelsInFlightCurrentDispatch)
{
    vine::appfw::EventBus bus;
    int                   before = 0;
    int                   after  = 0;
    auto                  s1     = bus.subscribe<PingEvent>([&](const PingEvent&) {
        ++before;
        bus.shutdown();  // 派发过程中停止总线
    });
    auto s2 = bus.subscribe<PingEvent>([&](const PingEvent&) { ++after; });

    bus.publish(std::make_shared<PingEvent>());
    EXPECT_EQ(before, 1);
    EXPECT_EQ(after, 0);  // shutdown() 后未开始的调用被取消
    EXPECT_TRUE(bus.isShutDown());
}

TEST(EventBusTest, SubscribeDuringDispatchAffectsOnlyLaterPublications)
{
    vine::appfw::EventBus      bus;
    int                        base = 0;
    vine::appfw::Subscription  base_sub;
    auto                       derived_sub = bus.subscribe<DerivedEvent>([&](const DerivedEvent&) {
        if (!base_sub.isActive()) {
            base_sub = bus.subscribe<BaseEvent>([&](const BaseEvent&) { ++base; });
        }
    });

    bus.publish(std::make_shared<DerivedEvent>());
    EXPECT_EQ(base, 0);  // 本次 publish 的计划已固定：新订阅不影响它

    bus.publish(std::make_shared<DerivedEvent>());
    EXPECT_EQ(base, 1);  // 下一次发布才生效
}

TEST(EventBusTest, ShutdownReleasesPendingEvent)
{
    vine::appfw::EventBus bus(mainDispatcher());
    ASSERT_TRUE(bus.pendingDeliveryCount() == 0);

    std::weak_ptr<const PingEvent> weak;
    auto                           event = std::make_shared<PingEvent>();
    weak                                 = event;
    auto sub = bus.subscribe<PingEvent>([](const PingEvent&) {},
                                        vine::appfw::SubscriptionThreadMode::Main);
    bus.publish(event);
    event.reset();

    EXPECT_FALSE(weak.expired());
    EXPECT_EQ(bus.pendingDeliveryCount(), 1u);  // 已停放在事件循环中

    bus.shutdown();
    EXPECT_TRUE(weak.expired());                // 未执行的投递不再持有事件
    EXPECT_EQ(bus.pendingDeliveryCount(), 0u);

    QCoreApplication::processEvents();
    EXPECT_TRUE(weak.expired());
}

TEST(EventBusTest, SubscriptionHandleReflectsCancellation)
{
    vine::appfw::EventBus bus;
    auto                  sub = bus.subscribe<PingEvent>([](const PingEvent&) {});
    EXPECT_TRUE(sub.isActive());

    sub.unsubscribe();
    EXPECT_FALSE(sub.isActive());

    auto other = bus.subscribe<PingEvent>([](const PingEvent&) {});
    bus.shutdown();
    EXPECT_FALSE(other.isActive());  // 总线关停后句柄明确为惰性
}

TEST(EventBusTest, MoveAssignmentCancelsPreviousSubscription)
{
    vine::appfw::EventBus bus;
    int                   first  = 0;
    int                   second = 0;
    auto                  a      = bus.subscribe<PingEvent>([&](const PingEvent&) { ++first; });
    auto                  b      = bus.subscribe<PingEvent>([&](const PingEvent&) { ++second; });

    a = std::move(b);
    EXPECT_FALSE(b.isActive());
    EXPECT_TRUE(a.isActive());

    bus.publish(std::make_shared<PingEvent>());
    EXPECT_EQ(first, 0);   // 被覆盖的旧订阅已取消
    EXPECT_EQ(second, 1);
}

TEST(EventBusTest, MainWithoutMarshallerRunsOnPublishingThread)
{
    vine::appfw::EventBus bus;  // 未注入 marshaller
    int                   received = 0;
    auto                  sub      = bus.subscribe<PingEvent>([&](const PingEvent&) { ++received; },
                                        vine::appfw::SubscriptionThreadMode::Main);

    bus.publish(std::make_shared<PingEvent>());
    EXPECT_EQ(received, 1);                       // 文档化的降级：在发布线程执行
    EXPECT_EQ(bus.pendingDeliveryCount(), 0u);
}

TEST(EventBusTest, SubscriberExceptionIsContained)
{
    vine::appfw::EventBus bus;
    int                   received = 0;
    auto                  s1       = bus.subscribe<PingEvent>([](const PingEvent&) { throw std::runtime_error("boom"); });
    auto                  s2       = bus.subscribe<PingEvent>([&](const PingEvent&) { ++received; });
    ASSERT_NO_THROW(bus.publish(std::make_shared<PingEvent>()));
    EXPECT_EQ(received, 1);  // remaining subscribers still run
}

TEST(EventBusTest, MainModeQueuesDelivery)
{
    vine::appfw::EventBus bus(mainDispatcher());
    ASSERT_NE(mainDispatcher(), nullptr);  // GuiEnv 提供 Application
    int                   received = 0;
    auto                  sub      = bus.subscribe<PingEvent>([&](const PingEvent&) { ++received; },
                                        vine::appfw::SubscriptionThreadMode::Main);

    bus.publish(std::make_shared<PingEvent>());
    EXPECT_EQ(received, 0);  // 未立即执行

    QCoreApplication::processEvents();  // 冲刷主线程投递队列
    EXPECT_EQ(received, 1);
}

TEST(EventBusTest, AutoModeOnMainIsSynchronous)
{
    vine::appfw::EventBus bus(mainDispatcher());
    int                   received = 0;
    auto                  sub      = bus.subscribe<PingEvent>([&](const PingEvent&) { ++received; },
                                        vine::appfw::SubscriptionThreadMode::Auto);
    // 测试线程即主线程 → Auto → Current（同步）
    bus.publish(std::make_shared<PingEvent>());
    EXPECT_EQ(received, 1);
}

TEST(EventBusTest, AutoModeOffMainQueuesToMain)
{
    vine::appfw::EventBus bus(mainDispatcher());
    std::atomic<int>      received{ 0 };
    auto                  sub = bus.subscribe<PingEvent>([&](const PingEvent&) { ++received; },
                                        vine::appfw::SubscriptionThreadMode::Auto);
    // 非主线程发布 → Auto → Main（排队到主线程）
    std::thread worker([&] { bus.publish(std::make_shared<PingEvent>()); });
    worker.join();
    EXPECT_EQ(received.load(), 0);
    QCoreApplication::processEvents();
    EXPECT_EQ(received.load(), 1);
}

TEST(EventBusTest, UnsubscribedBeforeQueuedDeliveryIsSkipped)
{
    vine::appfw::EventBus bus(mainDispatcher());
    int                   received = 0;
    auto                  sub      = bus.subscribe<PingEvent>([&](const PingEvent&) { ++received; },
                                        vine::appfw::SubscriptionThreadMode::Main);
    bus.publish(std::make_shared<PingEvent>());
    sub.unsubscribe();  // 主线程投递前退订
    QCoreApplication::processEvents();
    EXPECT_EQ(received, 0);  // 异步重校验跳过
}

TEST(EventBusTest, EventOutlivesPublishViaSharedPtr)
{
    vine::appfw::EventBus bus(mainDispatcher());
    std::weak_ptr<const PingEvent> weak;
    {
        auto event = std::make_shared<PingEvent>();
        weak       = event;
        auto sub = bus.subscribe<PingEvent>([](const PingEvent&) {},
                                            vine::appfw::SubscriptionThreadMode::Main);
        bus.publish(event);
        event.reset();
        EXPECT_FALSE(weak.expired());  // 排队任务持有 shared_ptr，事件存活
    }
    QCoreApplication::processEvents();
    EXPECT_TRUE(weak.expired());  // 投递完成后释放
}

TEST(EventBusTest, BusDestroyedBeforeQueuedDeliveryIsDropped)
{
    int received = 0;
    {
        vine::appfw::EventBus bus(mainDispatcher());
        auto                  sub = bus.subscribe<PingEvent>([&](const PingEvent&) { ++received; },
                                        vine::appfw::SubscriptionThreadMode::Main);
        bus.publish(std::make_shared<PingEvent>());
        EXPECT_EQ(received, 0);
    }  // bus 销毁时投递仍排在 Qt 队列里

    // 排队任务只持有“已脱离 bus 的注册表 + 自己的 payload”，不引用 bus：
    // bus 销毁时 payload 被释放，任务自行丢弃。
    QCoreApplication::processEvents();
    EXPECT_EQ(received, 0);
}

TEST(EventBusTest, TokenOutlivingBusIsInert)
{
    std::shared_ptr<vine::appfw::Subscription> sub;
    {
        vine::appfw::EventBus bus;
        sub = std::make_shared<vine::appfw::Subscription>(
            bus.subscribe<PingEvent>([](const PingEvent&) {}));
    }  // bus 先销毁，token 后销毁

    ASSERT_NO_THROW(sub->unsubscribe());
    ASSERT_NO_THROW(sub.reset());
}

TEST(EventBusTest, UnsubscribedHandlerIsReleasedWhileDeliveryQueued)
{
    vine::appfw::EventBus bus(mainDispatcher());
    auto                  sentinel = std::make_shared<int>(0);
    std::weak_ptr<int>    weak     = sentinel;
    int                   received = 0;
    auto                  sub      = bus.subscribe<PingEvent>([sentinel, &received](const PingEvent&) { ++received; },
                                        vine::appfw::SubscriptionThreadMode::Main);
    sentinel.reset();
    EXPECT_FALSE(weak.expired());  // 订阅期间由 channel 持有

    bus.publish(std::make_shared<PingEvent>());
    sub.unsubscribe();
    bus.publish(std::make_shared<PingEvent>());  // 触发非活跃条目的回收
    EXPECT_TRUE(weak.expired());                 // 排队中的投递不再持有该 handler

    QCoreApplication::processEvents();
    EXPECT_EQ(received, 0);
}

TEST(EventBusTest, ShutdownStopsDeliveryAndDropsPending)
{
    vine::appfw::EventBus bus(mainDispatcher());
    int                   current = 0;
    int                   queued  = 0;
    auto                  s1      = bus.subscribe<PingEvent>([&](const PingEvent&) { ++current; });
    auto                  s2      = bus.subscribe<PingEvent>([&](const PingEvent&) { ++queued; },
                                        vine::appfw::SubscriptionThreadMode::Main);
    bus.publish(std::make_shared<PingEvent>());
    EXPECT_EQ(current, 1);
    EXPECT_EQ(queued, 0);  // 已入队，尚未执行

    bus.shutdown();
    EXPECT_TRUE(bus.isShutDown());

    // 停止后不再产生任何投递：新的 publish 是 no-op，已排队的投递也被丢弃。
    bus.publish(std::make_shared<PingEvent>());
    EXPECT_EQ(current, 1);
    QCoreApplication::processEvents();
    EXPECT_EQ(queued, 0);

    // 停止后订阅得到惰性 token。
    auto late = bus.subscribe<PingEvent>([&](const PingEvent&) { ++current; });
    EXPECT_FALSE(late.isActive());
    bus.publish(std::make_shared<PingEvent>());
    EXPECT_EQ(current, 1);

    ASSERT_NO_THROW(bus.shutdown());  // 幂等
}

TEST(EventBusTest, InterfaceSubscriptionReceivesImplementingEvents)
{
    vine::appfw::EventBus bus;
    std::vector<int>      order;
    auto                  s1 = bus.subscribe<ITagged>([&](const ITagged&) { order.push_back(1); });
    auto                  s2 = bus.subscribe<ITaggedEx>([&](const ITaggedEx&) { order.push_back(2); });
    auto                  s3 = bus.subscribe<vine::EventArgs>([&](const vine::EventArgs&) { order.push_back(3); });

    bus.publish(std::make_shared<TaggedEvent>());
    // 顺序：派生类 → 它声明的接口（传递、声明序）→ 基类
    EXPECT_EQ(order, (std::vector<int>{ 2, 1, 3 }));

    order.clear();
    bus.publish(std::make_shared<BaseEvent>());
    EXPECT_EQ(order, (std::vector<int>{ 3 }));  // 未实现接口的事件不命中接口订阅
}

TEST(EventBusTest, InterfaceWalkDeliversEachSubscriptionOnce)
{
    vine::appfw::EventBus bus;
    int                   tagged = 0;
    auto                  s1     = bus.subscribe<ITagged>([&](const ITagged&) { ++tagged; });

    bus.publish(std::make_shared<TaggedDerivedEvent>());
    EXPECT_EQ(tagged, 1);  // ITagged 经多条路径可达，仍只投递一次
}

TEST(EventBusTest, GracefulShutdownDeliversParkedDelivery)
{
    vine::appfw::EventBus bus(mainDispatcher());
    int                   received = 0;
    auto                  sub      = bus.subscribe<PingEvent>([&](const PingEvent&) { ++received; },
                                        vine::appfw::SubscriptionThreadMode::Main);

    std::thread worker([&] { bus.publish(std::make_shared<PingEvent>()); });
    worker.join();
    EXPECT_EQ(received, 0);  // 已停放，尚未执行
    EXPECT_EQ(bus.pendingDeliveryCount(), 1u);

    EXPECT_TRUE(bus.shutdownGracefully(std::chrono::milliseconds(500)));
    EXPECT_EQ(received, 1);  // 关停前投递完成
    EXPECT_EQ(bus.pendingDeliveryCount(), 0u);
    EXPECT_TRUE(bus.isShutDown());
}

TEST(EventBusTest, GracefulShutdownWaitsForPublishingThread)
{
    vine::appfw::EventBus bus;
    std::atomic<bool>     inside{ false };
    std::atomic<bool>     release{ false };
    int                   completed = 0;
    auto                  sub       = bus.subscribe<PingEvent>([&](const PingEvent&) {
        inside.store(true);
        while (!release.load()) {
            std::this_thread::yield();
        }
        ++completed;
    });

    std::thread worker([&] { bus.publish(std::make_shared<PingEvent>()); });
    while (!inside.load()) {
        std::this_thread::yield();
    }

    // 生产者仍在 publish() 内：graceful 等它到超时，并如实报告未完成。
    EXPECT_FALSE(bus.shutdownGracefully(std::chrono::milliseconds(50)));
    release.store(true);
    worker.join();

    EXPECT_EQ(completed, 1);  // 已在执行的 handler 不被中断
    EXPECT_TRUE(bus.isShutDown());
}

TEST(EventBusTest, GracefulShutdownFromWorkerReportsDroppedWork)
{
    vine::appfw::EventBus bus(mainDispatcher());
    int                   received = 0;
    auto                  sub      = bus.subscribe<PingEvent>([&](const PingEvent&) { ++received; },
                                        vine::appfw::SubscriptionThreadMode::Main);

    bool graceful = true;
    std::thread worker([&] {
        bus.publish(std::make_shared<PingEvent>());
        // 非应用线程无法泵主队列：graceful 无法完成剩余投递，应报告 false。
        graceful = bus.shutdownGracefully(std::chrono::milliseconds(50));
    });
    worker.join();

    EXPECT_FALSE(graceful);
    QCoreApplication::processEvents();
    EXPECT_EQ(received, 0);  // 剩余投递已被丢弃
}

TEST(EventBusTest, ConcurrentGracefulShutdownsShareCompletion)
{
    vine::appfw::EventBus bus;
    std::atomic<bool>     inside{ false };
    std::atomic<bool>     release{ false };
    auto                  sub = bus.subscribe<PingEvent>([&](const PingEvent&) {
        inside.store(true);
        while (!release.load()) {
            std::this_thread::yield();
        }
    });

    std::thread publisher([&] { bus.publish(std::make_shared<PingEvent>()); });
    while (!inside.load()) {
        std::this_thread::yield();
    }

    // 第一次 graceful 会等到超时（生产者仍停在 publish 内）。
    std::atomic<bool> first_started{ false };
    std::atomic<bool> first_result{ true };
    std::thread       first([&] {
        first_started.store(true);
        first_result.store(bus.shutdownGracefully(std::chrono::milliseconds(100)));
    });
    while (!first_started.load()) {
        std::this_thread::yield();
    }

    // 第二次调用必须等第一次真正完成，并返回同一次的结果（而不是立即 true）。
    const bool second_result = bus.shutdownGracefully(std::chrono::milliseconds(500));

    release.store(true);
    publisher.join();
    first.join();

    EXPECT_FALSE(second_result);
    EXPECT_FALSE(first_result.load());
    EXPECT_TRUE(bus.isShutDown());
}

TEST(EventBusTest, GracefulShutdownFromHandlerDoesNotWaitForItself)
{
    vine::appfw::EventBus bus(mainDispatcher());
    bool                  inside_result = false;
    auto                  sub           = bus.subscribe<PingEvent>([&](const PingEvent&) {
        inside_result = bus.shutdownGracefully(std::chrono::milliseconds(100));
    });

    bus.publish(std::make_shared<PingEvent>());
    EXPECT_TRUE(inside_result);  // 自身这次 publish 被排除，不会因等待自己而超时
    EXPECT_TRUE(bus.isShutDown());
}

TEST(EventBusTest, PublishRacingShutdownIsSafe)
{
    vine::appfw::EventBus bus(mainDispatcher());
    std::atomic<bool>     go{ true };
    std::atomic<int>      delivered{ 0 };
    auto                  sub = bus.subscribe<PingEvent>([&](const PingEvent&) { delivered.fetch_add(1); });

    std::thread worker([&] {
        while (go.load(std::memory_order_relaxed)) {
            bus.publish(std::make_shared<PingEvent>());
        }
    });

    // 与发布线程并发停止：停止后不再有任何投递，也不会碰到已释放的状态。
    bus.shutdownGracefully(std::chrono::milliseconds(200));
    go.store(false, std::memory_order_relaxed);
    worker.join();

    EXPECT_TRUE(bus.isShutDown());
    const int before = delivered.load();
    bus.publish(std::make_shared<PingEvent>());
    EXPECT_EQ(delivered.load(), before);
    EXPECT_FALSE(bus.subscribe<PingEvent>([](const PingEvent&) {}).isActive());
}

TEST(EventBusTest, GracefulShutdownDoesNotCountAnotherBusCallAsItsOwn)
{
    // Bus A：另一个线程正卡在 A 的 handler 里 → A 有在飞调用。
    vine::appfw::EventBus bus_a;
    std::atomic<bool>     inside_a{ false };
    std::atomic<bool>     release_a{ false };
    auto                  sub_a = bus_a.subscribe<PingEvent>([&](const PingEvent&) {
        inside_a.store(true);
        while (!release_a.load()) {
            std::this_thread::yield();
        }
    });

    std::thread worker([&] { bus_a.publish(std::make_shared<PingEvent>()); });
    while (!inside_a.load()) {
        std::this_thread::yield();
    }

    // 本线程在 Bus B 的调用栈里对 Bus A 做 graceful shutdown：排除"自己的调用"
    // 必须按 bus 分别计数，不能把 B 的深度算到 A 头上（否则会立刻误判
    // "A 没有在飞调用" → 提前返回 true）。
    vine::appfw::EventBus bus_b;
    bool                  result = true;
    auto                  sub_b  = bus_b.subscribe<PingEvent>([&](const PingEvent&) {
        result = bus_a.shutdownGracefully(std::chrono::milliseconds(50));
    });
    bus_b.publish(std::make_shared<PingEvent>());

    release_a.store(true);
    worker.join();

    EXPECT_FALSE(result);  // A 确有在飞调用 → 等超时并如实报告 false
    EXPECT_TRUE(bus_a.isShutDown());
}

TEST(EventBusTest, DestructionWaitsForAdmittedCall)
{
    std::atomic<bool> inside{ false };
    std::atomic<bool> release{ false };
    std::atomic<bool> destroyed{ false };

    auto bus = std::make_unique<vine::appfw::EventBus>();
    auto sub = bus->subscribe<PingEvent>([&](const PingEvent&) {
        inside.store(true);
        while (!release.load()) {
            std::this_thread::yield();
        }
    });

    std::thread worker([&] { bus->publish(std::make_shared<PingEvent>()); });
    while (!inside.load()) {
        std::this_thread::yield();
    }

    // 另一线程析构：必须等在飞调用退出后才真正销毁 Impl（而不是悬垂的 Impl*）。
    std::thread destroyer([&] {
        bus.reset();
        destroyed.store(true);
    });

    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    EXPECT_FALSE(destroyed.load());  // 仍在等待在飞调用

    release.store(true);
    worker.join();
    destroyer.join();
    EXPECT_TRUE(destroyed.load());
}

TEST(EventBusTest, MultiInterfaceVisitOrderAndDedup)
{
    vine::appfw::EventBus bus;
    std::vector<int>      order;
    auto                  s1 = bus.subscribe<IBoth>([&](const IBoth&) { order.push_back(1); });
    auto                  s2 = bus.subscribe<ILeft>([&](const ILeft&) { order.push_back(2); });
    auto                  s3 = bus.subscribe<IRight>([&](const IRight&) { order.push_back(3); });
    auto                  s4 = bus.subscribe<vine::EventArgs>([&](const vine::EventArgs&) { order.push_back(4); });

    bus.publish(std::make_shared<BothEvent>());
    // 类型访问序：派生类 → 接口（声明序，父接口紧随其后）→ 基类；
    // ITagged 经 ILeft/IRight 两条路径可达，但只访问一次。
    EXPECT_EQ(order, (std::vector<int>{ 1, 2, 3, 4 }));
}

TEST(EventBusTest, ErrorHandlerReceivesCurrentFailure)
{
    vine::appfw::EventBus                     bus;
    std::vector<vine::appfw::EventBusError>   errors;
    bus.setErrorHandler([&](const vine::appfw::EventBusError& error) { errors.push_back(error); });

    int  delivered = 0;
    auto throwing  = bus.subscribe<PingEvent>([](const PingEvent&) { throw std::runtime_error("boom"); });
    auto following = bus.subscribe<PingEvent>([&](const PingEvent&) { ++delivered; });

    bus.publish(std::make_shared<PingEvent>());

    ASSERT_EQ(errors.size(), 1u);
    EXPECT_EQ(errors[0].event_type, PingEvent::desc());
    EXPECT_NE(errors[0].subscriber_id, 0u);  // 标识抛出异常的订阅
    EXPECT_EQ(errors[0].mode, vine::appfw::SubscriptionThreadMode::Current);
    EXPECT_FALSE(errors[0].deferred);
    ASSERT_NE(errors[0].error, nullptr);
    EXPECT_EQ(delivered, 1);  // 后续订阅者仍执行

    // 上下文中的 exception_ptr 可重新抛出，便于崩溃上报/测试断言。
    EXPECT_THROW(std::rethrow_exception(errors[0].error), std::runtime_error);
}

TEST(EventBusTest, ErrorHandlerReceivesDeferredFailure)
{
    vine::appfw::EventBus                   bus(mainDispatcher());
    std::vector<vine::appfw::EventBusError> errors;
    int                                     later_handler_calls = 0;
    bus.setErrorHandler([&](const vine::appfw::EventBusError& error) { errors.push_back(error); });

    auto sub = bus.subscribe<PingEvent>([](const PingEvent&) { throw std::runtime_error("deferred"); },
                                        vine::appfw::SubscriptionThreadMode::Main);
    bus.publish(std::make_shared<PingEvent>());
    EXPECT_TRUE(errors.empty());  // 尚未执行

    // 投递时安装的处理器被快照，之后更换不影响已排队的投递。
    bus.setErrorHandler([&](const vine::appfw::EventBusError&) { ++later_handler_calls; });
    QCoreApplication::processEvents();

    ASSERT_EQ(errors.size(), 1u);
    EXPECT_TRUE(errors[0].deferred);
    EXPECT_EQ(errors[0].mode, vine::appfw::SubscriptionThreadMode::Main);
    EXPECT_EQ(later_handler_calls, 0);
}

TEST(EventBusTest, ThrowingErrorHandlerIsIgnored)
{
    vine::appfw::EventBus bus;
    bus.setErrorHandler([](const vine::appfw::EventBusError&) { throw std::runtime_error("handler"); });

    int  delivered = 0;
    auto throwing  = bus.subscribe<PingEvent>([](const PingEvent&) { throw std::runtime_error("boom"); });
    auto following = bus.subscribe<PingEvent>([&](const PingEvent&) { ++delivered; });

    ASSERT_NO_THROW(bus.publish(std::make_shared<PingEvent>()));
    EXPECT_EQ(delivered, 1);  // 观测路径抛异常不得影响投递
}

TEST(EventBusTest, ErrorHandlerCanBeRemoved)
{
    vine::appfw::EventBus bus;
    int                   seen = 0;
    bus.setErrorHandler([&](const vine::appfw::EventBusError&) { ++seen; });

    auto throwing = bus.subscribe<PingEvent>([](const PingEvent&) { throw std::runtime_error("boom"); });
    bus.publish(std::make_shared<PingEvent>());
    EXPECT_EQ(seen, 1);

    bus.setErrorHandler(nullptr);
    bus.publish(std::make_shared<PingEvent>());
    EXPECT_EQ(seen, 1);  // 已移除，只剩日志
}

TEST(EventBusTest, ErrorHandlerReportsSubscriptionTag)
{
    vine::appfw::EventBus                   bus(mainDispatcher());
    std::vector<vine::appfw::EventBusError> errors;
    bus.setErrorHandler([&](const vine::appfw::EventBusError& error) { errors.push_back(error); });

    // 当前线程投递：标签连同上下文一起上报。
    auto current = bus.subscribe<PingEvent>([](const PingEvent&) { throw std::runtime_error("current"); },
                                           vine::appfw::SubscriptionThreadMode::Current,
                                           vine::String(u8"ConsolePanel"));
    bus.publish(std::make_shared<PingEvent>());
    ASSERT_EQ(errors.size(), 1u);
    EXPECT_TRUE(errors[0].tag == vine::String(u8"ConsolePanel"));
    EXPECT_FALSE(errors[0].deferred);

    // 排队投递：标签随 payload 一起被快照，主线程执行时照样带出来。
    current.unsubscribe();
    auto deferred = bus.subscribe<PingEvent>([](const PingEvent&) { throw std::runtime_error("deferred"); },
                                             vine::appfw::SubscriptionThreadMode::Main,
                                             vine::String(u8"DockPanel"));
    bus.publish(std::make_shared<PingEvent>());
    EXPECT_EQ(errors.size(), 1u);  // 尚未执行
    QCoreApplication::processEvents();
    ASSERT_EQ(errors.size(), 2u);
    EXPECT_TRUE(errors[1].tag == vine::String(u8"DockPanel"));
    EXPECT_TRUE(errors[1].deferred);

    // 未传标签时为空，便于“只关心有标签的失败”的过滤器。
    deferred.unsubscribe();
    auto untagged = bus.subscribe<BaseEvent>([](const BaseEvent&) { throw std::runtime_error("untagged"); });
    bus.publish(std::make_shared<BaseEvent>());
    ASSERT_EQ(errors.size(), 3u);
    EXPECT_TRUE(errors[2].tag.empty());
}

// 空事件是 no-op：publish(nullptr) 不得派发、不得排队、不得抛。
TEST(EventBusTest, PublishNullEventIsIgnored)
{
    vine::appfw::EventBus bus(mainDispatcher());

    int  current_runs = 0;
    auto current      = bus.subscribe<PingEvent>([&current_runs](const PingEvent&) { ++current_runs; });
    auto main_mode    = bus.subscribe<PingEvent>([](const PingEvent&) {}, vine::appfw::SubscriptionThreadMode::Main);

    EXPECT_NO_THROW(bus.publish(nullptr));
    EXPECT_EQ(current_runs, 0);
    EXPECT_EQ(bus.pendingDeliveryCount(), 0u);

    // 正常事件照旧派发/排队，说明空事件只是被忽略而不是把总线弄坏。
    bus.publish(std::make_shared<PingEvent>());
    EXPECT_EQ(current_runs, 1);
    EXPECT_EQ(bus.pendingDeliveryCount(), 1u);

    QCoreApplication::processEvents();
    EXPECT_EQ(bus.pendingDeliveryCount(), 0u);
}

// 关停结果是"共享的"，但它必须如实：一次已经丢弃了排队投递的普通 shutdown()
// 之后，shutdownGracefully() 不能宣称"所有投递都跑完了"（否则调用方会据此
// 认为关停是干净的，甚至在仍有 admitted 调用时就开始销毁外围对象）。
TEST(EventBusTest, GracefulResultAfterPlainShutdownDoesNotClaimDrainedWork)
{
    vine::appfw::EventBus bus(mainDispatcher());

    auto sub = bus.subscribe<PingEvent>([](const PingEvent&) {}, vine::appfw::SubscriptionThreadMode::Main);
    ASSERT_TRUE(sub.isActive());

    // 排一条 Main 投递，然后用非优雅关停把它丢掉（不 drain）。
    bus.publish(std::make_shared<PingEvent>());
    ASSERT_EQ(bus.pendingDeliveryCount(), 1u);
    bus.shutdown();
    EXPECT_TRUE(bus.isShutDown());
    EXPECT_EQ(bus.pendingDeliveryCount(), 0u);

    // 之后（或并发）的优雅关停调用只能报"有工作被丢弃"。
    EXPECT_FALSE(bus.shutdownGracefully(std::chrono::milliseconds(50)));
}

namespace
{
/// 析构时再次发起关停：挂在订阅闭包上，闭包随关停回收被销毁。
class ShutdownOnDestroy {
  public:
    explicit ShutdownOnDestroy(vine::appfw::EventBus* bus)
      : bus_(bus)
    {}

    ~ShutdownOnDestroy() { bus_->shutdown(); }

  private:
    vine::appfw::EventBus* bus_;
};
} // namespace

// 停止线程上的重入关停不得等自己：订阅闭包（捕获的对象）在 cancelSubscriptions()
// 里被销毁，若它的析构又调用 shutdown()，那就是"正在停止的这条线程"再请求停止——
// 等下去只有死锁一条路。工作线程 + 有界等待的写法：回归时用例失败而不是挂住套件。
TEST(EventBusTest, ReentrantShutdownFromReleasedHandlerDoesNotWaitForItself)
{
    std::atomic<bool> finished{ false };

    std::thread worker([&finished] {
        {
            vine::appfw::EventBus bus;
            {
                auto guard = std::make_shared<ShutdownOnDestroy>(&bus);
                auto sub   = bus.subscribe<PingEvent>([guard](const PingEvent&) {});
                ASSERT_TRUE(sub.isActive());
            }
            // 本地句柄全部销毁，但订阅仍由总线持有：闭包要等到关停回收时才析构。
            bus.shutdown();  // 回收订阅 → 守卫析构 → 重入 shutdown()
        }
        finished.store(true);
    });

    for (int i = 0; i < 300 && !finished.load(); ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    EXPECT_TRUE(finished.load()) << "重入的 shutdown() 在停止线程上等待了自己";

    if (finished.load()) {
        worker.join();
    }
    else {
        // 回归时线程仍卡在死锁里：detach 让用例以断言失败收场，而不是拖住整个套件。
        worker.detach();
    }
}
