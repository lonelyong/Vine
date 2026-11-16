/**
 * @brief The device-free core of the rewritten vsg backend: the P0 contract, as tables.
 *
 * This file pins the decisions the backend settles BEFORE any GPU code exists (see `.ai/design/vsg-backend.md`
 * §2.5) and that belong to ITS portable half rather than to the SDK seam:
 *
 *   * the protocol is a state machine that answers only "is this call legal" (P0-3) - so its rules are
 *     exercised as a transition table rather than as branches inside the renderer;
 *   * a frame's plan owns its own storage, so a borrowed host span cannot outlive the call that lent it
 *     (P0-1) - the arena's contract is "growth preserves, reset invalidates";
 *   * the cross-check between the retention queue and the observations that feed it;
 *   * the evidence format a self-test prints, and the depth probe a readback is judged with.
 *
 * The types the SDK seam shares with the other backend - the target plan, the retirement queue, the key audit,
 * readback and the device clock - are pinned in `tests/BackendSeamTest.cpp`, which every test binary that links
 * the seam compiles: a seam fact belongs in a binary that carries both backends (see
 * `.ai/research/graphics-2026-11-16-redesign.md` §3.2.1).
 *
 * Every case here is device-free by construction: the types under test never dereference a GPU object
 * and never include anything from `vsg::`. The suites are named `Core*` after the layer they pin.
 */

#include <gtest/gtest.h>

#include <cstdint>
#include <span>
#include <string>
#include <vector>

#include <vine/graphics/backend/DepthProbe.hpp>
#include <vine/graphics/backend/FrameArena.hpp>
#include <vine/graphics/backend/FrameTimeline.hpp>
#include <vine/graphics/backend/Observe.hpp>
#include <vine/graphics/backend/PhaseTable.hpp>
#include <vine/graphics/backend/RetirementQueue.hpp>
#include <vine/backend_vsg/internal/DeviceRequirements.hpp>

using vn::graphics::backend::FrameArena;
using vn::graphics::backend::FrameTimeline;
using vn::graphics::backend::Observe;
using vn::graphics::backend::Phase;
using vn::graphics::backend::PhaseTable;
using vn::graphics::backend::RetirementQueue;

TEST(CoreTimelineTest, SubmittedAndCompletedAreDifferentClocks)
{
    FrameTimeline timeline;
    EXPECT_FALSE(timeline.hasOpenFrame());

    const auto token = timeline.begin();
    EXPECT_TRUE(timeline.hasOpenFrame());
    EXPECT_TRUE(timeline.isOpenFrame(token));

    timeline.submitted(token);
    EXPECT_FALSE(timeline.hasOpenFrame());
    EXPECT_EQ(timeline.submittedFrame(), 1u);
    EXPECT_EQ(timeline.completedFrame(), 0u); // submitted is not completed

    timeline.completeUpTo(1);
    EXPECT_EQ(timeline.completedFrame(), 1u);

    // Completion is a fact about the past: a lower report cannot un-do it.
    timeline.completeUpTo(0);
    EXPECT_EQ(timeline.completedFrame(), 1u);
}

TEST(CoreTimelineTest, AStaleTokenDoesNotAdvanceTheTimeline)
{
    FrameTimeline timeline;
    const auto    first = timeline.begin();
    timeline.submitted(first);

    const auto second = timeline.begin();
    EXPECT_EQ(second.frame, 2u);

    // A token from the previous frame must not commit the open one.
    timeline.submitted(first);
    EXPECT_TRUE(timeline.hasOpenFrame());
    EXPECT_EQ(timeline.submittedFrame(), 1u);
}

