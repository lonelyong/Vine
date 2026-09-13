#include <gtest/gtest.h>

#include <algorithm>

#include <vine/graphics/Geometry.hpp>
#include <vine/graphics/Material.hpp>
#include <vine/graphics/RenderCommand.hpp>
#include <vine/graphics/ShaderProgram.hpp>
#include <vine/vsg/SceneBridge.hpp>

#include <vsg/commands/BindVertexBuffers.h>
#include <vsg/commands/Commands.h>
#include <vsg/commands/DrawIndexed.h>
#include <vsg/core/Array.h>
#include <vsg/io/Options.h>
#include <vsg/nodes/Group.h>
#include <vsg/utils/ShaderSet.h>

using namespace vine::graphics;
using vine::math::Mat4d;

namespace
{

/// Builds a channel that owns its packed floats.
AttributeChannel packedChannel(const std::vector<float>& floats, std::uint32_t components)
{
    return AttributeChannel::packed(floats, components);
}

/// Attaches an attribute buffer at a location.
void addChannel(Geometry* geom, std::uint32_t location, std::uint32_t components,
                const std::vector<float>& floats)
{
    geom->addBuffer(location, packedChannel(floats, components));
}

/// One custom channel to attach: location + components + packed floats.
struct Channel
{
    std::uint32_t             location;
    std::uint32_t             components;
    std::vector<float> floats;
};

/// A triangle whose position (loc0) floats can be extended with custom channels.
GeometryPtr makeTriangleWithChannels(const std::vector<Channel>& channels)
{
    auto geom = GeometryPtr(new Geometry());
    addChannel(geom.get(), 0u, 3u, { 0.0f, 0.0f, 0.0f, 1.0f, 0.0f, 0.0f, 0.0f, 1.0f, 0.0f });
    for (const auto& ch : channels) {
        addChannel(geom.get(), ch.location, ch.components, ch.floats);
    }
    return geom;
}

/// A minimal custom program (positions only in the vertex stage).
ShaderProgramPtr makeProgram()
{
    auto program = ShaderProgramPtr(new ShaderProgram());
    vine::graphics::ShaderStage vs;
    vs.type   = vine::graphics::ShaderStageType::Vertex;
    vs.source = u8"#version 450\n"
                u8"layout(location = 0) in vec3 vine_Vertex;\n"
                u8"void main() { gl_Position = vec4(vine_Vertex, 1.0); }\n";
    program->addStage(vs);
    vine::graphics::ShaderStage fs;
    fs.type   = vine::graphics::ShaderStageType::Fragment;
    fs.source = u8"#version 450\n"
                u8"layout(location = 0) out vec4 outColor;\n"
                u8"void main() { outColor = vec4(1.0, 0.0, 0.0, 1.0); }\n";
    program->addStage(fs);
    return program;
}

vsg::BindVertexBuffers* findBindVertexBuffers(vsg::Node* node)
{
    if (node == nullptr) {
        return nullptr;
    }
    if (auto bvb = node->cast<vsg::BindVertexBuffers>()) {
        return bvb;
    }
    if (auto commands = node->cast<vsg::Commands>()) {
        for (const auto& child : commands->children) {
            if (auto* hit = findBindVertexBuffers(child.get())) {
                return hit;
            }
        }
    }
    if (auto group = node->cast<vsg::Group>()) {
        for (const auto& child : group->children) {
            if (auto* hit = findBindVertexBuffers(child.get())) {
                return hit;
            }
        }
    }
    return nullptr;
}

vsg::Data* boundData(vsg::Node* node, std::size_t binding)
{
    if (node == nullptr) {
        return nullptr;
    }
    // Channels live in one command each, so the binding is resolved through firstBinding and the walk
    // CONTINUES past a command that does not carry it (see SceneBridge::RetainedBinds).
    if (auto* bvb = node->cast<vsg::BindVertexBuffers>()) {
        const std::size_t first = static_cast<std::size_t>(bvb->firstBinding);
        if (binding >= first && binding - first < bvb->arrays.size() && bvb->arrays[binding - first] != nullptr) {
            return bvb->arrays[binding - first]->data.get();
        }
    }
    if (auto commands = node->cast<vsg::Commands>()) {
        for (const auto& child : commands->children) {
            if (auto* hit = boundData(child.get(), binding)) {
                return hit;
            }
        }
    }
    if (auto group = node->cast<vsg::Group>()) {
        for (const auto& child : group->children) {
            if (auto* hit = boundData(child.get(), binding)) {
                return hit;
            }
        }
    }
    return nullptr;
}

/// Number of contiguous vertex bindings the data node fills (binding 0..N-1).
///
/// Each channel lives in its own BindVertexBuffers command (see SceneBridge::RetainedBinds), so "how many
/// channels are bound" is the highest (firstBinding + array count) the node reaches, not one command's list
/// length.
std::size_t boundVertexBindingCount(vsg::Node* node)
{
    if (node == nullptr) {
        return 0u;
    }
    if (auto bvb = node->cast<vsg::BindVertexBuffers>()) {
        return static_cast<std::size_t>(bvb->firstBinding) + bvb->arrays.size();
    }
    std::size_t count = 0u;
    if (auto commands = node->cast<vsg::Commands>()) {
        for (const auto& child : commands->children) {
            count = std::max(count, boundVertexBindingCount(child.get()));
        }
    }
    if (auto group = node->cast<vsg::Group>()) {
        for (const auto& child : group->children) {
            count = std::max(count, boundVertexBindingCount(child.get()));
        }
    }
    return count;
}

vsg::DrawIndexed* findDrawIndexed(vsg::Node* node)
{
    if (node == nullptr) {
        return nullptr;
    }
    if (auto draw = node->cast<vsg::DrawIndexed>()) {
        return draw;
    }
    if (auto commands = node->cast<vsg::Commands>()) {
        for (const auto& child : commands->children) {
            if (auto* hit = findDrawIndexed(child.get())) {
                return hit;
            }
        }
    }
    if (auto group = node->cast<vsg::Group>()) {
        for (const auto& child : group->children) {
            if (auto* hit = findDrawIndexed(child.get())) {
                return hit;
            }
        }
    }
    return nullptr;
}

}  // namespace

