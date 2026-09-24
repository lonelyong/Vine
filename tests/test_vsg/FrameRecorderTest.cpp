/**
 * @brief The frame's COLLECTION stage (see `.ai/design/vsg-reimplementation.md` D2, milestone M3d).
 *
 * Device-free by construction: the recorder touches borrowed host values, the frame arena and the one
 * diagnostic route - never a GPU object - so the whole contract of the collection side is pinned here.
 *
 * What these cases are for. Collection is where the two facts that make "record as you are called"
 * impossible get handled, and both are exactly testable:
 *
 *   * EVERY ARGUMENT IS BORROWED. The host may destroy the camera, the lights, the target and the
 *     command list as soon as the call returns, and the engine reuses its own containers between
 *     passes. The record is therefore a copy made at the moment of the call, and the test drives the
 *     engine's reuse pattern (one vector, cleared and refilled) and then mutates the host's objects.
 *     A plan that held a span or a pointer into any of them fails here rather than in a frame.
 *   * THE RULES HAVE THREE OUTCOMES, NOT TWO. A call may be allowed, dropped (legal but inert: a scope
 *     attribute with no scope open) or refused (illegal: a draw with no scope, a scope whose target was
 *     released). Each case below pins one outcome, because "the call was silently redirected" is the
 *     failure this backend family keeps producing.
 */

#include <gtest/gtest.h>

#include <cstdint>
#include <vector>

#include <vine/graphics/Camera.hpp>
#include <vine/graphics/DepthMode.hpp>
#include <vine/graphics/Light.hpp>
#include <vine/graphics/RenderCommand.hpp>
#include <vine/graphics/RenderTarget.hpp>
#include <vine/graphics/ShaderProgram.hpp>
#include <vine/vsg/core/FrameRecorder.hpp>

using vn::graphics::DepthMode;
using vn::graphics::DiagnosticCategory;
using vn::graphics::Light;
using vn::graphics::RenderCommand;
using vn::graphics::RenderTarget;
using vn::graphics::ShaderProgram;
using vn::vsg::core::ClearPolicy;
using vn::vsg::core::Diagnostics;
using vn::vsg::core::FrameArena;
using vn::vsg::core::FrameDescription;
using vn::vsg::core::FrameRecorder;
using vn::vsg::core::FrameToken;
using vn::vsg::core::Observe;

namespace
{

/// @brief One recorder with everything it needs (the arena is large enough for every case here).
struct Rig
{
    FrameArena    arena{ 64 * 1024 };
    Diagnostics   diagnostics;
    Observe       observe;
    FrameRecorder recorder{ arena, diagnostics, observe };

    /// @brief Opens a frame with the given frame number.
    void open(std::uint64_t number = 1)
    {
        EXPECT_TRUE(recorder.beginFrame(FrameToken{ number }));
    }

    /// @brief Closes the open frame and returns its description (valid until the next beginFrame).
    const FrameDescription& seal()
    {
        EXPECT_TRUE(recorder.endFrame());
        return recorder.description();
    }
};

/// @brief A one-command list, because most cases only care that something was drawn.
std::vector<RenderCommand> oneCommand()
{
    return std::vector<RenderCommand>{ RenderCommand{} };
}

}  // namespace

TEST(FrameRecorderTest, TheScopeAttributesOfEachPassAreCollectedWithItsDrawingCalls)
{
    Rig r;
    r.open();

    int                              target = 0;
    const std::vector<RenderCommand> commands = oneCommand();

    EXPECT_TRUE(r.recorder.beginPass(7));
    EXPECT_TRUE(r.recorder.setPassOrder(-1));
    EXPECT_TRUE(r.recorder.setRenderTarget(&target));
    EXPECT_TRUE(r.recorder.setDepthMode(DepthMode::Disabled));
    EXPECT_TRUE(r.recorder.render(commands, nullptr));
    EXPECT_TRUE(r.recorder.endPass());

    EXPECT_TRUE(r.recorder.beginPass(9));
    EXPECT_TRUE(r.recorder.setPassOrder(0));
    EXPECT_TRUE(r.recorder.render(commands, nullptr));
    EXPECT_TRUE(r.recorder.endPass());

    const FrameDescription& description = r.seal();
    ASSERT_EQ(description.passes.size(), 2u);

    EXPECT_EQ(description.passes[0].pass, 7u);
    EXPECT_EQ(description.passes[0].order, -1);
    EXPECT_EQ(description.passes[0].target, &target);
    EXPECT_EQ(description.passes[0].depth, DepthMode::Disabled);
    ASSERT_EQ(description.passes[0].draws.size(), 1u);

    EXPECT_EQ(description.passes[1].pass, 9u);
    EXPECT_EQ(description.passes[1].order, 0);
    EXPECT_EQ(description.passes[1].target, nullptr);  // the default framebuffer, recorded as a fact
    EXPECT_EQ(description.passes[1].depth, DepthMode::TestAndWrite);
    EXPECT_FALSE(description.passes[1].has_clear);

    EXPECT_EQ(r.observe.counters().passes, 2u);
    EXPECT_EQ(r.observe.counters().draws, 2u);
    EXPECT_TRUE(r.diagnostics.clean());
}

