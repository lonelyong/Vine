/**
 * @brief The frame's COMPILATION stage: normalize, target/depth resolution, order (see
 * `.ai/design/vsg-reimplementation.md` D2 / §2.4, milestone M3d).
 *
 * Device-free by construction: the compiler reads the recorder's collection plus a facts table (the API
 * layer's account of its targets) and produces plain values.
 *
 * What these cases pin - each one is a picture on the other side:
 *
 *   * a viewport nobody announced is the WHOLE TARGET, not a zero rect and not "whatever the last pass
 *     set" (the two classic ways a picture ends up squeezed into a corner);
 *   * the BOOTSTRAP pass is the first writer into attachments that were just built: an UNDEFINED image
 *     cannot be loaded, so somebody must clear it, and the second writer into the same target must NOT
 *     (clearing there would erase what the first one drew);
 *   * the pass' depth policy applies only where the content authored none (the reverse choice draws a HUD
 *     overlay over everything after it, or drops depth writes from translucent content);
 *   * a command with no program of its own is shaded by the frame's default - resolved here so the
 *     executor never has to ask;
 *   * what a pass READS decides when it runs (the schedule) and a cycle skips its component rather than
 *     being run in call order - "验证层干净、画面却少了一个 pass" is the failure this replaces.
 */

#include <gtest/gtest.h>

#include <cstdint>
#include <vector>

#include <vine/graphics/DepthMode.hpp>
#include <vine/graphics/RenderCommand.hpp>
#include <vine/graphics/RenderTarget.hpp>
#include <vine/graphics/ShaderProgram.hpp>
#include <vine/vsg/core/FrameCompiler.hpp>

using vine::graphics::DepthMode;
using vine::graphics::DiagnosticCategory;
using vine::graphics::RenderCommand;
using vine::graphics::RenderTarget;
using vine::graphics::ShaderProgram;
using vine::vsg::core::ClearPolicy;
using vine::vsg::core::CompiledFrame;
using vine::vsg::core::DepthFacts;
using vine::vsg::core::Diagnostics;
using vine::vsg::core::FrameArena;
using vine::vsg::core::FrameCompiler;
using vine::vsg::core::FrameFacts;
using vine::vsg::core::FrameRecorder;
using vine::vsg::core::FrameToken;
using vine::vsg::core::Observe;
using vine::vsg::core::RepairReason;
using vine::vsg::core::TargetAction;
using vine::vsg::core::TargetFacts;
using vine::vsg::core::TargetShape;

namespace
{

/// @brief One recorder and one compiler over one arena, with the facts table they are driven with.
struct Rig
{
    FrameArena    arena{ 128 * 1024 };
    Diagnostics   diagnostics;
    Observe       observe;
    FrameRecorder recorder{ arena, diagnostics, observe };
    FrameCompiler compiler{ arena, diagnostics, observe };
    std::vector<TargetFacts> facts;

    /// @brief Compiles the frame the recorder just collected.
    const CompiledFrame& compile()
    {
        return compiler.compile(recorder.description(), FrameFacts{ facts });
    }

    /// @brief Adds the facts of a one-colour-attachment target, built or not.
    void addTarget(const void* identity, int width, int height, bool built)
    {
        TargetFacts entry;
        entry.target         = identity;
        entry.wanted.width   = width;
        entry.wanted.height  = height;
        entry.wanted.shape   = colorShape();
        entry.current.desc   = entry.wanted;
        entry.current.built  = built;
        facts.push_back(entry);
    }

    /// @brief A one-colour-attachment shape.
    static TargetShape colorShape()
    {
        TargetShape shape;
        shape.color_formats.push_back(RenderTarget::ColorFormat::RGBA8);
        return shape;
    }
};

/// @brief One command, because most cases only care that something was drawn.
std::vector<RenderCommand> oneCommand()
{
    return std::vector<RenderCommand>{ RenderCommand{} };
}

}  // namespace