/**
 * @brief Custom vertex channels (location >= 3) are forwarded into the data
 * node after the four canonical arrays, each materialised to the typed array
 * its components imply, in ascending location order.
 */
TEST(CustomAttributeTest, CustomChannelsAreForwardedAndTyped)
{
    vine::vsg::SceneBridge bridge;
    bridge.setShaderSet(vsg::createPhongShaderSet());
    auto root     = vsg::Group::create();
    auto material = MaterialPtr(new Material());

    // loc3 = vec3 channel, loc4 = scalar (float) channel, one per vertex.
    auto geom = makeTriangleWithChannels(
        { { 3u, 3u, { 0.1f, 0.2f, 0.3f, 1.1f, 1.2f, 1.3f, 2.1f, 2.2f, 2.3f } },
          { 4u, 1u, { 7.0f, 8.0f, 9.0f } } });

    RenderCommand cmd(geom, material, Mat4d());
    cmd.program = makeProgram();
    std::vector<vsg::ref_ptr<vsg::Node>> created;
    bridge.syncRenderCommands(std::vector<RenderCommand>{ cmd }, root.get(), &created);

    ASSERT_EQ(root->children.size(), 1u);
    ASSERT_NE(findBindVertexBuffers(root.get()), nullptr);
    // 4 canonical bindings (vertex / normal / texcoord / colour) + loc3 + loc4.
    ASSERT_EQ(boundVertexBindingCount(root.get()), 6u);

    auto* v3 = boundData(root.get(), 4u);
    ASSERT_NE(v3, nullptr);
    auto* vec3 = v3->cast<vsg::vec3Array>();
    ASSERT_NE(vec3, nullptr);
    ASSERT_EQ(vec3->size(), 3u);
    EXPECT_FLOAT_EQ((*vec3)[0].x, 0.1f);
    EXPECT_FLOAT_EQ((*vec3)[2].z, 2.3f);

    auto* v4 = boundData(root.get(), 5u);
    ASSERT_NE(v4, nullptr);
    auto* f1 = v4->cast<vsg::floatArray>();
    ASSERT_NE(f1, nullptr);
    ASSERT_EQ(f1->size(), 3u);
    EXPECT_FLOAT_EQ((*f1)[2], 9.0f);

    ASSERT_NE(findDrawIndexed(root.get()), nullptr);
    // One pipeline variant for the custom program + this layout.
    EXPECT_EQ(bridge.pipelineVariantCount(), 1u);
}

/**
 * @brief Geometry bound to the same program with the SAME custom-channel
 * layout share one ShaderSet + one pipeline variant.
 */