TEST(FrameRecorderTest, TheHostsReusedCommandListIsCopiedAtTheCallNotHeld)
{
    Rig r;
    r.open();

    // The engine's own pattern: ONE vector, cleared and refilled per pass (the pass is handed a shared
    // list, and the resolved inputs are a reused member - see RenderEngine). A plan holding a span into
    // it would be reading the NEXT pass' commands by the time it is recorded.
    std::vector<RenderCommand> shared = oneCommand();
    shared[0].opacity                 = 0.25F;

    EXPECT_TRUE(r.recorder.beginPass(1));
    EXPECT_TRUE(r.recorder.render(shared, nullptr));
    EXPECT_TRUE(r.recorder.endPass());

    shared.clear();
    shared.push_back(RenderCommand{});
    shared[0].opacity = 1.0F;
    shared.push_back(RenderCommand{});

    EXPECT_TRUE(r.recorder.beginPass(2));
    EXPECT_TRUE(r.recorder.render(shared, nullptr));
    EXPECT_TRUE(r.recorder.endPass());

    const FrameDescription& description = r.seal();
    ASSERT_EQ(description.passes.size(), 2u);
    ASSERT_EQ(description.passes[0].draws.size(), 1u);
    ASSERT_EQ(description.passes[0].draws[0].commands.size(), 1u);
    EXPECT_FLOAT_EQ(description.passes[0].draws[0].commands[0].opacity, 0.25F);
    ASSERT_EQ(description.passes[1].draws.size(), 1u);
    EXPECT_EQ(description.passes[1].draws[0].commands.size(), 2u);
}

TEST(FrameRecorderTest, TheAnnouncedLightsAreCopiedNumbersNotBorrowedPointers)
{
    Rig r;
    r.open();

    auto                          sun = Light::createDirectional(vn::math::Vec3d(0.0, 0.0, -1.0));
    std::vector<const Light*>     lights{ sun.get() };
    const std::vector<RenderCommand> commands = oneCommand();

    EXPECT_TRUE(r.recorder.beginPass(1));
    EXPECT_TRUE(r.recorder.setLights(lights));
    EXPECT_TRUE(r.recorder.render(commands, nullptr));
    EXPECT_TRUE(r.recorder.endPass());

    // The contract: a light is borrowed for the call. The host may destroy it as soon as the call
    // returns, and the plan must still know what the picture was lit by.
    sun->setIntensity(9.0F);
    const FrameDescription& description = r.seal();
    sun.reset();

    ASSERT_EQ(description.passes.size(), 1u);
    ASSERT_EQ(description.passes[0].draws.size(), 1u);
    const std::span<const vn::vsg::core::LightRef> collected = description.passes[0].draws[0].lights;
    ASSERT_EQ(collected.size(), 1u);
    EXPECT_TRUE(collected[0].enabled);
    EXPECT_FLOAT_EQ(collected[0].intensity, 1.0F);  // the value at the call, not the later edit
    EXPECT_TRUE(collected[0].has_direction);
}

