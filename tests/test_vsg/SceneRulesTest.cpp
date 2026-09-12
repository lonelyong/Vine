#include <gtest/gtest.h>

#include <cmath>
#include <cstdint>
#include <memory>
#include <vector>

#include <vsg/core/Array.h>
#include <vsg/core/ref_ptr.h>
#include <vsg/state/ColorBlendState.h>
#include <vsg/utils/ShaderSet.h>

#include <vine/geometry/Array.hpp>
#include <vine/graphics/Geometry.hpp>
#include <vine/graphics/ShaderProgram.hpp>
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
using vine::vsg::detail::customAttributeName;
using vine::vsg::detail::faceNormal;
using vine::vsg::detail::formatForComponents;
using vine::vsg::detail::hashCombine;
using vine::vsg::detail::hashStateVariant;
using vine::vsg::detail::ignoredChannelMessage;
using vine::vsg::detail::ignoredNormalChannelMessage;
using vine::vsg::detail::kHashSeed;
using vine::vsg::detail::kLayoutSeed;
using vine::vsg::detail::makeIndexedNormals;
using vine::vsg::detail::makeNormals;
using vine::vsg::detail::makeTypedVertexData;
using vine::vsg::detail::makeWhiteColors;
using vine::vsg::detail::normalIsUsable;
using vine::vsg::detail::sampleVertexData;
using vine::vsg::detail::stageFlag;
using vine::vsg::detail::unpackXyz;
using vine::vsg::detail::vertexLayoutHash;
using vine::vsg::detail::XyzUnpack;
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

