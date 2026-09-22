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

#include <vine/graphics/RenderCommand.hpp>
#include <vine/graphics/RenderTarget.hpp>

#include <vine/vsg/api/Device.hpp>
#include <vine/vsg/api/OffscreenTarget.hpp>
#include <vine/vsg/core/Diagnostics.hpp>
#include <vine/vsg/core/FrameArena.hpp>
#include <vine/vsg/core/FrameCompiler.hpp>
#include <vine/vsg/core/FrameRecorder.hpp>
#include <vine/vsg/core/FrameTimeline.hpp>
#include <vine/vsg/core/Observe.hpp>
#include <vine/vsg/core/RetirementQueue.hpp>

#include "DevicePhases.hpp"

using vine::graphics::RenderCommand;
using vine::graphics::RenderTarget;
using vine::vsg::core::CompiledPass;
using vine::vsg::core::Diagnostics;
using vine::vsg::core::FrameArena;
using vine::vsg::core::FrameCompiler;
using vine::vsg::core::FrameRecorder;
using vine::vsg::core::FrameToken;
using vine::vsg::core::Observe;
using vine::vsg::core::TargetAction;
using vine::vsg::core::TargetFacts;
using vine::vsg::core::TargetInstance;
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
/// @param with_depth_capture Whether the frame records the depth copy too (a target that has no depth
///        readback records nothing, so the flag is safe on any target).
/// @param color_captures How many colour attachments the frame records copies of (a target with more than
///        one colour attachment needs one per attachment a case reads back).
/// @return The frame's viewer and graph, both alive.
RecordedFrame recordOneFrame(const DeviceResult& created, OffscreenTarget& target,
                             const vine::vsg::core::ClearPolicy& policy, bool with_depth_capture = false,
                             std::uint32_t color_captures = 1U)
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
    for (std::uint32_t attachment = 0U; attachment < color_captures; ++attachment) {
        if (const ::vsg::ref_ptr<::vsg::Node> capture = target.capture(attachment)) {
            frame.graph->addChild(capture);
        }
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

void runOffscreenReadbackPhase(const vine::vsg::DeviceResult& device, DevicePhaseCounters& counters)
{
    auto target = OffscreenTarget::create(device.device, OffscreenTarget::Layout{ 8U, 4U,
                                                                                   { 0.25F, 0.5F, 0.75F, 1.0F } });
    ASSERT_NE(target, nullptr) << "the target (image, pass, framebuffer, copy-back) must be creatable";
    ++counters.targets_built;

    auto viewer        = ::vsg::Viewer::create();
    auto command_graph = ::vsg::CommandGraph::create(device.device, device.queue_family);
    command_graph->addChild(target->renderGraph());
    command_graph->addChild(target->capture());  // recorded AFTER the pass, on the same queue, in order
    viewer->assignRecordAndSubmitTaskAndPresentation(::vsg::CommandGraphs{ command_graph });
    ASSERT_TRUE(viewer->compile()) << "the graph must compile";

    viewer->advanceToNextFrame();
    viewer->handleEvents();
    viewer->recordAndSubmit();
    viewer->deviceWaitIdle();
    ++counters.frames;

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

void runTargetResizePhase(const vine::vsg::DeviceResult& device, DevicePhaseCounters& counters)
{
    auto target = OffscreenTarget::create(device.device,
                                          OffscreenTarget::Layout{ 8U, 4U, { 0.25F, 0.5F, 0.75F, 1.0F } });
    ASSERT_NE(target, nullptr);
    ++counters.targets_built;

    const float kClear[4]{ 0.25F, 0.5F, 0.75F, 1.0F };

    // Frame 1 through the graph the executor derives: the target works BEFORE the resize, so "it still works
    // after" is a change rather than the first time anything ran.
    const RecordedFrame first_frame = recordOneFrame(device, *target, clearPolicy(kClear));
    ++counters.frames;
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
    const RecordedFrame second_frame = recordOneFrame(device, *target, clearPolicy(kClear, /*asked*/ false));
    ++counters.frames;
    ++counters.resizes_replaced;
    ++counters.parked;
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

void runTargetRebuildPhase(const vine::vsg::DeviceResult& device, DevicePhaseCounters& counters)
{
    const float kFirst[4]{ 0.25F, 0.5F, 0.75F, 1.0F };
    const float kSecond[4]{ 0.1F, 0.2F, 0.3F, 1.0F };

    auto target = OffscreenTarget::create(device.device,
                                          OffscreenTarget::Layout{ 8U, 4U,
                                                                   { kFirst[0], kFirst[1], kFirst[2], 1.0F } });
    ASSERT_NE(target, nullptr);
    ++counters.targets_built;

    // Frame 1 into the shape the target was created with: a rebuilt target is one that WORKED before, so
    // "the new shape renders" is a change and not the first thing that ever ran.
    const RecordedFrame first = recordOneFrame(device, *target, clearPolicy(kFirst));
    ++counters.frames;
    ASSERT_TRUE(target->written()) << "a pass has been recorded into the attachments";
    EXPECT_EQ(target->colorAttachmentCount(), 1U);
    EXPECT_FALSE(target->hasDepth());
    {
        const PixelProbe probe = target->probe();
        ASSERT_TRUE(probe.valid());
        EXPECT_EQ(probe.width(), 8);
        EXPECT_EQ(probe.height(), 4);
    }

    // The shape that is about to be replaced: its compatibility half is what a pipeline key was built from,
    // and the graph of its attachments is the observable that tells parking from dropping (this case holds
    // one reference, the frame that recorded it holds another).
    const vine::vsg::core::TargetShape        old_shape     = target->shape();
    const ::vsg::ref_ptr<::vsg::RenderGraph>  replaced      = target->renderGraph();
    const unsigned int                        counts_before = replaced->referenceCount();

    // The new shape, in the terms the SDK asks for: TWO colour attachments and a depth, at a new extent -
    // the whole point of the Rebuild arm (the count is even visible to the executor's plan-vs-target check,
    // and the depth format is part of compatibility).
    OffscreenTarget::TargetLayout wanted;
    wanted.width         = 16U;
    wanted.height        = 12U;
    wanted.color_formats = { RenderTarget::ColorFormat::RGBA8, RenderTarget::ColorFormat::RGBA8 };
    wanted.depth_format  = RenderTarget::DepthFormat::D32F;
    wanted.clear         = clearPolicy(kSecond);

    FrameTimeline timeline;
    const auto    token = timeline.begin();
    timeline.submitted(token);  // the frame that recorded the attachments being replaced
    RetirementQueue queue(3U);

    const OffscreenTarget::Rebuilt rebuilt = target->rebuild(wanted, timeline, queue);

    EXPECT_EQ(static_cast<int>(rebuilt.decision.action), static_cast<int>(TargetAction::Rebuild))
        << "the shape changed: compatibility is the plan's answer, not this call's guess";
    EXPECT_FALSE(rebuilt.refused);
    EXPECT_TRUE(rebuilt.replaced) << "the target serves the new shape after the call";
    EXPECT_TRUE(rebuilt.parked) << "the old attachments AND the old render pass are parked, not freed";
    EXPECT_EQ(rebuilt.generation, 1U);
    EXPECT_EQ(queue.pending(), 1U);
    EXPECT_EQ(queue.deviceWaits(), 0U) << "parking is what keeps this path from stopping the device";
    EXPECT_EQ(target->generation(), 1U);
    EXPECT_EQ(target->width(), 16U);
    EXPECT_EQ(target->height(), 12U);
    EXPECT_EQ(target->colorAttachmentCount(), 2U);
    EXPECT_TRUE(target->hasDepth());
    EXPECT_FALSE(target->written()) << "the new attachments have never been drawn into: the next pass in clears";
    EXPECT_EQ(target->passVariantCount(), 1U)
        << "the old shape's load-op variants went with the old render pass: their passes were built from the "
           "old formats, and a pass of the new shape must not be handed one of them";
    EXPECT_TRUE(target->shape().color_formats == wanted.color_formats)
        << "the shape the target reports IS the new one: a pipeline key is built from it";
    const vine::vsg::core::TargetShape new_shape = target->shape();
    EXPECT_TRUE(new_shape.depth_format.has_value());
    EXPECT_FALSE(new_shape.compatibility() == old_shape.compatibility())
        << "the compatibility half really moved: every pipeline compiled for the old shape is invalid, and "
           "that is what a caller re-compiles against";
    EXPECT_TRUE(target->instance().desc.shape == new_shape)
        << "the facts a frame is compiled with and the shape the target answers with are one fact";
    EXPECT_NE(target->renderGraph(), replaced)
        << "the graph built around the old framebuffer cannot serve the new shape";
    EXPECT_EQ(replaced->referenceCount(), counts_before)
        << "the replaced set is still alive right after the rebuild: the queue holds it, and a rebuild that "
           "dropped it would free a pass a frame in flight may still name";

    // Frame 2 through the graph the rebuild built: the new shape has to RENDER, not just exist. `written()`
    // being false makes this pass the first writer, so the fresh images are cleared - and the three readbacks
    // (attachment 0, attachment 1, the depth) are the evidence that the NEW pass and the NEW framebuffer
    // really have two colours and a depth.
    const RecordedFrame second =
        recordOneFrame(device, *target, clearPolicy(kSecond, /*asked*/ false), /*with_depth_capture*/ true,
                       /*color_captures*/ 2U);
    ++counters.frames;
    ++counters.rebuilds_replaced;
    ++counters.parked;
    {
        const PixelProbe probe = target->probe();
        ASSERT_TRUE(probe.valid());
        EXPECT_EQ(probe.width(), 16);
        EXPECT_EQ(probe.height(), 12) << "the probe follows the target's own extent, so a stale capture shows";
        const Rgba8 expected{ quantise(kSecond[0]), quantise(kSecond[1]), quantise(kSecond[2]), 255U };
        const Rgba8 sampled = probe.pixel(8, 6);
        EXPECT_NEAR(sampled.r, expected.r, 1);
        EXPECT_NEAR(sampled.g, expected.g, 1);
        EXPECT_NEAR(sampled.b, expected.b, 1);
        EXPECT_TRUE(probe.wholeImageMatches(sampled))
            << "the whole new extent holds the clear: a rebuild that left part of the image (or the old "
               "framebuffer's extent) behind would show here";
    }
    {
        // The second colour attachment is a new object in every sense: it did not exist before the rebuild,
        // and the pass clears it with the plan's extra-attachment value (transparent black).
        EXPECT_EQ(static_cast<int>(refusalOf(*target, ReadbackKind::Color, 1U)),
                  static_cast<int>(ReadbackRefusal::None))
            << "the rebuilt target has a SECOND colour attachment, and it is readable";
        const PixelProbe probe = target->probe(1U);
        ASSERT_TRUE(probe.valid());
        EXPECT_EQ(probe.width(), 16);
        EXPECT_EQ(probe.height(), 12);
        const Rgba8 sampled = probe.pixel(8, 6);
        EXPECT_EQ(sampled.r, 0U) << "an extra colour attachment clears to transparent black";
        EXPECT_EQ(sampled.g, 0U);
        EXPECT_EQ(sampled.b, 0U);
        EXPECT_EQ(sampled.a, 0U);
        EXPECT_TRUE(probe.wholeImageMatches(sampled));
    }
    {
        // The depth is the third new attachment, and it is a D32F image the bootstrap clears to the
        // reverse-Z far plane - the same number the created target answered with, from a shape that did
        // not exist until this call built it.
        const vine::vsg::core::DepthProbe depth = target->depthProbe();
        ASSERT_TRUE(depth.valid()) << "the depth copy was recorded too";
        EXPECT_EQ(depth.width(), 16);
        EXPECT_EQ(depth.height(), 12);
        EXPECT_NEAR(depth.depthAt(8, 6), 0.0F, 0.0001F);
    }

    // The release the queue was waiting for: past the retire point, the parked set goes - and it is the
    // queue's hold (and only it) that goes away.
    timeline.completeUpTo(queue.retirePoint(timeline));
    queue.advance(timeline);
    EXPECT_EQ(queue.released(), 1U);
    EXPECT_EQ(queue.pending(), 0U);
    EXPECT_EQ(replaced->referenceCount() + 1U, counts_before)
        << "the queue held the replaced set and let go exactly once";
    EXPECT_TRUE(target->probe().valid()) << "the release of the old set must not touch the live one";
}

void runLostSubmissionPhase(const vine::vsg::DeviceResult& device, DevicePhaseCounters& counters)
{
    const float kRed[4]{ 1.0F, 0.0F, 0.0F, 1.0F };
    const float kGreen[4]{ 0.0F, 1.0F, 0.0F, 1.0F };
    const float kBlue[4]{ 0.0F, 0.0F, 1.0F, 1.0F };

    auto target = OffscreenTarget::create(device.device, OffscreenTarget::Layout{ 8U, 8U, { kRed[0], kRed[1], kRed[2], 1.0F } });
    ASSERT_NE(target, nullptr);
    ++counters.targets_built;

    // The executor's loop, without the executor: the target's OWN facts go to the plan, the plan's bootstrap
    // flag goes to the pass graph, and the pixels say whether the two agreed. Nothing here keeps a private
    // idea of when the target must clear - that is the point of the case.
    FrameArena    arena(64 * 1024);
    Diagnostics   diagnostics;
    Observe       observe;
    FrameRecorder recorder{ arena, diagnostics, observe };
    FrameCompiler compiler{ arena, diagnostics, observe };

    vine::intrusive_ptr<RenderTarget> handle(new RenderTarget());
    const std::vector<RenderCommand>  one_draw{ RenderCommand{} };

    const auto facts_of = [&]() {
        TargetFacts facts;
        facts.target  = handle.get();
        facts.wanted  = vine::vsg::core::TargetDesc{ 8, 8, target->shape() };
        facts.current = target->instance();
        return facts;
    };
    const auto record_frame = [&](std::uint64_t token, const vine::vsg::core::ClearPolicy& policy) {
        EXPECT_TRUE(recorder.beginFrame(FrameToken{ token }));
        EXPECT_TRUE(recorder.beginPass(1U));
        EXPECT_TRUE(recorder.setRenderTarget(handle.get()));
        EXPECT_TRUE(recorder.setClearPolicy(policy));
        EXPECT_TRUE(recorder.render(one_draw, nullptr));
        EXPECT_TRUE(recorder.endPass());
        EXPECT_TRUE(recorder.endFrame());
        EXPECT_TRUE(recorder.swapBuffers());
        const std::vector<TargetFacts> table{ facts_of() };
        return &compiler.compile(recorder.description(), vine::vsg::core::FrameFacts{ table });
    };
    const auto submit = [&](const vine::vsg::core::CompiledPass& pass) {
        const ::vsg::ref_ptr<::vsg::RenderGraph> graph =
            target->passGraph(pass.clear, pass.bootstrap, pass.depth_preserved);
        EXPECT_NE(graph, nullptr);
        auto viewer        = ::vsg::Viewer::create();
        auto command_graph = ::vsg::CommandGraph::create(device.device, device.queue_family);
        command_graph->addChild(graph);
        if (const ::vsg::ref_ptr<::vsg::Node> capture = target->capture()) {
            command_graph->addChild(capture);
        }
        viewer->assignRecordAndSubmitTaskAndPresentation(::vsg::CommandGraphs{ command_graph });
        EXPECT_TRUE(viewer->compile());
        viewer->advanceToNextFrame();
        viewer->handleEvents();
        viewer->recordAndSubmit();
        viewer->deviceWaitIdle();
    };
    const auto sampled = [&]() { return target->probe().pixel(4, 4); };
    const auto expects = [&](const float (&color)[4], const char* what) {
        const Rgba8 pixel = sampled();
        EXPECT_NEAR(pixel.r, quantise(color[0]), 1) << what;
        EXPECT_NEAR(pixel.g, quantise(color[1]), 1) << what;
        EXPECT_NEAR(pixel.b, quantise(color[2]), 1) << what;
    };

    // Frame 1: the target EXISTS but has never been written into, so the plan must call it a bootstrap and the
    // pass has to clear - the fact comes from instance(), not from a caller's assumption.
    const vine::vsg::core::TargetInstance fresh = target->instance();
    EXPECT_EQ(fresh.desc.width, 8);
    EXPECT_EQ(fresh.desc.height, 8);
    EXPECT_EQ(fresh.generation, 0U);
    EXPECT_FALSE(fresh.built) << "images exist, but nothing has been written into them: loading is impossible";
    EXPECT_FALSE(fresh.attachments_invalidated);

    vine::vsg::core::ClearPolicy fill;
    fill.color          = true;
    fill.color_value[0] = kRed[0];
    fill.color_value[1] = kRed[1];
    fill.color_value[2] = kRed[2];
    fill.color_value[3] = 1.0F;

    const auto* first_frame = record_frame(1U, fill);
    ASSERT_NE(first_frame, nullptr);
    ASSERT_EQ(first_frame->passes.size(), 1U);
    EXPECT_EQ(static_cast<int>(first_frame->targets[0].decision.action),
              static_cast<int>(vine::vsg::core::TargetAction::Repair));
    EXPECT_EQ(static_cast<int>(first_frame->targets[0].decision.reason),
              static_cast<int>(vine::vsg::core::RepairReason::Bootstrap));
    EXPECT_TRUE(first_frame->passes[0].bootstrap) << "the first writer into an unwritten target clears";
    submit(first_frame->passes[0]);
    ++counters.frames;
    expects(kRed, "the bootstrap clear is what the frame shows");

    // The submission into that target is LOST (or the device went away): the contents can no longer be
    // trusted, and the target says so instead of pretending the frame landed.
    target->invalidateAttachments();
    EXPECT_TRUE(target->instance().attachments_invalidated);
    EXPECT_TRUE(target->instance().built) << "'lost' is not 'never written': the images still exist";

    // Frame 2 does NOT ask for a clear, and paints its own colour. If the plan honours the invalidation, the
    // pass is the bootstrap one and the colour below IS what the frame shows; if it does not, this frame loads
    // whatever the lost submission left behind (red).
    vine::vsg::core::ClearPolicy after_loss;
    after_loss.color          = false;
    after_loss.color_value[0] = kGreen[0];
    after_loss.color_value[1] = kGreen[1];
    after_loss.color_value[2] = kGreen[2];
    after_loss.color_value[3] = 1.0F;

    // A caller that IGNORES the plan (a load on an invalidated target) does not repair anything: loading an
    // image whose contents are unknown cannot make them known, so the fact has to survive it.
    const ::vsg::ref_ptr<::vsg::RenderGraph> ignored_plan =
        target->passGraph(after_loss, /*bootstrap*/ false, /*depth_preserved*/ false);
    ASSERT_NE(ignored_plan, nullptr);
    EXPECT_TRUE(target->instance().attachments_invalidated)
        << "only a pass that CLEARS repairs a lost submission; a load must not be able to claim it did";

    const auto* second_frame = record_frame(2U, after_loss);
    ASSERT_NE(second_frame, nullptr);
    EXPECT_EQ(static_cast<int>(second_frame->targets[0].decision.reason),
              static_cast<int>(vine::vsg::core::RepairReason::Bootstrap));
    EXPECT_TRUE(second_frame->passes[0].bootstrap) << "the frame that repairs the target clears";
    submit(second_frame->passes[0]);
    ++counters.frames;
    expects(kGreen, "the frame that re-bootstrapped the target cleared it with its own colour");

    // ... and ONE frame is enough: the clear repaired the fact, so the next frame loads what it left.
    EXPECT_FALSE(target->instance().attachments_invalidated) << "the bootstrapping frame repaired the fact";
    EXPECT_EQ(static_cast<int>(vine::vsg::core::planTarget(target->instance(),
                                                          vine::vsg::core::TargetDesc{ 8, 8, target->shape() })
                                   .action),
              static_cast<int>(vine::vsg::core::TargetAction::None));

    vine::vsg::core::ClearPolicy steady;
    steady.color          = false;
    steady.color_value[0] = kBlue[0];
    steady.color_value[1] = kBlue[1];
    steady.color_value[2] = kBlue[2];
    steady.color_value[3] = 1.0F;

    const auto* third_frame = record_frame(3U, steady);
    ASSERT_NE(third_frame, nullptr);
    EXPECT_FALSE(third_frame->passes[0].bootstrap) << "a repaired target is a load, not another clear";
    submit(third_frame->passes[0]);
    ++counters.frames;
    expects(kGreen, "the repair happens ONCE: a second clear would have painted this frame's colour");

    // And the same fact feeds the other lifecycle arm: a resize replaces the set the invalidation was about.
    target->invalidateAttachments();
    FrameTimeline   timeline;
    RetirementQueue queue(3U);
    const OffscreenTarget::Resized resized = target->resize(16U, 8U, timeline, queue);
    EXPECT_TRUE(resized.replaced);
    EXPECT_FALSE(target->instance().attachments_invalidated) << "the images that were lost are gone with the set";
    EXPECT_EQ(target->instance().generation, 1U);
    EXPECT_FALSE(target->instance().built) << "the new images have never been written into either";
}

TEST(OffscreenTargetTest, AClearedTargetReadsBackAsItsClearColour)
{
    // The phase body is also what the phase table runs (see DevicePhases.hpp): ONE spelling of the
    // capability, read out here as a gtest case and there as a `[selftest]` line.
    const auto created = createDevice();
    if (!created.ok) {
        GTEST_SKIP() << "no device satisfies the device-floor requirements: "
                     << std::string(reinterpret_cast<const char*>(created.error.data()), created.error.size());
    }
    DevicePhaseCounters counters;
    runOffscreenReadbackPhase(created, counters);
    EXPECT_EQ(counters.frames, 1U) << "the phase submitted its frame";
    EXPECT_EQ(counters.targets_built, 1U);
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
    DevicePhaseCounters counters;
    runTargetResizePhase(created, counters);
    EXPECT_EQ(counters.resizes_replaced, 1U);
    EXPECT_EQ(counters.parked, 1U);
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

TEST(OffscreenTargetTest, ARebuildIsRefusedWhileADepthLeaseIsInForce)
{
    const auto created = createDevice();
    if (!created.ok) {
        GTEST_SKIP() << "no device satisfies the device-floor requirements";
    }

    OffscreenTarget::TargetLayout layout;
    layout.width                = 8U;
    layout.height               = 6U;
    layout.color_formats        = { vine::graphics::RenderTarget::ColorFormat::RGBA8 };
    layout.depth_format         = vine::graphics::RenderTarget::DepthFormat::D32;
    layout.clear.color          = true;
    layout.clear.color_value[0] = 0.5F;

    auto lender = OffscreenTarget::create(created.device, layout);
    ASSERT_NE(lender, nullptr);
    auto borrower = OffscreenTarget::create(created.device, layout, lender.get());
    ASSERT_NE(borrower, nullptr) << "the borrower (its own colour, the lender's depth) must be creatable";

    // A shape a rebuild would really serve (two colours and a depth): it has to differ from the current one,
    // or the plan answers "nothing structural changed" before the lease is ever consulted.
    OffscreenTarget::TargetLayout wanted  = layout;
    wanted.color_formats                  = { vine::graphics::RenderTarget::ColorFormat::RGBA8,
                                              vine::graphics::RenderTarget::ColorFormat::RGBA8 };

    FrameTimeline   timeline;
    RetirementQueue queue(3U);

    // The lender half: every borrower's framebuffer names ITS image, and the lender does not know who its
    // borrowers are - so the shape it serves is not the lender's to change while one exists. The plan wanted
    // the rebuild; the lease is what stops it.
    const OffscreenTarget::Rebuilt lent = lender->rebuild(wanted, timeline, queue);
    EXPECT_EQ(static_cast<int>(lent.decision.action), static_cast<int>(TargetAction::Rebuild));
    EXPECT_TRUE(lent.refused) << "a target another target loads the depth of does not rebuild";
    EXPECT_FALSE(lent.replaced);
    EXPECT_EQ(lent.generation, 0U);
    EXPECT_EQ(lender->colorAttachmentCount(), 1U) << "the lender still serves the shape it had";
    EXPECT_EQ(queue.pending(), 0U) << "nothing was replaced, so there is nothing to park";

    // The borrower half refuses too: it holds another target's depth, and a new pass with new attachments
    // would have to adopt an image whose extent and layout belong to a lender this call was not told about.
    const OffscreenTarget::Rebuilt borrowed = borrower->rebuild(wanted, timeline, queue);
    EXPECT_EQ(static_cast<int>(borrowed.decision.action), static_cast<int>(TargetAction::Rebuild));
    EXPECT_TRUE(borrowed.refused) << "a shape is not the borrower's to change while it draws against another "
                                     "target's depth";
    EXPECT_FALSE(borrowed.replaced);
    EXPECT_EQ(borrower->colorAttachmentCount(), 1U);
    EXPECT_EQ(queue.pending(), 0U);

    // The lease is the only thing that blocked it: once the borrower is gone, the same call rebuilds.
    borrower.reset();
    const OffscreenTarget::Rebuilt loan_repaid = lender->rebuild(wanted, timeline, queue);
    EXPECT_FALSE(loan_repaid.refused) << "the refusal is the lease, not the target";
    EXPECT_TRUE(loan_repaid.replaced);
    EXPECT_TRUE(loan_repaid.parked);
    EXPECT_EQ(lender->colorAttachmentCount(), 2U);
    EXPECT_EQ(lender->generation(), 1U);

    // And the rebuilt lender offers its NEW depth as a source: the half a bookkeeping-only assertion misses.
    auto next_borrower =
        OffscreenTarget::create(created.device, OffscreenTarget::TargetLayout{ 8U, 6U,
                                                                               { vine::graphics::RenderTarget::ColorFormat::RGBA8 },
                                                                               vine::graphics::RenderTarget::DepthFormat::D32 },
                                lender.get());
    EXPECT_NE(next_borrower, nullptr) << "a rebuilt lender offers its new depth like the old one";

    // The pixels agree with the bookkeeping: the refusals changed nothing, and the rebuilt shape renders.
    const RecordedFrame frame =
        recordOneFrame(created, *lender, clearPolicy(layout.clear.color_value), /*with_depth_capture*/ false,
                       /*color_captures*/ 2U);
    {
        const PixelProbe probe = lender->probe();
        ASSERT_TRUE(probe.valid());
        EXPECT_EQ(probe.width(), 8);
        EXPECT_EQ(probe.height(), 6);
        const Rgba8 sampled = probe.pixel(4, 3);
        EXPECT_NEAR(sampled.r, quantise(0.5F), 1);
        EXPECT_TRUE(probe.wholeImageMatches(sampled));
        EXPECT_TRUE(lender->probe(1U).valid()) << "the shape the rebuild served has two colour attachments";
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

TEST(OffscreenTargetTest, ALostSubmissionIsRepairedByTheNextFrameAndOnlyOnce)
{
    const auto created = createDevice();
    if (!created.ok) {
        GTEST_SKIP() << "no device satisfies the device-floor requirements";
    }
    DevicePhaseCounters counters;
    runLostSubmissionPhase(created, counters);
    EXPECT_EQ(counters.frames, 3U) << "the phase drives three frames: fresh, repaired, steady";
}
