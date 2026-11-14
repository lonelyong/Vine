/**
 * @brief The per-draw block's VALUE PATH: the numbers a host authored reach the bytes a shader reads.
 *
 * The block's packing rule is the SDK's (`DrawAbi.hpp`'s `packDrawBlock`) and its arithmetic cases live with it in
 * `tests/test_graphics/DrawBlockTest.cpp`. What needs the plan is this case: a translation the host authored travels
 * through the recorder's snapshot, the compiler's plan and the packer, and it is read back out of the FLAT array
 * (indices 12..14 = the fourth column) - the one assertion that says the number on screen came from the host's model
 * matrix and not from a default left behind somewhere in the middle.
 */

#include <gtest/gtest.h>

#include <cstddef>
#include <cstdint>
#include <vector>

#include <vine/graphics/DrawAbi.hpp>
#include <vine/graphics/RenderCommand.hpp>
#include <vine/graphics/RenderTarget.hpp>
#include <vine/math/Matrix4x4.hpp>

#include <vine/graphics/backend/FrameCompiler.hpp>
#include <vine/graphics/backend/FrameRecorder.hpp>

using vn::graphics::Diagnostics;
using vn::graphics::RenderCommand;
using vn::graphics::RenderTarget;
using vn::graphics::backend::CompiledCommand;
using vn::graphics::backend::CompiledFrame;
using vn::graphics::backend::FrameArena;
using vn::graphics::backend::FrameCompiler;
using vn::graphics::backend::FrameFacts;
using vn::graphics::backend::FrameRecorder;
using vn::graphics::backend::FrameToken;
using vn::graphics::backend::Observe;
using vn::graphics::backend::TargetFacts;
using vn::math::Mat4d;

TEST(DrawBlockTest, TheValuesThePlanCarriesAreTheOnesThatReachTheBytes)
{
    FrameArena    arena{ 32 * 1024 };
    Diagnostics   diagnostics;
    Observe       observe;
    FrameRecorder recorder{ arena, diagnostics, observe };
    FrameCompiler compiler{ arena, diagnostics, observe };

    vn::intrusive_ptr<RenderTarget> target(new RenderTarget());

    TargetFacts facts;
    facts.target        = target.get();
    facts.wanted.width  = 64;
    facts.wanted.height = 64;
    facts.wanted.shape.color_formats.push_back(RenderTarget::ColorFormat::RGBA8);
    facts.current.desc  = facts.wanted;
    facts.current.built = true;
    const std::vector<TargetFacts> table{ facts };

    Mat4d model;
    model(0, 3) = 5.0;  // x translation
    model(1, 3) = -2.0; // y translation
    model(2, 3) = 1.5;  // z translation

    RenderCommand command;
    command.model_matrix = model;
    command.opacity      = 0.25F;
    const std::vector<RenderCommand> commands{ command };

    recorder.beginFrame(FrameToken{ 1 });
    recorder.beginPass(1U);
    recorder.setRenderTarget(target.get());
    recorder.render(commands, nullptr);
    recorder.endPass();
    recorder.endFrame();

    const CompiledFrame& frame = compiler.compile(recorder.description(), FrameFacts{ table });
    ASSERT_EQ(frame.passes.size(), 1U);
    ASSERT_EQ(frame.passes[0].draws.size(), 1U);
    ASSERT_EQ(frame.passes[0].draws[0].commands.size(), 1U);

    vn::graphics::VineDrawBlock block;
    const vn::graphics::backend::CompiledCommand& compiled = frame.passes[0].draws[0].commands[0];
    vn::graphics::packDrawBlock(compiled.model, compiled.opacity, compiled.point_size, block);

    // The translation the host authored - through the recorder's snapshot, the compiler's plan and the packer.
    EXPECT_FLOAT_EQ(block.model[12], 5.0F);
    EXPECT_FLOAT_EQ(block.model[13], -2.0F);
    EXPECT_FLOAT_EQ(block.model[14], 1.5F);
    EXPECT_FLOAT_EQ(block.params[0], 0.25F) << "the compiled command's opacity, not the host's live value";
}
