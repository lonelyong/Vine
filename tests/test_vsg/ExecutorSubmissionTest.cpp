#include <gtest/gtest.h>

#include <memory>
#include <vector>

#include <vsg/app/CommandGraph.h>
#include <vsg/core/ref_ptr.h>

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