TEST(FrameRecorderTest, OneViewportAndLightAnnouncementServesOneDrawingCall)
{
    Rig r;
    r.open();

    auto                             sun      = Light::createDirectional(vn::math::Vec3d(0.0, 0.0, -1.0));
    std::vector<const Light*>        lights{ sun.get() };
    const std::vector<RenderCommand> commands = oneCommand();

    EXPECT_TRUE(r.recorder.beginPass(1));
    {
        EXPECT_TRUE(r.recorder.setViewport(1, 2, 3, 4));
        EXPECT_TRUE(r.recorder.setLights(lights));
        EXPECT_TRUE(r.recorder.render(commands, nullptr));  // consumes both announcements
    }
    EXPECT_TRUE(r.recorder.render(commands, nullptr));  // nothing announced: the whole target, default lights
    {
        EXPECT_TRUE(r.recorder.setLights(lights));
        EXPECT_TRUE(r.recorder.render(commands, nullptr));  // announced again
    }
    EXPECT_TRUE(r.recorder.endPass());

    const FrameDescription& description = r.seal();
    ASSERT_EQ(description.passes.size(), 1u);
    const std::span<const vn::vsg::core::CollectedDraw> draws = description.passes[0].draws;
    ASSERT_EQ(draws.size(), 3u);

    EXPECT_TRUE(draws[0].has_viewport);
    EXPECT_EQ(draws[0].viewport.x, 1);
    EXPECT_EQ(draws[0].viewport.width, 3);
    EXPECT_EQ(draws[0].lights.size(), 1u);

    EXPECT_FALSE(draws[1].has_viewport);  // the compiler resolves this to the whole target
    EXPECT_TRUE(draws[1].lights.empty());// empty = the backend default
    EXPECT_TRUE(draws[2].lights.size() == 1u);
    EXPECT_FALSE(draws[2].has_viewport);  // a viewport announcement is not replayed either

    EXPECT_EQ(r.observe.counters().draws, 3u);
}

TEST(FrameRecorderTest, ADrawingCallWithNothingToDrawIsNotADrawAtAll)
{
    // WHAT THE APPLICATION'S FIRST FRAME DID (measured 2026-09-24). The engine's axis-gizmo pass calls
    // render() with an EMPTY command list while its surface size is still unknown - a legal call, and the
    // only thing the pass announces that frame is "I ran". The recorder turned it into a draw with no
    // commands, and the content layer read that as "this pass has content, but no compiled content half was
    // built for it": it refused the WHOLE pass, the clear it carries included, and reported a warning - the
    // last warning the demo's own log carried.
    //
    // A call with nothing to draw is still a CALL: it consumes its announcements exactly as the case above
    // pins for a call that draws (one announcement, one call), and the pass survives on a clear. Nothing is
    // recorded for it, because there is nothing to record - and nothing reaches the content layer to be
    // refused later.
    Rig r;
    r.open();

    auto                             sun      = Light::createDirectional(vn::math::Vec3d(0.0, 0.0, -1.0));
    std::vector<const Light*>        lights{ sun.get() };
    const std::vector<RenderCommand> commands = oneCommand();

    ClearPolicy clear;
    clear.color = true;

    EXPECT_TRUE(r.recorder.beginPass(1));
    EXPECT_TRUE(r.recorder.setClearPolicy(clear));
    {
        EXPECT_TRUE(r.recorder.setViewport(1, 2, 3, 4));
        EXPECT_TRUE(r.recorder.setLights(lights));
        EXPECT_TRUE(r.recorder.render(std::span<const RenderCommand>{}, nullptr));  // nothing to draw
    }
    EXPECT_TRUE(r.recorder.render(commands, nullptr));  // nothing announced: the whole target, default lights
    EXPECT_TRUE(r.recorder.endPass());

    const FrameDescription& description = r.seal();
    ASSERT_EQ(description.passes.size(), 1u);
    EXPECT_TRUE(description.passes[0].has_clear);
    const std::span<const vn::vsg::core::CollectedDraw> draws = description.passes[0].draws;
    ASSERT_EQ(draws.size(), 1u) << "the empty call left no draw behind: there was nothing to record";
    EXPECT_EQ(draws[0].commands.size(), 1u);
    EXPECT_FALSE(draws[0].has_viewport) << "the empty call CONSUMED the announcement (one call, one viewport)";
    EXPECT_TRUE(draws[0].lights.empty()) << "and the lights with it";
    EXPECT_EQ(r.observe.counters().draws, 1u) << "the counter counts draws that were RECORDED";
    EXPECT_TRUE(r.diagnostics.clean());

    // ...and a pass whose only call is empty and which clears nothing is no pass at all - the same rule a
    // pass with no draws and no clear has always had (see endPass).
    Rig bare;
    bare.open();
    EXPECT_TRUE(bare.recorder.beginPass(2));
    EXPECT_TRUE(bare.recorder.render(std::span<const RenderCommand>{}, nullptr));
    EXPECT_TRUE(bare.recorder.endPass());
    EXPECT_TRUE(bare.seal().passes.empty()) << "nothing to draw and nothing to clear is not a pass";
    EXPECT_EQ(bare.observe.counters().draws, 0u);
}

