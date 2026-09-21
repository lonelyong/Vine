/**
 * @brief The off-screen target and its readback: the evidence path itself, on a real device.
 *
 * "It rendered" and "validation was clean" are both satisfiable by a program that draws nothing, so this
 * backend's picture is only ever evidenced by PIXELS. These cases prove the path that produces them: a
 * pass that clears, a render pass that leaves the image where the copy needs it, a copy recorded in the
 * same command graph, and a probe that reads tightly packed RGBA8 rows - all with NO window, so the phase
 * that uses it later does not depend on a display server.
 *
 * The assertions are the two a phase will make: every pixel holds the clear colour (nothing partial, no
 * straggler from a previous frame), and a second frame reads back the same picture (the path is stable,
 * not accidentally correct once).
 */

#include <gtest/gtest.h>

#include <cmath>
#include <cstdint>
#include <optional>

#include <vsg/app/CommandGraph.h>
#include <vsg/app/Viewer.h>
#include <vsg/vk/Device.h>

#include <vine/vsg/api/Device.hpp>
#include <vine/vsg/api/OffscreenTarget.hpp>
#include <vine/vsg/core/FrameTimeline.hpp>
#include <vine/vsg/core/RetirementQueue.hpp>

using vine::vsg::DeviceResult;
using vine::vsg::OffscreenTarget;
using vine::vsg::createDevice;
using vine::vsg::core::FrameTimeline;
using vine::vsg::core::PixelProbe;
using vine::vsg::core::ReadbackKind;
using vine::vsg::core::ReadbackRefusal;
using vine::vsg::core::ReadbackRequest;
using vine::vsg::core::RetirementQueue;
using vine::vsg::core::Rgba8;

namespace
{

/// @brief The 8-bit value a clear colour quantises to (UNORM conversion, round to nearest).
std::uint8_t quantise(float value)
{
    return static_cast<std::uint8_t>(std::lround(value * 255.0F));
}

/// @brief The viewer and command graph of one recorded frame, kept alive so a case can still name what it
///        recorded (the reference counts of the replaced objects are part of one case's evidence).
struct RecordedFrame
{
    ::vsg::ref_ptr<::vsg::Viewer>       viewer;
    ::vsg::ref_ptr<::vsg::CommandGraph> graph;
};

/// @brief Records one frame into @p target (its pass, then its readback), submits it and waits for it.
///
/// The pass is built the way the executor builds it - `bootstrap = !written()` - so a case that resizes reads
/// exactly the fact a resize has to reset: a target whose attachments were replaced has never been written
/// into, and its next pass clears. That derivation is also what makes "the flag was not reset" visible at
/// all: recording the bootstrap graph directly would clear regardless and hide it.
///
/// A probe reads the mapped copy-back buffer, and nothing writes that buffer until a frame records the copy -
/// so no case may assert on pixels without going through a frame first, or it would assert on whatever the
/// buffer happened to hold.
///
/// @param created The device the target belongs to.
/// @param target  The target to record into (its current attachments and readback).
/// @param policy  What the frame asks its pass to clear (the target's own values, as a caller would pass them).
/// @return The frame's viewer and graph, both alive.
RecordedFrame recordOneFrame(const DeviceResult& created, OffscreenTarget& target,
                             const vine::vsg::core::ClearPolicy& policy, bool with_depth_capture = false)
{
    const ::vsg::ref_ptr<::vsg::RenderGraph> pass =
        target.passGraph(policy, /*bootstrap*/ !target.written(), /*depth_preserved*/ false);
    EXPECT_NE(pass, nullptr) << "the pass graph of a target with attachments is never empty";

    RecordedFrame frame;
    frame.viewer        = ::vsg::Viewer::create();
    frame.graph         = ::vsg::CommandGraph::create(created.device, created.queue_family);
    frame.graph->addChild(pass);
    // A readback the target refuses has no node at all (an unreadable format builds no buffer and no copy),
    // so the frame records what exists and nothing else.
    if (const ::vsg::ref_ptr<::vsg::Node> capture = target.capture()) {
        frame.graph->addChild(capture);
    }
    if (with_depth_capture) {
        if (const ::vsg::ref_ptr<::vsg::Node> depth_capture = target.captureDepth()) {
            frame.graph->addChild(depth_capture);
        }
    }
    frame.viewer->assignRecordAndSubmitTaskAndPresentation(::vsg::CommandGraphs{ frame.graph });
    EXPECT_TRUE(frame.viewer->compile());
    frame.viewer->advanceToNextFrame();
    frame.viewer->handleEvents();
    frame.viewer->recordAndSubmit();
    frame.viewer->deviceWaitIdle();
    return frame;
}

/// @brief The refusal @p target reports for one readback (the classification every entry point shares).
ReadbackRefusal refusalOf(const OffscreenTarget& target, ReadbackKind kind, std::uint32_t attachment = 0U)
{
    return target.readbackResult(ReadbackRequest{ kind, attachment }).refusal;
}

/// @brief The policy that reproduces a target's clear values (what a caller of a simple target passes).
///
/// @param color       The target's clear colour.
/// @param asked       Whether the pass ASKS to clear. A pass that does not is the interesting case after a
///                    resize: it must still end up over a cleared image, because the first writer's clear comes
///                    from the bootstrap rule and not from this flag.
/// @param depth_value The depth value the pass clears with, when it asks for one (the reverse-Z far plane is
///                    the default, so a case that reads the depth back has to say what it expects).
vine::vsg::core::ClearPolicy clearPolicy(const float (&color)[4], bool asked = true,
                                         std::optional<float> depth_value = std::nullopt)
{
    vine::vsg::core::ClearPolicy policy;
    policy.color = asked;
    for (std::size_t index = 0; index < 4U; ++index) {
        policy.color_value[index] = color[index];
    }
    if (depth_value.has_value()) {
        policy.depth       = true;
        policy.depth_value = depth_value.value();
    }
    return policy;
}

}  // namespace

