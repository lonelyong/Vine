#include <gtest/gtest.h>

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include <vsg/app/CommandGraph.h>
#include <vsg/app/RecordTraversal.h>
#include <vsg/app/Viewer.h>
#include <vsg/core/Exception.h>
#include <vsg/core/ref_ptr.h>
#include <vsg/nodes/Group.h>

#include <vine/graphics/RenderTarget.hpp>

#include <vine/vsg/api/Device.hpp>
#include <vine/vsg/api/OffscreenTarget.hpp>
#include <vine/vsg/api/VsgExecutor.hpp>
#include <vine/vsg/core/FrameCompiler.hpp>
#include <vine/vsg/core/FrameRecorder.hpp>

using vine::graphics::RenderTarget;
using vine::vsg::core::ClearPolicy;
using vine::vsg::core::CompiledFrame;
using vine::vsg::core::Diagnostics;
using vine::vsg::core::FrameArena;
using vine::vsg::core::FrameCompiler;
using vine::vsg::core::FrameFacts;
using vine::vsg::core::FrameRecorder;
using vine::vsg::core::FrameToken;
using vine::vsg::core::Observe;
using vine::vsg::core::Rgba8;
using vine::vsg::core::TargetFacts;

namespace
{

constexpr std::uint32_t kSize = 64U;

/// @brief The plan's facts for one off-screen target (what the compiler needs to resolve it).
TargetFacts factsOf(std::unique_ptr<vine::vsg::OffscreenTarget>& target)
{
    TargetFacts entry;
    entry.target        = target.get();
    entry.wanted.width  = static_cast<int>(kSize);
    entry.wanted.height = static_cast<int>(kSize);
    entry.wanted.shape.color_formats.push_back(RenderTarget::ColorFormat::RGBA8);
    entry.current = target->instance();
    return entry;
}

}  // namespace

TEST(ExecutorSubmissionTest, ALostSubmissionIsRepairedByTheNextFrameExactlyOnce)
{
    // A submission that does not happen leaves the attachments a frame WROTE in a state nobody can trust:
    // the writes were recorded, not performed (api/VsgExecutor::noteLostSubmission). The next compiled plan
    // says "repair" for that target, the first BOOTSTRAPPING pass into it clears it - and the frame after
    // that is back to an ordinary plan. This is the seam that turns the target's
    // `attachments_invalidated` fact (which the tests could only set by hand until now) into something a
    // frame drive produces.
    const vine::vsg::DeviceResult created = vine::vsg::createDevice();
    if (!created.ok)
    {
        GTEST_SKIP() << "no Vulkan device available (lavapipe + X11 are needed): " << created.error.as_std_str();
    }

    vine::vsg::OffscreenTarget::Layout layout;
    layout.width  = kSize;
    layout.height = kSize;
    std::unique_ptr<vine::vsg::OffscreenTarget> written = vine::vsg::OffscreenTarget::create(created.device, layout);
    std::unique_ptr<vine::vsg::OffscreenTarget> other   = vine::vsg::OffscreenTarget::create(created.device, layout);
    ASSERT_NE(written, nullptr);
    ASSERT_NE(other, nullptr);

    FrameArena              arena{ 64 * 1024 };
    Diagnostics             diagnostics;
    Observe                 observe;
    FrameRecorder           recorder{ arena, diagnostics, observe };
    FrameCompiler           compiler{ arena, diagnostics, observe };
    vine::vsg::VsgExecutor  executor(diagnostics);
    executor.addTarget(written.get(), written.get());
    executor.addTarget(other.get(), other.get());

    ClearPolicy clear;
    clear.color          = true;
    clear.color_value[3] = 1.0F;

    const auto record_frame = [&](std::uint64_t number, std::uint32_t pass_id,
                                  const std::unique_ptr<vine::vsg::OffscreenTarget>& into) {
        recorder.beginFrame(FrameToken{ number });
        recorder.beginPass(pass_id);
        recorder.setRenderTarget(into.get());
        recorder.setClearPolicy(clear);
        recorder.endPass();
        recorder.endFrame();
        recorder.swapBuffers();  // endFrame() leaves the pass; THIS closes the frame (see the protocol)
        const std::vector<TargetFacts> table{ factsOf(written), factsOf(other) };
        return &compiler.compile(recorder.description(), FrameFacts{ table });
    };

    // Frame 1: recorded (so the target counts as WRITTEN), then its submission is lost.
    const CompiledFrame* first = record_frame(1U, 1U, written);
    ::vsg::ref_ptr<::vsg::CommandGraph> graph = ::vsg::CommandGraph::create(created.device, created.queue_family);
    ASSERT_TRUE(executor.record(*first, graph));
    EXPECT_FALSE(written->instance().attachments_invalidated);

    ASSERT_EQ(first->passes.size(), 1U) << "one pass was announced";
    ASSERT_EQ(first->targets.size(), 1U);
    EXPECT_EQ(executor.noteLostSubmission(*first), 1U)
        << "only the target the frame actually wrote is marked";
    EXPECT_TRUE(written->instance().attachments_invalidated)
        << "what the lost submission recorded was never performed";
    EXPECT_FALSE(other->instance().attachments_invalidated)
        << "a registered target the frame did not write is untouched";

    // Frame 2: the plan answers "repair", and the bootstrapping pass clears the flag.
    const CompiledFrame* second = record_frame(2U, 2U, written);
    ASSERT_EQ(second->passes.size(), 1U);
    EXPECT_TRUE(second->passes[0].bootstrap) << "the plan says the repair: the pass clears what nobody can trust";
    ::vsg::ref_ptr<::vsg::CommandGraph> second_graph = ::vsg::CommandGraph::create(created.device, created.queue_family);
    ASSERT_TRUE(executor.record(*second, second_graph));
    EXPECT_FALSE(written->instance().attachments_invalidated)
        << "the bootstrapping pass repaired it: contents are known again";

    // (The "exactly once" half of the flag is OffscreenTargetTest.ALostSubmissionIsRepairedByTheNextFrame
    // AndOnlyOnce's claim, and it holds: a second plan is an ordinary one.)
}

