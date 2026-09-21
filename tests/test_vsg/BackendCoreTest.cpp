/**
 * @brief The device-free core of the rewritten vsg backend: the P0 contract, as tables.
 *
 * This file pins the four decisions that must be settled BEFORE any GPU code exists (see
 * `.ai/design/vsg-reimplementation.md` §2.5):
 *
 *   * the protocol is a state machine that answers only "is this call legal" (P0-3) - so its rules are
 *     exercised as a transition table rather than as branches inside the renderer;
 *   * a frame's plan owns its own storage, so a borrowed host span cannot outlive the call that lent it
 *     (P0-1) - the arena's contract is "growth preserves, reset invalidates";
 *   * what changes a target's GPU objects is a decision function with three distinct answers, and
 *     extent is not identity (P0-5) - so "a resize must not rebuild" is checkable without a device;
 *   * deferred destruction is ordered by one timeline, and completion is evidence rather than
 *     assumption (P0-9 / D5).
 *
 * Every case here is device-free by construction: the types under test never dereference a GPU object
 * and never include anything from `vsg::`. The suites are named `Core*` after the layer they pin.
 */

#include <gtest/gtest.h>

#include <array>
#include <cstdint>
#include <span>
#include <string>
#include <vector>

#include <vine/vsg/core/DepthProbe.hpp>
#include <vine/vsg/core/Readback.hpp>
#include <vine/vsg/core/DeviceRequirements.hpp>
#include <vine/vsg/core/FrameArena.hpp>
#include <vine/vsg/core/FrameTimeline.hpp>
#include <vine/vsg/core/Keys.hpp>
#include <vine/vsg/core/Observe.hpp>
#include <vine/vsg/core/PhaseTable.hpp>
#include <vine/vsg/core/Protocol.hpp>
#include <vine/vsg/core/RetirementQueue.hpp>
#include <vine/vsg/core/TargetPlan.hpp>

using vine::graphics::RenderTarget;
using vine::vsg::core::CallKind;
using vine::vsg::core::Decision;
using vine::vsg::core::FrameArena;
using vine::vsg::core::FrameTimeline;
using vine::vsg::core::LoadOpVariantKey;
using vine::vsg::core::Observe;
using vine::vsg::core::Phase;
using vine::vsg::core::PhaseTable;
using vine::vsg::core::PipelineKey;
using vine::vsg::core::Protocol;
using vine::vsg::core::RepairReason;
using vine::vsg::core::RetirementQueue;
using vine::vsg::core::ReadbackRequest;
using vine::vsg::core::ReadbackState;
using vine::vsg::core::TargetAction;
using vine::vsg::core::TargetDecision;
using vine::vsg::core::TargetDesc;
using vine::vsg::core::TargetInstance;
using vine::vsg::core::Verdict;

namespace
{

/// @brief One line of the protocol's transition table: a call, and what it must answer.
struct ProtocolStep
{
    CallKind kind;
    Verdict  verdict;
    bool     report;
};

/// @brief Plays @p steps against a fresh protocol and checks every answer.
void expectProtocol(const std::vector<ProtocolStep>& steps)
{
    Protocol protocol;
    int      index = 0;
    for (const ProtocolStep& step : steps)
    {
        const Decision decision = protocol.onCall(step.kind);
        EXPECT_EQ(static_cast<int>(decision.verdict), static_cast<int>(step.verdict))
            << "step " << index << " verdict";
        EXPECT_EQ(decision.report, step.report) << "step " << index << " report";
        ++index;
    }
}

}  // namespace

TEST(CoreProtocolTest, AHealthyFrameIsAllowedThroughout)
{
    // The order the engine drives: frame, scope, state, draw, close, present.
    expectProtocol({
        {CallKind::BeginFrame, Verdict::Allow, false},
        {CallKind::BeginPass, Verdict::Allow, false},
        {CallKind::SetScopeAttribute, Verdict::Allow, false},
        {CallKind::Draw, Verdict::Allow, false},
        {CallKind::EndPass, Verdict::Allow, false},
        {CallKind::EndFrame, Verdict::Allow, false},
        {CallKind::SwapBuffers, Verdict::Allow, false},
    });
}