/** @brief A buffer whose packed floats are @p values at @p components per vertex. */
AttributeBuffer packed(std::uint32_t components, std::vector<float> values)
{
    AttributeBuffer buffer;
    buffer.components = components;
    buffer.data       = std::make_shared<std::vector<float>>(std::move(values));
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

// ---------------------------------------------------------------------------
// A forwarded channel's vertex binding: its name, format and sample data must agree.
// ---------------------------------------------------------------------------

TEST(SceneRulesTest, FormatForComponentsTracksTheComponentCount)
{
    EXPECT_EQ(formatForComponents(1u), VK_FORMAT_R32_SFLOAT);
    EXPECT_EQ(formatForComponents(2u), VK_FORMAT_R32G32_SFLOAT);
    EXPECT_EQ(formatForComponents(3u), VK_FORMAT_R32G32B32_SFLOAT);
    EXPECT_EQ(formatForComponents(4u), VK_FORMAT_R32G32B32A32_SFLOAT);
    // 0 / >4 are rejected by channelShape before a binding is ever declared; the mapping still has
    // to stay a legal four-component format rather than an undefined one.
    EXPECT_EQ(formatForComponents(0u), VK_FORMAT_R32G32B32A32_SFLOAT);
    EXPECT_EQ(formatForComponents(5u), VK_FORMAT_R32G32B32A32_SFLOAT);
}

TEST(SceneRulesTest, SampleVertexDataMatchesTheBindingFormat)
{
    // vsg matches a bound array to a binding by value type, so the sample must be one element of
    // the array type whose component count (and therefore stride) equals the binding format's.
    EXPECT_NE(sampleVertexData(1u).cast<::vsg::floatArray>(), nullptr);
    EXPECT_NE(sampleVertexData(2u).cast<::vsg::vec2Array>(), nullptr);
    EXPECT_NE(sampleVertexData(3u).cast<::vsg::vec3Array>(), nullptr);
    EXPECT_NE(sampleVertexData(4u).cast<::vsg::vec4Array>(), nullptr);
    for (std::uint32_t components : { 1u, 2u, 3u, 4u }) {
        const auto sample = sampleVertexData(components);
        ASSERT_NE(sample, nullptr);
        EXPECT_EQ(sample->valueCount(), 1u);
        EXPECT_EQ(sample->valueSize(), static_cast<std::size_t>(components) * sizeof(float));
    }
    // Out-of-range counts fall back to the vec4 sample, agreeing with the format mapping above.
    EXPECT_NE(sampleVertexData(0u).cast<::vsg::vec4Array>(), nullptr);
    EXPECT_NE(sampleVertexData(5u).cast<::vsg::vec4Array>(), nullptr);
}

TEST(SceneRulesTest, CustomAttributeNameIsStableAndDistinctFromTheBuiltIns)
{
    // The ShaderSet binding and the configurator's assignArray both call this, so a rename that
    // updated only one side would leave the attribute silently unbound.
    EXPECT_EQ(customAttributeName(3u), "vine_Attribute3");
    EXPECT_EQ(customAttributeName(7u), "vine_Attribute7");
    EXPECT_EQ(customAttributeName(3u), customAttributeName(3u));
    EXPECT_NE(customAttributeName(3u), customAttributeName(4u));
    for (std::uint32_t location : { 3u, 4u, 10u }) {
        const std::string name = customAttributeName(location);
        EXPECT_NE(name, "vsg_Vertex");
        EXPECT_NE(name, "vsg_Normal");
        EXPECT_NE(name, "vsg_Color");
        EXPECT_NE(name, "material");
    }
}

TEST(SceneRulesTest, StageFlagMapsEveryStageToItsOwnVulkanBit)
{
    EXPECT_EQ(stageFlag(vine::graphics::ShaderStageType::Vertex), VK_SHADER_STAGE_VERTEX_BIT);
    EXPECT_EQ(stageFlag(vine::graphics::ShaderStageType::Fragment), VK_SHADER_STAGE_FRAGMENT_BIT);
    EXPECT_EQ(stageFlag(vine::graphics::ShaderStageType::Compute), VK_SHADER_STAGE_COMPUTE_BIT);
}

// ---------------------------------------------------------------------------
// Unpacking an xyz channel: the stride rule that keeps vertices from interleaving.
// ---------------------------------------------------------------------------

TEST(SceneRulesTest, UnpackXyzTakesTheFirstThreeOfEveryStride)
{
    vine::geometry::Vec3fArray out;
    // vec3: one vertex per three floats.
    EXPECT_EQ(unpackXyz(packed(3u, { 1.f, 2.f, 3.f, 4.f, 5.f, 6.f }), out), XyzUnpack::Ok);
    ASSERT_EQ(out.size(), 2u);
    EXPECT_FLOAT_EQ(out[0].x, 1.f);
    EXPECT_FLOAT_EQ(out[0].z, 3.f);
    EXPECT_FLOAT_EQ(out[1].x, 4.f);
    EXPECT_FLOAT_EQ(out[1].y, 5.f);

    // vec4: the trailing w is skipped, never read as the next vertex's x (the interleave bug D1).
    EXPECT_EQ(unpackXyz(packed(4u, { 1.f, 2.f, 3.f, 9.f, 4.f, 5.f, 6.f, 9.f }), out), XyzUnpack::Ok);
    ASSERT_EQ(out.size(), 2u);
    EXPECT_FLOAT_EQ(out[0].z, 3.f);
    EXPECT_FLOAT_EQ(out[1].x, 4.f);
    EXPECT_FLOAT_EQ(out[1].z, 6.f);

    // A legal but empty channel unpacks to nothing and is still Ok.
    EXPECT_EQ(unpackXyz(packed(3u, {}), out), XyzUnpack::Ok);
    EXPECT_TRUE(out.empty());
}

TEST(SceneRulesTest, UnpackXyzRejectsAComponentCountThatIsNotAnXyzStride)
{
    vine::geometry::Vec3fArray out;
    EXPECT_EQ(unpackXyz(packed(1u, { 1.f, 2.f, 3.f }), out), XyzUnpack::NotXyzStride);
    EXPECT_EQ(unpackXyz(packed(2u, { 1.f, 2.f, 3.f, 4.f }), out), XyzUnpack::NotXyzStride);
    EXPECT_EQ(unpackXyz(packed(0u, {}), out), XyzUnpack::NotXyzStride);
    EXPECT_EQ(unpackXyz(packed(5u, { 1.f, 2.f, 3.f, 4.f, 5.f }), out), XyzUnpack::NotXyzStride);
}

TEST(SceneRulesTest, UnpackXyzRejectsAPartialVertex)
{
    vine::geometry::Vec3fArray out;
    EXPECT_EQ(unpackXyz(packed(3u, { 1.f, 2.f, 3.f, 4.f }), out), XyzUnpack::NotDivisible);
    EXPECT_EQ(unpackXyz(packed(4u, { 1.f, 2.f, 3.f, 4.f, 5.f }), out), XyzUnpack::NotDivisible);
}

TEST(SceneRulesTest, UnpackXyzReplacesTheOutputAndNeverTouchesItWhenRejected)
{
    vine::geometry::Vec3fArray out{ vine::math::Vec3f(9.f, 9.f, 9.f) };
    // Rejected before anything is written: the caller's array is left exactly as it was.
    EXPECT_EQ(unpackXyz(packed(2u, { 1.f, 2.f }), out), XyzUnpack::NotXyzStride);
    ASSERT_EQ(out.size(), 1u);
    EXPECT_FLOAT_EQ(out[0].x, 9.f);
    // Accepted: the array is replaced, never appended to (a consumer reuses one scratch array).
    EXPECT_EQ(unpackXyz(packed(3u, { 1.f, 2.f, 3.f, 4.f, 5.f, 6.f }), out), XyzUnpack::Ok);
    ASSERT_EQ(out.size(), 2u);
    EXPECT_FLOAT_EQ(out[0].x, 1.f);
    EXPECT_FLOAT_EQ(out[1].x, 4.f);
}

TEST(SceneRulesTest, IgnoredNormalChannelMessageCarriesEachBranchesOwnNumbers)
{
    // The two branches once shared one format string whose arguments were ordered for a single one
    // of them, printing the component count where the float count belongs: pin each branch's own
    // numbers so a merged format string cannot come back.
    const std::string stride =
        ignoredNormalChannelMessage(packed(2u, { 1.f, 2.f }), XyzUnpack::NotXyzStride).stdstr();
    EXPECT_NE(stride.find("loc1"), std::string::npos);
    EXPECT_NE(stride.find("components=2"), std::string::npos);
    EXPECT_NE(stride.find("3 or 4"), std::string::npos);
    EXPECT_NE(stride.find("derived"), std::string::npos);
    EXPECT_EQ(stride.find("floats"), std::string::npos);

    const std::string partial =
        ignoredNormalChannelMessage(packed(3u, { 1.f, 2.f, 3.f, 4.f }), XyzUnpack::NotDivisible).stdstr();
    EXPECT_NE(partial.find("loc1"), std::string::npos);
    EXPECT_NE(partial.find("4 floats"), std::string::npos);
    EXPECT_NE(partial.find("components=3"), std::string::npos);
    EXPECT_NE(partial.find("derived"), std::string::npos);
    EXPECT_EQ(partial.find("3 or 4"), std::string::npos);
}

// ---------------------------------------------------------------------------
// Derived normals and the default vertex colour: the rules that decide whether a mesh
// without normals lights up, or silently poisons itself with NaN.
// ---------------------------------------------------------------------------

TEST(SceneRulesTest, FaceNormalFollowsTheWindingAndIsZeroWhenDegenerate)
{
    const vine::math::Vec3f a(0.f, 0.f, 0.f);
    const vine::math::Vec3f b(1.f, 0.f, 0.f);
    const vine::math::Vec3f c(0.f, 1.f, 0.f);
    // Counter-clockwise in the xy plane faces +z.
    const vine::math::Vec3f n = faceNormal(a, b, c);
    EXPECT_FLOAT_EQ(n.x, 0.f);
    EXPECT_FLOAT_EQ(n.y, 0.f);
    EXPECT_FLOAT_EQ(n.z, 1.f);
    // Swapping two vertices reverses the winding, so the normal flips: the orientation is the rule.
    EXPECT_FLOAT_EQ(faceNormal(a, c, b).z, -1.f);

    // Degenerate (collinear / duplicated vertices) is EXACTLY zero, never a NaN source.
    const vine::math::Vec3f collinear = faceNormal(a, b, vine::math::Vec3f(2.f, 0.f, 0.f));
    EXPECT_EQ(collinear.x, 0.f);
    EXPECT_EQ(collinear.y, 0.f);
    EXPECT_EQ(collinear.z, 0.f);
    const vine::math::Vec3f duplicate = faceNormal(b, b, c);
    EXPECT_EQ(duplicate.x, 0.f);
    EXPECT_EQ(duplicate.y, 0.f);
    EXPECT_EQ(duplicate.z, 0.f);
}

TEST(SceneRulesTest, NormalIsUsableRejectsOnlyAZeroLength)
{
    EXPECT_FALSE(normalIsUsable(0.0f));
    EXPECT_FALSE(normalIsUsable(-1.0f)); // a squared length is never negative; treat it as unusable
    EXPECT_TRUE(normalIsUsable(1e-30f));
    EXPECT_TRUE(normalIsUsable(1.0f));
}

TEST(SceneRulesTest, MakeNormalsDerivesUnitFaceNormalsForANonIndexedMesh)
{
    const vine::geometry::Vec3fArray positions{ vine::math::Vec3f(0.f, 0.f, 0.f), vine::math::Vec3f(1.f, 0.f, 0.f),
                                                vine::math::Vec3f(0.f, 1.f, 0.f) };
    const auto normals = makeNormals(positions, {});
    ASSERT_NE(normals, nullptr);
    ASSERT_EQ(normals->size(), 3u);
    for (std::size_t i = 0; i < positions.size(); ++i) {
        EXPECT_FLOAT_EQ((*normals)[i].x, 0.f);
        EXPECT_FLOAT_EQ((*normals)[i].y, 0.f);
        EXPECT_FLOAT_EQ((*normals)[i].z, 1.f);
    }
}

TEST(SceneRulesTest, MakeNormalsKeepsADegenerateTriangleZeroInsteadOfNaN)
{
    // Three collinear vertices: the face normal is zero-length, and normalising it would write NaN
    // into every vertex of the triangle (nothing reports a NaN attribute).
    const vine::geometry::Vec3fArray positions{ vine::math::Vec3f(0.f, 0.f, 0.f), vine::math::Vec3f(1.f, 1.f, 1.f),
                                                vine::math::Vec3f(2.f, 2.f, 2.f) };
    const auto normals = makeNormals(positions, {});
    ASSERT_NE(normals, nullptr);
    ASSERT_EQ(normals->size(), 3u);
    for (const auto& n : *normals) {
        EXPECT_EQ(n.x, 0.f);
        EXPECT_EQ(n.y, 0.f);
        EXPECT_EQ(n.z, 0.f);
        EXPECT_FALSE(std::isnan(n.x));
        EXPECT_FALSE(std::isnan(n.y));
        EXPECT_FALSE(std::isnan(n.z));
    }
}

TEST(SceneRulesTest, MakeNormalsCopiesProvidedMeshNormalsVerbatim)
{
    const vine::geometry::Vec3fArray positions{ vine::math::Vec3f(0.f, 0.f, 0.f), vine::math::Vec3f(1.f, 0.f, 0.f),
                                                vine::math::Vec3f(0.f, 1.f, 0.f) };
    // Deliberately not unit length: provided normals are copied, not re-derived or re-normalised.
    const vine::geometry::Vec3fArray provided{ vine::math::Vec3f(0.1f, 0.2f, 0.3f), vine::math::Vec3f(0.4f, 0.5f, 0.6f),
                                               vine::math::Vec3f(0.7f, 0.8f, 0.9f) };
    const auto normals = makeNormals(positions, provided);
    ASSERT_NE(normals, nullptr);
    ASSERT_EQ(normals->size(), 3u);
    for (std::size_t i = 0; i < provided.size(); ++i) {
        EXPECT_FLOAT_EQ((*normals)[i].x, provided[i].x);
        EXPECT_FLOAT_EQ((*normals)[i].y, provided[i].y);
        EXPECT_FLOAT_EQ((*normals)[i].z, provided[i].z);
    }
}

TEST(SceneRulesTest, MakeIndexedNormalsAccumulatesEveryReferencedVertex)
{
    // A CCW unit quad (two triangles sharing the 0-2 diagonal): every vertex ends up +z.
    const vine::geometry::Vec3fArray positions{ vine::math::Vec3f(0.f, 0.f, 0.f), vine::math::Vec3f(1.f, 0.f, 0.f),
                                                vine::math::Vec3f(1.f, 1.f, 0.f), vine::math::Vec3f(0.f, 1.f, 0.f) };
    auto indices = ::vsg::uintArray::create(6u);
    (*indices)[0] = 0u;
    (*indices)[1] = 1u;
    (*indices)[2] = 2u;
    (*indices)[3] = 0u;
    (*indices)[4] = 2u;
    (*indices)[5] = 3u;
    const auto normals = makeIndexedNormals(positions, {}, *indices);
    ASSERT_NE(normals, nullptr);
    ASSERT_EQ(normals->size(), 4u);
    for (const auto& n : *normals) {
        EXPECT_FLOAT_EQ(n.x, 0.f);
        EXPECT_FLOAT_EQ(n.y, 0.f);
        EXPECT_FLOAT_EQ(n.z, 1.f);
    }
}

TEST(SceneRulesTest, MakeIndexedNormalsSkipsOutOfRangeIndices)
{
    // The second triangle names vertex 99: it is skipped, not read, and does not disturb the normals
    // the first triangle accumulated (the data path rejects such geometry; this must stay safe).
    const vine::geometry::Vec3fArray positions{ vine::math::Vec3f(0.f, 0.f, 0.f), vine::math::Vec3f(1.f, 0.f, 0.f),
                                                vine::math::Vec3f(0.f, 1.f, 0.f) };
    auto indices = ::vsg::uintArray::create(6u);
    (*indices)[0] = 0u;
    (*indices)[1] = 1u;
    (*indices)[2] = 2u;
    (*indices)[3] = 0u;
    (*indices)[4] = 1u;
    (*indices)[5] = 99u;
    const auto normals = makeIndexedNormals(positions, {}, *indices);
    ASSERT_NE(normals, nullptr);
    ASSERT_EQ(normals->size(), 3u);
    for (const auto& n : *normals) {
        EXPECT_FLOAT_EQ(n.z, 1.f);
    }
}

TEST(SceneRulesTest, MakeIndexedNormalsLeavesAnUnreferencedVertexZero)
{
    // A vertex no triangle references accumulates nothing: it must stay zero (a zero normal, not
    // NaN), which is what the unusable-length guard is for.
    const vine::geometry::Vec3fArray positions{ vine::math::Vec3f(0.f, 0.f, 0.f), vine::math::Vec3f(1.f, 0.f, 0.f),
                                                vine::math::Vec3f(0.f, 1.f, 0.f),
                                                vine::math::Vec3f(5.f, 5.f, 5.f) };
    auto indices = ::vsg::uintArray::create(3u);
    (*indices)[0] = 0u;
    (*indices)[1] = 1u;
    (*indices)[2] = 2u;
    const auto normals = makeIndexedNormals(positions, {}, *indices);
    ASSERT_NE(normals, nullptr);
    ASSERT_EQ(normals->size(), 4u);
    EXPECT_FLOAT_EQ((*normals)[3].x, 0.f);
    EXPECT_FLOAT_EQ((*normals)[3].y, 0.f);
    EXPECT_FLOAT_EQ((*normals)[3].z, 0.f);
    EXPECT_FALSE(std::isnan((*normals)[3].x));
}

TEST(SceneRulesTest, MakeWhiteColorsIsOpaqueWhiteForEveryVertex)
{
    const auto colors = makeWhiteColors(3u);
    ASSERT_NE(colors, nullptr);
    ASSERT_EQ(colors->size(), 3u);
    for (const auto& c : *colors) {
        EXPECT_FLOAT_EQ(c.r, 1.f);
        EXPECT_FLOAT_EQ(c.g, 1.f);
        EXPECT_FLOAT_EQ(c.b, 1.f);
        EXPECT_FLOAT_EQ(c.a, 1.f);
    }
    // No vertices is legal (an empty drawable) and yields an empty array, not a null one.
    const auto none = makeWhiteColors(0u);
    ASSERT_NE(none, nullptr);
    EXPECT_EQ(none->size(), 0u);
}

// ---------------------------------------------------------------------------
// Materialising a packed channel: the array type must match the binding format it is bound to.
// ---------------------------------------------------------------------------

TEST(SceneRulesTest, MakeTypedVertexDataPicksTheArrayTypeFromTheComponentCount)
{
    const std::vector<float> data{ 1.f, 2.f, 3.f, 4.f, 5.f, 6.f, 7.f, 8.f };
    EXPECT_NE(makeTypedVertexData(1u, data, 8u).cast<::vsg::floatArray>(), nullptr);
    EXPECT_NE(makeTypedVertexData(2u, data, 4u).cast<::vsg::vec2Array>(), nullptr);
    EXPECT_NE(makeTypedVertexData(3u, data, 2u).cast<::vsg::vec3Array>(), nullptr);
    EXPECT_NE(makeTypedVertexData(4u, data, 2u).cast<::vsg::vec4Array>(), nullptr);
    // 0 / >4 are rejected by channelShape before this runs; the fallback is still the vec4 form.
    EXPECT_NE(makeTypedVertexData(0u, data, 2u).cast<::vsg::vec4Array>(), nullptr);
    EXPECT_NE(makeTypedVertexData(5u, data, 1u).cast<::vsg::vec4Array>(), nullptr);
}

TEST(SceneRulesTest, MakeTypedVertexDataTypeAgreesWithTheBindingFormat)
{
    // The array built here is bound to the attribute whose Vulkan format formatForComponents declares:
    // if the two disagreed on the component count the configurator would accept it and the shader would
    // misread the attribute at draw time, with nothing to report.
    const std::vector<float> data{ 1.f, 2.f, 3.f, 4.f };
    for (std::uint32_t components : { 1u, 2u, 3u, 4u }) {
        const auto built  = makeTypedVertexData(components, data, 1u);
        const auto sample = sampleVertexData(components);
        ASSERT_NE(built, nullptr);
        ASSERT_NE(sample, nullptr);
        EXPECT_EQ(std::string(built->className()), std::string(sample->className()));
    }
}

TEST(SceneRulesTest, MakeTypedVertexDataCopiesEveryComponentInOrder)
{
    // A 3-component channel: each vertex reads its three scalars at its own stride (the same interleave
    // rule unpackXyz applies to positions), never three consecutive floats.
    const std::vector<float> data{ 1.f, 2.f, 3.f, 4.f, 5.f, 6.f };
    const auto arr = makeTypedVertexData(3u, data, 2u).cast<::vsg::vec3Array>();
    ASSERT_NE(arr, nullptr);
    ASSERT_EQ(arr->size(), 2u);
    EXPECT_FLOAT_EQ((*arr)[0].x, 1.f);
    EXPECT_FLOAT_EQ((*arr)[0].y, 2.f);
    EXPECT_FLOAT_EQ((*arr)[0].z, 3.f);
    EXPECT_FLOAT_EQ((*arr)[1].x, 4.f);
    EXPECT_FLOAT_EQ((*arr)[1].y, 5.f);
    EXPECT_FLOAT_EQ((*arr)[1].z, 6.f);

    // A 4-component channel keeps the fourth scalar in w: it is data, not padding.
    const std::vector<float> rgba{ 1.f, 2.f, 3.f, 4.f };
    const auto vec4s = makeTypedVertexData(4u, rgba, 1u).cast<::vsg::vec4Array>();
    ASSERT_NE(vec4s, nullptr);
    EXPECT_FLOAT_EQ((*vec4s)[0].w, 4.f);

    // Zero vertices is legal and yields an empty array, not a null one.
    const auto none = makeTypedVertexData(3u, {}, 0u);
    ASSERT_NE(none, nullptr);
    EXPECT_EQ(none->valueCount(), 0u);
}
