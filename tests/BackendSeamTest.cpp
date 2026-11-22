/**
 * @brief The SDK backend seam's device-free facts, compiled into EVERY test binary that links the seam.
 *
 * The seam's mechanisms live in `vine/graphics/` beside the host types: they are what a backend consumes and
 * the engine links, their types are pure values,
 * they need no device, and BOTH backends answer from them - so a case that pins one belongs in a binary that
 * carries both, not in the one backend's suite. The suites here are the seam's own (`Core*` after the layer they
 * pin, `FrameGateTest` after the type), and each of them used to live in `test_vsg`/`test_vk` alone:
 *
 *   * `CoreTargetPlanTest` - a target's GPU objects are a decision function, and extent is not identity;
 *   * `CoreRetirementQueueTest` - deferred destruction is ordered by one timeline, and completion is evidence;
 *   * `CoreKeysTest` - what a pipeline is keyed by, and the audit table that says so;
 *   * `CoreReadbackTest` - what can be read back, in which format, and how the depth decode works;
 *   * `CoreDeviceClockTest` - the completion watermark is what EVERY window proved, and nothing else;
 *   * `CoreTimelineTest` - the device's frame clock: submitted is not completed, tokens are minted, and a
 *     park's release point is derived from the slot count;
 *   * `FrameGateTest` - frame bodies share a gate, host verbs own it, and no caller waits for itself.
 *
 * The vsg-only core (the frame arena, the phase table, the observe cross-check, the depth probe and the device
 * requirements) stays in `tests/test_vsg/BackendCoreTest.cpp`: those headers are that backend's portable half, not
 * the seam (`.ai/decisions/ADR-2026-11-16-no-backend-half.md` records the relocation
 * that gave the vsg-only frame machinery its own home).
 */

#include <gtest/gtest.h>

#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <thread>
#include <vector>

#include <vine/graphics/DeviceClock.hpp>
#include <vine/graphics/FrameGate.hpp>
#include <vine/graphics/FrameTimeline.hpp>
#include <vine/graphics/DrawState.hpp>
#include <vine/graphics/Readback.hpp>
#include <vine/graphics/RetirementQueue.hpp>
#include <vine/graphics/TargetPlan.hpp>

using vn::graphics::RenderTarget;
using vn::graphics::DeviceClock;
using vn::graphics::FrameGate;
using vn::graphics::FrameTimeline;
using vn::graphics::LoadOpVariantKey;
using vn::graphics::PipelineKey;
using vn::graphics::ReadbackRequest;
using vn::graphics::ReadbackState;
using vn::graphics::RepairReason;
using vn::graphics::RetirementQueue;
using vn::graphics::TargetAction;
using vn::graphics::TargetDecision;
using vn::graphics::TargetDesc;
using vn::graphics::TargetInstance;

namespace
{

/// @brief One line of the target-plan table.
struct TargetCase {
    const char*  what;
    TargetDesc   current;
    bool         built;
    bool         invalidated;
    TargetDesc   wanted;
    TargetAction action;
    RepairReason reason;
};

/// @brief The compatible shape most cases share: one RGBA8 colour attachment, no depth.
vn::graphics::RenderPassCompatibility singleColorShape()
{
    vn::graphics::RenderPassCompatibility shape;
    shape.color_formats.push_back(vn::graphics::RenderTarget::ColorFormat::RGBA8);
    return shape;
}

/// @brief A shape with a depth attachment, used to move compatibility.
vn::graphics::RenderPassCompatibility colorAndDepthShape()
{
    vn::graphics::RenderPassCompatibility shape = singleColorShape();
    shape.depth_format                       = vn::graphics::RenderTarget::DepthFormat::D32;
    return shape;
}

} // namespace