TEST(CoreProtocolTest, ADrawOutsideAScopeIsRefusedOncePerFrameNotOncePerCall)
{
    // The host that lost track of its scopes hits this on every pass, so the report is per frame.
    expectProtocol({
        {CallKind::BeginFrame, Verdict::Allow, false},
        {CallKind::Draw, Verdict::Refuse, true},   // first: reported
        {CallKind::Draw, Verdict::Refuse, false},  // same episode: refused, silent
        {CallKind::SwapBuffers, Verdict::Allow, false},
        {CallKind::BeginFrame, Verdict::Allow, false},
        {CallKind::Draw, Verdict::Refuse, true},   // new frame: the episode re-armed
    });
}

TEST(CoreProtocolTest, SettersOutsideAScopeAreInertNotViolations)
{
    // The contract is explicit: state announced with no scope open is inert. Reporting it as misuse
    // would be noise on a legal call.
    Protocol protocol;
    EXPECT_EQ(static_cast<int>(protocol.onCall(CallKind::SetScopeAttribute).verdict),
              static_cast<int>(Verdict::Drop));
    EXPECT_FALSE(protocol.onCall(CallKind::SetScopeAttribute).report);
    EXPECT_EQ(protocol.refusalCount(), 0u);  // dropped, but not refused
}

TEST(CoreProtocolTest, ANestedScopeIsRefusedWithoutClosingTheOuterOne)
{
    Protocol protocol;
    (void)protocol.onCall(CallKind::BeginFrame);
    EXPECT_EQ(static_cast<int>(protocol.onCall(CallKind::BeginPass).verdict), static_cast<int>(Verdict::Allow));
    EXPECT_EQ(static_cast<int>(protocol.onCall(CallKind::BeginPass).verdict), static_cast<int>(Verdict::Refuse));
    EXPECT_TRUE(protocol.scopeOpen());  // the outer scope is still the open one
    EXPECT_EQ(static_cast<int>(protocol.onCall(CallKind::EndPass).verdict), static_cast<int>(Verdict::Allow));
}

TEST(CoreProtocolTest, AnUnpairedEndPassAndASwapInsideAScopeAreBothRefused)
{
    Protocol protocol;
    (void)protocol.onCall(CallKind::BeginFrame);
    EXPECT_EQ(static_cast<int>(protocol.onCall(CallKind::EndPass).verdict), static_cast<int>(Verdict::Refuse));
    (void)protocol.onCall(CallKind::BeginPass);
    EXPECT_EQ(static_cast<int>(protocol.onCall(CallKind::SwapBuffers).verdict),
              static_cast<int>(Verdict::Refuse));  // a scope is still open
}

TEST(CoreProtocolTest, AReleasedAnnouncedTargetIsDroppedAndTheScopeDiesOnce)
{
    int target = 0;

    Protocol protocol;
    (void)protocol.onCall(CallKind::BeginFrame);
    (void)protocol.onCall(CallKind::BeginPass);
    protocol.noteAnnouncedTarget(&target);

    (void)protocol.onCall(CallKind::ReleaseRenderTarget);
    protocol.noteTargetReleased(&target);

    // The contract: the announcement is DROPPED (the pointer may be destroyed), and the scope is dead
    // rather than redirected to the window.
    EXPECT_EQ(protocol.announcedTarget(), nullptr);
    EXPECT_TRUE(protocol.announcedTargetReleased());

    const Decision first_attr = protocol.onCall(CallKind::SetScopeAttribute);
    EXPECT_EQ(static_cast<int>(first_attr.verdict), static_cast<int>(Verdict::Drop));
    EXPECT_TRUE(first_attr.report);  // reported for this scope, once

    const Decision second_attr = protocol.onCall(CallKind::SetScopeAttribute);
    EXPECT_FALSE(second_attr.report);  // same episode

    const Decision draw = protocol.onCall(CallKind::Draw);
    EXPECT_EQ(static_cast<int>(draw.verdict), static_cast<int>(Verdict::Refuse));
    EXPECT_FALSE(draw.report);  // the scope already reported its condition

    // A new scope is a new episode: the shell is gone, but the rule still holds.
    (void)protocol.onCall(CallKind::EndPass);
    (void)protocol.onCall(CallKind::BeginPass);
    protocol.noteAnnouncedTarget(&target);
    protocol.noteTargetReleased(&target);
    EXPECT_TRUE(protocol.onCall(CallKind::Draw).report);
}

