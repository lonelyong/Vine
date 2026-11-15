/**
 * @brief The device's frame gate: "one frame at a time" as a checked fact (see FrameGate).
 *
 * WHY THIS FILE EXISTS. Before the gate, two threads entering a backend's `executeFrame` corrupted the device's
 * command pool, fence, uniform ring and descriptor pool SILENTLY - the contract said "one frame at a time" and
 * nothing made it true. These cases pin the two halves that a unit can pin without a GPU: the owning thread is
 * REFUSED (a re-entry would otherwise deadlock against itself) and a lease releases the gate. The blocking half is
 * pinned by `ConcurrentFrameTest`, where two threads really drive one device.
 */

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <optional>
#include <thread>

#include <vine/graphics/backend/FrameGate.hpp>

namespace
{

using vn::graphics::backend::FrameGate;

} // namespace

TEST(FrameGateTest, TheOwningThreadIsRefusedInsteadOfWaitingForItself)
{
    // A RE-ENTRY IS THE ONE CASE A MUTEX CANNOT ANSWER: a `before_draw` hook (or a diagnostic sink) driving another
    // engine runs on the thread that already holds the gate, so waiting would be the caller waiting for itself. The
    // gate answers an EMPTY lease at once, which the backend turns into one report and `Refused`.
    FrameGate gate;

    const FrameGate::Lease outer = gate.enter();
    ASSERT_TRUE(static_cast<bool>(outer)) << "a free gate refused its first caller";
    EXPECT_TRUE(gate.heldByThisThread());

    const FrameGate::Lease inner = gate.enter();
    EXPECT_FALSE(static_cast<bool>(inner)) << "the owning thread was let in twice: a re-entry would deadlock";
    EXPECT_TRUE(gate.heldByThisThread()) << "the refused call took the gate away from its holder";
}

TEST(FrameGateTest, ALeaseReleasesTheGateWhenItDies)
{
    // THE LEASE IS THE WHOLE POINT (see FrameGate): a frame has many early returns, and a forgotten `leave()` would
    // deadlock the NEXT frame instead of failing where the mistake is. Releasing is therefore the destructor's job,
    // and this case is what proves the destructor does it.
    FrameGate gate;

    {
        const FrameGate::Lease lease = gate.enter();
        ASSERT_TRUE(static_cast<bool>(lease));
        EXPECT_TRUE(gate.heldByThisThread());
    }
    EXPECT_FALSE(gate.heldByThisThread()) << "the lease died and the gate stayed taken";

    const FrameGate::Lease again = gate.enter();
    EXPECT_TRUE(static_cast<bool>(again)) << "the gate was never released";
}

TEST(FrameGateTest, AnotherThreadEntersOnlyAfterTheLeaseDies)
{
    // THE BLOCKING HALF, and the assertion is ORDERING, not duration: the second thread records that it is about to
    // ask, and the gate must not let it in while the lease lives. The short wait below only gives that thread time to
    // reach `enter()`; a gate that let it through would have recorded `2` already.
    FrameGate gate;
    std::atomic<int> phase{ 0 };

    std::optional<FrameGate::Lease> lease = gate.enter();
    ASSERT_TRUE(lease.has_value());

    std::thread other([&phase, &gate]() {
        phase.store(1); // "I am about to ask for the gate"
        const FrameGate::Lease theirs = gate.enter();
        if (static_cast<bool>(theirs)) {
            phase.store(2);
        }
    });

    while (phase.load() == 0) {
        std::this_thread::yield();
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    EXPECT_EQ(phase.load(), 1) << "a second thread drove a frame while one was held";

    lease.reset(); // the frame is over: the waiting thread may go
    other.join();
    EXPECT_EQ(phase.load(), 2) << "the waiting thread never got in after the lease died";

    const FrameGate::Lease after = gate.enter();
    EXPECT_TRUE(static_cast<bool>(after)) << "the gate was not released by the waiting thread's lease";
}