TEST(CustomAttributeTest, SameLayoutSharesVariant)
{
    vine::vsg::SceneBridge bridge;
    bridge.setShaderSet(vsg::createPhongShaderSet());
    auto root     = vsg::Group::create();
    auto material = MaterialPtr(new Material());
    auto program  = makeProgram();

    auto g0 = makeTriangleWithChannels({ { 3u, 3u, { 0.0f, 0.0f, 0.0f, 1.0f, 0.0f, 0.0f, 0.0f, 1.0f, 0.0f } } });
    auto g1 = makeTriangleWithChannels({ { 3u, 3u, { 1.0f, 0.0f, 0.0f, 2.0f, 0.0f, 0.0f, 1.0f, 1.0f, 0.0f } } });
    RenderCommand c0(g0, material, Mat4d());
    c0.program = program;
    RenderCommand c1(g1, material, Mat4d());
    c1.program = program;

    std::vector<vsg::ref_ptr<vsg::Node>> created;
    bridge.syncRenderCommands(std::vector<RenderCommand>{ c0, c1 }, root.get(), &created);

    ASSERT_EQ(root->children.size(), 2u);
    EXPECT_EQ(bridge.pipelineVariantCount(), 1u); // same program + same layout
    EXPECT_EQ(bridge.variantReuseCount(), 1u);    // second geometry reused
}

/**
 * @brief Geometry bound to the same program but with a DIFFERENT custom
 * channel layout is its own variant (and ShaderSet) — no cross-layout sharing.
 */
TEST(CustomAttributeTest, DifferentLayoutsAreSeparateVariants)
{
    vine::vsg::SceneBridge bridge;
    bridge.setShaderSet(vsg::createPhongShaderSet());
    auto root     = vsg::Group::create();
    auto material = MaterialPtr(new Material());
    auto program  = makeProgram();

    auto with_channel  = makeTriangleWithChannels({ { 3u, 3u, { 0.0f, 0.0f, 0.0f, 1.0f, 0.0f, 0.0f, 0.0f, 1.0f, 0.0f } } });
    auto without_channel = makeTriangleWithChannels({});
    RenderCommand c0(with_channel, material, Mat4d());
    c0.program = program;
    RenderCommand c1(without_channel, material, Mat4d());
    c1.program = program;

    std::vector<vsg::ref_ptr<vsg::Node>> created;
    bridge.syncRenderCommands(std::vector<RenderCommand>{ c0, c1 }, root.get(), &created);

    ASSERT_EQ(root->children.size(), 2u);
    // Two distinct vertex layouts -> two variants; the no-channel geometry must
    // not reuse the with-channel template.
    EXPECT_EQ(bridge.pipelineVariantCount(), 2u);
    EXPECT_EQ(bridge.variantReuseCount(), 0u);
}

/**
 * @brief The built-in (programless) path tolerates custom channels: the data
 * node still carries them (a program swap later needs no re-upload) while the
 * Phong pipeline binds only the canonical 0/1/2 arrays and the geometry draws.
 */
TEST(CustomAttributeTest, BuiltInPathCarriesChannelsAndDraws)
{
    vine::vsg::SceneBridge bridge;
    bridge.setShaderSet(vsg::createPhongShaderSet());
    auto root     = vsg::Group::create();
    auto material = MaterialPtr(new Material());

    auto geom = makeTriangleWithChannels({ { 3u, 3u, { 0.0f, 0.0f, 0.0f, 1.0f, 0.0f, 0.0f, 0.0f, 1.0f, 0.0f } } });
    RenderCommand cmd(geom, material, Mat4d());

    std::vector<vsg::ref_ptr<vsg::Node>> created;
    bridge.syncRenderCommands(std::vector<RenderCommand>{ cmd }, root.get(), &created);

    ASSERT_EQ(root->children.size(), 1u);
    ASSERT_NE(findDrawIndexed(root.get()), nullptr);
    // Data superset still bound (4 canonical + loc3) even on the built-in path.
    ASSERT_NE(findBindVertexBuffers(root.get()), nullptr);
    EXPECT_EQ(boundVertexBindingCount(root.get()), 5u);
    EXPECT_EQ(bridge.pipelineVariantCount(), 1u);
}

/**
 * @brief A malformed custom channel (vertex count mismatch) is ignored with a
 * diagnostic: the geometry still draws with only the canonical arrays.
 */
