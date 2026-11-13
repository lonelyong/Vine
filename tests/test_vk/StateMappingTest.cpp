/**
 * @brief M3's device-free evidence: the SDK's state vocabulary maps onto Vulkan's exactly, reverse-Z included.
 *
 * Why a table test and not a pixel test: the two conventions that are easy to get backwards (reverse-Z comparisons and
 * the front-face rule that follows the Y flip) fail SILENTLY in a picture - a scene that culls the wrong faces looks
 * like a modelling problem, and a depth test that compares the wrong way still draws something. Every row is pinned
 * here, so the pipeline builder or a pixel test never has to be the first witness.
 */

#include <gtest/gtest.h>

#include <vine/graphics/DepthRange.hpp>
#include <vine/graphics/PolygonMode.hpp>
#include <vine/graphics/RenderState.hpp>
#include <vine/graphics/Topology.hpp>

#include <vine/backend_vk/internal/StateMapping.hpp>

using vn::graphics::BlendFactor;
using vn::graphics::CompareOp;
using vn::graphics::CullMode;
using vn::graphics::DepthRange;
using vn::graphics::PolygonMode;
using vn::graphics::Topology;
using vn::backend_vk::StateMapping;

TEST(VkStateMappingTest, TheDepthComparisonIsReversedBecauseTheEngineRendersReverseZ)
{
    // The engine's far plane is 0 and its near plane is 1, so "closer passes" (Less) is Vulkan's GREATER.
    EXPECT_EQ(StateMapping::compareOp(CompareOp::Less), VK_COMPARE_OP_GREATER);
    EXPECT_EQ(StateMapping::compareOp(CompareOp::Greater), VK_COMPARE_OP_LESS);
    EXPECT_EQ(StateMapping::compareOp(CompareOp::LessEqual), VK_COMPARE_OP_GREATER_OR_EQUAL);
    EXPECT_EQ(StateMapping::compareOp(CompareOp::GreaterEqual), VK_COMPARE_OP_LESS_OR_EQUAL);

    // The symmetric comparisons are their own answer.
    EXPECT_EQ(StateMapping::compareOp(CompareOp::Never), VK_COMPARE_OP_NEVER);
    EXPECT_EQ(StateMapping::compareOp(CompareOp::Equal), VK_COMPARE_OP_EQUAL);
    EXPECT_EQ(StateMapping::compareOp(CompareOp::NotEqual), VK_COMPARE_OP_NOT_EQUAL);
    EXPECT_EQ(StateMapping::compareOp(CompareOp::Always), VK_COMPARE_OP_ALWAYS);
}

TEST(VkStateMappingTest, TheFrontFaceIsClockwiseBecauseTheProjectionFlipsY)
{
    // The SDK calls a triangle front-facing when it is counter-clockwise in a right-handed world; the vk backend flips
    // the projection's Y so the picture is upright in Vulkan's top-left framebuffer space, which turns that polygon
    // into a clockwise one in framebuffer coordinates.
    EXPECT_EQ(StateMapping::kFrontFace, VK_FRONT_FACE_CLOCKWISE);
    EXPECT_EQ(StateMapping::cullMode(CullMode::None), VK_CULL_MODE_NONE);
    EXPECT_EQ(StateMapping::cullMode(CullMode::Front), VK_CULL_MODE_FRONT_BIT);
    EXPECT_EQ(StateMapping::cullMode(CullMode::Back), VK_CULL_MODE_BACK_BIT);
}

TEST(VkStateMappingTest, EveryBlendFactorHasItsVulkanSpelling)
{
    EXPECT_EQ(StateMapping::blendFactor(BlendFactor::Zero), VK_BLEND_FACTOR_ZERO);
    EXPECT_EQ(StateMapping::blendFactor(BlendFactor::One), VK_BLEND_FACTOR_ONE);
    EXPECT_EQ(StateMapping::blendFactor(BlendFactor::SrcAlpha), VK_BLEND_FACTOR_SRC_ALPHA);
    EXPECT_EQ(StateMapping::blendFactor(BlendFactor::OneMinusSrcAlpha), VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA);
    EXPECT_EQ(StateMapping::blendFactor(BlendFactor::DstAlpha), VK_BLEND_FACTOR_DST_ALPHA);
    EXPECT_EQ(StateMapping::blendFactor(BlendFactor::OneMinusDstAlpha), VK_BLEND_FACTOR_ONE_MINUS_DST_ALPHA);
    EXPECT_EQ(StateMapping::blendFactor(BlendFactor::SrcColor), VK_BLEND_FACTOR_SRC_COLOR);
    EXPECT_EQ(StateMapping::blendFactor(BlendFactor::OneMinusSrcColor), VK_BLEND_FACTOR_ONE_MINUS_SRC_COLOR);
    EXPECT_EQ(StateMapping::blendFactor(BlendFactor::DstColor), VK_BLEND_FACTOR_DST_COLOR);
    EXPECT_EQ(StateMapping::blendFactor(BlendFactor::OneMinusDstColor), VK_BLEND_FACTOR_ONE_MINUS_DST_COLOR);
}

TEST(VkStateMappingTest, PolygonModesAndTopologiesMapOneToOne)
{
    EXPECT_EQ(StateMapping::polygonMode(PolygonMode::Fill), VK_POLYGON_MODE_FILL);
    EXPECT_EQ(StateMapping::polygonMode(PolygonMode::Line), VK_POLYGON_MODE_LINE);
    EXPECT_EQ(StateMapping::polygonMode(PolygonMode::Point), VK_POLYGON_MODE_POINT);

    EXPECT_EQ(StateMapping::topology(Topology::Triangles), VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST);
    EXPECT_EQ(StateMapping::topology(Topology::TriangleStrip), VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP);
    EXPECT_EQ(StateMapping::topology(Topology::TriangleFan), VK_PRIMITIVE_TOPOLOGY_TRIANGLE_FAN);
    EXPECT_EQ(StateMapping::topology(Topology::Points), VK_PRIMITIVE_TOPOLOGY_POINT_LIST);
    EXPECT_EQ(StateMapping::topology(Topology::Lines), VK_PRIMITIVE_TOPOLOGY_LINE_LIST);
    EXPECT_EQ(StateMapping::topology(Topology::LineStrip), VK_PRIMITIVE_TOPOLOGY_LINE_STRIP);
}

TEST(VkStateMappingTest, TheDepthBandBecomesTheViewportDepthRange)
{
    // No band is the whole buffer; a band is passed through (min = far end = minDepth under reverse-Z).
    EXPECT_EQ(StateMapping::viewportDepth(std::nullopt), (std::pair<float, float>{ 0.0F, 1.0F }));

    const DepthRange band{ 0.25F, 0.75F };
    EXPECT_EQ(StateMapping::viewportDepth(band), (std::pair<float, float>{ 0.25F, 0.75F }));
}