TEST(OffscreenTargetTest, AClearedTargetReadsBackAsItsClearColour)
{
    const auto created = createDevice();
    if (!created.ok) {
        GTEST_SKIP() << "no device satisfies the device-floor requirements: "
                     << std::string(reinterpret_cast<const char*>(created.error.data()), created.error.size());
    }

    auto target = OffscreenTarget::create(created.device, OffscreenTarget::Layout{ 8U, 4U,
                                                                                   { 0.25F, 0.5F, 0.75F, 1.0F } });
    ASSERT_NE(target, nullptr) << "the target (image, pass, framebuffer, copy-back) must be creatable";

    auto viewer        = ::vsg::Viewer::create();
    auto command_graph = ::vsg::CommandGraph::create(created.device, created.queue_family);
    command_graph->addChild(target->renderGraph());
    command_graph->addChild(target->capture());  // recorded AFTER the pass, on the same queue, in order
    viewer->assignRecordAndSubmitTaskAndPresentation(::vsg::CommandGraphs{ command_graph });
    ASSERT_TRUE(viewer->compile()) << "the graph must compile";

    viewer->advanceToNextFrame();
    viewer->handleEvents();
    viewer->recordAndSubmit();
    viewer->deviceWaitIdle();

    const PixelProbe probe = target->probe();
    ASSERT_TRUE(probe.valid());
    EXPECT_EQ(probe.width(), 8);
    EXPECT_EQ(probe.height(), 4);

    // Every pixel is the clear colour, within the one-step tolerance a UNORM conversion can introduce.
    const Rgba8 expected{ quantise(0.25F), quantise(0.5F), quantise(0.75F), 255U };
    const Rgba8 sampled = probe.pixel(4, 2);
    EXPECT_NEAR(sampled.r, expected.r, 1);
    EXPECT_NEAR(sampled.g, expected.g, 1);
    EXPECT_NEAR(sampled.b, expected.b, 1);
    EXPECT_EQ(sampled.a, expected.a);
    EXPECT_TRUE(probe.wholeImageMatches(sampled))
        << "the clear is uniform: any pixel that differs is a region the pass did not write";
    EXPECT_DOUBLE_EQ(probe.nonBlackFraction(), 1.0) << "a clear colour that is not black is still not 'empty'";
}

TEST(OffscreenTargetTest, ABlackClearReadsBackAsNoContent)
{
    const auto created = createDevice();
    if (!created.ok) {
        GTEST_SKIP() << "no device satisfies the device-floor requirements";
    }

    auto target = OffscreenTarget::create(created.device, OffscreenTarget::Layout{ 4U, 4U, { 0.0F, 0.0F, 0.0F, 1.0F } });
    ASSERT_NE(target, nullptr);

    auto viewer        = ::vsg::Viewer::create();
    auto command_graph = ::vsg::CommandGraph::create(created.device, created.queue_family);
    command_graph->addChild(target->renderGraph());
    command_graph->addChild(target->capture());
    viewer->assignRecordAndSubmitTaskAndPresentation(::vsg::CommandGraphs{ command_graph });
    ASSERT_TRUE(viewer->compile());

    viewer->advanceToNextFrame();
    viewer->handleEvents();
    viewer->recordAndSubmit();
    viewer->deviceWaitIdle();

    const PixelProbe probe = target->probe();
    ASSERT_TRUE(probe.valid());
    EXPECT_EQ(probe.nonBlackPixels(), 0U) << "a phase that draws nothing must be able to see that it drew nothing";
    EXPECT_TRUE(probe.wholeImageMatches(Rgba8{ 0U, 0U, 0U, 255U }));
}