TEST(CustomAttributeTest, MalformedCustomChannelIgnored)
{
    vine::vsg::SceneBridge bridge;
    bridge.setShaderSet(vsg::createPhongShaderSet());
    auto root     = vsg::Group::create();
    auto material = MaterialPtr(new Material());

    // loc3 carries only 2 vertices while the mesh has 3 -> skipped.
    auto geom = makeTriangleWithChannels({ { 3u, 3u, { 1.0f, 2.0f, 3.0f, 4.0f, 5.0f, 6.0f } } });
    RenderCommand cmd(geom, material, Mat4d());
    cmd.program = makeProgram();

    std::vector<vsg::ref_ptr<vsg::Node>> created;
    bridge.syncRenderCommands(std::vector<RenderCommand>{ cmd }, root.get(), &created);

    ASSERT_EQ(root->children.size(), 1u); // still drawable
    ASSERT_NE(findBindVertexBuffers(root.get()), nullptr);
    EXPECT_EQ(boundVertexBindingCount(root.get()), 4u); // 4 canonical bindings; not bound: the malformed one
}

/**
 * @brief A user buffer at location 2 is not forwarded on the built-in path
 * (location 2 stays the internal opacity carrier / vine_Color array), so the
 * data node keeps exactly the canonical three arrays.
 */
TEST(CustomAttributeTest, LocationTwoRemainsCanonicalCarrier)
{
    vine::vsg::SceneBridge bridge;
    bridge.setShaderSet(vsg::createPhongShaderSet());
    auto root     = vsg::Group::create();
    auto material = MaterialPtr(new Material());

    auto geom = makeTriangleWithChannels(
        { { 2u, 4u, { 1.0f, 0.0f, 0.0f, 1.0f, 0.0f, 1.0f, 0.0f, 1.0f, 1.0f, 0.0f, 0.0f, 1.0f } } });
    RenderCommand cmd(geom, material, Mat4d());

    std::vector<vsg::ref_ptr<vsg::Node>> created;
    bridge.syncRenderCommands(std::vector<RenderCommand>{ cmd }, root.get(), &created);

    ASSERT_EQ(root->children.size(), 1u);
    ASSERT_NE(findBindVertexBuffers(root.get()), nullptr);
    EXPECT_EQ(boundVertexBindingCount(root.get()), 4u); // 4 canonical bindings; loc2 stays the carrier
    // Built-in path ignores the authored loc2: index 3 is the white carrier.
    auto* c = boundData(root.get(), 3u)->cast<vsg::vec4Array>();
    ASSERT_NE(c, nullptr);
    ASSERT_EQ(c->size(), 3u);
    EXPECT_FLOAT_EQ((*c)[0].x, 1.0f);
    EXPECT_FLOAT_EQ((*c)[0].y, 1.0f);
}

/**
 * @brief On the CUSTOM-program path an authored loc2 colour (vec4 per vertex)
 * is bound verbatim as vine_Color: the program owns opacity (no carrier
 * rewrite), so the authored colour and alpha reach the shader unchanged.
 */
TEST(CustomAttributeTest, CustomLoc2ColorIsBoundOnProgramPath)
{
    vine::vsg::SceneBridge bridge;
    bridge.setShaderSet(vsg::createPhongShaderSet());
    auto root     = vsg::Group::create();
    auto material = MaterialPtr(new Material());

    auto geom = makeTriangleWithChannels(
        { { 2u, 4u, { 1.0f, 0.0f, 0.0f, 0.5f,
                      0.0f, 1.0f, 0.0f, 0.6f,
                      0.0f, 0.0f, 1.0f, 0.7f } } });
    RenderCommand cmd(geom, material, Mat4d());
    cmd.program = makeProgram();

    std::vector<vsg::ref_ptr<vsg::Node>> created;
    bridge.syncRenderCommands(std::vector<RenderCommand>{ cmd }, root.get(), &created);

    ASSERT_EQ(root->children.size(), 1u);
    ASSERT_NE(findBindVertexBuffers(root.get()), nullptr);
    EXPECT_EQ(boundVertexBindingCount(root.get()), 4u); // 4 canonical; loc2 is vine_Color, not an extra
    auto* c = boundData(root.get(), 3u)->cast<vsg::vec4Array>();
    ASSERT_NE(c, nullptr);
    ASSERT_EQ(c->size(), 3u);
    EXPECT_FLOAT_EQ((*c)[0].x, 1.0f);
    EXPECT_FLOAT_EQ((*c)[0].a, 0.5f); // authored alpha preserved (no carrier rewrite)
    EXPECT_FLOAT_EQ((*c)[1].y, 1.0f);
    EXPECT_FLOAT_EQ((*c)[1].a, 0.6f);
    EXPECT_FLOAT_EQ((*c)[2].z, 1.0f);
    EXPECT_FLOAT_EQ((*c)[2].a, 0.7f);
}