TEST(CoreTimelineTest, AnAbandonedFrameIsOverWithoutBecomingASubmission)
{
    FrameTimeline timeline;
    const auto    token = timeline.begin();
    timeline.abandoned(token);

    // The frame is over - the next one can be opened - but the submitted watermark is a count of
    // SUBMISSIONS, so it does not move: nothing was handed to the queue.
    EXPECT_FALSE(timeline.hasOpenFrame());
    EXPECT_EQ(timeline.submittedFrame(), 0u);

    // NUMBERS ARE MINTED AT OPENING AND NEVER REUSED (the same rule the residency table's claim tokens
    // follow): a frame that never reached the queue still spent its number. That is what lets two drivers
    // hold open frames at the same time without either one's token naming the other's frame - and it is
    // why a token is not a receipt either way: an abandoned token is stale for submission, and the number
    // it carried never comes back.
    const auto next = timeline.begin();
    EXPECT_NE(next.frame, token.frame);
    EXPECT_GT(next.frame, token.frame);
    timeline.abandoned(next);
    EXPECT_EQ(timeline.submittedFrame(), 0u);

    // A real submission moves it TO THE NUMBER THAT WAS SUBMITTED (numbers are minted, so the abandoned
    // frames spent 1 and 2 and this one is 3).
    const auto real = timeline.begin();
    timeline.submitted(real);
    EXPECT_EQ(timeline.submittedFrame(), real.frame);
    EXPECT_NE(real.frame, token.frame);

    // The dating a park during an abandoned frame gets: the last frame that really was submitted - which
    // is what makes the object releasable at all (nothing in flight names the abandoned frame's objects).
    timeline.abandoned(timeline.begin());
    EXPECT_EQ(timeline.submittedFrame(), real.frame);
    EXPECT_EQ(FrameTimeline::retirePoint(timeline.submittedFrame(), 3), real.frame + 4U);
}

TEST(CoreTimelineTest, TwoDriversMayHoldAFrameOpenAtTheSameTime)
{
    // THE DEVICE TIMELINE WITH TWO FRAME DRIVERS (S6c-2): each driver opens its own frame, and the two are
    // two frames - not one shared token. The old single-slot form handed the second caller the first's
    // token, which made two drivers collapse into one frame number in every watermark comparison.
    FrameTimeline timeline;
    const auto    first  = timeline.begin();
    const auto    second = timeline.begin();

    EXPECT_NE(first.frame, second.frame);
    EXPECT_TRUE(timeline.hasOpenFrame());
    EXPECT_TRUE(timeline.isOpenFrame(first));
    EXPECT_TRUE(timeline.isOpenFrame(second));
    EXPECT_EQ(timeline.current().frame, second.frame); // the most recently opened one

    // Committing one leaves the other open: the submission watermark is the highest frame handed to a
    // queue, and a stale token for the *other* open frame is not stale - it is still open.
    timeline.submitted(second);
    EXPECT_TRUE(timeline.isOpenFrame(first));
    EXPECT_FALSE(timeline.isOpenFrame(second));
    EXPECT_EQ(timeline.submittedFrame(), second.frame);

    timeline.abandoned(first);
    EXPECT_FALSE(timeline.hasOpenFrame());
    EXPECT_EQ(timeline.submittedFrame(), second.frame);

    // A later frame is minted after both, even though the second was the only submission.
    const auto third = timeline.begin();
    EXPECT_GT(third.frame, second.frame);
    timeline.abandoned(third);
}

TEST(CoreTimelineTest, TheRetirePointIsOneMoreThanTheSlotCount)
{
    // Derived, not magic: the slot that could still reference a parked object has been recycled (and so
    // had its fence waited) by then.
    EXPECT_EQ(FrameTimeline::retirePoint(1, 3), 5u);
    EXPECT_EQ(FrameTimeline::retirePoint(0, 1), 2u);
}