TEST(OffscreenTargetTest, ASecondFrameReadsBackTheSamePicture)
{
    const auto created = createDevice();
    if (!created.ok) {
        GTEST_SKIP() << "no device satisfies the device-floor requirements";
    }

    auto target = OffscreenTarget::create(created.device, OffscreenTarget::Layout{ 16U, 16U, { 1.0F, 0.0F, 0.0F, 1.0F } });
    ASSERT_NE(target, nullptr);

    auto viewer        = ::vsg::Viewer::create();
    auto command_graph = ::vsg::CommandGraph::create(created.device, created.queue_family);
    command_graph->addChild(target->renderGraph());
    command_graph->addChild(target->capture());
    viewer->assignRecordAndSubmitTaskAndPresentation(::vsg::CommandGraphs{ command_graph });
    ASSERT_TRUE(viewer->compile());

    Rgba8 first_pixel{};
    for (int frame = 0; frame < 2; ++frame) {
        viewer->advanceToNextFrame();
        viewer->handleEvents();
        viewer->recordAndSubmit();
        viewer->deviceWaitIdle();

        const PixelProbe probe = target->probe();
        ASSERT_TRUE(probe.valid());
        const Rgba8 center = probe.pixel(8, 8);
        EXPECT_NEAR(center.r, 255, 1) << "frame " << frame;
        EXPECT_NEAR(center.g, 0, 1);
        EXPECT_NEAR(center.b, 0, 1);
        EXPECT_TRUE(probe.wholeImageMatches(center)) << "frame " << frame;
        if (frame == 0) {
            first_pixel = center;
        }
        else {
            EXPECT_EQ(center, first_pixel) << "the readback must be stable frame to frame";
        }
    }
}

TEST(OffscreenTargetTest, AResizeReplacesTheExtentAndParksWhatItReplaced)
{
    const auto created = createDevice();
    if (!created.ok) {
        GTEST_SKIP() << "no device satisfies the device-floor requirements";
    }

    auto target = OffscreenTarget::create(created.device,
                                          OffscreenTarget::Layout{ 8U, 4U, { 0.25F, 0.5F, 0.75F, 1.0F } });
    ASSERT_NE(target, nullptr);

    const float kClear[4]{ 0.25F, 0.5F, 0.75F, 1.0F };

    // Frame 1 through the graph the executor derives: the target works BEFORE the resize, so "it still works
    // after" is a change rather than the first time anything ran.
    const RecordedFrame first_frame = recordOneFrame(created, *target, clearPolicy(kClear));
    {
        const PixelProbe probe = target->probe();
        ASSERT_TRUE(probe.valid());
        EXPECT_EQ(probe.width(), 8);
        EXPECT_EQ(probe.height(), 4);
    }
    EXPECT_EQ(target->generation(), 0U) << "a fresh target has replaced nothing yet";
    EXPECT_TRUE(target->written()) << "a pass has been recorded into the attachments";

    // The graph of the set that is about to be replaced. Its REFERENCE COUNT is the observable that tells
    // parking from dropping: this case holds one reference, the frame that recorded it holds another, and the
    // set the target no longer serves still has to be reachable - from the queue.
    const ::vsg::ref_ptr<::vsg::RenderGraph> replaced_graph = target->renderGraph();
    const unsigned int                       counts_before  = replaced_graph->referenceCount();

    FrameTimeline timeline;
    const auto    token = timeline.begin();
    timeline.submitted(token);  // the frame that recorded those attachments: retire point = 1 + 3 + 1
    RetirementQueue queue(3U);

    const OffscreenTarget::Resized resized = target->resize(16U, 12U, timeline, queue);

    EXPECT_EQ(static_cast<int>(resized.decision.action), static_cast<int>(vine::vsg::core::TargetAction::ResizeInPlace));
    EXPECT_FALSE(resized.refused);
    EXPECT_TRUE(resized.replaced);
    EXPECT_TRUE(resized.parked) << "the objects a submission may still name are parked, not freed";
    EXPECT_EQ(resized.generation, 1U);
    EXPECT_EQ(queue.pending(), 1U);
    EXPECT_EQ(queue.deviceWaits(), 0U) << "parking is exactly what keeps this path from stopping the device";
    EXPECT_EQ(target->generation(), 1U);
    EXPECT_EQ(target->width(), 16U);
    EXPECT_EQ(target->height(), 12U);
    EXPECT_FALSE(target->written()) << "the new attachments have never been drawn into: the next pass in clears";
    EXPECT_NE(target->renderGraph(), replaced_graph)
        << "the graph built around the old framebuffer cannot serve the new extent";
    EXPECT_EQ(replaced_graph->referenceCount(), counts_before)
        << "the replaced set is still alive right after the resize: the queue holds it, and a resize that "
           "dropped it would free images a frame in flight may still name";

    // Frame 2 through the graph the resize built: the new extent has to RENDER, not just exist. `written()`
    // being false is what makes this pass the first writer again, so the fresh images are CLEARED - and the
    // pass does not ask for a clear, so a target that forgot it had been replaced would LOAD images nobody
    // wrote, which is what the picture below catches.
    const RecordedFrame second_frame = recordOneFrame(created, *target, clearPolicy(kClear, /*asked*/ false));
    {
        const PixelProbe probe = target->probe();
        ASSERT_TRUE(probe.valid());
        EXPECT_EQ(probe.width(), 16);
        EXPECT_EQ(probe.height(), 12) << "the probe follows the target's own extent, so a stale capture shows";
        const Rgba8 expected{ quantise(0.25F), quantise(0.5F), quantise(0.75F), 255U };
        const Rgba8 sampled = probe.pixel(8, 6);
        EXPECT_NEAR(sampled.r, expected.r, 1);
        EXPECT_NEAR(sampled.g, expected.g, 1);
        EXPECT_NEAR(sampled.b, expected.b, 1);
        EXPECT_TRUE(probe.wholeImageMatches(sampled))
            << "the whole new extent holds the clear: a resize that left part of the image (or the old "
               "framebuffer's extent) behind would show here";
    }

    // The release the queue was waiting for: past the retire point, the parked set goes - and it is the
    // queue's hold (and only it) that goes away.
    timeline.completeUpTo(queue.retirePoint(timeline));
    queue.advance(timeline);
    EXPECT_EQ(queue.released(), 1U);
    EXPECT_EQ(queue.pending(), 0U);
    EXPECT_EQ(replaced_graph->referenceCount() + 1U, counts_before)
        << "the queue held the replaced set and let go exactly once";
    EXPECT_TRUE(target->probe().valid()) << "the release of the old set must not touch the live one";
}

