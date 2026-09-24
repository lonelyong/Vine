#include <gtest/gtest.h>

#include <vine/graphics/StateNode.hpp>
#include <vine/vsg/RenderStateMapper.hpp>

using namespace vn::graphics;
using vn::vsg::RenderStateObjects;
using vn::vsg::makeRenderStateObjects;

namespace
{

/** @brief Builds a resolved state with a custom depth comparison. */
ResolvedRenderState resolveWithDepth(CompareOp op)
{
    ResolvedRenderState state;
    state.depth.compare = op;
    return state;
}

/** @brief Builds a resolved state with a custom primitive topology. */
ResolvedRenderState resolveWithTopology(Topology topology)
{
    ResolvedRenderState state;
    state.topology = topology;
    return state;
}

TEST(RenderStateMapperTest, DefaultStateReproducesBackendDefaults)
{
    const RenderStateObjects o = makeRenderStateObjects(ResolvedRenderState());

    // Depth: test/write on; the reverse-Z backend needs GREATER for the
    // semantic default "closer wins" (CompareOp::Less).
    ASSERT_NE(o.depthStencil, nullptr);
    EXPECT_EQ(o.depthStencil->depthTestEnable, VK_TRUE);
    EXPECT_EQ(o.depthStencil->depthWriteEnable, VK_TRUE);
    EXPECT_EQ(o.depthStencil->depthCompareOp, VK_COMPARE_OP_GREATER);

    // Rasterisation: two-sided (no culling), filled, and CLOCKWISE front faces. The sign is the
    // backend's answer to vsg's Y-inverting projection: the SDK's rule is "front = counter-clockwise in
    // world space" (StateNode.hpp), the projection flips the screen-space orientation
    // (vsg/maths/transform.h: `perspective` negates the Y term, "Y NDC coordinates are inverted in
    // Vulkan"), so declaring the framebuffer-space front faces CLOCKWISE is what keeps cullMode meaning
    // what the SDK says. Pinned with the cull modes below because the two lines are one decision.
    ASSERT_NE(o.rasterization, nullptr);
    EXPECT_EQ(o.rasterization->cullMode, VK_CULL_MODE_NONE);
    EXPECT_EQ(o.rasterization->polygonMode, VK_POLYGON_MODE_FILL);
    EXPECT_EQ(o.rasterization->frontFace, VK_FRONT_FACE_CLOCKWISE);

    // Blending: always on (per-vertex opacity path) with the standard alpha
    // equation when no StateNode opts into custom factors.
    ASSERT_NE(o.colorBlend, nullptr);
    ASSERT_EQ(o.colorBlend->attachments.size(), 1u);
    const auto& a = o.colorBlend->attachments[0];
    EXPECT_EQ(a.blendEnable, VK_TRUE);
    EXPECT_EQ(a.srcColorBlendFactor, VK_BLEND_FACTOR_SRC_ALPHA);
    EXPECT_EQ(a.dstColorBlendFactor, VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA);

    // Topology: triangle list.
    ASSERT_NE(o.inputAssembly, nullptr);
    EXPECT_EQ(o.inputAssembly->topology, VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST);
}

TEST(RenderStateMapperTest, DepthCompareInvertsForReverseZ)
{
    // The SDK compare ops describe closeness; vsg's reverse-Z projection maps
    // "closer/farther" onto the inverted Vulkan op.
    EXPECT_EQ(makeRenderStateObjects(resolveWithDepth(CompareOp::Less)).depthStencil->depthCompareOp,
              VK_COMPARE_OP_GREATER);
    EXPECT_EQ(makeRenderStateObjects(resolveWithDepth(CompareOp::LessEqual)).depthStencil->depthCompareOp,
              VK_COMPARE_OP_GREATER_OR_EQUAL);
    EXPECT_EQ(makeRenderStateObjects(resolveWithDepth(CompareOp::Greater)).depthStencil->depthCompareOp,
              VK_COMPARE_OP_LESS);
    EXPECT_EQ(makeRenderStateObjects(resolveWithDepth(CompareOp::GreaterEqual)).depthStencil->depthCompareOp,
              VK_COMPARE_OP_LESS_OR_EQUAL);
    EXPECT_EQ(makeRenderStateObjects(resolveWithDepth(CompareOp::Equal)).depthStencil->depthCompareOp,
              VK_COMPARE_OP_EQUAL);
    EXPECT_EQ(makeRenderStateObjects(resolveWithDepth(CompareOp::NotEqual)).depthStencil->depthCompareOp,
              VK_COMPARE_OP_NOT_EQUAL);
    EXPECT_EQ(makeRenderStateObjects(resolveWithDepth(CompareOp::Always)).depthStencil->depthCompareOp,
              VK_COMPARE_OP_ALWAYS);
    EXPECT_EQ(makeRenderStateObjects(resolveWithDepth(CompareOp::Never)).depthStencil->depthCompareOp,
              VK_COMPARE_OP_NEVER);
}

TEST(RenderStateMapperTest, DepthTestAndWriteMapDirectly)
{
    ResolvedRenderState state;
    state.depth.test = false;
    state.depth.write = false;
    const auto o = makeRenderStateObjects(state);
    EXPECT_EQ(o.depthStencil->depthTestEnable, VK_FALSE);
    EXPECT_EQ(o.depthStencil->depthWriteEnable, VK_FALSE);
}

TEST(RenderStateMapperTest, CullModeAndPolygonMap)
{
    ResolvedRenderState state;
    state.cullMode = CullMode::Back;
    state.polygonMode = PolygonMode::Line;
    auto o = makeRenderStateObjects(state);
    EXPECT_EQ(o.rasterization->cullMode, VK_CULL_MODE_BACK_BIT);
    EXPECT_EQ(o.rasterization->polygonMode, VK_POLYGON_MODE_LINE);
    // The mask means what the SDK says only PAIRED with the front face (see the default-state test): with
    // the declaration flipped, "cull back" would cull the SDK's front faces -- the demo's single-sided box
    // would draw its interior instead of its outside, silently.
    EXPECT_EQ(o.rasterization->frontFace, VK_FRONT_FACE_CLOCKWISE);

    state.cullMode = CullMode::Front;
    state.polygonMode = PolygonMode::Point;
    o = makeRenderStateObjects(state);
    EXPECT_EQ(o.rasterization->cullMode, VK_CULL_MODE_FRONT_BIT);
    EXPECT_EQ(o.rasterization->polygonMode, VK_POLYGON_MODE_POINT);
}

TEST(RenderStateMapperTest, TopologyMaps)
{
    EXPECT_EQ(makeRenderStateObjects(resolveWithTopology(Topology::Points)).inputAssembly->topology,
              VK_PRIMITIVE_TOPOLOGY_POINT_LIST);
    EXPECT_EQ(makeRenderStateObjects(resolveWithTopology(Topology::Lines)).inputAssembly->topology,
              VK_PRIMITIVE_TOPOLOGY_LINE_LIST);
}

TEST(RenderStateMapperTest, BlendFactorsAppliedWhenEnabled)
{
    ResolvedRenderState state;
    state.blend.enabled = true;
    state.blend.src = BlendFactor::SrcColor;
    state.blend.dst = BlendFactor::DstColor;
    const auto o = makeRenderStateObjects(state);
    ASSERT_EQ(o.colorBlend->attachments.size(), 1u);
    const auto& a = o.colorBlend->attachments[0];
    EXPECT_EQ(a.blendEnable, VK_TRUE);
    EXPECT_EQ(a.srcColorBlendFactor, VK_BLEND_FACTOR_SRC_COLOR);
    EXPECT_EQ(a.dstColorBlendFactor, VK_BLEND_FACTOR_DST_COLOR);
    EXPECT_EQ(a.srcAlphaBlendFactor, VK_BLEND_FACTOR_SRC_COLOR);
    EXPECT_EQ(a.dstAlphaBlendFactor, VK_BLEND_FACTOR_DST_COLOR);
}

}  // namespace