TEST(CoreProtocolTest, ReleasingSomeOtherTargetLeavesTheScopeAlone)
{
    int announced = 0;
    int other     = 0;

    Protocol protocol;
    (void)protocol.onCall(CallKind::BeginFrame);
    (void)protocol.onCall(CallKind::BeginPass);
    protocol.noteAnnouncedTarget(&announced);
    protocol.noteTargetReleased(&other);

    EXPECT_EQ(protocol.announcedTarget(), &announced);
    EXPECT_FALSE(protocol.announcedTargetReleased());
    EXPECT_EQ(static_cast<int>(protocol.onCall(CallKind::Draw).verdict), static_cast<int>(Verdict::Allow));
}

namespace
{

/// @brief One line of the target-plan table.
struct TargetCase
{
    const char*  what;
    TargetDesc   current;
    bool         built;
    bool         invalidated;
    TargetDesc   wanted;
    TargetAction action;
    RepairReason reason;
};

/// @brief The compatible shape most cases share: one RGBA8 colour attachment, no depth.
vine::vsg::core::TargetShape singleColorShape()
{
    vine::vsg::core::TargetShape shape;
    shape.color_formats.push_back(vine::graphics::RenderTarget::ColorFormat::RGBA8);
    return shape;
}

/// @brief A shape with a depth attachment, used to move compatibility.
vine::vsg::core::TargetShape colorAndDepthShape()
{
    vine::vsg::core::TargetShape shape = singleColorShape();
    shape.depth_format                 = vine::graphics::RenderTarget::DepthFormat::D32;
    return shape;
}

}  // namespace

TEST(CoreTargetPlanTest, TheActionTableIsDecidedByShapeSizeAndBuildState)
{
    const vine::vsg::core::TargetShape single  = singleColorShape();
    const vine::vsg::core::TargetShape with_d  = colorAndDepthShape();

    TargetDesc built_at_512{512, 256, single};
    TargetDesc built_at_1024{1024, 256, single};

    const std::vector<TargetCase> cases{
        {"same size, same shape", built_at_512, true, false, built_at_512, TargetAction::None, RepairReason::None},
        {"new extent, same shape", built_at_512, true, false, built_at_1024, TargetAction::ResizeInPlace,
         RepairReason::None},
        {"new shape at the same size", built_at_512, true, false, TargetDesc{512, 256, with_d},
         TargetAction::Rebuild, RepairReason::None},
        {"new shape AND new extent: compatibility wins", built_at_512, true, false, TargetDesc{1024, 256, with_d},
         TargetAction::Rebuild, RepairReason::None},
        {"never built", built_at_512, false, false, built_at_512, TargetAction::Repair, RepairReason::Bootstrap},
        {"attachments invalidated", built_at_512, true, true, built_at_512, TargetAction::Repair,
         RepairReason::Bootstrap},
        {"resized to nothing", built_at_512, true, false, TargetDesc{0, 256, single}, TargetAction::Repair,
         RepairReason::SizeUnknown},
        {"laid out at zero", built_at_512, true, false, TargetDesc{512, 0, single}, TargetAction::Repair,
         RepairReason::SizeUnknown},
    };

    for (const TargetCase& test_case : cases)
    {
        TargetInstance instance;
        instance.desc                    = test_case.current;
        instance.built                   = test_case.built;
        instance.attachments_invalidated = test_case.invalidated;

        const TargetDecision decision = vine::vsg::core::planTarget(instance, test_case.wanted);
        EXPECT_EQ(static_cast<int>(decision.action), static_cast<int>(test_case.action)) << test_case.what;
        EXPECT_EQ(static_cast<int>(decision.reason), static_cast<int>(test_case.reason)) << test_case.what;
    }
}

TEST(CoreTargetPlanTest, AnUnusableExtentIsDistinguishableFromInvalidatedAttachments)
{
    // "Resized to nothing" and "attachments invalidated" are both repairs, and they have different
    // reasons precisely so a phase (and a log line) can tell them apart.
    TargetInstance instance;
    instance.desc  = TargetDesc{8, 8, singleColorShape()};
    instance.built = true;

    EXPECT_EQ(static_cast<int>(vine::vsg::core::planTarget(instance, TargetDesc{0, 0, {}}).reason),
              static_cast<int>(RepairReason::SizeUnknown));

    instance.attachments_invalidated = true;
    EXPECT_EQ(static_cast<int>(vine::vsg::core::planTarget(instance, instance.desc).reason),
              static_cast<int>(RepairReason::Bootstrap));
}