TEST(FrameRecorderTest, ThePassInputsAreCopiedAndBelongToThePass)
{
    Rig r;
    r.open();

    vn::intrusive_ptr<RenderTarget> first(new RenderTarget());
    vn::intrusive_ptr<RenderTarget> second(new RenderTarget());

    std::vector<RenderTarget*> inputs{ first.get(), nullptr, second.get() };
    const std::vector<RenderCommand> commands = oneCommand();

    EXPECT_TRUE(r.recorder.beginPass(1));
    EXPECT_TRUE(r.recorder.setPassInputs(inputs));
    EXPECT_TRUE(r.recorder.render(commands, nullptr));
    EXPECT_TRUE(r.recorder.endPass());

    // The engine refills its resolved-input vector for the next pass (a null entry means "nothing
    // produced it"); the plan must keep this frame's answer, including the null.
    inputs.assign(1, nullptr);
    const FrameDescription& description = r.seal();

    ASSERT_EQ(description.passes.size(), 1u);
    const std::span<const vn::vsg::core::InputRef> collected = description.passes[0].inputs;
    ASSERT_EQ(collected.size(), 3u);
    EXPECT_EQ(collected[0].target, first.get());
    EXPECT_EQ(collected[1].target, nullptr);
    EXPECT_EQ(collected[2].target, second.get());
}

TEST(FrameRecorderTest, AScopeThatDrewNothingIsDroppedUnlessItAnnouncedAClear)
{
    Rig r;
    r.open();

    int target = 0;

    // A pass that announced a target and drew nothing: nothing it asked for can reach a picture, so it
    // does not enter the plan. This is what "unconsumed scope attributes are discarded at endPass" means
    // in observable terms.
    EXPECT_TRUE(r.recorder.beginPass(1));
    EXPECT_TRUE(r.recorder.setPassOrder(3));
    EXPECT_TRUE(r.recorder.setRenderTarget(&target));
    EXPECT_TRUE(r.recorder.setViewport(0, 0, 8, 8));
    EXPECT_TRUE(r.recorder.endPass());

    // A pass that announced a CLEAR and drew nothing is a real pass: the clear happens through the pass'
    // own load-op, so dropping the scope would erase the picture's background.
    ClearPolicy policy;
    policy.color       = true;
    policy.color_value[0] = 0.5F;
    EXPECT_TRUE(r.recorder.beginPass(2));
    EXPECT_TRUE(r.recorder.setClearPolicy(policy));
    EXPECT_TRUE(r.recorder.endPass());

    const FrameDescription& description = r.seal();
    ASSERT_EQ(description.passes.size(), 1u);
    EXPECT_EQ(description.passes[0].pass, 2u);
    EXPECT_TRUE(description.passes[0].has_clear);
    EXPECT_FLOAT_EQ(description.passes[0].clear.color_value[0], 0.5F);
    EXPECT_TRUE(description.passes[0].draws.empty());
    EXPECT_EQ(r.observe.counters().passes, 1u);
}

