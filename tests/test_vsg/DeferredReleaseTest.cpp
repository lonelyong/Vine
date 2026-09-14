/**
 * @brief The deferral clock the rings and the slot pool run on (VsgDeferredRelease).
 *
 * A value whose GPU handle a submitted frame may still name cannot be released the moment it is
 * dropped, so it is PARKED and released once each frame in flight has been re-recorded. Three users
 * share that rule: the session's retire ring (replaced render passes / framebuffers / program nodes
 * and the per-slot retained nodes) and the per-draw slot pool. This pins the three things a user of
 * the clock depends on:
 *
 *  1. the countdown is kDeferredReleaseFrames, no matter who parks;
 *  2. values come back in the order they were parked, one frame group at a time;
 *  3. the release callback runs for each value before it is destroyed — that is where a slot goes
 *     back to its pool's free list, and a callback that ran after the value was gone would have
 *     nothing to hand back.
 */

#include <gtest/gtest.h>

#include <vine/vsg/VsgDeferredRelease.hpp>
#include <vine/vsg/VsgRetireRing.hpp>

#include <string>
#include <vector>

using vine::vsg::VsgDeferredRelease;

TEST(DeferredReleaseTest, AValueIsReleasedAfterTheFramesThatMayStillReferenceIt)
{
    VsgDeferredRelease<std::string> clock;
    std::vector<std::string>        released;

    clock.park("a");
    ASSERT_EQ(clock.parkedCount(), 1u);

    for (std::size_t i = 1; i < vine::vsg::kDeferredReleaseFrames; ++i) {
        clock.advance([&](std::string& value) { released.push_back(value); });
        EXPECT_EQ(clock.parkedCount(), 1u) << "still in flight after " << i << " advance(s)";
        EXPECT_TRUE(released.empty());
    }
    clock.advance([&](std::string& value) { released.push_back(value); });
    EXPECT_EQ(clock.parkedCount(), 0u);
    ASSERT_EQ(released.size(), 1u);
    EXPECT_EQ(released.front(), "a");
}

TEST(DeferredReleaseTest, TheCallbackRunsOncePerValueInParkOrder)
{
    VsgDeferredRelease<int> clock;
    std::vector<int>        released;

    clock.park(1);
    clock.park(2);
    clock.advance(); // moves to the next bucket: the two parked above are still waiting
    clock.park(3);

    // One frame GROUP at a time: the two parked before the advance come back together, and the one
    // parked after it is a frame behind — the clock's unit is "the values parked during one frame".
    for (std::size_t i = 1; i < vine::vsg::kDeferredReleaseFrames; ++i) {
        clock.advance([&](int& value) { released.push_back(value); });
    }
    ASSERT_EQ(released.size(), 2u) << "the first frame's values are due, the second frame's are not";
    EXPECT_EQ(released[0], 1) << "values come back in the order they were parked";
    EXPECT_EQ(released[1], 2);
    EXPECT_EQ(clock.parkedCount(), 1u);

    clock.advance([&](int& value) { released.push_back(value); });
    ASSERT_EQ(released.size(), 3u);
    EXPECT_EQ(released[2], 3);
    EXPECT_EQ(clock.parkedCount(), 0u);
}

TEST(DeferredReleaseTest, NoCallbackMeansTheValuesAreSimplyDropped)
{
    // The ring's use: nothing to hand back, the value's destructor does the work.
    VsgDeferredRelease<std::string> clock;
    clock.park("gone");

    std::size_t released = 0;
    for (std::size_t i = 0; i < vine::vsg::kDeferredReleaseFrames; ++i) {
        released += clock.advance();
    }
    EXPECT_EQ(released, 1u) << "advance() reports how many it released";
    EXPECT_EQ(clock.parkedCount(), 0u);
}

TEST(DeferredReleaseTest, AdvancingAnEmptyClockIsANoOp)
{
    VsgDeferredRelease<int> clock;
    for (std::size_t i = 0; i < vine::vsg::kDeferredReleaseFrames * 2u; ++i) {
        EXPECT_EQ(clock.advance(), 0u);
    }
    EXPECT_EQ(clock.parkedCount(), 0u);
}

TEST(DeferredReleaseTest, TheRingRunsOnThatSameClock)
{
    // One depth for every user: the ring's published name resolves to the clock's constant.
    static_assert(vine::vsg::VsgRetireRing::kRetireRingDepth == vine::vsg::kDeferredReleaseFrames);
    EXPECT_EQ(vine::vsg::VsgRetireRing::kRetireRingDepth, vine::vsg::kDeferredReleaseFrames);
}
