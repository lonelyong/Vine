#include <gtest/gtest.h>

#include <cstdint>
#include <memory>
#include <vector>

#include <vsg/core/ref_ptr.h>
#include <vsg/state/ColorBlendState.h>
#include <vsg/utils/ShaderSet.h>

#include <vine/graphics/Geometry.hpp>
#include <vine/graphics/StateNode.hpp>
#include <vine/vsg/RenderStateMapper.hpp>
#include <vine/vsg/VsgSceneRules.hpp>

using vine::graphics::AttributeBuffer;
using vine::graphics::BlendFactor;
using vine::graphics::CompareOp;
using vine::graphics::CullMode;
using vine::graphics::PolygonMode;
using vine::graphics::ResolvedRenderState;
using vine::graphics::Topology;
using vine::vsg::detail::applyOpaqueBlendForAttachments;
using vine::vsg::detail::ChannelShape;
using vine::vsg::detail::channelShape;
using vine::vsg::detail::colourAttachmentCount;
using vine::vsg::detail::hashCombine;
using vine::vsg::detail::hashStateVariant;
using vine::vsg::detail::ignoredChannelMessage;
using vine::vsg::detail::kHashSeed;
using vine::vsg::detail::kLayoutSeed;
using vine::vsg::detail::vertexLayoutHash;
using vine::vsg::makeRenderStateObjects;
using vine::vsg::RenderStateObjects;

namespace
{

/** @brief One forwarded custom channel, as vertexLayoutHash sees it. */
struct TestChannel
{
    std::uint32_t location   = 0;
    std::uint32_t components = 0;
};

/** @brief A channel whose payload holds @p floats and declares @p components per vertex. */
AttributeBuffer channel(std::uint32_t components, std::size_t floats)
{
    AttributeBuffer buffer;
    buffer.components = components;
    buffer.data       = std::make_shared<std::vector<float>>(floats, 1.0f);
    return buffer;
}

/** @brief A shader set carrying one colour blend state with @p attachments entries. */
::vsg::ref_ptr<::vsg::ShaderSet> shaderSetWithAttachments(std::size_t attachments)
{
    auto set   = ::vsg::ShaderSet::create();
    auto blend = ::vsg::ColorBlendState::create();
    blend->attachments.resize(attachments);
    set->defaultGraphicsPipelineStates.push_back(blend);
    return set;
}

/** @brief The variant key of @p state with no program / material and a built-in layout. */
std::uint64_t variantHash(const ResolvedRenderState& state, std::uint64_t layout = kLayoutSeed)
{
    return hashStateVariant(nullptr, nullptr, state, layout);
}

} // namespace

// ---------------------------------------------------------------------------
// Custom vertex channels: the rule that decides whether a channel may be built.
// ---------------------------------------------------------------------------

TEST(SceneRulesTest, ChannelShapeAcceptsExactlyOneValuePerVertex)
{
    // 3 components, 9 floats, a 3-vertex mesh: the one shape that can be built.
    EXPECT_EQ(channelShape(channel(3u, 9u), 3u), ChannelShape::Ok);
    // Single-component channels are legal (a scalar per vertex).
    EXPECT_EQ(channelShape(channel(1u, 4u), 4u), ChannelShape::Ok);
    EXPECT_EQ(channelShape(channel(4u, 8u), 2u), ChannelShape::Ok);
}

TEST(SceneRulesTest, ChannelShapeRejectsComponentCountsOutsideOneToFour)
{
    // A zero-component channel has no stride at all (the division below is what the
    // order of the checks protects), and Vulkan has no attribute beyond four.
    EXPECT_EQ(channelShape(channel(0u, 9u), 3u), ChannelShape::Components);
    EXPECT_EQ(channelShape(channel(5u, 10u), 2u), ChannelShape::Components);
}

TEST(SceneRulesTest, ChannelShapeRejectsPayloadThatIsNotAWholeNumberOfVertices)
{
    EXPECT_EQ(channelShape(channel(3u, 8u), 2u), ChannelShape::NotDivisible);
    EXPECT_EQ(channelShape(channel(2u, 7u), 3u), ChannelShape::NotDivisible);
}