TEST(OffscreenTargetTest, ARedundantResizeRepairsNothingAndReplacesNothing)
{
    const auto created = createDevice();
    if (!created.ok) {
        GTEST_SKIP() << "no device satisfies the device-floor requirements";
    }

    auto target = OffscreenTarget::create(created.device,
                                          OffscreenTarget::Layout{ 8U, 4U, { 0.0F, 1.0F, 0.0F, 1.0F } });
    ASSERT_NE(target, nullptr);

    const float kClear[4]{ 0.0F, 1.0F, 0.0F, 1.0F };

    const RecordedFrame frame = recordOneFrame(created, *target, clearPolicy(kClear));
    const ::vsg::ref_ptr<::vsg::RenderGraph> graph = target->renderGraph();

    FrameTimeline   timeline;
    RetirementQueue queue(3U);

    // The same extent: the plan says None, and "the caller asked for the extent it already has" must not be a
    // replacement - that would regenerate images (and bump the generation) once per frame for nothing.
    const OffscreenTarget::Resized resized = target->resize(8U, 4U, timeline, queue);
    EXPECT_EQ(static_cast<int>(resized.decision.action), static_cast<int>(vine::vsg::core::TargetAction::None));
    EXPECT_FALSE(resized.replaced);
    EXPECT_FALSE(resized.refused);
    EXPECT_FALSE(resized.parked);
    EXPECT_EQ(resized.generation, 0U);
    EXPECT_EQ(target->width(), 8U);
    EXPECT_EQ(target->height(), 4U);
    EXPECT_EQ(queue.pending(), 0U) << "nothing was replaced, so there is nothing to park";
    EXPECT_EQ(target->renderGraph(), graph) << "the same extent keeps the same attachments";

    // An extent of 0 is the other "nothing changes" answer: the plan repairs (a frame may not have learned its
    // size yet) and no GPU object is touched.
    const OffscreenTarget::Resized repaired = target->resize(0U, 4U, timeline, queue);
    EXPECT_EQ(static_cast<int>(repaired.decision.action), static_cast<int>(vine::vsg::core::TargetAction::Repair));
    EXPECT_EQ(static_cast<int>(repaired.decision.reason), static_cast<int>(vine::vsg::core::RepairReason::SizeUnknown));
    EXPECT_FALSE(repaired.replaced);
    EXPECT_FALSE(repaired.refused);
    EXPECT_EQ(repaired.generation, 0U);
    EXPECT_EQ(target->width(), 8U) << "a refused extent leaves the target serving the one it has";
    EXPECT_EQ(target->renderGraph(), graph);
    EXPECT_EQ(queue.pending(), 0U);

    // And it still renders: "nothing changed" is a claim about the pictures.
    const RecordedFrame again = recordOneFrame(created, *target, clearPolicy(kClear));
    {
        const PixelProbe probe = target->probe();
        ASSERT_TRUE(probe.valid());
        EXPECT_EQ(probe.width(), 8);
        const Rgba8 sampled = probe.pixel(4, 2);
        EXPECT_NEAR(sampled.g, 255, 1);
        EXPECT_TRUE(probe.wholeImageMatches(sampled));
    }
}