TEST(CoreTargetPlanTest, DepthPromotionBorrowingAndPreservationAreOneDecision)
{
    using vine::vsg::core::DepthFacts;
    using vine::vsg::core::DepthPlan;
    using vine::vsg::core::depthPlan;

    int lender = 0;

    // No depth at all: nothing to plan.
    {
        DepthFacts facts;
        const DepthPlan plan = depthPlan(facts);
        EXPECT_FALSE(plan.has_depth);
        EXPECT_FALSE(plan.sampleable);
    }

    // Promotion with nothing preserving it: sampleable.
    {
        DepthFacts facts;
        facts.has_depth   = true;
        facts.promotion   = true;
        const DepthPlan plan = depthPlan(facts);
        EXPECT_TRUE(plan.sampleable);
        EXPECT_FALSE(plan.preserve);
    }

    // A depth-preserving pass revokes the promotion for the whole target.
    {
        DepthFacts facts;
        facts.has_depth                  = true;
        facts.promotion                  = true;
        facts.any_pass_preserves_depth   = true;
        const DepthPlan plan             = depthPlan(facts);
        EXPECT_FALSE(plan.sampleable);
        EXPECT_TRUE(plan.preserve);
    }

    // A borrowed depth: the policy is the lender's, so the borrower neither samples nor preserves it.
    {
        DepthFacts facts;
        facts.has_depth = true;
        facts.promotion = true;
        facts.borrowed  = true;
        facts.source    = &lender;
        const DepthPlan plan = depthPlan(facts);
        EXPECT_TRUE(plan.borrowed);
        EXPECT_EQ(plan.source, &lender);
        EXPECT_FALSE(plan.sampleable);
        EXPECT_FALSE(plan.preserve);
    }
}

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
    EXPECT_EQ(timeline.completedFrame(), 0u);  // submitted is not completed

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

TEST(CoreTimelineTest, TheRetirePointIsOneMoreThanTheSlotCount)
{
    // Derived, not magic: the slot that could still reference a parked object has been recycled (and so
    // had its fence waited) by then.
    EXPECT_EQ(FrameTimeline::retirePoint(1, 3), 5u);
    EXPECT_EQ(FrameTimeline::retirePoint(0, 1), 2u);
}

TEST(CoreRetirementQueueTest, NothingIsReleasedBeforeTheEvidenceArrives)
{
    FrameTimeline    timeline;
    RetirementQueue  queue(3);
    int              releases = 0;

    const auto token = timeline.begin();
    timeline.submitted(token);  // submitted frame 1 -> retire at 1 + 3 + 1 = 5
    ASSERT_TRUE(queue.retire(timeline, [&releases] { ++releases; }));

    EXPECT_EQ(queue.retirePoint(timeline), 5u);
    EXPECT_EQ(queue.pending(), 1u);

    timeline.completeUpTo(4);
    queue.advance(timeline);
    EXPECT_EQ(releases, 0);  // not yet: the slot that recorded it may still be in flight
    EXPECT_EQ(queue.pending(), 1u);

    timeline.completeUpTo(5);
    queue.advance(timeline);
    EXPECT_EQ(releases, 1);
    EXPECT_EQ(queue.pending(), 0u);
    EXPECT_EQ(queue.released(), 1u);

    // The point of parking instead of idling: this path never stopped the device.
    EXPECT_EQ(queue.deviceWaits(), 0u);
}

TEST(CoreRetirementQueueTest, AReleaseCallbackMayParkSomethingNew)
{
    FrameTimeline   timeline;
    RetirementQueue queue(1);
    int             releases = 0;

    const auto first = timeline.begin();
    timeline.submitted(first);
    ASSERT_TRUE(queue.retire(timeline, [&] {
        ++releases;
        // Re-entrant park: the queue compacts before it releases, so this must not corrupt it.
        EXPECT_TRUE(queue.retire(timeline, [&releases] { ++releases; }));
    }));

    timeline.completeUpTo(queue.retirePoint(timeline));
    queue.advance(timeline);

    EXPECT_EQ(releases, 1);
    EXPECT_EQ(queue.pending(), 1u);  // the new park is dated against the same submitted frame
    EXPECT_EQ(queue.deviceWaits(), 0u);
}