TEST(CoreArenaTest, GrowthPreservesWhatWasAlreadyHandedOut)
{
    FrameArena             arena(64);       // deliberately tiny, so the copy below must grow it
    std::vector<int>       borrowed(64, 7); // 256 bytes: more than the arena reserved
    const std::vector<int> source{ 101, 102, 103 };

    const auto first_copy = arena.copy<int>(std::span<const int>(source));
    ASSERT_EQ(first_copy.size(), 3u);

    // A big copy forces the arena to grow; the earlier span must still read the same values, because
    // growth moves the bytes rather than starting over.
    const auto large = arena.copy<int>(std::span<const int>(borrowed));
    ASSERT_EQ(large.size(), borrowed.size());

    EXPECT_GT(arena.allocations(), 0u);
    EXPECT_EQ(first_copy[0], 101);
    EXPECT_EQ(first_copy[2], 103);
    EXPECT_EQ(large[63], 7);

    // reset() is the one place a span dies: the cursor goes back, the capacity does not.
    const std::size_t reserved = arena.bytesReserved();
    arena.reset();
    EXPECT_EQ(arena.bytesUsed(), 0u);
    EXPECT_EQ(arena.bytesReserved(), reserved);
}

TEST(CoreArenaTest, ASteadyFrameDoesNotGrowTheArena)
{
    FrameArena        arena(1024);
    const std::vector sizes{ 16, 64, 256 };

    arena.reset();
    for (const int size : sizes) {
        auto values = arena.makeArray<int>(static_cast<std::size_t>(size));
        values[0]   = size;
    }
    const std::size_t after_first_frame = arena.allocations();

    arena.reset();
    for (const int size : sizes) {
        auto values = arena.makeArray<int>(static_cast<std::size_t>(size));
        values[0]   = size;
    }
    // Repeating the same frame reuses the capacity: this is the "steady frame allocates nothing" rule.
    EXPECT_EQ(arena.allocations(), after_first_frame);
}

TEST(CoreObserveTest, TheAggregatesMustAgreeWithTheQueueThatProducedThem)
{
    FrameTimeline   timeline;
    RetirementQueue queue(3);
    Observe         observe;

    EXPECT_TRUE(observe.agreesWith(queue));

    // One object released by the queue, accounted for in both places: still agreeing.
    const auto token = timeline.begin();
    timeline.submitted(token);
    EXPECT_TRUE(queue.retire(timeline, [] {}));
    timeline.completeUpTo(queue.retirePoint(timeline));
    queue.advance(timeline);
    observe.retention().released_nodes = queue.released();
    observe.retention().device_waits   = queue.deviceWaits();
    EXPECT_TRUE(observe.agreesWith(queue));

    // A number incremented in one place and forgotten in the other is exactly what the cross-check is
    // for: the counter would otherwise keep saying "no leaks" while nobody updates it.
    observe.retention().released_nodes = 7;
    EXPECT_FALSE(observe.agreesWith(queue));
}

TEST(CorePhaseTableTest, PhasesPrintTheEvidenceFormatAndOnlyCloseWhenEverythingPassed)
{
    int counter = 0;

    PhaseTable table;
    table.add(Phase{ "a passing phase", [] { return true; } });
    table.add(Phase{ "a failing phase", [] { return false; } });
    table.add(Phase{ "a counter that stays put",
                     [] { return true; },
                     [&counter] { return static_cast<std::uint64_t>(counter); },
                     [](std::uint64_t before, std::uint64_t after) { return before == after; } });
    table.add(Phase{ "a phase that moved a counter it promised not to",
                     [&counter] {
                         ++counter;
                         return true;
                     },
                     [&counter] { return static_cast<std::uint64_t>(counter); },
                     [](std::uint64_t before, std::uint64_t after) { return before == after; } });
    table.add(Phase{ "a nameless phase is ignored", {} });

    ASSERT_EQ(table.size(), 4u);

    const auto report = table.runAll();
    ASSERT_EQ(report.lines.size(), 4u);
    EXPECT_EQ(report.lines[0], "[selftest] a passing phase");
    EXPECT_EQ(report.lines[1], "[selftest] a failing phase FAILED: assertion failed");
    EXPECT_EQ(report.lines[2], "[selftest] a counter that stays put");
    // Asserting correctly is not enough: a phase that rebuilt everything while claiming it would not is
    // exactly the regression the counter expectation exists to catch.
    EXPECT_EQ(report.lines[3], "[selftest] a phase that moved a counter it promised not to FAILED: counter expectation not met");
    EXPECT_FALSE(report.ok());
    EXPECT_EQ(report.passed, 2u);
    EXPECT_EQ(report.failed, 2u);

    // A table that passed entirely ends with the line the evidence script expects.
    PhaseTable passing;
    passing.add(Phase{ "only phase", [] { return true; } });
    const auto clean = passing.runAll();
    ASSERT_EQ(clean.lines.size(), 2u);
    EXPECT_EQ(clean.lines[1], "[selftest] done");
    EXPECT_TRUE(clean.ok());
}

