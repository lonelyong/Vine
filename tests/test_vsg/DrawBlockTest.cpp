/**
 * @brief Packing the per-draw ABI block from a compiled command (see `.ai/design/vsg-reimplementation.md`
 * §11.17 and ShaderAbi.hpp).
 *
 * Device-free by construction: packing is arithmetic over the plan's values.
 *
 * The first case packs a NON-SYMMETRIC matrix on purpose. A transposed packing is invisible with an identity,
 * a scale or any other symmetric matrix - which is most test content - and shows up later as "the model
 * rotates the wrong way / the object is somewhere else", which reads as a modelling bug. The values here are
 * chosen so every element is distinct and its position in the flat array says which convention was used.
 *
 * The second case drives the whole path the value travels (recorder → compiler → packer) and reads the
 * TRANSLATION out of the flat array: it is the one assertion that says the number on screen came from the
 * host's model matrix and not from a default left behind somewhere in the middle.
 */

#include <gtest/gtest.h>

#include <cstddef>
#include <cstdint>
#include <vector>

#include <vine/graphics/RenderCommand.hpp>
#include <vine/graphics/RenderTarget.hpp>
#include <vine/math/Matrix4x4.hpp>

#include <vine/vsg/api/DrawBlock.hpp>
#include <vine/vsg/core/FrameCompiler.hpp>
#include <vine/vsg/core/FrameRecorder.hpp>

using vn::graphics::RenderCommand;
using vn::graphics::RenderTarget;
using vn::math::Mat4d;
using vn::vsg::packDrawBlock;
using vn::vsg::core::CompiledCommand;
using vn::vsg::core::CompiledFrame;
using vn::vsg::core::Diagnostics;
using vn::vsg::core::FrameArena;
using vn::vsg::core::FrameCompiler;
using vn::vsg::core::FrameFacts;
using vn::vsg::core::FrameRecorder;
using vn::vsg::core::FrameToken;
using vn::vsg::core::Observe;
using vn::vsg::core::TargetFacts;

namespace
{

/// @brief A matrix whose element (row, column) is `row * 10 + column`: every element distinct, not symmetric.
Mat4d nonSymmetric()
{
    Mat4d matrix;
    for (int row = 0; row < 4; ++row)
    {
        for (int column = 0; column < 4; ++column)
        {
            matrix(row, column) = static_cast<double>(row * 10 + column);
        }
    }
    return matrix;
}

}  // namespace

TEST(DrawBlockTest, TheModelMatrixLandsColumnMajorAndTheOpacityInTheParameterSlot)
{
    CompiledCommand command;
    command.model   = nonSymmetric();
    command.opacity = 0.375F;

    vn::graphics::VineDrawBlock block;
    packDrawBlock(command, block);

    // (row, column) at `column * 4 + row`: a row-major packing would put 1 where 10 belongs (and vice versa).
    for (int row = 0; row < 4; ++row)
    {
        for (int column = 0; column < 4; ++column)
        {
            const std::size_t index = static_cast<std::size_t>(column) * 4U + static_cast<std::size_t>(row);
            EXPECT_FLOAT_EQ(block.model[index], static_cast<float>(row * 10 + column))
                << "row " << row << ", column " << column;
        }
    }

    // Translation lives in the fourth column, i.e. at the end of the flat array - the same fact seen from the
    // bytes rather than from the math.
    EXPECT_FLOAT_EQ(block.model[12], 3.0F) << "x translation";
    EXPECT_FLOAT_EQ(block.model[13], 13.0F) << "y translation";
    EXPECT_FLOAT_EQ(block.model[14], 23.0F) << "z translation";
    EXPECT_FLOAT_EQ(block.model[15], 33.0F);

    EXPECT_FLOAT_EQ(block.params[0], 0.375F);
    EXPECT_FLOAT_EQ(block.params[1], 0.0F);  // the reserved user slot packs as zero, not as a leftover
    EXPECT_FLOAT_EQ(block.params[2], 0.0F);
    EXPECT_FLOAT_EQ(block.params[3], 0.0F);
}

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
    model(0, 3) = 5.0;   // x translation
    model(1, 3) = -2.0;  // y translation
    model(2, 3) = 1.5;   // z translation

    RenderCommand command;
    command.modelMatrix = model;
    command.opacity     = 0.25F;
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
    packDrawBlock(frame.passes[0].draws[0].commands[0], block);

    // The translation the host authored - through the recorder's snapshot, the compiler's plan and the packer.
    EXPECT_FLOAT_EQ(block.model[12], 5.0F);
    EXPECT_FLOAT_EQ(block.model[13], -2.0F);
    EXPECT_FLOAT_EQ(block.model[14], 1.5F);
    EXPECT_FLOAT_EQ(block.params[0], 0.25F) << "the compiled command's opacity, not the host's live value";
}