TEST(SceneRulesTest, ChannelShapeRejectsAChannelWhoseVertexCountDiffersFromTheMesh)
{
    EXPECT_EQ(channelShape(channel(3u, 9u), 4u), ChannelShape::VertexCount);
    EXPECT_EQ(channelShape(channel(4u, 8u), 3u), ChannelShape::VertexCount);
}

TEST(SceneRulesTest, IgnoredChannelMessageNamesTheReasonAndTheLocation)
{
    const std::string components = ignoredChannelMessage(3u, channel(5u, 10u), 2u, ChannelShape::Components).stdstr();
    EXPECT_NE(components.find("loc3"), std::string::npos);
    EXPECT_NE(components.find("components=5"), std::string::npos);
    EXPECT_NE(components.find("ignored"), std::string::npos);

    const std::string not_divisible =
        ignoredChannelMessage(2u, channel(3u, 8u), 2u, ChannelShape::NotDivisible).stdstr();
    EXPECT_NE(not_divisible.find("loc2"), std::string::npos);
    EXPECT_NE(not_divisible.find("8 floats"), std::string::npos);

    const std::string vertex_count = ignoredChannelMessage(7u, channel(3u, 9u), 4u, ChannelShape::VertexCount).stdstr();
    EXPECT_NE(vertex_count.find("loc7"), std::string::npos);
    EXPECT_NE(vertex_count.find("expected 4"), std::string::npos);

    // Reported only for a REJECTED channel: an accepted one has nothing to say.
    EXPECT_TRUE(ignoredChannelMessage(0u, channel(3u, 9u), 3u, ChannelShape::Ok).empty());
}

// ---------------------------------------------------------------------------
// The cache keys: a rule that is wrong here is invisible (it only stops sharing).
// ---------------------------------------------------------------------------

TEST(SceneRulesTest, VertexLayoutHashOfTheBuiltInLayoutIsItsOwnSeed)
{
    // No custom channel must never hash like a layout that HAS channels: the layout is part of
    // the identity of the per-layout ShaderSet, so a collision would hand a geometry the wrong
    // binding set.
    EXPECT_EQ(vertexLayoutHash(std::vector<TestChannel>{}), kLayoutSeed);
    EXPECT_NE(vertexLayoutHash(std::vector<TestChannel>{}), kHashSeed);
}

TEST(SceneRulesTest, VertexLayoutHashDependsOnLocationComponentsAndOrder)
{
    const std::vector<TestChannel> one{ { 3u, 4u } };
    const std::vector<TestChannel> other_location{ { 4u, 4u } };
    const std::vector<TestChannel> other_width{ { 3u, 2u } };
    const std::vector<TestChannel> two{ { 3u, 4u }, { 4u, 3u } };
    const std::vector<TestChannel> two_swapped{ { 4u, 3u }, { 3u, 4u } };

    EXPECT_NE(vertexLayoutHash(one), kLayoutSeed);
    EXPECT_NE(vertexLayoutHash(one), vertexLayoutHash(other_location));
    EXPECT_NE(vertexLayoutHash(one), vertexLayoutHash(other_width));
    EXPECT_NE(vertexLayoutHash(two), vertexLayoutHash(one));
    // The same channels in the other order are a different vertex layout (the bindings are
    // declared in order), so they must not share a ShaderSet.
    EXPECT_NE(vertexLayoutHash(two), vertexLayoutHash(two_swapped));
}

TEST(SceneRulesTest, VariantHashFoldsEveryFieldThePipelineHonours)
{
    const ResolvedRenderState base;
    const std::uint64_t      base_hash = variantHash(base);
    // A state equal to the base hashes the same: the key must be a function of the state.
    EXPECT_EQ(variantHash(ResolvedRenderState()), base_hash);

    ResolvedRenderState changed = base;
    changed.depth.test = !base.depth.test;
    EXPECT_NE(variantHash(changed), base_hash);

    changed           = base;
    changed.depth.write = !base.depth.write;
    EXPECT_NE(variantHash(changed), base_hash);

    changed               = base;
    changed.depth.compare = CompareOp::Greater;
    EXPECT_NE(variantHash(changed), base_hash);

    changed          = base;
    changed.cullMode = CullMode::Back;
    EXPECT_NE(variantHash(changed), base_hash);

    changed               = base;
    changed.blend.enabled = !base.blend.enabled;
    EXPECT_NE(variantHash(changed), base_hash);

    changed           = base;
    changed.blend.src = BlendFactor::OneMinusDstColor;
    EXPECT_NE(variantHash(changed), base_hash);

    changed           = base;
    changed.blend.dst = BlendFactor::One;
    EXPECT_NE(variantHash(changed), base_hash);

    changed             = base;
    changed.polygonMode = PolygonMode::Line;
    EXPECT_NE(variantHash(changed), base_hash);

    changed           = base;
    changed.topology = Topology::Lines;
    EXPECT_NE(variantHash(changed), base_hash);

    // The vertex layout is folded in too, so geometry with a different binding set never shares
    // the base state's variant template.
    EXPECT_NE(variantHash(base, kLayoutSeed + 1u), base_hash);
}