TEST(CoreDeviceRequirementsTest, TheVersionFloorIgnoresThePatch)
{
    using namespace vn::graphics::backend;

    // Below the floor is refused - including a device whose patch is enormous, because the policy is
    // about major/minor capability and drivers report their patch freely.
    EXPECT_FALSE(supportsRequiredVersion(makeApiVersion(1, 3)));
    EXPECT_FALSE(supportsRequiredVersion(makeApiVersion(1, 3, 999)));
    EXPECT_FALSE(supportsRequiredVersion(makeApiVersion(0, 99)));

    EXPECT_TRUE(supportsRequiredVersion(kRequiredApiVersion));
    EXPECT_TRUE(supportsRequiredVersion(makeApiVersion(1, 4, 3)));
    EXPECT_TRUE(supportsRequiredVersion(makeApiVersion(1, 5)));
}

TEST(CoreDeviceRequirementsTest, EveryRequiredFeatureIsNamedAndCounted)
{
    using namespace vn::graphics::backend;

    DeviceFacts bare;
    bare.api_version = kRequiredApiVersion;

    // The right version with no features is still refused - the two halves are both required.
    EXPECT_EQ(missingFeatureCount(bare), kDeviceFeatureCount);
    EXPECT_FALSE(satisfiesRequirements(bare));

    for (std::size_t i = 0; i < kDeviceFeatureCount; ++i) {
        const auto feature = static_cast<DeviceFeature>(i);
        EXPECT_STRNE(featureName(feature), "") << "feature " << i << " has no name to report";
        bare.note(feature);
    }

    // The features alone are not enough either: the extensions carry the states themselves, and a device that
    // offers every feature bit but not the extension may not be told to set those states at all.
    EXPECT_EQ(missingFeatureCount(bare), 0U);
    EXPECT_EQ(missingExtensionCount(bare), kDeviceExtensionCount);
    EXPECT_FALSE(satisfiesRequirements(bare));

    for (std::size_t i = 0; i < kDeviceExtensionCount; ++i) {
        const auto extension = static_cast<DeviceExtension>(i);
        EXPECT_STRNE(extensionName(extension), "") << "extension " << i << " has no name to report";
        bare.note(extension);
    }

    // Every feature and every extension: one bit each, so nothing aliases anything else.
    EXPECT_EQ(missingExtensionCount(bare), 0U);
    EXPECT_TRUE(satisfiesRequirements(bare));
    EXPECT_STREQ(featureName(DeviceFeature::ExtendedDynamicState3PolygonMode), "extendedDynamicState3PolygonMode");
    EXPECT_STREQ(extensionName(DeviceExtension::ExtendedDynamicState3), "VK_EXT_extended_dynamic_state3");
    EXPECT_FALSE(bare.has(DeviceFeature::Count)) << "Count is the table's size, not a feature";
    EXPECT_FALSE(bare.has(DeviceExtension::Count)) << "Count is the table's size, not an extension";
}