TEST(FrameCompilerTest, AViewportNobodyAnnouncedBecomesTheWholeTarget)
{
    Rig r;
    vine::intrusive_ptr<RenderTarget> target(new RenderTarget());
    r.addTarget(target.get(), 640, 360, true);

    const std::vector<RenderCommand> commands = oneCommand();

    r.recorder.beginFrame(FrameToken{ 1 });
    r.recorder.beginPass(1);
    r.recorder.setRenderTarget(target.get());
    r.recorder.render(commands, nullptr);  // nothing announced
    r.recorder.setViewport(10, 20, 30, 40);
    r.recorder.render(commands, nullptr);  // this one announced a rectangle
    r.recorder.endPass();
    r.recorder.endFrame();

    const CompiledFrame& frame = r.compile();
    ASSERT_EQ(frame.passes.size(), 1u);
    EXPECT_EQ(frame.passes[0].viewport.x, 0);
    EXPECT_EQ(frame.passes[0].viewport.width, 640);
    EXPECT_EQ(frame.passes[0].viewport.height, 360);

    ASSERT_EQ(frame.passes[0].draws.size(), 2u);
    EXPECT_EQ(frame.passes[0].draws[0].viewport.width, 640);  // resolved, not left at zero
    EXPECT_EQ(frame.passes[0].draws[1].viewport.x, 10);
    EXPECT_EQ(frame.passes[0].draws[1].viewport.width, 30);
}

TEST(FrameCompilerTest, TheFirstWriterOfAFreshTargetBootstrapsAndTheSecondDoesNot)
{
    Rig r;
    vine::intrusive_ptr<RenderTarget> target(new RenderTarget());
    r.addTarget(target.get(), 64, 64, /*built=*/false);  // freshly laid out: nothing to load

    const std::vector<RenderCommand> commands = oneCommand();

    r.recorder.beginFrame(FrameToken{ 1 });
    for (const std::uint32_t id : { 1u, 2u })
    {
        r.recorder.beginPass(id);
        r.recorder.setRenderTarget(target.get());
        r.recorder.render(commands, nullptr);
        r.recorder.endPass();
    }
    r.recorder.endFrame();

    const CompiledFrame& frame = r.compile();
    ASSERT_EQ(frame.passes.size(), 2u);
    EXPECT_TRUE(frame.passes[0].bootstrap);
    EXPECT_FALSE(frame.passes[1].bootstrap);  // it loads what the first one wrote

    ASSERT_EQ(frame.targets.size(), 1u);
    EXPECT_EQ(frame.targets[0].decision.action, TargetAction::Repair);
    EXPECT_EQ(frame.targets[0].decision.reason, RepairReason::Bootstrap);
}

TEST(FrameCompilerTest, ASteadyTargetNeedsNoBootstrap)
{
    Rig r;
    vine::intrusive_ptr<RenderTarget> target(new RenderTarget());
    r.addTarget(target.get(), 64, 64, /*built=*/true);

    const std::vector<RenderCommand> commands = oneCommand();

    r.recorder.beginFrame(FrameToken{ 1 });
    r.recorder.beginPass(1);
    r.recorder.setRenderTarget(target.get());
    r.recorder.setClearPolicy(ClearPolicy{});  // announced, but nothing is being rebuilt
    r.recorder.render(commands, nullptr);
    r.recorder.endPass();
    r.recorder.endFrame();

    const CompiledFrame& frame = r.compile();
    ASSERT_EQ(frame.passes.size(), 1u);
    EXPECT_FALSE(frame.passes[0].bootstrap);
    EXPECT_EQ(frame.targets[0].decision.action, TargetAction::None);
}

TEST(FrameCompilerTest, TheTargetTableSaysWhatEachTargetNeeds)
{
    Rig r;
    vine::intrusive_ptr<RenderTarget> steady(new RenderTarget());
    vine::intrusive_ptr<RenderTarget> resized(new RenderTarget());
    vine::intrusive_ptr<RenderTarget> reshaped(new RenderTarget());

    r.addTarget(steady.get(), 64, 64, true);
    r.addTarget(resized.get(), 64, 64, true);
    r.addTarget(reshaped.get(), 64, 64, true);

    // Same shape, new extent: the images are replaced, the pipelines are not.
    r.facts[1].wanted.width  = 128;
    r.facts[1].wanted.height = 96;

    // Changed shape: compatibility really moved, so the pass graph and the pipelines go with it.
    r.facts[2].wanted.shape.depth_format = RenderTarget::DepthFormat::D32;

    const std::vector<RenderCommand> commands = oneCommand();
    r.recorder.beginFrame(FrameToken{ 1 });
    for (const auto& target : { steady, resized, reshaped })
    {
        r.recorder.beginPass(1);
        r.recorder.setRenderTarget(target.get());
        r.recorder.render(commands, nullptr);
        r.recorder.endPass();
    }
    r.recorder.endFrame();

    const CompiledFrame& frame = r.compile();
    ASSERT_EQ(frame.targets.size(), 3u);
    EXPECT_EQ(frame.targets[0].decision.action, TargetAction::None);
    EXPECT_EQ(frame.targets[1].decision.action, TargetAction::ResizeInPlace);
    EXPECT_EQ(frame.targets[2].decision.action, TargetAction::Rebuild);
}

