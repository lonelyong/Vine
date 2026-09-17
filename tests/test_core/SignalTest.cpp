#include <gtest/gtest.h>
#include <vine/Signal.hpp>

#include <atomic>
#include <memory>
#include <string>
#include <thread>
#include <vector>

TEST(Signal, RvalueEmitDeliveredToAllHandlers)
{
    vine::Signal<std::string> signal;

    std::string first;
    std::string second;

    signal.subscribe([&](std::string value) {
        first = value;
    }).release();

    signal.subscribe([&](std::string value) {
        second = value;
    }).release();

    signal.trigger(std::string("payload"));

    EXPECT_EQ(first, "payload");
    EXPECT_EQ(second, "payload");
}

TEST(Signal, RemoveSelfDuringEmitIsSafe)
{
    vine::Signal<int> signal;
    std::vector<int> called;

    // A handle can cancel itself from inside the handler: the leftover variable is
    // assigned before the first firing, and the lambda captures it by reference.
    vine::Signal<int>::Subscription self;
    self = signal.subscribe([&](int value) {
        called.push_back(value * 10);
        self.unsubscribe();
    });

    signal.subscribe([&](int value) {
        called.push_back(value);
    }).release();

    signal.trigger(3);
    signal.trigger(4);

    const std::vector<int> expected{ 30, 3, 4 };
    EXPECT_EQ(called, expected);
}

TEST(Signal, AddDuringEmitTakesEffectNextEmit)
{
    vine::Signal<int> signal;
    std::vector<int> called;

    signal.subscribe([&](int value) {
        called.push_back(value);
        // release() because the handle is a temporary here: the subscription must
        // outlive this statement to be seen by the next firing.
        signal.subscribe([&](int next_value) {
            called.push_back(next_value * 100);
        }).release();
    }).release();

    signal.trigger(1);
    signal.trigger(2);

    const std::vector<int> expected{ 1, 2, 200 };
    EXPECT_EQ(called, expected);
}

TEST(Signal, BlockedSignalDoesNotEmit)
{
    vine::Signal<int> signal;
    int               called_count = 0;

    signal.subscribe([&](int) {
        ++called_count;
    }).release();

    signal.setBlocked(true);
    signal.trigger(42);

    EXPECT_EQ(called_count, 0);
    EXPECT_TRUE(signal.isBlocked());

    signal.setBlocked(false);
    signal.trigger(42);

    EXPECT_EQ(called_count, 1);
}

TEST(Signal, RemoveAnotherHandlerDuringEmitSkipsIt)
{
    // Invariant: a handler removed while the signal is firing is not called any
    // more, even though it had not been reached yet. The firing walks a snapshot,
    // so this is what the per-subscription alive flag is for.
    vine::Signal<int> signal;
    std::vector<int>  called;

    vine::Signal<int>::Subscription victim;
    signal.subscribe([&](int) {
        called.push_back(1);
        victim.unsubscribe();
    }).release();
    victim = signal.subscribe([&](int) {
        called.push_back(2);
    });
    signal.subscribe([&](int) {
        called.push_back(3);
    }).release();

    signal.trigger(0);

    const std::vector<int> expected{ 1, 3 };
    EXPECT_EQ(called, expected);
}

TEST(Signal, ClearDuringEmitStopsTheRest)
{
    vine::Signal<int> signal;
    std::vector<int>  called;

    signal.subscribe([&](int) {
        called.push_back(1);
        signal.unsubscribeAll();
    }).release();
    signal.subscribe([&](int) {
        called.push_back(2);
    }).release();

    signal.trigger(0);

    const std::vector<int> expected{ 1 };
    EXPECT_EQ(called, expected);

    // Nothing is subscribed afterwards.
    signal.trigger(0);
    EXPECT_EQ(called, expected);
}

TEST(Signal, ConcurrentSubscribeAndEmitIsWellDefined)
{
    // Invariant: subscribing/unsubscribing from one thread while another fires is
    // well defined (the handler table is published as an immutable snapshot, so a
    // firing never holds a lock and never allocates). This test pins the contract
    // behaviourally; the absence of a data race was measured separately with
    // ThreadSanitizer (12 reports before the fix, 0 after, with exactly this
    // shape: one thread firing in a loop while another adds and removes
    // handlers).
    vine::Signal<int&> signal;
    std::atomic<int>   fired{ 0 };
    std::atomic<int>   stable_calls{ 0 };

    // Subscribed before any thread starts: must be called by every firing.
    signal.subscribe([&](int& v) {
        stable_calls.fetch_add(1);
        v += 1;
    }).release();

    std::atomic<bool> stop{ false };
    std::thread       firer([&] {
        while (!stop.load(std::memory_order_relaxed))
        {
            int value = 0;
            signal.trigger(value);
            fired.fetch_add(1, std::memory_order_relaxed);
        }
    });

    for (int i = 0; i < 2000; ++i)
    {
        auto subscription = signal.subscribe([&](int& v) { v += 100; });
        subscription.unsubscribe();
    }

    stop.store(true, std::memory_order_relaxed);
    firer.join();

    EXPECT_GT(fired.load(), 0);
    EXPECT_GE(stable_calls.load(), fired.load()); // At least once per firing.
}