TEST(OffscreenTargetTest, AResizeWithoutAParkingWindowCountsTheDeviceIdle)
{
    const auto created = createDevice();
    if (!created.ok) {
        GTEST_SKIP() << "no device satisfies the device-floor requirements";
    }

    auto target = OffscreenTarget::create(created.device,
                                          OffscreenTarget::Layout{ 8U, 4U, { 1.0F, 1.0F, 0.0F, 1.0F } });
    ASSERT_NE(target, nullptr);

    // The frame that names the old attachments is submitted and NOT waited for: the work may still be in
    // flight, which is the situation the fallback exists for. Only the queue's counted device idle makes the
    // release safe - and the count is the judge of that, because "the driver finished quickly" is not
    // evidence.
    auto viewer        = ::vsg::Viewer::create();
    auto command_graph = ::vsg::CommandGraph::create(created.device, created.queue_family);
    command_graph->addChild(target->renderGraph());
    command_graph->addChild(target->capture());
    viewer->assignRecordAndSubmitTaskAndPresentation(::vsg::CommandGraphs{ command_graph });
    ASSERT_TRUE(viewer->compile());
    viewer->advanceToNextFrame();
    viewer->handleEvents();
    viewer->recordAndSubmit();

    const ::vsg::ref_ptr<::vsg::RenderGraph> replaced_graph = target->renderGraph();
    const unsigned int                       counts_before  = replaced_graph->referenceCount();

    FrameTimeline timeline;
    const auto    token = timeline.begin();
    timeline.submitted(token);
    RetirementQueue queue(0U);  // a caller that never learned how many frames may be in flight

    const OffscreenTarget::Resized resized = target->resize(16U, 8U, timeline, queue);

    EXPECT_TRUE(resized.replaced);
    EXPECT_FALSE(resized.parked) << "no parking window means no parking, and the queue must say so";
    EXPECT_EQ(queue.pending(), 0U);
    EXPECT_EQ(queue.deviceWaits(), 1U) << "the release runs under a COUNTED device idle: that count is the "
                                         "difference between a safe destroy and a guess";
    EXPECT_LT(replaced_graph->referenceCount(), counts_before)
        << "with no safe window the objects really go, and only under the counted idle";
    EXPECT_EQ(target->width(), 16U);
    EXPECT_EQ(target->height(), 8U);

    viewer->deviceWaitIdle();
    const float kClear[4]{ 1.0F, 1.0F, 0.0F, 1.0F };
    const RecordedFrame frame = recordOneFrame(created, *target, clearPolicy(kClear));
    {
        const PixelProbe probe = target->probe();
        ASSERT_TRUE(probe.valid());
        EXPECT_EQ(probe.width(), 16);
        EXPECT_EQ(probe.height(), 8);
        const Rgba8 sampled = probe.pixel(8, 4);
        EXPECT_NEAR(sampled.r, 255, 1);
        EXPECT_NEAR(sampled.g, 255, 1);
        EXPECT_TRUE(probe.wholeImageMatches(sampled));
    }
}

TEST(OffscreenTargetTest, AResizeRebuildsTheDepthReadbackForTheNewExtent)
{
    const auto created = createDevice();
    if (!created.ok) {
        GTEST_SKIP() << "no device satisfies the device-floor requirements";
    }

    OffscreenTarget::TargetLayout layout;
    layout.width         = 8U;
    layout.height        = 6U;
    layout.color_formats = { vine::graphics::RenderTarget::ColorFormat::RGBA8 };
    layout.depth_format  = vine::graphics::RenderTarget::DepthFormat::D32;
    layout.clear.color   = true;
    layout.clear.color_value[0] = 0.0F;
    layout.clear.color_value[1] = 0.0F;
    layout.clear.color_value[2] = 0.0F;
    layout.clear.color_value[3] = 1.0F;

    auto target = OffscreenTarget::create(created.device, layout);
    ASSERT_NE(target, nullptr);

    // A frame that records the pass AND the depth readback: the depth copy is what makes the depth probe
    // meaningful (it reads the mapped copy-back buffer, not the image).
    {
        auto viewer        = ::vsg::Viewer::create();
        auto command_graph = ::vsg::CommandGraph::create(created.device, created.queue_family);
        command_graph->addChild(target->renderGraph());
        command_graph->addChild(target->capture());
        command_graph->addChild(target->captureDepth());
        viewer->assignRecordAndSubmitTaskAndPresentation(::vsg::CommandGraphs{ command_graph });
        ASSERT_TRUE(viewer->compile());
        viewer->advanceToNextFrame();
        viewer->handleEvents();
        viewer->recordAndSubmit();
        viewer->deviceWaitIdle();
    }
    const vine::vsg::core::DepthProbe before = target->depthProbe();
    if (!before.valid()) {
        GTEST_SKIP() << "the depth attachment of this device cannot be read back";
    }
    EXPECT_EQ(before.width(), 8);
    EXPECT_EQ(before.height(), 6);

    FrameTimeline   timeline;
    RetirementQueue queue(3U);
    const OffscreenTarget::Resized resized = target->resize(20U, 10U, timeline, queue);
    EXPECT_TRUE(resized.replaced);
    EXPECT_TRUE(resized.parked);

    {
        auto viewer        = ::vsg::Viewer::create();
        auto command_graph = ::vsg::CommandGraph::create(created.device, created.queue_family);
        command_graph->addChild(target->renderGraph());
        command_graph->addChild(target->capture());
        command_graph->addChild(target->captureDepth());
        viewer->assignRecordAndSubmitTaskAndPresentation(::vsg::CommandGraphs{ command_graph });
        ASSERT_TRUE(viewer->compile());
        viewer->advanceToNextFrame();
        viewer->handleEvents();
        viewer->recordAndSubmit();
        viewer->deviceWaitIdle();
    }

    const vine::vsg::core::DepthProbe after = target->depthProbe();
    ASSERT_TRUE(after.valid()) << "the depth readback has to survive a resize (buffer, mapping and copy are all "
                                  "extent-sized)";
    EXPECT_EQ(after.width(), 20) << "the depth copy follows the new extent";
    EXPECT_EQ(after.height(), 10);
    EXPECT_NEAR(after.depthAt(10, 5), before.depthAt(4, 3), 0.0001F)
        << "the depth is cleared by the same policy before and after: the value a caller reads must not "
           "change because the image was rebuilt";
}