TEST(CoreTargetPlanTest, TheActionTableIsDecidedByShapeSizeAndBuildState)
{
    const vn::graphics::RenderPassCompatibility single = singleColorShape();
    const vn::graphics::RenderPassCompatibility with_d = colorAndDepthShape();

    TargetDesc built_at_512{ 512, 256, single };
    TargetDesc built_at_1024{ 1024, 256, single };

    const std::vector<TargetCase> cases{
        { "same size, same shape",                        built_at_512, true,  false, built_at_512,                    TargetAction::None,          RepairReason::None        },
        { "new extent, same shape",                       built_at_512, true,  false, built_at_1024,                   TargetAction::ResizeInPlace, RepairReason::None        },
        { "new shape at the same size",                   built_at_512, true,  false, TargetDesc{ 512, 256, with_d },  TargetAction::Rebuild,       RepairReason::None        },
        { "new shape AND new extent: compatibility wins",
         built_at_512,                                                  true,
         false,                                                                       TargetDesc{ 1024, 256, with_d },
         TargetAction::Rebuild,
         RepairReason::None                                                                                                                                                   },
        { "never built",                                  built_at_512, false, false, built_at_512,                    TargetAction::Repair,        RepairReason::Bootstrap   },
        { "attachments invalidated",                      built_at_512, true,  true,  built_at_512,                    TargetAction::Repair,        RepairReason::Bootstrap   },
        // A shape change outranks the load-op repairs: "the first pass in clears" is an instruction about
        // attachment CONTENTS, and a rebuild replaces the pass those attachments belong to - the caller
        // has to hear about the compatibility change even when nobody has written into the target yet
        // (the rebuilt target's fresh attachments answer Repair(Bootstrap) on the next plan).
        { "never built, new shape",                       built_at_512, false, false, TargetDesc{ 512, 256, with_d },  TargetAction::Rebuild,       RepairReason::None        },
        { "invalidated, new shape",                       built_at_512, true,  true,  TargetDesc{ 512, 256, with_d },  TargetAction::Rebuild,       RepairReason::None        },
        { "resized to nothing",                           built_at_512, true,  false, TargetDesc{ 0, 256, single },    TargetAction::Repair,        RepairReason::SizeUnknown },
        { "laid out at zero",                             built_at_512, true,  false, TargetDesc{ 512, 0, single },    TargetAction::Repair,        RepairReason::SizeUnknown },
    };

    for (const TargetCase& test_case : cases) {
        TargetInstance instance;
        instance.desc                    = test_case.current;
        instance.built                   = test_case.built;
        instance.attachments_invalidated = test_case.invalidated;

        const TargetDecision decision = vn::graphics::planTarget(instance, test_case.wanted);
        EXPECT_EQ(static_cast<int>(decision.action), static_cast<int>(test_case.action)) << test_case.what;
        EXPECT_EQ(static_cast<int>(decision.reason), static_cast<int>(test_case.reason)) << test_case.what;
    }
}

TEST(CoreTargetPlanTest, AnUnusableExtentIsDistinguishableFromInvalidatedAttachments)
{
    // "Resized to nothing" and "attachments invalidated" are both repairs, and they have different
    // reasons precisely so a phase (and a log line) can tell them apart.
    TargetInstance instance;
    instance.desc  = TargetDesc{ 8, 8, singleColorShape() };
    instance.built = true;

    EXPECT_EQ(static_cast<int>(vn::graphics::planTarget(instance, TargetDesc{ 0, 0, {} }).reason), static_cast<int>(RepairReason::SizeUnknown));

    instance.attachments_invalidated = true;
    EXPECT_EQ(static_cast<int>(vn::graphics::planTarget(instance, instance.desc).reason), static_cast<int>(RepairReason::Bootstrap));
}

TEST(CoreTargetPlanTest, DepthPromotionBorrowingAndPreservationAreOneDecision)
{
    using vn::graphics::DepthFacts;
    using vn::graphics::DepthPlan;
    using vn::graphics::depthPlan;

    int lender = 0;

    // No depth at all: nothing to plan.
    {
        DepthFacts      facts;
        const DepthPlan plan = depthPlan(facts);
        EXPECT_FALSE(plan.has_depth);
        EXPECT_FALSE(plan.sampleable);
    }

    // Promotion with nothing preserving it: sampleable.
    {
        DepthFacts facts;
        facts.has_depth      = true;
        facts.promotion      = true;
        const DepthPlan plan = depthPlan(facts);
        EXPECT_TRUE(plan.sampleable);
        EXPECT_FALSE(plan.preserve);
    }

    // A depth-preserving pass revokes the promotion for the whole target.
    {
        DepthFacts facts;
        facts.has_depth                = true;
        facts.promotion                = true;
        facts.any_pass_preserves_depth = true;
        const DepthPlan plan           = depthPlan(facts);
        EXPECT_FALSE(plan.sampleable);
        EXPECT_TRUE(plan.preserve);
    }

    // A borrowed depth: the policy is the lender's, so the borrower never samples it - and it PRESERVES it:
    // the image holds the depth the lender's pass wrote, and a pass of the borrower that cleared it (a
    // bootstrap clears) would erase the lender's depth for every reader after it (see depthPlan).
    {
        DepthFacts facts;
        facts.has_depth      = true;
        facts.promotion      = true;
        facts.borrowed       = true;
        facts.source         = &lender;
        const DepthPlan plan = depthPlan(facts);
        EXPECT_TRUE(plan.borrowed);
        EXPECT_EQ(plan.source, &lender);
        EXPECT_FALSE(plan.sampleable);
        EXPECT_TRUE(plan.preserve);
    }
}

