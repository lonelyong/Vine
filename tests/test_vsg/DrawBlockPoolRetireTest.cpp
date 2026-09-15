/**
 * @brief The per-draw slot pool's RETIRED queue: the countdown that keeps a dropped slot out of use.
 *
 * A slot's offset is baked into the state wrapper of the drawable that bound it, so the frames still
 * in flight may read it: `retire()` parks the slot and `advanceRetired()` — one call per SUBMITTED
 * frame — hands it back after the shared clock's depth (VsgRetireRing::kRetireRingDepth). The queue belongs to the pool on purpose. It used to
 * live in the SceneBridge that dropped the slot, and that caller is destroyed by the very teardown
 * that drops it (an offscreen target rebuilt at a new size, a pass retargeted, a target released):
 * the slots it had parked — one per retained drawable — died with it and never came back to the
 * session's pool, which then allocated a fresh chunk (a new buffer) whenever its free list ran out.
 *
 * No device is needed for this half: the countdown is arithmetic, and the pool is constructible
 * without a device (its reserve() refuses, which is exactly why the other half — a slot actually
 * being handed out again after the countdown — is not tested here: it needs a device).
 */

#include <gtest/gtest.h>

#include <memory>

#include <vine/vsg/VsgDrawBlockPool.hpp>
#include <vine/vsg/VsgRetireRing.hpp>

#include <vsg/vk/Device.h>

using vine::vsg::VsgDrawBlockPool;

namespace
{

/// A pool with no device: enough for the retired queue, not for reserving.
std::shared_ptr<VsgDrawBlockPool> makeQueueOnlyPool()
{
    ::vsg::ref_ptr<::vsg::Device> no_device;
    return VsgDrawBlockPool::create(no_device);
}

/// Advances the queue by one COMMITTED frame: the token states what the test is simulating.
void commitOneFrame(const std::shared_ptr<VsgDrawBlockPool>& pool)
{
    pool->advanceRetired(vine::vsg::FrameCommit::submitted());
}

} // namespace

TEST(DrawBlockPoolRetireTest, ARetiredSlotWaitsExactlyTheFramesThatMayStillBindIt)
{
    const auto pool = makeQueueOnlyPool();
    EXPECT_EQ(pool->stats().retired, 0u);

    const VsgDrawBlockPool::Slot slot{ 0u, 7u };
    pool->retire(slot);
    EXPECT_EQ(pool->stats().retired, 1u) << "a valid slot goes to the retired queue";

    // It must not be usable before the countdown ends: the frames in flight are still reading it.
    for (std::uint32_t i = 1; i < vine::vsg::VsgRetireRing::kRetireRingDepth; ++i) {
        commitOneFrame(pool);
        EXPECT_EQ(pool->stats().retired, 1u)
            << "after " << i << " submitted frame(s) the slot is still in flight";
    }
    commitOneFrame(pool);
    EXPECT_EQ(pool->stats().retired, 0u)
        << "and the depth-th advance is what releases it to the free list";
}

TEST(DrawBlockPoolRetireTest, AnInvalidSlotIsNeverQueued)
{
    const auto pool = makeQueueOnlyPool();

    pool->retire(VsgDrawBlockPool::Slot{}); // no chunk: "no slot"
    EXPECT_EQ(pool->stats().retired, 0u) << "a slot nobody reserved is not something to wait out";
    EXPECT_EQ(pool->stats().reserved, 0u);
}

TEST(DrawBlockPoolRetireTest, TheQueueDrainsEachSlotOnItsOwnCountdown)
{
    const auto pool = makeQueueOnlyPool();

    pool->retire(VsgDrawBlockPool::Slot{ 0u, 0u });
    commitOneFrame(pool); // the first slot is one frame into its countdown
    pool->retire(VsgDrawBlockPool::Slot{ 0u, 1u });
    EXPECT_EQ(pool->stats().retired, 2u);

    // Three more advances bring the first slot's four to an end; the second, retired one advance
    // later, is still one short -- the countdown is per slot, not per queue.
    for (std::uint32_t i = 1; i < vine::vsg::VsgRetireRing::kRetireRingDepth; ++i) {
        commitOneFrame(pool);
    }
    EXPECT_EQ(pool->stats().retired, 1u) << "the slot retired a frame later is a frame behind";
    commitOneFrame(pool);
    EXPECT_EQ(pool->stats().retired, 0u) << "and its own depth worth of advances release it";
}

TEST(DrawBlockPoolRetireTest, AdvancingAnEmptyQueueIsANoOp)
{
    const auto pool = makeQueueOnlyPool();
    for (int i = 0; i < 4; ++i) {
        commitOneFrame(pool);
    }
    EXPECT_EQ(pool->stats().retired, 0u);
    EXPECT_EQ(pool->stats().reserved, 0u);
    // A pool with no device allocated nothing, so nothing was released either.
    EXPECT_EQ(pool->stats().chunks, 0u);
    EXPECT_EQ(pool->stats().capacity, 0u);
}