TEST(CoreArenaTest, GrowthPreservesWhatWasAlreadyHandedOut)
{
    FrameArena              arena(64);  // deliberately tiny, so the copy below must grow it
    std::vector<int>        borrowed(64, 7);  // 256 bytes: more than the arena reserved
    const std::vector<int>  source{101, 102, 103};

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
    FrameArena         arena(1024);
    const std::vector  sizes{16, 64, 256};

    arena.reset();
    for (const int size : sizes)
    {
        auto values = arena.makeArray<int>(static_cast<std::size_t>(size));
        values[0]   = size;
    }
    const std::size_t after_first_frame = arena.allocations();

    arena.reset();
    for (const int size : sizes)
    {
        auto values = arena.makeArray<int>(static_cast<std::size_t>(size));
        values[0]   = size;
    }
    // Repeating the same frame reuses the capacity: this is the "steady frame allocates nothing" rule.
    EXPECT_EQ(arena.allocations(), after_first_frame);
}

TEST(CoreKeysTest, AnExtentIsNotPartOfAPipelineKey)
{
    auto shape = singleColorShape();

    // Two descriptions whose EXTENT differs: a resize. building the identity from the shape alone - and
    // not from the description - is what makes "a resize must not recompile" structural.
    const TargetDesc small{256, 256, shape};
    const TargetDesc large{2048, 1024, shape};

    PipelineKey a;
    a.compatibility.color_formats = small.shape.color_formats;
    a.compatibility.samples       = small.shape.samples;

    PipelineKey b;
    b.compatibility.color_formats = large.shape.color_formats;
    b.compatibility.samples       = large.shape.samples;

    EXPECT_TRUE(a == b);
    EXPECT_EQ(static_cast<int>(vine::vsg::core::planTarget(TargetInstance{small, 1, true, false}, large).action),
              static_cast<int>(TargetAction::ResizeInPlace));
}

TEST(CoreKeysTest, CompatibilityIsPartOfTheKeyWhileLoadOpVariantsAreNot)
{
    PipelineKey a;
    a.compatibility.color_formats.push_back(vine::graphics::RenderTarget::ColorFormat::RGBA8);

    PipelineKey b = a;
    EXPECT_TRUE(a == b);

    // A different attachment format is a different pipeline.
    b.compatibility.color_formats[0] = vine::graphics::RenderTarget::ColorFormat::RGBA16F;
    EXPECT_FALSE(a == b);

    // A clear instead of a load is a different VARIANT of the same pipeline, not a different pipeline:
    // the API's compatibility rule excludes load ops, which is why the bootstrap can swap one in.
    LoadOpVariantKey loaded;
    LoadOpVariantKey cleared;
    cleared.color_load = vine::vsg::core::LoadOp::Clear;
    EXPECT_FALSE(loaded == cleared);
}