TEST(CoreDeviceRequirementsTest, AFeatureOffMeansTheDeviceIsRefused)
{
    using namespace vn::graphics::backend;

    DeviceFacts facts;
    facts.api_version = makeApiVersion(1, 4);
    for (std::size_t i = 0; i < kDeviceFeatureCount; ++i) {
        facts.note(static_cast<DeviceFeature>(i));
    }
    for (std::size_t i = 0; i < kDeviceExtensionCount; ++i) {
        facts.note(static_cast<DeviceExtension>(i));
    }
    ASSERT_TRUE(satisfiesRequirements(facts));

    // Take one away: the verdict flips, and the count says how many are missing (the diagnostic the
    // session reports is built from exactly this).
    const auto index = static_cast<std::uint32_t>(DeviceFeature::IndependentBlend);
    facts.features &= ~(1U << index);
    EXPECT_EQ(missingFeatureCount(facts), 1U);
    EXPECT_FALSE(satisfiesRequirements(facts));
    EXPECT_FALSE(facts.has(DeviceFeature::IndependentBlend));

    // And the version alone cannot rescue it.
    facts.api_version = makeApiVersion(1, 6);
    EXPECT_FALSE(satisfiesRequirements(facts));

    // The same for an extension: one missing name flips the verdict, and the count names it as the missing
    // half - which is the difference between "this device cannot deliver the state" and "this backend never
    // asked for the name that carries it".
    facts.note(DeviceFeature::IndependentBlend);
    facts.api_version = makeApiVersion(1, 4);
    ASSERT_TRUE(satisfiesRequirements(facts));
    const auto extension_index = static_cast<std::uint32_t>(DeviceExtension::ExtendedDynamicState3);
    facts.extensions &= ~(1U << extension_index);
    EXPECT_EQ(missingExtensionCount(facts), 1U);
    EXPECT_EQ(missingFeatureCount(facts), 0U) << "the two halves are counted separately on purpose";
    EXPECT_FALSE(satisfiesRequirements(facts));
    EXPECT_FALSE(facts.has(DeviceExtension::ExtendedDynamicState3));
}

TEST(CoreDepthProbeTest, AnInvalidProbeAnswersNothingRatherThanZeroes)
{
    using namespace vn::graphics::backend;

    const DepthProbe empty;
    EXPECT_FALSE(empty.valid());
    EXPECT_EQ(empty.width(), 0);
    EXPECT_EQ(empty.height(), 0);
    EXPECT_FLOAT_EQ(empty.depthAt(0, 0), 0.0F);
    EXPECT_EQ(empty.countNear(0.0F, 0.01F), 0U);

    // A probe whose value count does not match its extent is not a probe: it would answer with whatever the
    // coordinates happened to index.
    const DepthProbe short_image(4, 4, std::vector<float>(15U, 0.5F));
    EXPECT_FALSE(short_image.valid());
}

TEST(CoreDepthProbeTest, TheValuesAreRowMajorAndCountedWithinATolerance)
{
    using namespace vn::graphics::backend;

    std::vector<float> values(4U * 3U, 0.0F);
    values[1U * 4U + 2U] = 0.75F; // row 1, column 2
    const DepthProbe probe(4, 3, std::move(values));

    ASSERT_TRUE(probe.valid());
    EXPECT_EQ(probe.width(), 4);
    EXPECT_EQ(probe.height(), 3);
    EXPECT_FLOAT_EQ(probe.depthAt(2, 1), 0.75F) << "row-major, like the copy that filled it";
    EXPECT_FLOAT_EQ(probe.depthAt(1, 1), 0.0F);
    EXPECT_FLOAT_EQ(probe.depthAt(-1, 0), 0.0F) << "outside the image answers 0, and valid() says so first";
    EXPECT_FLOAT_EQ(probe.depthAt(4, 0), 0.0F);
    EXPECT_EQ(probe.countNear(0.0F, 0.01F), 11U);
    EXPECT_EQ(probe.countNear(0.75F, 0.001F), 1U) << "the tolerance is absolute: a clear value and a written depth do not arrive by the same path";
    EXPECT_EQ(probe.countNear(0.75F, 0.0F), 1U);
}