TEST(OffscreenTargetTest, AResizeIsRefusedWhileADepthLeaseIsInForce)
{
    const auto created = createDevice();
    if (!created.ok) {
        GTEST_SKIP() << "no device satisfies the device-floor requirements";
    }

    OffscreenTarget::TargetLayout layout;
    layout.width         = 8U;
    layout.height        = 6U;
    layout.color_formats = { vine::graphics::RenderTarget::ColorFormat::RGBA8 };
    layout.depth_format  = vine::graphics::RenderTarget::DepthFormat::D32;
    layout.clear.color   = true;
    layout.clear.color_value[0] = 0.5F;

    auto lender = OffscreenTarget::create(created.device, layout);
    ASSERT_NE(lender, nullptr);
    auto borrower = OffscreenTarget::create(created.device, layout, lender.get());
    ASSERT_NE(borrower, nullptr) << "the borrower (its own colour, the lender's depth) must be creatable";

    FrameTimeline   timeline;
    RetirementQueue queue(3U);

    // The image a borrower's framebuffer names belongs to the LENDER: replacing it would leave the borrower
    // pointing at a destroyed image, and this object does not know who its borrowers are. The plan wanted the
    // resize; the lease is what stops it.
    const OffscreenTarget::Resized lent = lender->resize(16U, 12U, timeline, queue);
    EXPECT_EQ(static_cast<int>(lent.decision.action), static_cast<int>(vine::vsg::core::TargetAction::ResizeInPlace));
    EXPECT_TRUE(lent.refused) << "a target another target loads the depth of does not resize";
    EXPECT_FALSE(lent.replaced);
    EXPECT_EQ(lent.generation, 0U);
    EXPECT_EQ(lender->width(), 8U);
    EXPECT_EQ(lender->height(), 6U);
    EXPECT_EQ(queue.pending(), 0U) << "nothing was replaced, so there is nothing to park";

    // The borrower side refuses too, for a different reason: its new framebuffer would name the lender's image
    // at the LENDER's extent, and a framebuffer attachment must be at least as large as the framebuffer.
    const OffscreenTarget::Resized borrowed = borrower->resize(16U, 12U, timeline, queue);
    EXPECT_TRUE(borrowed.refused) << "an extent is not the borrower's to move while it draws against another "
                                     "target's depth";
    EXPECT_FALSE(borrowed.replaced);
    EXPECT_EQ(borrower->width(), 8U);
    EXPECT_EQ(queue.pending(), 0U);

    // The lease is the only thing that blocked it: once the borrower is gone, the same call replaces the
    // attachments.
    borrower.reset();
    const OffscreenTarget::Resized loan_repaid = lender->resize(16U, 12U, timeline, queue);
    EXPECT_FALSE(loan_repaid.refused) << "the refusal is the lease, not the target";
    EXPECT_TRUE(loan_repaid.replaced);
    EXPECT_TRUE(loan_repaid.parked);
    EXPECT_EQ(lender->width(), 16U);
    EXPECT_EQ(lender->height(), 12U);
    EXPECT_EQ(lender->generation(), 1U);

    // And the resized lender can lend again: the new depth image is a legal source, which is the half a
    // bookkeeping-only assertion would miss.
    auto next_borrower = OffscreenTarget::create(created.device, layout, lender.get());
    EXPECT_NE(next_borrower, nullptr) << "a resized lender offers its new depth like the old one";

    // The pixels agree with the bookkeeping: nothing about the refusals changed what the lender serves.
    const RecordedFrame frame = recordOneFrame(created, *lender, clearPolicy(layout.clear.color_value));
    {
        const PixelProbe probe = lender->probe();
        ASSERT_TRUE(probe.valid());
        EXPECT_EQ(probe.width(), 16);
        EXPECT_EQ(probe.height(), 12);
        const Rgba8 sampled = probe.pixel(8, 6);
        EXPECT_NEAR(sampled.r, quantise(0.5F), 1);
        EXPECT_TRUE(probe.wholeImageMatches(sampled));
    }
}