TEST(CoreRetirementQueueTest, NothingIsReleasedBeforeTheEvidenceArrives)
{
    FrameTimeline   timeline;
    RetirementQueue queue(3);
    int             releases = 0;

    const auto token = timeline.begin();
    timeline.submitted(token); // submitted frame 1 -> retire at 1 + 3 + 1 = 5
    ASSERT_TRUE(queue.retire(timeline, [&releases] { ++releases; }));

    EXPECT_EQ(queue.retirePoint(timeline), 5u);
    EXPECT_EQ(queue.pending(), 1u);

    timeline.completeUpTo(4);
    queue.advance(timeline);
    EXPECT_EQ(releases, 0); // not yet: the slot that recorded it may still be in flight
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
    EXPECT_EQ(queue.pending(), 1u); // the new park is dated against the same submitted frame
    EXPECT_EQ(queue.deviceWaits(), 0u);
}

TEST(CoreRetirementQueueTest, ATeardownRunsEverythingParkedInOrder)
{
    // THE TEARDOWN VERB (see RetirementQueue::releaseAll): parking exists to wait for the evidence that no submitted
    // frame can still name the object, and a device being torn down has produced that evidence by idling its queue.
    // Without this call every parked object would leak with the session - so the case pins both halves: everything
    // runs, and it runs in the order it was parked (a release may name an object another one is nested inside).
    FrameTimeline   timeline;
    RetirementQueue queue(1);
    std::vector<int> order;

    const auto token = timeline.begin();
    timeline.submitted(token); // frame 1; nothing is completed, so advance() would release nothing

    ASSERT_TRUE(queue.retire(timeline, [&order] { order.push_back(1); }));
    ASSERT_TRUE(queue.retire(timeline, [&order] { order.push_back(2); }));
    EXPECT_EQ(queue.pending(), 2u);

    queue.advance(timeline);
    EXPECT_TRUE(order.empty()) << "advance() released something the timeline has not proven yet";

    queue.releaseAll();
    EXPECT_EQ(order, (std::vector<int>{ 1, 2 })) << "the teardown did not run every release in park order";
    EXPECT_EQ(queue.pending(), 0u);
    EXPECT_EQ(queue.released(), 2u) << "the teardown's releases were not counted";
    EXPECT_EQ(queue.deviceWaits(), 0u) << "flushing the queue is not a device idle";
}

TEST(CoreKeysTest, AnExtentIsNotPartOfAPipelineKey)
{
    auto shape = singleColorShape();

    // Two descriptions whose EXTENT differs: a resize. building the identity from the shape alone - and
    // not from the description - is what makes "a resize must not recompile" structural.
    const TargetDesc small{ 256, 256, shape };
    const TargetDesc large{ 2048, 1024, shape };

    PipelineKey a;
    a.compatibility.color_formats = small.shape.color_formats;
    a.compatibility.samples       = small.shape.samples;

    PipelineKey b;
    b.compatibility.color_formats = large.shape.color_formats;
    b.compatibility.samples       = large.shape.samples;

    EXPECT_TRUE(a == b);
    EXPECT_EQ(static_cast<int>(vn::graphics::planTarget(TargetInstance{ small, 1, true, false }, large).action),
              static_cast<int>(TargetAction::ResizeInPlace));
}