TEST(FrameRecorderTest, ScopeAttributesOutsideAScopeAreDroppedRatherThanReported)
{
    Rig r;
    r.open();

    const std::vector<RenderCommand> commands = oneCommand();

    // The host may announce state before it opens a scope; the calls are inert, not mistakes. Nothing is
    // drawn with them, and nothing is reported.
    EXPECT_FALSE(r.recorder.setPassOrder(4));
    EXPECT_FALSE(r.recorder.setViewport(0, 0, 1, 1));
    EXPECT_FALSE(r.recorder.setClearPolicy(ClearPolicy{}));
    EXPECT_FALSE(r.recorder.setDepthMode(DepthMode::Disabled));
    EXPECT_FALSE(r.recorder.setPassInputs(std::span<const RenderTarget* const>{}));
    EXPECT_FALSE(r.recorder.setLights(std::span<const Light* const>{}));

    EXPECT_EQ(r.recorder.protocol().droppedCount(), 6u);
    EXPECT_EQ(r.recorder.protocol().refusalCount(), 0u);
    EXPECT_TRUE(r.diagnostics.clean());

    // ...and the next pass starts from an empty request: none of it leaked.
    EXPECT_TRUE(r.recorder.beginPass(1));
    EXPECT_TRUE(r.recorder.render(commands, nullptr));
    EXPECT_TRUE(r.recorder.endPass());

    const FrameDescription& description = r.seal();
    ASSERT_EQ(description.passes.size(), 1u);
    EXPECT_EQ(description.passes[0].order, 0);
    EXPECT_FALSE(description.passes[0].has_clear);
    EXPECT_EQ(description.passes[0].depth, DepthMode::TestAndWrite);
    EXPECT_TRUE(description.passes[0].inputs.empty());
}

TEST(FrameRecorderTest, ADrawingCallWithNoScopeIsRefusedAndReportedOncePerFrame)
{
    Rig r;
    r.open();

    const std::vector<RenderCommand> commands = oneCommand();

    EXPECT_FALSE(r.recorder.render(commands, nullptr));
    EXPECT_FALSE(r.recorder.render(commands, nullptr));
    EXPECT_EQ(r.recorder.protocol().refusalCount(), 2u);
    EXPECT_EQ(r.diagnostics.count(DiagnosticCategory::PassProtocolViolation), 1u);  // once per FRAME
    EXPECT_EQ(r.observe.counters().draws, 0u);  // a refused draw is not counted as a draw

    // A new frame is a new episode: a host that still has not fixed its bookkeeping is told again.
    EXPECT_TRUE(r.recorder.endFrame());
    EXPECT_TRUE(r.recorder.swapBuffers());
    r.open(2);
    EXPECT_FALSE(r.recorder.render(commands, nullptr));
    EXPECT_EQ(r.diagnostics.count(DiagnosticCategory::PassProtocolViolation), 2u);
}

TEST(FrameRecorderTest, ANestedBeginPassIsRefusedAndTheOuterScopeKeepsItsState)
{
    Rig r;
    r.open();

    const std::vector<RenderCommand> commands = oneCommand();

    EXPECT_TRUE(r.recorder.beginPass(1));
    EXPECT_TRUE(r.recorder.setPassOrder(2));
    EXPECT_FALSE(r.recorder.beginPass(2));  // refused: the inner pass does not open
    EXPECT_TRUE(r.recorder.inPass());

    // The caller's bookkeeping is off by one scope, so what it says now belongs to the OUTER scope - and
    // the matching endPass() closes that one.
    EXPECT_TRUE(r.recorder.setPassOrder(5));
    EXPECT_TRUE(r.recorder.render(commands, nullptr));
    EXPECT_TRUE(r.recorder.endPass());
    EXPECT_FALSE(r.recorder.inPass());

    const FrameDescription& description = r.seal();
    ASSERT_EQ(description.passes.size(), 1u);
    EXPECT_EQ(description.passes[0].pass, 1u);
    EXPECT_EQ(description.passes[0].order, 5);
    EXPECT_EQ(r.diagnostics.count(DiagnosticCategory::PassProtocolViolation), 1u);
}

TEST(FrameRecorderTest, AnUnpairedEndPassIsRefusedAndReported)
{
    Rig r;
    r.open();

    EXPECT_FALSE(r.recorder.endPass());
    EXPECT_EQ(r.recorder.protocol().refusalCount(), 1u);
    EXPECT_EQ(r.diagnostics.count(DiagnosticCategory::PassProtocolViolation), 1u);
}