TEST(CoreKeysTest, TheAuditTableNamesEveryKeyAndKeepsExtentsOutOfIdentity)
{
    const auto audit = vine::vsg::core::keyAuditTable();
    ASSERT_EQ(audit.size(), 8u);

    const std::vector<std::string> expected_keys{
        "DataKey",           "VertexLayoutKey",   "RenderPassCompatibility", "LoadOpVariantKey",
        "PipelineKey",       "DynamicState",      "InstanceSlot",            "TargetDesc.shape",
    };
    for (std::size_t i = 0; i < expected_keys.size(); ++i)
    {
        EXPECT_EQ(std::string(audit[i].key), expected_keys[i]) << "audit line " << i;
    }

    // The rule the table exists for: no identity key may be described as taking an extent, and the one
    // line that mentions it must be the one that forbids it.
    for (const auto& entry : audit) {
        const std::string allowed{entry.allowed};
        const bool mentions_extent = allowed.find("extent") != std::string::npos;
        EXPECT_EQ(mentions_extent, std::string(entry.key) == "TargetDesc.shape") << entry.key;
    }
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
    table.add(Phase{"a passing phase", [] { return true; }});
    table.add(Phase{"a failing phase", [] { return false; }});
    table.add(Phase{
        "a counter that stays put", [] { return true; },
        [&counter] { return static_cast<std::uint64_t>(counter); },
        [](std::uint64_t before, std::uint64_t after) { return before == after; }});
    table.add(Phase{
        "a phase that moved a counter it promised not to",
        [&counter] {
            ++counter;
            return true;
        },
        [&counter] { return static_cast<std::uint64_t>(counter); },
        [](std::uint64_t before, std::uint64_t after) { return before == after; }});
    table.add(Phase{"a nameless phase is ignored", {}});

    ASSERT_EQ(table.size(), 4u);

    const auto report = table.runAll();
    ASSERT_EQ(report.lines.size(), 4u);
    EXPECT_EQ(report.lines[0], "[selftest] a passing phase");
    EXPECT_EQ(report.lines[1], "[selftest] a failing phase FAILED: assertion failed");
    EXPECT_EQ(report.lines[2], "[selftest] a counter that stays put");
    // Asserting correctly is not enough: a phase that rebuilt everything while claiming it would not is
    // exactly the regression the counter expectation exists to catch.
    EXPECT_EQ(report.lines[3],
              "[selftest] a phase that moved a counter it promised not to FAILED: counter expectation not met");
    EXPECT_FALSE(report.ok());
    EXPECT_EQ(report.passed, 2u);
    EXPECT_EQ(report.failed, 2u);

    // A table that passed entirely ends with the line the evidence script expects.
    PhaseTable passing;
    passing.add(Phase{"only phase", [] { return true; }});
    const auto clean = passing.runAll();
    ASSERT_EQ(clean.lines.size(), 2u);
    EXPECT_EQ(clean.lines[1], "[selftest] done");
    EXPECT_TRUE(clean.ok());
}