TEST(CoreKeysTest, CompatibilityIsPartOfTheKeyWhileLoadOpVariantsAreNot)
{
    PipelineKey a;
    a.compatibility.color_formats.push_back(vn::graphics::RenderTarget::ColorFormat::RGBA8);

    PipelineKey b = a;
    EXPECT_TRUE(a == b);

    // A different attachment format is a different pipeline.
    b.compatibility.color_formats[0] = vn::graphics::RenderTarget::ColorFormat::RGBA16F;
    EXPECT_FALSE(a == b);

    // A clear instead of a load is a different VARIANT of the same pipeline, not a different pipeline:
    // the API's compatibility rule excludes load ops, which is why the bootstrap can swap one in.
    LoadOpVariantKey loaded;
    LoadOpVariantKey cleared;
    cleared.color_load = vn::graphics::LoadOp::Clear;
    EXPECT_FALSE(loaded == cleared);
}

TEST(CoreKeysTest, TheAuditTableNamesEveryKeyAndKeepsExtentsOutOfIdentity)
{
    const auto audit = vn::graphics::detail::keyAuditTable();
    ASSERT_EQ(audit.size(), 7u);

    const std::vector<std::string> expected_keys{
        "VertexLayoutKey", "RenderPassCompatibility", "LoadOpVariantKey", "PipelineKey", "DynamicState", "InstanceRecord", "TargetDesc.shape",
    };
    for (std::size_t i = 0; i < expected_keys.size(); ++i) {
        EXPECT_EQ(std::string(audit[i].key), expected_keys[i]) << "audit line " << i;
    }

    // The rule the table exists for: no identity key may be described as taking an extent, and the one
    // line that mentions it must be the one that forbids it.
    for (const auto& entry : audit) {
        const std::string allowed{ entry.allowed };
        const bool        mentions_extent = allowed.find("extent") != std::string::npos;
        EXPECT_EQ(mentions_extent, std::string(entry.key) == "TargetDesc.shape") << entry.key;
    }
}

TEST(CoreReadbackTest, TheTableClassifiesBeforeAnythingRuns)
{
    using vn::graphics::ReadbackKind;
    using vn::graphics::readbackOf;
    using vn::graphics::ReadbackRefusal;

    ReadbackState state;
    state.color_attachments = 2U;
    state.color_format      = RenderTarget::ColorFormat::RGBA8;
    state.depth_format      = RenderTarget::DepthFormat::D32;

    // Nothing has recorded a copy yet: the answer is "not yet" - a frame can fix it, and a caller that gets
    // this one may retry instead of hunting for a format bug.
    EXPECT_EQ(static_cast<int>(readbackOf(state, ReadbackRequest{ ReadbackKind::Color, 0U }).refusal), static_cast<int>(ReadbackRefusal::NotCaptured));
    EXPECT_EQ(static_cast<int>(readbackOf(state, ReadbackRequest{ ReadbackKind::Depth, 0U }).refusal), static_cast<int>(ReadbackRefusal::NotCaptured));
    EXPECT_FALSE(readbackOf(state, ReadbackRequest{ ReadbackKind::Color, 0U }).ok);

    state.color_captured = true;
    state.depth_captured = true;
    EXPECT_TRUE(readbackOf(state, ReadbackRequest{ ReadbackKind::Color, 0U }).ok) << "a captured RGBA8 attachment";
    EXPECT_TRUE(readbackOf(state, ReadbackRequest{ ReadbackKind::Color, 1U }).ok) << "and the second one too";
    EXPECT_TRUE(readbackOf(state, ReadbackRequest{ ReadbackKind::Depth, 0U }).ok);
    EXPECT_EQ(static_cast<int>(readbackOf(state, ReadbackRequest{ ReadbackKind::Color, 0U }).refusal), static_cast<int>(ReadbackRefusal::None))
        << "ok and a category are two views of one answer, never contradictory";

    // The request's own existence comes first: index 2 does not exist, and a target without a depth cannot
    // answer a depth readback - whatever has been captured.
    EXPECT_EQ(static_cast<int>(readbackOf(state, ReadbackRequest{ ReadbackKind::Color, 2U }).refusal), static_cast<int>(ReadbackRefusal::UnknownAttachment));
    ReadbackState colorless = state;
    colorless.depth_format.reset();
    EXPECT_EQ(static_cast<int>(readbackOf(colorless, ReadbackRequest{ ReadbackKind::Depth, 0U }).refusal),
              static_cast<int>(ReadbackRefusal::UnknownAttachment));
}