TEST(FrameRecorderTest, AReleasedAnnouncedTargetKillsTheScopeAndNothingIsRedirected)
{
    Rig r;
    r.open();

    int                              target = 0;
    int                              other  = 0;
    const std::vector<RenderCommand> commands = oneCommand();

    EXPECT_TRUE(r.recorder.beginPass(1));
    EXPECT_TRUE(r.recorder.setRenderTarget(&target));
    EXPECT_TRUE(r.recorder.releaseRenderTarget(&target));  // the announcement is dropped here
    EXPECT_EQ(r.recorder.protocol().announcedTarget(), nullptr);

    // The contract: a call that would still have used the released target is SKIPPED, never redirected to
    // the default framebuffer (the content is missing rather than drawn where nobody asked for it).
    EXPECT_FALSE(r.recorder.render(commands, nullptr));
    EXPECT_FALSE(r.recorder.render(commands, nullptr));

    // A new announcement does not revive the scope: the protocol's rule is that the dropped announcement
    // ends the scope's useful life, and the recorder honours the verdict instead of working around it.
    EXPECT_FALSE(r.recorder.setRenderTarget(&other));

    EXPECT_TRUE(r.recorder.endPass());  // the scope drew nothing, so it enters no plan
    const FrameDescription& description = r.seal();
    EXPECT_TRUE(description.passes.empty());
    EXPECT_EQ(r.observe.counters().draws, 0u);
    EXPECT_EQ(r.recorder.protocol().refusalCount(), 2u);
    EXPECT_EQ(r.diagnostics.count(DiagnosticCategory::PassProtocolViolation), 1u);  // once for this scope
}

TEST(FrameRecorderTest, AnEmptyFrameIsStillAFrameWithItsOwnToken)
{
    Rig r;
    EXPECT_TRUE(r.recorder.beginFrame(FrameToken{ 12 }));
    EXPECT_TRUE(r.recorder.inFrame());
    EXPECT_TRUE(r.recorder.endFrame());

    const FrameDescription& description = r.recorder.description();
    EXPECT_EQ(description.token.frame, 12u);
    EXPECT_TRUE(description.passes.empty());

    EXPECT_TRUE(r.recorder.swapBuffers());
    EXPECT_FALSE(r.recorder.inFrame());

    // A second present with no frame open is refused: the contract makes swapBuffers the frame's last call
    // exactly once.
    EXPECT_FALSE(r.recorder.swapBuffers());
    EXPECT_EQ(r.diagnostics.count(DiagnosticCategory::PassProtocolViolation), 1u);
}

TEST(FrameRecorderTest, SwapBuffersInsideAnOpenScopeIsRefused)
{
    Rig r;
    r.open();

    EXPECT_TRUE(r.recorder.beginPass(1));
    EXPECT_FALSE(r.recorder.swapBuffers());
    EXPECT_TRUE(r.recorder.inFrame());  // the frame is still open, and the scope still needs its endPass
    EXPECT_EQ(r.diagnostics.count(DiagnosticCategory::PassProtocolViolation), 1u);
}

TEST(FrameRecorderTest, TheDefaultProgramIsAFrameLevelFactTheCompilerResolves)
{
    Rig r;

    const vn::intrusive_ptr<ShaderProgram> program(new ShaderProgram());
    EXPECT_TRUE(r.recorder.setDefaultContentProgram(program.get()));

    // A command with no program of its own records "none": substituting the default is the compiler's job
    // (the description keeps "unresolved" and "explicitly none" apart by construction).
    std::vector<RenderCommand> commands = oneCommand();
    EXPECT_TRUE(r.recorder.beginFrame(FrameToken{ 3 }));
    EXPECT_TRUE(r.recorder.beginPass(1));
    EXPECT_TRUE(r.recorder.render(commands, nullptr));
    EXPECT_TRUE(r.recorder.endPass());

    const FrameDescription& description = r.seal();
    EXPECT_EQ(description.default_program.program, program.get());
    ASSERT_EQ(description.passes.size(), 1u);
    ASSERT_EQ(description.passes[0].draws.size(), 1u);
    ASSERT_EQ(description.passes[0].draws[0].commands.size(), 1u);
    EXPECT_EQ(description.passes[0].draws[0].commands[0].program.program, nullptr);

    // The revision travels with the identity: a program edited after the call is not the one recorded.
    const std::uint64_t before = program->revision();
    program->replaceStages({});
    EXPECT_NE(program->revision(), before);
    EXPECT_EQ(description.default_program.revision, before);
}