TEST(OffscreenTargetTest, AProbeBeforeAnyCaptureIsRefusedNotAnswered)
{
    const auto created = createDevice();
    if (!created.ok) {
        GTEST_SKIP() << "no device satisfies the device-floor requirements";
    }

    OffscreenTarget::TargetLayout layout;
    layout.width         = 8U;
    layout.height        = 8U;
    layout.color_formats = { vine::graphics::RenderTarget::ColorFormat::RGBA8 };
    layout.depth_format  = vine::graphics::RenderTarget::DepthFormat::D32;
    layout.clear.color   = true;
    layout.clear.color_value[0] = 0.25F;
    layout.clear.color_value[1] = 0.5F;

    auto target = OffscreenTarget::create(created.device, layout);
    ASSERT_NE(target, nullptr);

    // Nothing has run: the mapped buffers hold whatever the allocation held, which is not a picture of anything.
    // A probe must say so instead of answering with it, and the classification must say WHY before any device
    // work - "run a frame first" is a category, not an empty buffer.
    EXPECT_FALSE(target->probe().valid());
    EXPECT_FALSE(target->depthProbe().valid());
    EXPECT_EQ(static_cast<int>(refusalOf(*target, ReadbackKind::Color)),
              static_cast<int>(ReadbackRefusal::NotCaptured));
    EXPECT_EQ(static_cast<int>(refusalOf(*target, ReadbackKind::Depth)),
              static_cast<int>(ReadbackRefusal::NotCaptured));
    EXPECT_TRUE(target->capture() != nullptr) << "the node exists: it is the RECORDING that has not happened";

    // The frame that records both copies: from here on both readbacks are servable, and they carry the pass'
    // clear values.
    const float kClear[4]{ 0.25F, 0.5F, 0.75F, 1.0F };
    const RecordedFrame frame = recordOneFrame(created, *target, clearPolicy(kClear), /*with_depth_capture*/ true);

    EXPECT_EQ(static_cast<int>(refusalOf(*target, ReadbackKind::Color)), static_cast<int>(ReadbackRefusal::None));
    EXPECT_EQ(static_cast<int>(refusalOf(*target, ReadbackKind::Depth)), static_cast<int>(ReadbackRefusal::None));

    const PixelProbe pixels = target->probe();
    ASSERT_TRUE(pixels.valid());
    EXPECT_EQ(pixels.width(), 8);
    EXPECT_EQ(pixels.height(), 8);
    const Rgba8 sampled = pixels.pixel(4, 4);
    EXPECT_NEAR(sampled.r, quantise(0.25F), 1);
    EXPECT_NEAR(sampled.g, quantise(0.5F), 1);
    EXPECT_TRUE(pixels.wholeImageMatches(sampled));

    const vine::vsg::core::DepthProbe depth = target->depthProbe();
    ASSERT_TRUE(depth.valid()) << "the depth copy was recorded too";
    EXPECT_EQ(depth.width(), 8);
    EXPECT_NEAR(depth.depthAt(4, 4), 0.0F, 0.0001F) << "the bootstrap clears the depth to the reverse-Z far plane";

    // And the refusal a request that can never be served gets is the same for the whole target's life: asking
    // for an attachment that does not exist stays `UnknownAttachment` once everything is captured.
    EXPECT_EQ(static_cast<int>(refusalOf(*target, ReadbackKind::Color, 3U)),
              static_cast<int>(ReadbackRefusal::UnknownAttachment));
}