TEST(CoreReadbackTest, AnUnreadableFormatIsPermanentAndOutranksNotCaptured)
{
    using vn::graphics::ReadbackKind;
    using vn::graphics::readbackOf;
    using vn::graphics::ReadbackRefusal;

    ReadbackState state;
    state.color_attachments = 1U;
    state.color_format      = RenderTarget::ColorFormat::RGBA16F;
    state.depth_format      = RenderTarget::DepthFormat::D24;

    // NOTHING captured: the format is still the answer, because a frame cannot fix it. Reporting "not captured"
    // here would send a caller off to run another frame that cannot change anything.
    EXPECT_EQ(static_cast<int>(readbackOf(state, ReadbackRequest{ ReadbackKind::Color, 0U }).refusal), static_cast<int>(ReadbackRefusal::UnreadableFormat));
    EXPECT_EQ(static_cast<int>(readbackOf(state, ReadbackRequest{ ReadbackKind::Depth, 0U }).refusal), static_cast<int>(ReadbackRefusal::UnreadableFormat));

    state.color_captured = true;
    state.depth_captured = true;
    EXPECT_EQ(static_cast<int>(readbackOf(state, ReadbackRequest{ ReadbackKind::Color, 0U }).refusal), static_cast<int>(ReadbackRefusal::UnreadableFormat))
        << "capturing more changes nothing: the format table is what says so";
    EXPECT_EQ(static_cast<int>(readbackOf(state, ReadbackRequest{ ReadbackKind::Depth, 0U }).refusal), static_cast<int>(ReadbackRefusal::UnreadableFormat));

    EXPECT_EQ(vn::graphics::detail::refusalName(ReadbackRefusal::UnknownAttachment), "unknown-attachment");
    EXPECT_EQ(vn::graphics::detail::refusalName(ReadbackRefusal::UnreadableFormat), "unreadable-format");
    EXPECT_EQ(vn::graphics::detail::refusalName(ReadbackRefusal::NotCaptured), "not-captured");
    EXPECT_EQ(vn::graphics::detail::refusalName(ReadbackRefusal::None), "none");
}