TEST(CoreDeviceRequirementsTest, TheVersionFloorIgnoresThePatch)
{
    using namespace vine::vsg::core;

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
    using namespace vine::vsg::core;

    DeviceFacts bare;
    bare.api_version = kRequiredApiVersion;

    // The right version with no features is still refused - the two halves are both required.
    EXPECT_EQ(missingFeatureCount(bare), kDeviceFeatureCount);
    EXPECT_FALSE(satisfiesRequirements(bare));

    for (std::size_t i = 0; i < kDeviceFeatureCount; ++i)
    {
        const auto feature = static_cast<DeviceFeature>(i);
        EXPECT_STRNE(featureName(feature), "") << "feature " << i << " has no name to report";
        bare.note(feature);
    }

    // The features alone are not enough either: the extensions carry the states themselves, and a device that
    // offers every feature bit but not the extension may not be told to set those states at all.
    EXPECT_EQ(missingFeatureCount(bare), 0U);
    EXPECT_EQ(missingExtensionCount(bare), kDeviceExtensionCount);
    EXPECT_FALSE(satisfiesRequirements(bare));

    for (std::size_t i = 0; i < kDeviceExtensionCount; ++i)
    {
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
    using namespace vine::vsg::core;

    DeviceFacts facts;
    facts.api_version = makeApiVersion(1, 4);
    for (std::size_t i = 0; i < kDeviceFeatureCount; ++i)
    {
        facts.note(static_cast<DeviceFeature>(i));
    }
    for (std::size_t i = 0; i < kDeviceExtensionCount; ++i)
    {
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
    using namespace vine::vsg::core;

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
    using namespace vine::vsg::core;

    std::vector<float> values(4U * 3U, 0.0F);
    values[1U * 4U + 2U] = 0.75F;  // row 1, column 2
    const DepthProbe probe(4, 3, std::move(values));

    ASSERT_TRUE(probe.valid());
    EXPECT_EQ(probe.width(), 4);
    EXPECT_EQ(probe.height(), 3);
    EXPECT_FLOAT_EQ(probe.depthAt(2, 1), 0.75F) << "row-major, like the copy that filled it";
    EXPECT_FLOAT_EQ(probe.depthAt(1, 1), 0.0F);
    EXPECT_FLOAT_EQ(probe.depthAt(-1, 0), 0.0F) << "outside the image answers 0, and valid() says so first";
    EXPECT_FLOAT_EQ(probe.depthAt(4, 0), 0.0F);
    EXPECT_EQ(probe.countNear(0.0F, 0.01F), 11U);
    EXPECT_EQ(probe.countNear(0.75F, 0.001F), 1U)
        << "the tolerance is absolute: a clear value and a written depth do not arrive by the same path";
    EXPECT_EQ(probe.countNear(0.75F, 0.0F), 1U);
}

TEST(CoreReadbackTest, TheTableClassifiesBeforeAnythingRuns)
{
    using vine::vsg::core::ReadbackKind;
    using vine::vsg::core::ReadbackRefusal;
    using vine::vsg::core::readbackOf;

    ReadbackState state;
    state.color_attachments = 2U;
    state.color_format      = RenderTarget::ColorFormat::RGBA8;
    state.depth_format      = RenderTarget::DepthFormat::D32;

    // Nothing has recorded a copy yet: the answer is "not yet" - a frame can fix it, and a caller that gets
    // this one may retry instead of hunting for a format bug.
    EXPECT_EQ(static_cast<int>(readbackOf(state, ReadbackRequest{ ReadbackKind::Color, 0U }).refusal),
              static_cast<int>(ReadbackRefusal::NotCaptured));
    EXPECT_EQ(static_cast<int>(readbackOf(state, ReadbackRequest{ ReadbackKind::Depth, 0U }).refusal),
              static_cast<int>(ReadbackRefusal::NotCaptured));
    EXPECT_FALSE(readbackOf(state, ReadbackRequest{ ReadbackKind::Color, 0U }).ok);

    state.color_captured = true;
    state.depth_captured = true;
    EXPECT_TRUE(readbackOf(state, ReadbackRequest{ ReadbackKind::Color, 0U }).ok) << "a captured RGBA8 attachment";
    EXPECT_TRUE(readbackOf(state, ReadbackRequest{ ReadbackKind::Color, 1U }).ok) << "and the second one too";
    EXPECT_TRUE(readbackOf(state, ReadbackRequest{ ReadbackKind::Depth, 0U }).ok);
    EXPECT_EQ(static_cast<int>(readbackOf(state, ReadbackRequest{ ReadbackKind::Color, 0U }).refusal),
              static_cast<int>(ReadbackRefusal::None))
        << "ok and a category are two views of one answer, never contradictory";

    // The request's own existence comes first: index 2 does not exist, and a target without a depth cannot
    // answer a depth readback - whatever has been captured.
    EXPECT_EQ(static_cast<int>(readbackOf(state, ReadbackRequest{ ReadbackKind::Color, 2U }).refusal),
              static_cast<int>(ReadbackRefusal::UnknownAttachment));
    ReadbackState colorless = state;
    colorless.depth_format.reset();
    EXPECT_EQ(static_cast<int>(readbackOf(colorless, ReadbackRequest{ ReadbackKind::Depth, 0U }).refusal),
              static_cast<int>(ReadbackRefusal::UnknownAttachment));
}

TEST(CoreReadbackTest, AnUnreadableFormatIsPermanentAndOutranksNotCaptured)
{
    using vine::vsg::core::ReadbackKind;
    using vine::vsg::core::ReadbackRefusal;
    using vine::vsg::core::readbackOf;

    ReadbackState state;
    state.color_attachments = 1U;
    state.color_format      = RenderTarget::ColorFormat::RGBA16F;
    state.depth_format      = RenderTarget::DepthFormat::D24;

    // NOTHING captured: the format is still the answer, because a frame cannot fix it. Reporting "not captured"
    // here would send a caller off to run another frame that cannot change anything.
    EXPECT_EQ(static_cast<int>(readbackOf(state, ReadbackRequest{ ReadbackKind::Color, 0U }).refusal),
              static_cast<int>(ReadbackRefusal::UnreadableFormat));
    EXPECT_EQ(static_cast<int>(readbackOf(state, ReadbackRequest{ ReadbackKind::Depth, 0U }).refusal),
              static_cast<int>(ReadbackRefusal::UnreadableFormat));

    state.color_captured = true;
    state.depth_captured = true;
    EXPECT_EQ(static_cast<int>(readbackOf(state, ReadbackRequest{ ReadbackKind::Color, 0U }).refusal),
              static_cast<int>(ReadbackRefusal::UnreadableFormat))
        << "capturing more changes nothing: the format table is what says so";
    EXPECT_EQ(static_cast<int>(readbackOf(state, ReadbackRequest{ ReadbackKind::Depth, 0U }).refusal),
              static_cast<int>(ReadbackRefusal::UnreadableFormat));

    EXPECT_EQ(vine::vsg::core::refusalName(ReadbackRefusal::UnknownAttachment), "unknown-attachment");
    EXPECT_EQ(vine::vsg::core::refusalName(ReadbackRefusal::UnreadableFormat), "unreadable-format");
    EXPECT_EQ(vine::vsg::core::refusalName(ReadbackRefusal::NotCaptured), "not-captured");
    EXPECT_EQ(vine::vsg::core::refusalName(ReadbackRefusal::None), "none");
}

TEST(CoreReadbackTest, TheFormatTablesSayWhatCanBeReadAndWithHowManyBytesPerTexel)
{
    using vine::vsg::core::colorReadbackOf;
    using vine::vsg::core::depthReadbackOf;
    using vine::vsg::core::ReadbackFormat;

    // The colour half packs RGBA8 only: the probes are 8-bit probes, and a float attachment converted into one
    // would be a picture of something the GPU never held.
    EXPECT_EQ(colorReadbackOf(RenderTarget::ColorFormat::RGBA8), (ReadbackFormat{ 4U, true }));
    EXPECT_FALSE(colorReadbackOf(RenderTarget::ColorFormat::RGBA16F).readable);
    EXPECT_FALSE(colorReadbackOf(RenderTarget::ColorFormat::RGBA32F).readable);
    EXPECT_EQ(colorReadbackOf(RenderTarget::ColorFormat::RGBA16F).bytes_per_texel, 0U);

    // The depth half: D16 is two bytes (and gets scaled on decode), D32 / D32F four (raw float), and the
    // combined format is refused rather than read as if its first bytes were depth.
    EXPECT_EQ(depthReadbackOf(RenderTarget::DepthFormat::D16), (ReadbackFormat{ 2U, true }));
    EXPECT_EQ(depthReadbackOf(RenderTarget::DepthFormat::D32), (ReadbackFormat{ 4U, true }));
    EXPECT_EQ(depthReadbackOf(RenderTarget::DepthFormat::D32F), (ReadbackFormat{ 4U, true }));
    EXPECT_FALSE(depthReadbackOf(RenderTarget::DepthFormat::D24).readable);
}

TEST(CoreReadbackTest, TheDepthDecodeIsTheFormatsOwnConversion)
{
    using vine::vsg::core::decodeDepth;

    // D32 / D32F: the stored bits are the value.
    {
        const std::array<float, 3> values{ 0.0F, 0.5F, 1.25F };
        const auto                 bytes = std::as_bytes(std::span(values));
        const auto                 decoded = decodeDepth(RenderTarget::DepthFormat::D32, bytes);
        ASSERT_EQ(decoded.size(), 3U);
        EXPECT_FLOAT_EQ(decoded[0], 0.0F);
        EXPECT_FLOAT_EQ(decoded[1], 0.5F);
        EXPECT_FLOAT_EQ(decoded[2], 1.25F) << "raw, not clamped: the attachment's own numbers";
    }

    // D16_UNORM: the stored integer divided by its full scale (65535) - the conversion the format defines.
    {
        const std::array<std::uint16_t, 4> stored{ 0U, 32768U, 65535U, 1U };
        const auto bytes = std::as_bytes(std::span(stored));
        const auto decoded = decodeDepth(RenderTarget::DepthFormat::D16, bytes);
        ASSERT_EQ(decoded.size(), 4U);
        EXPECT_FLOAT_EQ(decoded[0], 0.0F);
        EXPECT_NEAR(decoded[1], 0.5000076F, 1.0e-6F) << "0.5 is not exactly representable in 16 bits, and the "
                                                        "decode must not pretend otherwise";
        EXPECT_FLOAT_EQ(decoded[2], 1.0F);
        EXPECT_NEAR(decoded[3], 1.0F / 65535.0F, 1.0e-9F);
    }

    // A format that cannot be read, and a buffer that is not a whole number of texels: no probe at all rather
    // than a plausible-looking half image.
    {
        const std::array<std::byte, 4> bytes{};
        EXPECT_TRUE(decodeDepth(RenderTarget::DepthFormat::D24, bytes).empty());
        const std::array<std::byte, 3> odd{};
        EXPECT_TRUE(decodeDepth(RenderTarget::DepthFormat::D16, odd).empty())
            << "half a texel is not a value: a probe built from it would claim a region is empty";
        EXPECT_TRUE(decodeDepth(RenderTarget::DepthFormat::D32, odd).empty());
    }
}