TEST(OffscreenTargetTest, AD16DepthIsScaledByItsRangeAndAD24DepthIsRefused)
{
    const auto created = createDevice();
    if (!created.ok) {
        GTEST_SKIP() << "no device satisfies the device-floor requirements";
    }

    // D16: the attachment stores an unsigned integer, and the readback has to divide by ITS range - the
    // conversion the format defines. The clear value is what the pass really wrote, so the number a probe
    // answers with is the device's own conversion seen from the CPU.
    {
        OffscreenTarget::TargetLayout layout;
        layout.width         = 4U;
        layout.height        = 4U;
        layout.color_formats = { vine::graphics::RenderTarget::ColorFormat::RGBA8 };
        layout.depth_format  = vine::graphics::RenderTarget::DepthFormat::D16;
        layout.clear.color   = true;
        layout.clear.depth   = true;
        layout.clear.depth_value = 0.5F;

        auto target = OffscreenTarget::create(created.device, layout);
        ASSERT_NE(target, nullptr) << "a D16 depth attachment is an ordinary target";

        const float kClear[4]{ 0.0F, 0.0F, 0.0F, 1.0F };
        const RecordedFrame frame =
            recordOneFrame(created, *target, clearPolicy(kClear, /*asked*/ true, /*depth_value*/ 0.5F),
                           /*with_depth_capture*/ true);

        EXPECT_EQ(static_cast<int>(refusalOf(*target, ReadbackKind::Depth)),
                  static_cast<int>(ReadbackRefusal::None));
        const vine::vsg::core::DepthProbe depth = target->depthProbe();
        ASSERT_TRUE(depth.valid());
        EXPECT_NEAR(depth.depthAt(1, 1), 0.5F, 0.0001F)
            << "0.5 clears to 32768 of 65535; reading the stored integer raw would answer ~32768, and reading "
               "it as a float would answer nonsense";
        EXPECT_NEAR(depth.depthAt(0, 0), 0.5F, 0.0001F);
    }

    // D24 is the COMBINED depth/stencil format: there is no plain depth copy, so the honest answer is a
    // refusal - and the rest of the target is still a picture.
    {
        OffscreenTarget::TargetLayout layout;
        layout.width         = 4U;
        layout.height        = 4U;
        layout.color_formats = { vine::graphics::RenderTarget::ColorFormat::RGBA8 };
        layout.depth_format  = vine::graphics::RenderTarget::DepthFormat::D24;
        layout.clear.color   = true;
        layout.clear.color_value[0] = 0.0F;
        layout.clear.color_value[1] = 0.5F;

        auto target = OffscreenTarget::create(created.device, layout);
        ASSERT_NE(target, nullptr);

        EXPECT_EQ(static_cast<int>(refusalOf(*target, ReadbackKind::Depth)),
                  static_cast<int>(ReadbackRefusal::UnreadableFormat));
        EXPECT_EQ(target->captureDepth(), nullptr)
            << "no depth destination and no copy are built for a format that cannot be read";
        EXPECT_FALSE(target->depthProbe().valid());
        EXPECT_TRUE(target->capture() != nullptr) << "the colour half is unaffected";
        EXPECT_TRUE(target->readback() != nullptr);

        const float kClear[4]{ 0.0F, 0.5F, 0.0F, 1.0F };
        const RecordedFrame frame = recordOneFrame(created, *target, clearPolicy(kClear), /*with_depth_capture*/ true);
        const PixelProbe pixels = target->probe();
        ASSERT_TRUE(pixels.valid()) << "a target whose depth cannot be read still renders";
        const Rgba8 sampled = pixels.pixel(2, 2);
        EXPECT_NEAR(sampled.g, quantise(0.5F), 1);
        EXPECT_TRUE(pixels.wholeImageMatches(sampled));
    }
}

TEST(OffscreenTargetTest, AFloatColourAttachmentIsRenderedButNotReadBack)
{
    const auto created = createDevice();
    if (!created.ok) {
        GTEST_SKIP() << "no device satisfies the device-floor requirements";
    }

    OffscreenTarget::TargetLayout layout;
    layout.width         = 4U;
    layout.height        = 4U;
    layout.color_formats = { vine::graphics::RenderTarget::ColorFormat::RGBA16F };
    layout.depth_format  = vine::graphics::RenderTarget::DepthFormat::D32;
    layout.clear.color   = true;
    layout.clear.depth   = true;
    layout.clear.depth_value = 0.8F;

    auto target = OffscreenTarget::create(created.device, layout);
    ASSERT_NE(target, nullptr) << "an RGBA16F target is an ordinary target: only its READBACK is refused";

    // No destination buffer and no copy: the readback packs RGBA8, and a buffer sized as if a 16-bit texel were
    // 8-bit would make the copy write past its end.
    EXPECT_EQ(target->capture(), nullptr);
    EXPECT_EQ(target->readback(), nullptr) << "the executor appends what a target offers: this one offers nothing";
    EXPECT_EQ(static_cast<int>(refusalOf(*target, ReadbackKind::Color)),
              static_cast<int>(ReadbackRefusal::UnreadableFormat));
    EXPECT_FALSE(target->probe().valid());

    // The pass still runs, and the DEPTH attachment (D32, readable) says so: the frame clears it to the policy's
    // value through this target's render pass. The colour half is proven elsewhere (the same pass, the same
    // graph); what this case pins is that refusing a readback never turns into refusing to render.
    const float kClear[4]{ 1.0F, 0.0F, 0.0F, 1.0F };
    const RecordedFrame frame =
        recordOneFrame(created, *target, clearPolicy(kClear, /*asked*/ true, /*depth_value*/ 0.8F),
                       /*with_depth_capture*/ true);

    EXPECT_EQ(static_cast<int>(refusalOf(*target, ReadbackKind::Depth)),
              static_cast<int>(ReadbackRefusal::None));
    const vine::vsg::core::DepthProbe depth = target->depthProbe();
    ASSERT_TRUE(depth.valid());
    EXPECT_NEAR(depth.depthAt(2, 2), 0.8F, 0.0001F)
        << "the depth attachment of a float-colour target is read back like any other, and it holds this pass' "
           "clear value";
    EXPECT_EQ(static_cast<int>(refusalOf(*target, ReadbackKind::Color)),
              static_cast<int>(ReadbackRefusal::UnreadableFormat))
        << "capturing the depth does not change what the colour half can do";
}