TEST(SceneRulesTest, HashCombineMixesOrderAndValue)
{
    const std::uint64_t a = hashCombine(kHashSeed, 1u);
    const std::uint64_t b = hashCombine(kHashSeed, 2u);
    EXPECT_NE(a, b);
    EXPECT_NE(hashCombine(a, 2u), hashCombine(b, 1u));

    // The mix must keep the running hash's own bits: folding a value into the seed and folding
    // the same value twice are different keys (that is what makes the field order above work).
    EXPECT_NE(hashCombine(hashCombine(kHashSeed, 7u), 7u), hashCombine(kHashSeed, 7u));
}

// ---------------------------------------------------------------------------
// Multi-attachment pipelines: the count and the opaque write.
// ---------------------------------------------------------------------------

TEST(SceneRulesTest, ColourAttachmentCountIsAtLeastOne)
{
    EXPECT_EQ(colourAttachmentCount(nullptr), 1);
    // A shader set that declares no blend state carries the single-attachment default.
    EXPECT_EQ(colourAttachmentCount(::vsg::ref_ptr<::vsg::ShaderSet>()), 1);
    EXPECT_EQ(colourAttachmentCount(::vsg::ShaderSet::create()), 1);
    // An empty blend state is still one attachment, not zero: Vulkan rejects a pipeline with no
    // colour blend entry for a colour attachment.
    EXPECT_EQ(colourAttachmentCount(shaderSetWithAttachments(0u)), 1);
    EXPECT_EQ(colourAttachmentCount(shaderSetWithAttachments(1u)), 1);
    EXPECT_EQ(colourAttachmentCount(shaderSetWithAttachments(3u)), 3);
}

TEST(SceneRulesTest, OpaqueBlendDisablesBlendingOnEveryAttachment)
{
    RenderStateObjects states = makeRenderStateObjects(ResolvedRenderState());
    applyOpaqueBlendForAttachments(states, 3);

    ASSERT_NE(states.colorBlend, nullptr);
    ASSERT_EQ(states.colorBlend->attachments.size(), 3u);
    for (const auto& attachment : states.colorBlend->attachments) {
        // A G-buffer attachment whose alpha is not 1 must not be attenuated (the normal
        // attachment carries shininess/256 in alpha), so every entry writes ONE / ZERO and all
        // four channels.
        EXPECT_EQ(attachment.blendEnable, VK_FALSE);
        EXPECT_EQ(attachment.srcColorBlendFactor, VK_BLEND_FACTOR_ONE);
        EXPECT_EQ(attachment.dstColorBlendFactor, VK_BLEND_FACTOR_ZERO);
        EXPECT_EQ(attachment.colorBlendOp, VK_BLEND_OP_ADD);
        EXPECT_EQ(attachment.srcAlphaBlendFactor, VK_BLEND_FACTOR_ONE);
        EXPECT_EQ(attachment.dstAlphaBlendFactor, VK_BLEND_FACTOR_ZERO);
        EXPECT_EQ(attachment.alphaBlendOp, VK_BLEND_OP_ADD);
        EXPECT_EQ(attachment.colorWriteMask,
                  VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT | VK_COLOR_COMPONENT_B_BIT |
                      VK_COLOR_COMPONENT_A_BIT);
    }

    // Re-applied with another count: the entries are replaced, never appended to.
    applyOpaqueBlendForAttachments(states, 1);
    EXPECT_EQ(states.colorBlend->attachments.size(), 1u);
    EXPECT_EQ(states.colorBlend->attachments[0].blendEnable, VK_FALSE);
}