TEST(CoreReadbackTest, TheFormatTablesSayWhatCanBeReadAndWithHowManyBytesPerTexel)
{
    using vn::graphics::colorReadbackOf;
    using vn::graphics::depthReadbackOf;
    using vn::graphics::ReadbackFormat;

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
    using vn::graphics::decodeDepth;

    // D32 / D32F: the stored bits are the value.
    {
        const std::array<float, 3> values{ 0.0F, 0.5F, 1.25F };
        const auto                 bytes   = std::as_bytes(std::span(values));
        const auto                 decoded = decodeDepth(RenderTarget::DepthFormat::D32, bytes);
        ASSERT_EQ(decoded.size(), 3U);
        EXPECT_FLOAT_EQ(decoded[0], 0.0F);
        EXPECT_FLOAT_EQ(decoded[1], 0.5F);
        EXPECT_FLOAT_EQ(decoded[2], 1.25F) << "raw, not clamped: the attachment's own numbers";
    }

    // D16_UNORM: the stored integer divided by its full scale (65535) - the conversion the format defines.
    {
        const std::array<std::uint16_t, 4> stored{ 0U, 32768U, 65535U, 1U };
        const auto                         bytes   = std::as_bytes(std::span(stored));
        const auto                         decoded = decodeDepth(RenderTarget::DepthFormat::D16, bytes);
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

/**
 * @brief The device's completion watermark is what EVERY window proved, and nothing else.
 *
 * This is the one rule one device with several windows rests on (see graphics::DeviceClock): a buffer or a pipeline
 * the host replaced may still be named by a submission from ANY window, so "may it be destroyed" is answered by
 * the minimum over the windows - the frame every one of them has a recycled slot for.
 */
TEST(CoreDeviceClockTest, TheWatermarkIsWhatEveryWindowProved)
{
    DeviceClock clock;
    const auto  a = clock.addWindow();
    const auto  b = clock.addWindow();
    EXPECT_EQ(clock.windows(), 2U);
    EXPECT_EQ(clock.completedFrame(), 0U);

    // Reports before a window knows how many frames it may have in flight prove NOTHING: its evidence would be a
    // guess, and a guess in this direction destroys an object a submitted command buffer still names.
    clock.windowCompleted(a, 10U);
    clock.windowCompleted(b, 10U);
    EXPECT_EQ(clock.completedFrame(), 0U);

    clock.setWindowSlots(a, 3U);
    clock.setWindowSlots(b, 2U);
    clock.windowCompleted(a, 7U);
    clock.windowCompleted(b, 9U);
    EXPECT_EQ(clock.completedFrame(), 7U) << "the window that is behind decides";

    clock.windowCompleted(b, 12U);
    EXPECT_EQ(clock.completedFrame(), 7U) << "one window moving on does not prove anything for the other";

    clock.windowCompleted(a, 8U);
    EXPECT_EQ(clock.completedFrame(), 8U);
    EXPECT_EQ(clock.timeline().completedFrame(), 8U) << "and the frame clock carries the same number";

    // Completion is a fact about the past: an older report never moves the device back.
    clock.windowCompleted(a, 1U);
    EXPECT_EQ(clock.completedFrame(), 8U);

    // A window that leaves stops constraining the watermark (otherwise a closed window would hold the device at
    // its last evidence forever).
    clock.removeWindow(b);
    clock.windowCompleted(a, 20U);
    EXPECT_EQ(clock.completedFrame(), 20U);
    EXPECT_EQ(clock.windows(), 1U);

    // A window that joins LATER starts at the watermark: it existed for no earlier frame, so it must not roll the
    // device's evidence back to zero.
    const auto c = clock.addWindow();
    clock.setWindowSlots(c, 3U);
    clock.windowCompleted(c, 20U);
    EXPECT_EQ(clock.completedFrame(), 20U) << "the newcomer did not roll the evidence back";

    EXPECT_EQ(clock.widestSlots(), 3U) << "the park window is the widest a window owns, not the narrowest";
}

TEST(FrameGateTest, TheOwningThreadIsRefusedInsteadOfWaitingForItself)
{
    // A RE-ENTRY IS THE ONE CASE A MUTEX CANNOT ANSWER: a `FrameUpdate::onFrame()` call (or a diagnostic sink)
    // driving another engine runs on the thread that already holds the gate, so waiting would be the caller waiting for itself. The
    // gate answers an EMPTY lease at once, which the backend turns into one report and `Refused`.
    FrameGate gate;

    const FrameGate::Exclusive outer = gate.enterExclusive();
    ASSERT_TRUE(static_cast<bool>(outer)) << "a free gate refused its first caller";
    EXPECT_TRUE(gate.heldByThisThread());
    EXPECT_FALSE(gate.inSharedEntry()) << "the exclusive side is not the shared one";

    const FrameGate::Exclusive inner = gate.enterExclusive();
    EXPECT_FALSE(static_cast<bool>(inner)) << "the owning thread was let in twice: a re-entry would deadlock";
    EXPECT_TRUE(gate.heldByThisThread()) << "the refused call took the gate away from its holder";
    EXPECT_EQ(gate.counters().acquisitions, 1U) << "a refused re-entry is not an acquisition";
}

TEST(FrameGateTest, ALeaseReleasesTheGateWhenItDies)
{
    // THE LEASE IS THE WHOLE POINT (see FrameGate): a frame has many early returns, and a forgotten `leave()` would
    // deadlock the NEXT frame instead of failing where the mistake is. Releasing is therefore the destructor's job,
    // and this case is what proves the destructor does it.
    FrameGate gate;

    {
        const FrameGate::Exclusive lease = gate.enterExclusive();
        ASSERT_TRUE(static_cast<bool>(lease));
        EXPECT_TRUE(gate.heldByThisThread());
    }
    EXPECT_FALSE(gate.heldByThisThread()) << "the lease died and the gate stayed taken";

    const FrameGate::Exclusive again = gate.enterExclusive();
    EXPECT_TRUE(static_cast<bool>(again)) << "the gate was never released";
}

TEST(FrameGateTest, AnotherThreadEntersOnlyAfterTheLeaseDies)
{
    // THE BLOCKING HALF, and the assertion is ORDERING, not duration: the second thread records that it is about to
    // ask, and the gate must not let it in while the lease lives. The short wait below only gives that thread time to
    // reach `enter()`; a gate that let it through would have recorded `2` already.
    FrameGate gate;
    std::atomic<int> phase{ 0 };

    std::optional<FrameGate::Exclusive> lease = gate.enterExclusive();
    ASSERT_TRUE(lease.has_value());

    std::thread other([&phase, &gate]() {
        phase.store(1); // "I am about to ask for the gate"
        const FrameGate::Exclusive theirs = gate.enterExclusive();
        if (static_cast<bool>(theirs)) {
            phase.store(2);
        }
    });

    while (phase.load() == 0) {
        std::this_thread::yield();
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    EXPECT_EQ(phase.load(), 1) << "a second thread drove a frame while one was held";
    EXPECT_EQ(gate.counters().waits, 1U) << "the blocked entry was not counted as a wait";

    lease.reset(); // the frame is over: the waiting thread may go
    other.join();
    EXPECT_EQ(phase.load(), 2) << "the waiting thread never got in after the lease died";
    EXPECT_EQ(gate.counters().acquisitions, 2U) << "both entries were taken once";

    const FrameGate::Exclusive after = gate.enterExclusive();
    EXPECT_TRUE(static_cast<bool>(after)) << "the gate was not released by the waiting thread's lease";
}

TEST(FrameGateTest, BodiesShareTheGateAndABodyInsideABodyIsRefused)
{
    // THE SHARED SIDE IS WHAT MAKES TWO DRIVERS' FRAMES OVERLAP: a second thread's body enters at once (no wait), and
    // the SAME thread asking again is refused - that is the frame-inside-a-frame case, which would otherwise be a
    // thread waiting for itself.
    FrameGate gate;

    const FrameGate::Shared outer = gate.enterShared();
    ASSERT_TRUE(static_cast<bool>(outer));
    EXPECT_TRUE(gate.inSharedEntry());
    EXPECT_TRUE(gate.heldByThisThread());

    std::atomic<bool> shared_at_once{ false };
    std::thread other([&gate, &shared_at_once]() {
        const FrameGate::Shared theirs = gate.enterShared();
        shared_at_once.store(static_cast<bool>(theirs));
    });
    other.join();
    EXPECT_TRUE(shared_at_once.load()) << "a second body waited for the first: the shared side is not shared";

    const FrameGate::Shared again = gate.enterShared();
    EXPECT_FALSE(static_cast<bool>(again)) << "a body entered itself: two frames would share one driver's machinery";
    EXPECT_TRUE(gate.inSharedEntry()) << "the refused call took the outer body's entry away";
    EXPECT_EQ(gate.counters().waits, 0U) << "no shared entry had to wait";
}

TEST(FrameGateTest, AHostVerbIsRefusedFromInsideABodyAndWaitsForAnotherThreads)
{
    // THE TWO DIRECTIONS THE DEVICE GATE HAS TO ANSWER: a host verb called from a frame body's thread would wait for
    // the frame it is itself inside (refused), and one called while ANOTHER thread's body runs waits and gets in
    // (that wait is the contract). Both are what turns "a sink called back into the device" into a refusal.
    FrameGate gate;

    std::optional<FrameGate::Shared> body = gate.enterShared();
    ASSERT_TRUE(body.has_value());

    const FrameGate::Exclusive from_inside = gate.enterExclusive();
    EXPECT_FALSE(static_cast<bool>(from_inside)) << "a host verb waited for the body it was called from";
    EXPECT_TRUE(gate.inSharedEntry()) << "the refused verb took the body's entry away";

    body.reset();
    const FrameGate::Exclusive from_outside = gate.enterExclusive();
    EXPECT_TRUE(static_cast<bool>(from_outside)) << "the gate stayed taken after the body ended";
}

TEST(FrameGateTest, ANestedGateOnAnotherObjectIsNotARefusal)
{
    // THE NESTING A FRAME REALLY HAS (see FrameGate): one gate's exclusive side around the frame, another gate's
    // shared side for the device. A single per-thread depth would call the inner one a re-entry - which is exactly
    // why the nesting is tracked per gate.
    FrameGate driver;
    FrameGate device;

    const FrameGate::Exclusive frame = driver.enterExclusive();
    ASSERT_TRUE(static_cast<bool>(frame));

    const FrameGate::Shared body = device.enterShared();
    EXPECT_TRUE(static_cast<bool>(body)) << "the device's gate mistook the driver's gate for itself";
    EXPECT_TRUE(driver.heldByThisThread());
    EXPECT_TRUE(device.inSharedEntry());
    EXPECT_FALSE(driver.inSharedEntry()) << "the sides of two gates were mixed up";
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