/// @brief A node that fails the record step of a submission on command.
///
/// WHY A NODE AND NOT A REAL DEVICE-LOST: the failure a submission can really meet here is an exception -
/// vsg throws `vsg::Exception` when it cannot make the step (a command buffer that cannot be allocated) -
/// and a device-lost cannot be summoned on demand on lavapipe. So this node throws exactly that exception
/// type, at exactly that step, at a frame a case chooses: everything above it (the executor's submit, the
/// mark, the report) is the production code.
class FailingStepNode : public ::vsg::Group
{
  public:
    bool armed{false};  ///< Whether the next record step throws (disarmed: an ordinary empty group).

    void accept(::vsg::RecordTraversal& visitor) const override
    {
        if (armed)
        {
            throw ::vsg::Exception{ "the submission step failed (test)", VK_ERROR_DEVICE_LOST };
        }
        ::vsg::Group::traverse(visitor);
    }
};

TEST(ExecutorSubmissionTest, TheSubmissionStepItselfMarksWhatAFailedFrameWrote)
{
    // What this case is for: `noteLostSubmission` (the case above) is a fact somebody had to produce - "this
    // frame was not submitted" - and until now that somebody was the host. `VsgExecutor::submit` makes the
    // submission the STEP that says it: record, submit, and on a failure mark exactly the targets the
    // frame wrote, report, answer false. No host has to interpret a vsg failure, and no target is drawn
    // over with contents nobody can vouch for.
    //
    // The picture half is here too, because "the step happened" must mean the frame HAPPENED: the first
    // submission's clear is in the target's pixels, and nothing needed repair.
    const vine::vsg::DeviceResult created = vine::vsg::createDevice();
    if (!created.ok)
    {
        GTEST_SKIP() << "no Vulkan device available (lavapipe + X11 are needed): " << created.error.as_std_str();
    }

    vine::vsg::OffscreenTarget::Layout layout;
    layout.width  = kSize;
    layout.height = kSize;
    std::unique_ptr<vine::vsg::OffscreenTarget> written = vine::vsg::OffscreenTarget::create(created.device, layout);
    std::unique_ptr<vine::vsg::OffscreenTarget> other   = vine::vsg::OffscreenTarget::create(created.device, layout);
    ASSERT_NE(written, nullptr);
    ASSERT_NE(other, nullptr);

    FrameArena             arena{ 64 * 1024 };
    Diagnostics            diagnostics;
    Observe                observe;
    FrameRecorder          recorder{ arena, diagnostics, observe };
    FrameCompiler          compiler{ arena, diagnostics, observe };
    vine::vsg::VsgExecutor executor(diagnostics);
    executor.addTarget(written.get(), written.get());
    executor.addTarget(other.get(), other.get());

    ClearPolicy clear;
    clear.color          = true;
    clear.color_value[3] = 1.0F;

    const auto record_frame = [&](std::uint64_t number, std::uint32_t pass_id,
                                  const std::unique_ptr<vine::vsg::OffscreenTarget>& into) {
        recorder.beginFrame(FrameToken{ number });
        recorder.beginPass(pass_id);
        recorder.setRenderTarget(into.get());
        recorder.setClearPolicy(clear);
        recorder.endPass();
        recorder.endFrame();
        recorder.swapBuffers();  // endFrame() leaves the pass; THIS closes the frame (see the protocol)
        const std::vector<TargetFacts> table{ factsOf(written), factsOf(other) };
        return &compiler.compile(recorder.description(), FrameFacts{ table });
    };

    // Frame 1: recorded and SUBMITTED through the executor. The step happened, the target is ordinary, and
    // the frame is on the GPU - the clear it recorded is what the target's pixels hold.
    const CompiledFrame* first = record_frame(1U, 1U, written);
    ::vsg::ref_ptr<::vsg::CommandGraph> graph = ::vsg::CommandGraph::create(created.device, created.queue_family);
    ASSERT_TRUE(executor.record(*first, graph));

    ::vsg::ref_ptr<::vsg::Viewer> viewer = ::vsg::Viewer::create();
    ASSERT_NE(viewer, nullptr);
    viewer->assignRecordAndSubmitTaskAndPresentation(::vsg::CommandGraphs{ graph });
    ASSERT_TRUE(viewer->compile());
    viewer->advanceToNextFrame();
    viewer->handleEvents();
    EXPECT_TRUE(executor.submit(*first, *viewer)) << "the submission of a frame that can be submitted happens";
    viewer->deviceWaitIdle();

    EXPECT_FALSE(written->instance().attachments_invalidated)
        << "a submission that happened leaves nothing to repair";
    const Rgba8 centre = written->probe().pixel(static_cast<int>(kSize) / 2, static_cast<int>(kSize) / 2);
    EXPECT_EQ(centre.r, 0U) << "the frame was submitted, so the clear it recorded is in the target";
    EXPECT_EQ(centre.g, 0U);
    EXPECT_EQ(centre.b, 0U);
    EXPECT_EQ(centre.a, 255U);

    // Frame 2: the same step, made to fail INSIDE the submission. The executor marks the target THIS frame
    // wrote (not the one an earlier frame wrote), reports once, and says the submission did not happen.
    const CompiledFrame* second = record_frame(2U, 2U, other);
    ::vsg::ref_ptr<::vsg::CommandGraph> failing_graph =
        ::vsg::CommandGraph::create(created.device, created.queue_family);
    ASSERT_TRUE(executor.record(*second, failing_graph));
    ::vsg::ref_ptr<FailingStepNode> failing(new FailingStepNode());
    failing_graph->addChild(failing);

    ::vsg::ref_ptr<::vsg::Viewer> failing_viewer = ::vsg::Viewer::create();
    ASSERT_NE(failing_viewer, nullptr);
    failing_viewer->assignRecordAndSubmitTaskAndPresentation(::vsg::CommandGraphs{ failing_graph });
    ASSERT_TRUE(failing_viewer->compile()) << "the step's graph compiles; it is the SUBMISSION that fails";
    failing_viewer->advanceToNextFrame();
    failing_viewer->handleEvents();
    failing->armed = true;

    const std::uint64_t failures_before =
        diagnostics.count(vine::graphics::DiagnosticCategory::SubmissionFailed);
    EXPECT_FALSE(executor.submit(*second, *failing_viewer)) << "the step says the submission did not happen";
    EXPECT_EQ(diagnostics.count(vine::graphics::DiagnosticCategory::SubmissionFailed), failures_before + 1U)
        << "a lost frame is reported, with the category a host can switch on";
    EXPECT_TRUE(other->instance().attachments_invalidated)
        << "what this frame recorded was never performed, so its target's contents are unknown";
    EXPECT_FALSE(written->instance().attachments_invalidated)
        << "the target this frame did NOT write is not implicated";

    // And the mark is the fact the plan repairs (the case above proves that half end to end): the next plan
    // for that target says the pass bootstraps - it clears what nobody can vouch for.
    const CompiledFrame* third = record_frame(3U, 3U, other);
    ASSERT_EQ(third->passes.size(), 1U);
    EXPECT_TRUE(third->passes[0].bootstrap) << "the plan answers the mark: the pass clears it";
}
