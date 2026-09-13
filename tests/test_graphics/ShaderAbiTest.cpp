/**
 * @brief The engine's shader ABI: attribute locations and the L1 data-block layouts.
 *
 * The attribute table and the block structs are the contract a backend materialises
 * (see .ai/design/graphics-shader.md §11/§12). Their values are pinned here so a change
 * that would silently re-map a shader's inputs, or shift a block's std140/D3D-cbuffer
 * offsets, fails a unit test instead of showing up as a wrong picture.
 */

#include <gtest/gtest.h>

#include <vine/graphics/ShaderAbi.hpp>

#include <cstddef>

using namespace vine::graphics;

namespace
{

TEST(ShaderAbiTest, AttributeLocationsAreTheCanonicalValues)
{
    // 8 is the reserved texcoord slot, deliberately not the next number after the
    // custom-channel range: a forwarded custom channel keeps its source location, so
    // a canonical role must not sit where a custom channel could land.
    EXPECT_EQ(attributeLocation(VertexAttribute::Position), 0u);
    EXPECT_EQ(attributeLocation(VertexAttribute::Normal), 1u);
    EXPECT_EQ(attributeLocation(VertexAttribute::Color), 2u);
    EXPECT_EQ(attributeLocation(VertexAttribute::TexCoord0), 8u);
}

TEST(ShaderAbiTest, ViewBlockIsFourMatricesAndTwoVec4)
{
    // std140 and D3D cbuffer packing agree on this shape: 16-byte aligned, all
    // members mat4/vec4, so one layout serves both APIs.
    EXPECT_EQ(sizeof(VineViewBlock), 288u);
    EXPECT_EQ(alignof(VineViewBlock), 16u);
    EXPECT_EQ(offsetof(VineViewBlock, view), 0u);
    EXPECT_EQ(offsetof(VineViewBlock, inv_view), 64u);
    EXPECT_EQ(offsetof(VineViewBlock, proj), 128u);
    EXPECT_EQ(offsetof(VineViewBlock, view_proj), 192u);
    EXPECT_EQ(offsetof(VineViewBlock, cam_pos), 256u);
    EXPECT_EQ(offsetof(VineViewBlock, frame), 272u);
}

TEST(ShaderAbiTest, DrawBlockIsOneMatrixPlusTheParameterSlot)
{
    EXPECT_EQ(sizeof(VineDrawBlock), 80u);
    EXPECT_EQ(alignof(VineDrawBlock), 16u);
    EXPECT_EQ(offsetof(VineDrawBlock, model), 0u);
    EXPECT_EQ(offsetof(VineDrawBlock, params), 64u);
}

}  // namespace