TEST(FrameCompilerTest, APassIntoATargetWhoseExtentIsNotUsableIsNotCompiledAndNotABug)
{
    Rig r;
    vine::intrusive_ptr<RenderTarget> target(new RenderTarget());
    r.addTarget(target.get(), 0, 0, /*built=*/false);  // laid out at 0x0: nothing may be drawn into it yet

    const std::vector<RenderCommand> commands = oneCommand();
    r.recorder.beginFrame(FrameToken{ 1 });
    r.recorder.beginPass(1);
    r.recorder.setRenderTarget(target.get());
    r.recorder.render(commands, nullptr);
    r.recorder.endPass();
    r.recorder.endFrame();

    const CompiledFrame& frame = r.compile();
    EXPECT_TRUE(frame.passes.empty());
    ASSERT_EQ(frame.targets.size(), 1u);
    EXPECT_EQ(frame.targets[0].decision.reason, RepairReason::SizeUnknown);
    EXPECT_TRUE(r.diagnostics.clean());  // a target with no size yet is a state, not a mistake
}

TEST(FrameCompilerTest, APassDrawingIntoAnUnknownTargetIsNotCompiledAndIsReported)
{
    Rig r;  // no facts at all: the backend has never heard of this target

    const std::vector<RenderCommand> commands = oneCommand();
    r.recorder.beginFrame(FrameToken{ 1 });
    r.recorder.beginPass(1);
    r.recorder.setRenderTarget(reinterpret_cast<const void*>(0x1234));
    r.recorder.render(commands, nullptr);
    r.recorder.endPass();
    r.recorder.endFrame();

    const CompiledFrame& frame = r.compile();
    EXPECT_TRUE(frame.passes.empty());
    EXPECT_EQ(r.diagnostics.count(DiagnosticCategory::ContentSkipped), 1u);
}

TEST(FrameCompilerTest, ThePassDepthAppliesOnlyWhereTheContentAuthoredNone)
{
    Rig r;
    vine::intrusive_ptr<RenderTarget> target(new RenderTarget());
    r.addTarget(target.get(), 64, 64, true);

    // A command that authored its own depth state (a StateNode set it) and one that did not.
    RenderCommand authored;
    authored.depthExplicit = true;  // ResolvedRenderState's default is test + write
    RenderCommand inherited;
    inherited.depthExplicit = false;

    const std::vector<RenderCommand> commands{ authored, inherited };

    r.recorder.beginFrame(FrameToken{ 1 });
    r.recorder.beginPass(1);
    r.recorder.setRenderTarget(target.get());
    r.recorder.setDepthMode(DepthMode::Disabled);  // the pass draws HUD content
    r.recorder.render(commands, nullptr);
    r.recorder.endPass();
    r.recorder.endFrame();

    const CompiledFrame& frame = r.compile();
    ASSERT_EQ(frame.passes.size(), 1u);
    EXPECT_EQ(frame.passes[0].depth, DepthMode::Disabled);
    ASSERT_EQ(frame.passes[0].draws.size(), 1u);
    ASSERT_EQ(frame.passes[0].draws[0].commands.size(), 2u);

    // The authored state wins; the content that authored none follows the pass.
    EXPECT_EQ(frame.passes[0].draws[0].commands[0].dynamic.depth, DepthMode::TestAndWrite);
    EXPECT_EQ(frame.passes[0].draws[0].commands[1].dynamic.depth, DepthMode::Disabled);
}

TEST(FrameCompilerTest, ACommandWithoutAProgramGetsTheFramesDefault)
{
    Rig r;
    vine::intrusive_ptr<RenderTarget> target(new RenderTarget());
    r.addTarget(target.get(), 64, 64, true);

    const vine::intrusive_ptr<ShaderProgram> fallback(new ShaderProgram());
    const vine::intrusive_ptr<ShaderProgram> own(new ShaderProgram());
    ASSERT_TRUE(r.recorder.setDefaultContentProgram(fallback.get()));

    RenderCommand with_own;
    with_own.program = own;
    RenderCommand without;
    const std::vector<RenderCommand> commands{ with_own, without };

    r.recorder.beginFrame(FrameToken{ 1 });
    r.recorder.beginPass(1);
    r.recorder.setRenderTarget(target.get());
    r.recorder.render(commands, nullptr);
    r.recorder.endPass();
    r.recorder.endFrame();

    const CompiledFrame& frame = r.compile();
    ASSERT_EQ(frame.passes[0].draws.size(), 1u);
    ASSERT_EQ(frame.passes[0].draws[0].commands.size(), 2u);
    EXPECT_EQ(frame.passes[0].draws[0].commands[0].program.program, own.get());
    EXPECT_EQ(frame.passes[0].draws[0].commands[1].program.program, fallback.get());
    EXPECT_EQ(frame.default_program.program, fallback.get());
}