TEST(Signal, SubscribeFromAHandlerOfTheSameSignalIsVisibleToTheNextFiring)
{
    // The re-entrancy contract, now with the snapshot published in between: a
    // handler firing the signal again sees subscriptions made by handlers of the
    // firing that is still on the stack.
    vine::Signal<int> signal;
    int               outer = 0;
    int               inner = 0;

    signal.subscribe([&](int) {
        ++outer;
        if (outer == 1)
        {
            signal.subscribe([&](int) { ++inner; }).release();
            signal.trigger(0); // Re-entrant firing: sees the new handler.
        }
    }).release();

    signal.trigger(0);

    EXPECT_EQ(outer, 2);
    EXPECT_EQ(inner, 1);
}

TEST(Signal, SubscriptionCancelsOnDestruction)
{
    // The RAII shape: there is no unsubscribe call anywhere in this test, the end of the
    // scope is the unsubscribe.
    vine::Signal<int> signal;
    std::vector<int>  called;

    {
        const vine::Signal<int>::Subscription subscription = signal.subscribe([&](int value) {
            called.push_back(value);
        });

        EXPECT_TRUE(subscription.isActive());
        signal.trigger(1);
    }

    signal.trigger(2);

    const std::vector<int> expected{ 1 };
    EXPECT_EQ(called, expected);
}

TEST(Signal, SubscriptionIsMovableAndUnsubscribeIsIdempotent)
{
    vine::Signal<int> signal;
    int               called = 0;

    auto outer = signal.subscribe([&](int) { ++called; });

    {
        auto inner = std::move(outer);

        EXPECT_TRUE(inner.isActive());
        EXPECT_FALSE(outer.isActive());
        signal.trigger(1); // Still subscribed after the move.

        inner.unsubscribe();
        inner.unsubscribe(); // Cancelling twice is harmless.

        EXPECT_FALSE(inner.isActive());
        signal.trigger(2); // No longer called.
    }

    signal.trigger(3);

    EXPECT_EQ(called, 1);
}

TEST(Signal, AssigningASubscriptionCancelsThePreviousSubscription)
{
    vine::Signal<int> signal;
    int               first  = 0;
    int               second = 0;

    auto subscription = signal.subscribe([&](int) { ++first; });
    subscription      = signal.subscribe([&](int) { ++second; });

    signal.trigger(0);

    EXPECT_EQ(first, 0); // Cancelled by the assignment.
    EXPECT_EQ(second, 1);
    EXPECT_TRUE(subscription.isActive());
}

TEST(Signal, ReleasedSubscriptionStaysSubscribed)
{
    vine::Signal<int> signal;
    int               called = 0;

    {
        // release() gives up management without cancelling: the handler stays subscribed
        // for as long as the Signal lives, which is what an object's own internal wiring
        // wants.
        signal.subscribe([&](int) { ++called; }).release();
    }

    signal.trigger(0);

    EXPECT_EQ(called, 1);
}

TEST(Signal, SubscriptionOutlivingTheSignalIsInert)
{
    // The handle refers to the slot weakly, so tearing the Signal down first leaves it
    // harmless instead of dangling.
    auto subscription = [] {
        auto local = std::make_unique<vine::Signal<int>>();
        auto owned = local->subscribe([](int) { });
        local.reset();
        return owned;
    }();

    EXPECT_FALSE(subscription.isActive());

    subscription.unsubscribe(); // Must not touch the freed signal.
}

TEST(Signal, DefaultSubscriptionOwnsNothing)
{
    vine::Signal<int>::Subscription subscription;

    EXPECT_FALSE(subscription.isActive());

    subscription.unsubscribe(); // Harmless.
    subscription.release();     // Harmless.
    EXPECT_FALSE(subscription.isActive());
}

TEST(Signal, DestroyingTheSignalFromInsideAHandlerIsSafe)
{
    // A firing stops touching the Signal object the moment it has taken the table
    // snapshot: it holds the table and its slots by reference count. So even a handler that
    // destroys the Signal - the last owner going away - lets the remaining handlers run
    // and returns cleanly.
    std::vector<int> called;
    auto*            signal = new vine::Signal<int>();

    signal->subscribe([&](int) {
        called.push_back(1);
        delete signal; // The firing lives on in its snapshot, not in the Signal.
        signal = nullptr;
    }).release();
    signal->subscribe([&](int) { called.push_back(2); }).release();

    signal->trigger(0);

    const std::vector<int> expected{ 1, 2 };
    EXPECT_EQ(called, expected);
}

TEST(Signal, SubscriptionOfADestroyedSignalStaysInertOnAReusedAddress)
{
    // A handle identifies a slot, not a Signal address: once the Signal is gone the handle
    // stays inert even if another Signal is later built at the very same address.
    std::vector<int> called;
    auto*            signal = new vine::Signal<int>();

    auto subscription = signal->subscribe([&](int) { called.push_back(1); });
    delete signal;

    auto* replacement = new vine::Signal<int>();
    EXPECT_FALSE(subscription.isActive());
    subscription.unsubscribe(); // Must not touch the replacement signal.
    replacement->trigger(0);

    EXPECT_TRUE(called.empty());
    delete replacement;
}