/**
 * @brief One program used with several distinct custom-channel layouts compiles
 * its GLSL stages ONCE (L1a) and only assembles per-layout ShaderSets (L1b):
 * the stage-compile counter stays 1 while the layouts stay separate variants.
 */
TEST(CustomAttributeTest, SameProgramMultipleLayoutsCompileStagesOnce)
{
    vine::vsg::SceneBridge bridge;
    bridge.setShaderSet(vsg::createPhongShaderSet());
    auto root     = vsg::Group::create();
    auto material = MaterialPtr(new Material());
    auto program  = makeProgram();

    auto g_loc3 = makeTriangleWithChannels({ { 3u, 3u, { 1.0f, 0.0f, 0.0f, 1.0f, 0.0f, 0.0f, 0.0f, 1.0f, 0.0f } } });
    auto g_loc4 = makeTriangleWithChannels({ { 4u, 1u, { 1.0f, 2.0f, 3.0f } } });
    auto g_plain = makeTriangleWithChannels({});
    RenderCommand a(g_loc3, material, Mat4d());
    a.program = program;
    RenderCommand b(g_loc4, material, Mat4d());
    b.program = program;
    RenderCommand c(g_plain, material, Mat4d());
    c.program = program;

    std::vector<vsg::ref_ptr<vsg::Node>> created;
    bridge.syncRenderCommands(std::vector<RenderCommand>{ a, b, c }, root.get(), &created);

    ASSERT_EQ(root->children.size(), 3u);
    // Three distinct layouts of ONE program: three pipeline variants, but a
    // single glslang stage compile shared by all of them.
    EXPECT_EQ(bridge.pipelineVariantCount(), 3u);
    EXPECT_EQ(bridge.programStageCompileCount(), 1u);
}

/**
 * @brief Adding a custom channel to a live geometry rebuilds the state
 * wrapper for the new per-layout variant.
 *
 * The data/state decoupling rebuilds only the data node on a pure data
 * revision bump; the state wrapper (per-layout shader set / pipeline) must
 * follow when the forwarded channel SET changed, or a live-added channel would
 * stay unbound (and a removed one leave a stale binding) until a program /
 * material / render-state change happened to trigger a rebuild.
 */
TEST(CustomAttributeTest, LiveChannelAddForcesStateRebuild)
{
    vine::vsg::SceneBridge bridge;
    bridge.setShaderSet(vsg::createPhongShaderSet());
    auto root     = vsg::Group::create();
    auto material = MaterialPtr(new Material());
    auto program  = makeProgram();

    auto geom = makeTriangleWithChannels({});
    RenderCommand cmd(geom, material, Mat4d());
    cmd.program = program;

    std::vector<vsg::ref_ptr<vsg::Node>> created;
    bridge.syncRenderCommands(std::vector<RenderCommand>{ cmd }, root.get(), &created);
    ASSERT_EQ(bridge.pipelineVariantCount(), 1u);

    // Live-append a custom channel (loc3), then announce it: the retained state wrapper must be rebuilt
    // for the new layout even though the program / material / render state are all unchanged.
    addChannel(geom.get(), 3u, 3u, { 1.0f, 0.0f, 0.0f, 1.0f, 0.0f, 0.0f, 0.0f, 1.0f, 0.0f });
    geom->setRevision(geom->revision() + 1u);
    std::vector<vsg::ref_ptr<vsg::Node>> created2;
    bridge.syncRenderCommands(std::vector<RenderCommand>{ cmd }, root.get(), &created2);
    EXPECT_EQ(bridge.pipelineVariantCount(), 2u);

    // A same-layout data edit (only vertex floats changed) must NOT add yet
    // another variant: the state wrapper is reused.
    addChannel(geom.get(), 0u, 3u, { 0.0f, 0.0f, 0.0f, 2.0f, 0.0f, 0.0f, 0.0f, 2.0f, 0.0f });
    geom->setRevision(geom->revision() + 1u);
    std::vector<vsg::ref_ptr<vsg::Node>> created3;
    bridge.syncRenderCommands(std::vector<RenderCommand>{ cmd }, root.get(), &created3);
    EXPECT_EQ(bridge.pipelineVariantCount(), 2u);
}