TEST(FrameCompilerTest, WhatAPassReadsDecidesWhenItRunsNotWhenItWasAnnounced)
{
    Rig r;
    vine::intrusive_ptr<RenderTarget> produced(new RenderTarget());
    vine::intrusive_ptr<RenderTarget> composed(new RenderTarget());
    r.addTarget(produced.get(), 64, 64, true);
    r.addTarget(composed.get(), 64, 64, true);

    std::vector<RenderTarget*> inputs{ produced.get() };
    const std::vector<RenderCommand> commands = oneCommand();

    r.recorder.beginFrame(FrameToken{ 1 });

    // The consumer is announced FIRST (order 10) and the producer after it (order 0), which is exactly the
    // shape the graph exists for: registration order is not execution order.
    r.recorder.beginPass(7);
    r.recorder.setPassOrder(10);
    r.recorder.setRenderTarget(composed.get());
    r.recorder.setPassInputs(inputs);
    r.recorder.render(commands, nullptr);
    r.recorder.endPass();

    r.recorder.beginPass(9);
    r.recorder.setPassOrder(0);
    r.recorder.setRenderTarget(produced.get());
    r.recorder.render(commands, nullptr);
    r.recorder.endPass();

    r.recorder.endFrame();

    const CompiledFrame& frame = r.compile();
    ASSERT_EQ(frame.passes.size(), 2u);
    EXPECT_EQ(frame.passes[0].pass, 9u);  // the producer runs first, whatever the announced order said
    EXPECT_EQ(frame.passes[1].pass, 7u);
    EXPECT_EQ(frame.passes[0].schedule_index, 0u);
    EXPECT_EQ(frame.passes[1].schedule_index, 1u);
}

TEST(FrameCompilerTest, ACycleIsSkippedTheFrameStillRunsAndTheCounterMoves)
{
    Rig r;
    vine::intrusive_ptr<RenderTarget> left(new RenderTarget());
    vine::intrusive_ptr<RenderTarget> right(new RenderTarget());
    vine::intrusive_ptr<RenderTarget> plain(new RenderTarget());
    r.addTarget(left.get(), 64, 64, true);
    r.addTarget(right.get(), 64, 64, true);
    r.addTarget(plain.get(), 64, 64, true);

    std::vector<RenderTarget*> reads_right{ right.get() };
    std::vector<RenderTarget*> reads_left{ left.get() };
    const std::vector<RenderCommand> commands = oneCommand();

    r.recorder.beginFrame(FrameToken{ 1 });

    r.recorder.beginPass(1);
    r.recorder.setRenderTarget(left.get());
    r.recorder.setPassInputs(reads_right);  // pass 1 reads what pass 2 writes
    r.recorder.render(commands, nullptr);
    r.recorder.endPass();

    r.recorder.beginPass(2);
    r.recorder.setRenderTarget(right.get());
    r.recorder.setPassInputs(reads_left);  // and pass 2 reads what pass 1 writes
    r.recorder.render(commands, nullptr);
    r.recorder.endPass();

    r.recorder.beginPass(3);  // nothing to do with the cycle
    r.recorder.setRenderTarget(plain.get());
    r.recorder.render(commands, nullptr);
    r.recorder.endPass();

    r.recorder.endFrame();

    const CompiledFrame& frame = r.compile();

    // The cycle is SKIPPED, not run in call order: those two passes draw nothing this frame.
    ASSERT_EQ(frame.passes.size(), 1u);
    EXPECT_EQ(frame.passes[0].pass, 3u);
    EXPECT_EQ(frame.cycles, 1u);
    EXPECT_EQ(r.observe.counters().invalid_schedules, 1u);
    EXPECT_EQ(r.diagnostics.count(DiagnosticCategory::ContentSkipped), 1u);

    // The frame itself is still a frame: the token is carried through for the submission that follows.
    EXPECT_EQ(frame.token.frame, 1u);
}

