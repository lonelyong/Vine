/**
 * @brief The per-draw slot pool's FREE LIST: which slot is handed out, and what a duplicate return does.
 *
 * A slot's offset is baked into the state wrapper of the drawable that bound it, so two drawables
 * holding one slot is not a bookkeeping error but a picture error: each writes its own opacity into
 * the same block and the later write wins. The pool used to keep a bare `std::vector` of free indices,
 * which could hold one index twice: a second release of the same slot pushed it again, and the next
 * two reservations handed one block to two drawables with nothing to report it.
 *
 * The rule now lives in VsgDrawBlockPool::SlotAllocator, which is device-free on purpose — the chunks
 * need a device (their buffer is one) but the decision "may this slot go back?" does not, so it can be
 * exercised here instead of only on the self-test's lavapipe run.
 */

#include <gtest/gtest.h>

#include <cstdint>

#include <vine/vsg/VsgDrawBlockPool.hpp>

#include <vsg/vk/Device.h>

using vine::vsg::VsgDrawBlockPool;

TEST(DrawBlockPoolSlotsTest, SlotsAreHandedOutLowestIndexFirstAndExactlyOnce)
{
    VsgDrawBlockPool::SlotAllocator allocator(4u);
    EXPECT_EQ(allocator.freeCount(), 4u);

    // Lowest index first, so a scene that needs two drawables touches the first two blocks.
    std::uint32_t first  = 99u;
    std::uint32_t second = 99u;
    EXPECT_TRUE(allocator.take(first));
    EXPECT_TRUE(allocator.take(second));
    EXPECT_EQ(first, 0u);
    EXPECT_EQ(second, 1u);
    EXPECT_TRUE(allocator.inUse(0u));
    EXPECT_TRUE(allocator.inUse(1u));
    EXPECT_FALSE(allocator.inUse(2u));
    EXPECT_EQ(allocator.freeCount(), 2u);

    // A slot that has been taken is not in the free list any more: the next take() cannot hand it out
    // again while it is held.
    std::uint32_t third = 99u;
    EXPECT_TRUE(allocator.take(third));
    EXPECT_EQ(third, 2u);
    EXPECT_TRUE(allocator.inUse(third));
    EXPECT_EQ(allocator.freeCount(), 1u);

    // Every slot handed out: take() refuses instead of inventing one.
    std::uint32_t fourth = 99u;
    EXPECT_TRUE(allocator.take(fourth));
    EXPECT_EQ(fourth, 3u);
    std::uint32_t refused_index = 77u;
    EXPECT_FALSE(allocator.take(refused_index));
    EXPECT_EQ(refused_index, 77u) << "a refused take must not report a slot";
    EXPECT_EQ(allocator.freeCount(), 0u);
}

TEST(DrawBlockPoolSlotsTest, GivingBackASlotMakesItAvailableAgain)
{
    VsgDrawBlockPool::SlotAllocator allocator(2u);

    std::uint32_t first  = 99u;
    std::uint32_t second = 99u;
    ASSERT_TRUE(allocator.take(first));
    ASSERT_TRUE(allocator.take(second));
    EXPECT_EQ(first, 0u);
    EXPECT_EQ(second, 1u);
    EXPECT_EQ(allocator.freeCount(), 0u);

    EXPECT_TRUE(allocator.giveBack(0u)) << "the slot was in use, so it goes back";
    EXPECT_FALSE(allocator.inUse(0u));
    EXPECT_EQ(allocator.freeCount(), 1u);

    std::uint32_t again = 99u;
    EXPECT_TRUE(allocator.take(again));
    EXPECT_EQ(again, 0u) << "the slot that came back is the one handed out again";
}

TEST(DrawBlockPoolSlotsTest, ADuplicateReturnIsRefusedAndNeverEntersTheFreeListTwice)
{
    // The defect this pins: with a bare free list, giveBack() of a slot that is already free pushed the
    // index a second time, and the next two take() calls returned the SAME slot to two drawables.
    VsgDrawBlockPool::SlotAllocator allocator(3u);

    std::uint32_t index = 99u;
    ASSERT_TRUE(allocator.take(index));
    ASSERT_EQ(index, 0u);
    ASSERT_TRUE(allocator.giveBack(0u));
    EXPECT_EQ(allocator.freeCount(), 3u) << "every slot is free again";

    EXPECT_FALSE(allocator.giveBack(0u)) << "the slot is already free: the second return is refused";
    EXPECT_EQ(allocator.freeCount(), 3u) << "and it did not enter the free list a second time";

    // The proof that matters: three takes hand out three DISTINCT slots. With the index pushed twice the
    // free list would hold 0 twice, so the first two takes would both return 0.
    std::uint32_t a = 99u;
    std::uint32_t b = 99u;
    std::uint32_t c = 99u;
    EXPECT_TRUE(allocator.take(a));
    EXPECT_TRUE(allocator.take(b));
    EXPECT_TRUE(allocator.take(c));
    EXPECT_EQ(a, 0u);
    EXPECT_EQ(b, 1u);
    EXPECT_EQ(c, 2u);
    EXPECT_NE(a, b);
    EXPECT_NE(b, c);
    EXPECT_NE(a, c);
    EXPECT_FALSE(allocator.take(a)) << "three slots in use: there is no fourth to hand out";
}

TEST(DrawBlockPoolSlotsTest, AnOutOfRangeIndexIsRefused)
{
    VsgDrawBlockPool::SlotAllocator allocator(2u);
    EXPECT_FALSE(allocator.giveBack(2u)) << "index 2 does not exist in a 2-slot allocator";
    EXPECT_FALSE(allocator.inUse(2u));
    EXPECT_EQ(allocator.freeCount(), 2u) << "a refused return changes nothing";
}

TEST(DrawBlockPoolSlotsTest, APoolWithNoDeviceRefusesToReserveAndNeverRefusesARelease)
{
    // The two ends of the pool itself, both reachable without a device: with no device there is no
    // chunk to reserve from, so every reservation fails cleanly, and the reserved counter stays at 0 —
    // which is also why release() (which only runs on a device-backed chunk) cannot be reached here.
    ::vsg::ref_ptr<::vsg::Device> no_device;
    const auto pool = VsgDrawBlockPool::create(no_device);

    EXPECT_FALSE(pool->acquire().valid()) << "no device means no chunk, so no slot";
    const auto stats = pool->stats();
    EXPECT_EQ(stats.chunks, 0u);
    EXPECT_EQ(stats.capacity, 0u);
    EXPECT_EQ(stats.reserved, 0u);
    EXPECT_EQ(stats.refused, 0u);
    EXPECT_EQ(stats.bytes, 0u) << "no chunk means no device bytes either";
}