TEST(FrameCompilerTest, APreservedDepthIsCarriedToThePassThatMustNotClearIt)
{
    Rig r;
    vine::intrusive_ptr<RenderTarget> target(new RenderTarget());
    r.addTarget(target.get(), 64, 64, true);
    r.facts[0].depth.has_depth               = true;
    r.facts[0].depth.promotion                = true;   // the host wanted a sampleable depth
    r.facts[0].depth.any_pass_preserves_depth = true;   // ...but a later pass reads what this one writes

    const std::vector<RenderCommand> commands = oneCommand();
    r.recorder.beginFrame(FrameToken{ 1 });
    r.recorder.beginPass(1);
    r.recorder.setRenderTarget(target.get());
    r.recorder.render(commands, nullptr);
    r.recorder.endPass();
    r.recorder.endFrame();

    const CompiledFrame& frame = r.compile();
    ASSERT_EQ(frame.targets.size(), 1u);
    EXPECT_TRUE(frame.targets[0].depth.preserve);
    EXPECT_FALSE(frame.targets[0].depth.sampleable);  // promotion revoked by the preserving pass
    ASSERT_EQ(frame.passes.size(), 1u);
    EXPECT_TRUE(frame.passes[0].depth_preserved);     // the clear rules read this
}

TEST(FrameCompilerTest, ThePlanCarriesWhatAPipelineIdentityNeedsFromThePass)
{
    Rig r;
    vine::intrusive_ptr<RenderTarget> target(new RenderTarget());
    r.addTarget(target.get(), 64, 64, true);

    // A two-colour-attachment target whose depth the host wants to sample.
    r.facts[0].wanted.shape.color_formats.push_back(RenderTarget::ColorFormat::RGBA16F);
    r.facts[0].current.desc = r.facts[0].wanted;
    r.facts[0].depth.has_depth  = true;
    r.facts[0].depth.promotion  = true;

    const std::vector<RenderCommand> commands = oneCommand();
    r.recorder.beginFrame(FrameToken{ 1 });
    r.recorder.beginPass(1);
    r.recorder.setRenderTarget(target.get());
    r.recorder.render(commands, nullptr);
    r.recorder.endPass();
    r.recorder.endFrame();

    const CompiledFrame& frame = r.compile();
    ASSERT_EQ(frame.passes.size(), 1u);

    // Both are part of a pipeline's identity (see PipelineKey), and both come from the target rather than
    // from the command - so the compiler resolves them once, from the facts it already looked up.
    EXPECT_EQ(frame.passes[0].color_attachments, 2U);
    EXPECT_TRUE(frame.passes[0].depth_sampleable);

    // ...and the SAME fact revokes it when a pass preserves the depth (the M3c rule, seen from the plan).
    r.facts[0].depth.any_pass_preserves_depth = true;
    const CompiledFrame& again =
        r.compiler.compile(r.recorder.description(), FrameFacts{ r.facts });
    ASSERT_EQ(again.passes.size(), 1u);
    EXPECT_TRUE(again.passes[0].depth_preserved);
    EXPECT_FALSE(again.passes[0].depth_sampleable);
}

TEST(FrameCompilerTest, TheFrameCarriesWhatTheRecorderSnapshottedWithoutReworkingIt)
{
    Rig r;
    vine::intrusive_ptr<RenderTarget> target(new RenderTarget());
    r.addTarget(target.get(), 32, 32, true);

    auto                       sun      = vine::graphics::Light::createDirectional(vine::math::Vec3d(0.0, 0.0, -1.0));
    std::vector<const vine::graphics::Light*> lights{ sun.get() };
    const std::vector<RenderCommand> commands = oneCommand();

    r.recorder.beginFrame(FrameToken{ 1 });
    r.recorder.beginPass(1);
    r.recorder.setRenderTarget(target.get());
    r.recorder.setLights(lights);
    r.recorder.setPassInputs(std::span<const RenderTarget* const>{});
    r.recorder.render(commands, nullptr);
    r.recorder.endPass();
    r.recorder.endFrame();

    const CompiledFrame& frame = r.compile();
    ASSERT_EQ(frame.passes.size(), 1u);
    ASSERT_EQ(frame.passes[0].draws.size(), 1u);
    EXPECT_EQ(frame.passes[0].draws[0].lights.size(), 1u);
    EXPECT_TRUE(frame.passes[0].draws[0].lights[0].has_direction);
    EXPECT_EQ(frame.passes[0].target_index, 0u);
    EXPECT_EQ(frame.targets[0].target, target.get());
}
