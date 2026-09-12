/**
 * @brief What a DATA rebuild must and must not redo.
 *
 * A data rebuild re-materialises the whole data node, but only some of its channels come from the model.
 * The white colour carrier, the zero UV array a mesh without UVs binds and the normals DERIVED when the
 * geometry authors none are BUILT, and each costs a pass over the vertices plus an allocation. Building
 * them again for an edit that changed none of their inputs is the waste these tests pin: the bridge
 * remembers them per retained item, keyed by exactly the inputs they were derived from.
 *
 * Asserted by the ADDRESS of the bound array, because that is what distinguishes "reused what it already
 * built" from "built an identical copy again" — the two draw the same picture, so no pixel test can tell
 * them apart, and the second one is the cost being avoided.
 */

#include <gtest/gtest.h>

#include <vine/graphics/Geometry.hpp>
#include <vine/graphics/Material.hpp>
#include <vine/graphics/RenderCommand.hpp>
#include <vine/vsg/SceneBridge.hpp>

#include <vsg/commands/BindIndexBuffer.h>
#include <vsg/commands/BindVertexBuffers.h>
#include <vsg/commands/Commands.h>
#include <vsg/commands/DrawIndexed.h>
#include <vsg/io/Options.h>
#include <vsg/nodes/Group.h>
#include <vsg/utils/ShaderSet.h>

#include <cstddef>
#include <vector>

using namespace vine::graphics;
using vine::math::Mat4d;

namespace
{

/// Binding indices of the canonical arrays, in the order the bridge binds them.
constexpr std::size_t kBindingPositions = 0u;
constexpr std::size_t kBindingNormals   = 1u;
constexpr std::size_t kBindingTexcoords = 2u;
constexpr std::size_t kBindingColors    = 3u;

/// Builds @p floats as a buffer that owns them.
vine::intrusive_ptr<const vine::Buffer<float>> packed(std::vector<float> floats)
{
    return vine::intrusive_ptr<const vine::Buffer<float>>(new vine::Buffer<float>(std::move(floats)));
}

/// Unit triangle at @p x, positions only: no normals, no UVs, no colour, so every optional channel is BUILT.
GeometryPtr triangle(float x)
{
    auto geometry = GeometryPtr(new Geometry());
    geometry->setPositions(packed({ x, 0.0f, 0.0f, x, 1.0f, 0.0f, x, 0.0f, 1.0f }));
    return geometry;
}

/// Finds the vertex Data bound at binding @p binding under a retained subtree.
vsg::Data* findBoundData(vsg::Node* node, std::size_t binding)
{
    if (node == nullptr) {
        return nullptr;
    }
    if (auto bvb = node->cast<vsg::BindVertexBuffers>()) {
        // A channel per command (see SceneBridge::RetainedBinds) means the binding has to be resolved
        // through firstBinding; a command that does not carry it is not the answer, so the walk continues.
        const std::size_t first = static_cast<std::size_t>(bvb->firstBinding);
        if (binding >= first && binding - first < bvb->arrays.size()) {
            const auto& info = bvb->arrays[binding - first];
            return info != nullptr ? info->data.get() : nullptr;
        }
    }
    if (auto commands = node->cast<vsg::Commands>()) {
        for (const auto& child : commands->children) {
            if (auto* hit = findBoundData(child.get(), binding)) {
                return hit;
            }
        }
    }
    if (auto group = node->cast<vsg::Group>()) {
        for (const auto& child : group->children) {
            if (auto* hit = findBoundData(child.get(), binding)) {
                return hit;
            }
        }
    }
    return nullptr;
}


/// Builds @p values as an index buffer that owns them.
vine::intrusive_ptr<const vine::Buffer<std::uint32_t>> packedIndices(std::vector<std::uint32_t> values)
{
    return vine::intrusive_ptr<const vine::Buffer<std::uint32_t>>(
        new vine::Buffer<std::uint32_t>(std::move(values)));
}

/// The triangle of triangle() as an INDEXED mesh (three indices).
GeometryPtr indexedTriangle(float x)
{
    auto geometry = triangle(x);
    geometry->setIndices(packedIndices({ 0u, 1u, 2u }));
    return geometry;
}


/// Finds the retained vertex-data node (the Commands holding the vertex/index binds) under a subtree.
vsg::Commands* findDataNode(vsg::Node* node)
{
    if (node == nullptr) {
        return nullptr;
    }
    if (auto commands = node->cast<vsg::Commands>()) {
        for (const auto& child : commands->children) {
            if (child != nullptr && child->cast<vsg::BindVertexBuffers>() != nullptr) {
                return commands;
            }
        }
    }
    if (auto group = node->cast<vsg::Group>()) {
        for (const auto& child : group->children) {
            if (auto* hit = findDataNode(child.get())) {
                return hit;
            }
        }
    }
    return nullptr;
}

/// Finds the index bind command under a retained subtree.
vsg::BindIndexBuffer* findBindIndexBuffer(vsg::Node* node)
{
    if (node == nullptr) {
        return nullptr;
    }
    if (auto bind = node->cast<vsg::BindIndexBuffer>()) {
        return bind;
    }
    if (auto commands = node->cast<vsg::Commands>()) {
        for (const auto& child : commands->children) {
            if (auto* hit = findBindIndexBuffer(child.get())) {
                return hit;
            }
        }
    }
    if (auto group = node->cast<vsg::Group>()) {
        for (const auto& child : group->children) {
            if (auto* hit = findBindIndexBuffer(child.get())) {
                return hit;
            }
        }
    }
    return nullptr;
}

/// Finds the DrawIndexed command under a retained subtree.
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

/// The Data the retained index bind copies from, or null when there is none.
vsg::Data* indexDataOf(vsg::Node* node)
{
    auto* bind = findBindIndexBuffer(node);
    return bind != nullptr && bind->indices != nullptr ? bind->indices->data.get() : nullptr;
}

/// Drives one sync of @p commands through a bridge and returns the retained root.
void sync(vine::vsg::SceneBridge& bridge, vsg::Group& root, const std::vector<RenderCommand>& commands)
{
    bridge.syncRenderCommands(commands, &root, nullptr);
}

}  // namespace

/**
 * @brief An edit that changes none of the derived channels' inputs does not rebuild them.
 *
 * The scenario is the everyday one: a custom channel is swapped (or any other edit that leaves the
 * positions and indices alone), which bumps the geometry's revision and therefore rebuilds the data node.
 * The three BUILT channels must come back as the very same arrays — the geometry's vertices did not change,
 * so neither did their derived normals, their white carrier or their zero UVs.
 */
TEST(SceneBridgeDataRebuildTest, DerivedChannelsSurviveAnUnrelatedRebuild)
{
    vine::vsg::SceneBridge bridge;
    bridge.setShaderSet(vsg::createPhongShaderSet());
    auto root     = vsg::Group::create();
    auto geometry = triangle(0.0f);
    auto material = MaterialPtr(new Material());

    geometry->addBuffer(5u, AttributeBuffer::packed({ 1.0f, 0.0f, 0.0f }, 3u));
    std::vector<RenderCommand> commands;
    commands.emplace_back(geometry, material, Mat4d());
    sync(bridge, *root, commands);

    auto* normals_before   = findBoundData(root.get(), kBindingNormals);
    auto* texcoord_before  = findBoundData(root.get(), kBindingTexcoords);
    auto* color_before     = findBoundData(root.get(), kBindingColors);
    ASSERT_NE(normals_before, nullptr) << "a mesh without normals binds the derived ones";
    ASSERT_NE(texcoord_before, nullptr) << "a mesh without UVs binds the zero array";
    ASSERT_NE(color_before, nullptr) << "a mesh without colour binds the white carrier";

    // Swap the custom channel (positions and indices untouched) and announce the change.
    geometry->addBuffer(5u, AttributeBuffer::packed({ 0.0f, 1.0f, 0.0f, 0.0f, 0.0f, 1.0f, 0.0f, 0.0f, 0.0f }, 3u));
    geometry->setRevision(geometry->revision() + 1u);
    sync(bridge, *root, commands);

    EXPECT_EQ(findBoundData(root.get(), kBindingNormals), normals_before)
        << "the derived normals were computed from positions that did not change";
    EXPECT_EQ(findBoundData(root.get(), kBindingTexcoords), texcoord_before)
        << "the zero UV array is a function of the vertex count, which did not change";
    EXPECT_EQ(findBoundData(root.get(), kBindingColors), color_before)
        << "the white carrier is a function of the vertex count, which did not change";
}

/**
 * @brief New positions DO rebuild the derived normals (the cache is keyed on them).
 *
 * The control for the test above: reusing derived data is only correct while its inputs are the same, so
 * replacing the positions buffer must produce a freshly derived array — even when the new buffer has the
 * same vertex count.
 */
TEST(SceneBridgeDataRebuildTest, DerivedNormalsFollowThePositionsTheyWereComputedFrom)
{
    vine::vsg::SceneBridge bridge;
    bridge.setShaderSet(vsg::createPhongShaderSet());
    auto root     = vsg::Group::create();
    auto geometry = triangle(0.0f);
    auto material = MaterialPtr(new Material());

    std::vector<RenderCommand> commands;
    commands.emplace_back(geometry, material, Mat4d());
    sync(bridge, *root, commands);

    auto* normals_before = findBoundData(root.get(), kBindingNormals);
    ASSERT_NE(normals_before, nullptr);

    // Same shape, different bytes: a NEW buffer object, which is what the key compares.
    geometry->setPositions(packed({ 0.0f, 0.0f, 0.0f, 1.0f, 0.0f, 0.0f, 0.0f, 1.0f, 0.0f }));
    geometry->setRevision(geometry->revision() + 1u);
    sync(bridge, *root, commands);

    EXPECT_NE(findBoundData(root.get(), kBindingNormals), normals_before)
        << "normals derived from the previous positions must not be reused for new ones";
}

/**
 * @brief A vertex-count change invalidates the size-dependent channels.
 *
 * The white carrier and the zero UV array are sized by the vertex count, so a mesh that grew must not bind
 * the arrays built for the previous size — that would bind too few vertices (or too many) and read the
 * wrong bytes. The check is the address again: a count change must produce new arrays.
 */
TEST(SceneBridgeDataRebuildTest, FallbackChannelsFollowTheVertexCount)
{
    vine::vsg::SceneBridge bridge;
    bridge.setShaderSet(vsg::createPhongShaderSet());
    auto root     = vsg::Group::create();
    auto geometry = triangle(0.0f);
    auto material = MaterialPtr(new Material());

    std::vector<RenderCommand> commands;
    commands.emplace_back(geometry, material, Mat4d());
    sync(bridge, *root, commands);

    auto* texcoord_before  = findBoundData(root.get(), kBindingTexcoords);
    auto* color_before     = findBoundData(root.get(), kBindingColors);
    auto* data_node_before = findDataNode(root.get());
    ASSERT_NE(texcoord_before, nullptr);
    ASSERT_NE(color_before, nullptr);
    ASSERT_NE(data_node_before, nullptr);

    // Grow the mesh: four vertices instead of three.
    geometry->setPositions(packed({ 0.0f, 0.0f, 0.0f, 1.0f, 0.0f, 0.0f, 1.0f, 1.0f, 0.0f, 0.0f, 1.0f, 0.0f }));
    geometry->setRevision(geometry->revision() + 1u);
    sync(bridge, *root, commands);

    EXPECT_NE(findDataNode(root.get()), data_node_before) << "a shape change rebuilds the node";
    EXPECT_NE(findBoundData(root.get(), kBindingTexcoords), texcoord_before)
        << "the zero array was sized for the previous vertex count";
    EXPECT_NE(findBoundData(root.get(), kBindingColors), color_before)
        << "the white carrier was sized for the previous vertex count";
}

/**
 * @brief An index-only edit replaces the index stream and leaves the vertex channels alone.
 *
 * The index stream lives in its OWN bind command, so its BufferInfo is the only one vsg re-creates and
 * copies: a mesh whose indices were refilled must not re-upload every vertex channel (52-56 bytes per
 * vertex) for that. The assertions are the addresses again: the vertex array must be the very same object,
 * and the retained subtree must be re-compiled IN PLACE rather than replaced — a new transform means the
 * whole node (and therefore every channel) was rebuilt.
 */
TEST(SceneBridgeDataRebuildTest, IndexOnlyEditReplacesTheIndexStreamAlone)
{
    vine::vsg::SceneBridge bridge;
    bridge.setShaderSet(vsg::createPhongShaderSet());
    auto root     = vsg::Group::create();
    auto geometry = indexedTriangle(0.0f);
    auto material = MaterialPtr(new Material());

    std::vector<RenderCommand>         commands;
    std::vector<vsg::ref_ptr<vsg::Node>> created;
    commands.emplace_back(geometry, material, Mat4d());
    bridge.syncRenderCommands(commands, root.get(), &created);
    ASSERT_EQ(created.size(), 1u) << "the first sync builds the subtree";
    auto* data_node_before = findDataNode(root.get());
    auto* positions_before = findBoundData(root.get(), kBindingPositions);
    auto* indices_before   = indexDataOf(root.get());
    ASSERT_NE(data_node_before, nullptr);
    ASSERT_NE(positions_before, nullptr);
    ASSERT_NE(indices_before, nullptr);

    // An index-only edit: the same index count, new bytes (a new buffer object), announced by a revision
    // bump — which is what a refilled index stream looks like from here.
    geometry->setIndices(packedIndices({ 0u, 2u, 1u }));
    geometry->setRevision(geometry->revision() + 1u);
    created.clear();
    bridge.syncRenderCommands(commands, root.get(), &created);

    EXPECT_EQ(findBoundData(root.get(), kBindingPositions), positions_before)
        << "a vertex channel that did not change must not be re-materialised";
    EXPECT_NE(indexDataOf(root.get()), indices_before) << "the retained bind must read the new indices";
    ASSERT_EQ(created.size(), 1u) << "the swapped index buffer still needs this frame's compile pass";
    EXPECT_EQ(findDataNode(root.get()), data_node_before)
        << "the data node must be kept and re-compiled in place; a new one means every channel was "
           "re-materialised (and re-uploaded) with the indices";
}

/**
 * @brief A vertex edit refreshes that channel's bind and leaves the others alone.
 *
 * Each channel has its own command, so a positions edit re-materialises (and re-uploads) the POSITIONS
 * only: the normals that were derived from them are refreshed too (they are what the positions fold into),
 * while the texcoord and colour arrays — which did not change and do not depend on the positions — keep
 * their very same objects. The data node itself is kept and re-compiled in place.
 */
TEST(SceneBridgeDataRebuildTest, AVertexEditRefreshesThatChannelInPlace)
{
    vine::vsg::SceneBridge bridge;
    bridge.setShaderSet(vsg::createPhongShaderSet());
    auto root     = vsg::Group::create();
    auto geometry = triangle(0.0f);
    auto material = MaterialPtr(new Material());

    std::vector<RenderCommand>           commands;
    std::vector<vsg::ref_ptr<vsg::Node>> created;
    commands.emplace_back(geometry, material, Mat4d());
    bridge.syncRenderCommands(commands, root.get(), &created);
    auto* data_node_before = findDataNode(root.get());
    auto* positions_before = findBoundData(root.get(), kBindingPositions);
    auto* texcoord_before  = findBoundData(root.get(), kBindingTexcoords);
    auto* color_before     = findBoundData(root.get(), kBindingColors);
    ASSERT_NE(data_node_before, nullptr);
    ASSERT_NE(positions_before, nullptr);
    ASSERT_NE(texcoord_before, nullptr);
    ASSERT_NE(color_before, nullptr);

    // Same vertex count, new positions: the shape is unchanged, so this is an in-place refresh.
    geometry->setPositions(packed({ 0.0f, 0.0f, 0.0f, 2.0f, 0.0f, 0.0f, 0.0f, 2.0f, 0.0f }));
    geometry->setRevision(geometry->revision() + 1u);
    created.clear();
    bridge.syncRenderCommands(commands, root.get(), &created);

    EXPECT_NE(findBoundData(root.get(), kBindingPositions), positions_before)
        << "the changed channel must be re-materialised";
    EXPECT_EQ(findBoundData(root.get(), kBindingTexcoords), texcoord_before)
        << "a channel that did not change must not be re-uploaded with it";
    EXPECT_EQ(findBoundData(root.get(), kBindingColors), color_before)
        << "a channel that did not change must not be re-uploaded with it";
    EXPECT_EQ(findDataNode(root.get()), data_node_before)
        << "the node is kept and re-compiled in place";
    ASSERT_EQ(created.size(), 1u) << "the refreshed binds still need this frame's compile pass";
}

/**
 * @brief A vertex edit AND an index edit in one revision refresh both streams in place.
 *
 * Both streams are refreshed through their own commands, so the mesh's other channels are not re-uploaded
 * with them — the case that used to force a whole-mesh rebuild.
 */
TEST(SceneBridgeDataRebuildTest, AVertexAndIndexEditRefreshesBothInPlace)
{
    vine::vsg::SceneBridge bridge;
    bridge.setShaderSet(vsg::createPhongShaderSet());
    auto root     = vsg::Group::create();
    auto geometry = indexedTriangle(0.0f);
    auto material = MaterialPtr(new Material());

    std::vector<RenderCommand>           commands;
    std::vector<vsg::ref_ptr<vsg::Node>> created;
    commands.emplace_back(geometry, material, Mat4d());
    bridge.syncRenderCommands(commands, root.get(), &created);
    auto* data_node_before = findDataNode(root.get());
    auto* positions_before = findBoundData(root.get(), kBindingPositions);
    auto* texcoord_before  = findBoundData(root.get(), kBindingTexcoords);
    auto* indices_before   = indexDataOf(root.get());
    ASSERT_NE(data_node_before, nullptr);
    ASSERT_NE(positions_before, nullptr);
    ASSERT_NE(indices_before, nullptr);

    geometry->setPositions(packed({ 0.0f, 0.0f, 0.0f, 2.0f, 0.0f, 0.0f, 0.0f, 2.0f, 0.0f }));
    geometry->setIndices(packedIndices({ 2u, 1u, 0u }));
    geometry->setRevision(geometry->revision() + 1u);
    created.clear();
    bridge.syncRenderCommands(commands, root.get(), &created);

    EXPECT_NE(findBoundData(root.get(), kBindingPositions), positions_before) << "new vertices must reach the GPU";
    EXPECT_NE(indexDataOf(root.get()), indices_before) << "new indices must reach the GPU";
    EXPECT_EQ(findBoundData(root.get(), kBindingTexcoords), texcoord_before)
        << "the untouched channels must not be re-uploaded";
    EXPECT_EQ(findDataNode(root.get()), data_node_before) << "nothing about the shape changed";
}

/**
 * @brief A revision the per-stream identities do NOT explain still rebuilds the data node.
 *
 * The refresh path is only an optimisation over "re-read everything", so it may only be taken when the
 * snapshots say WHICH stream changed: a revision bump that accounts for nothing (a buffer mutated in place
 * without bumping its own revision) must fall back to the rebuild, which re-reads the model. This is the
 * guard the existing DiagnosticsTest.RejectedGeometryIsReportedOncePerRevision caught when the refresh path
 * was naive about it.
 */
TEST(SceneBridgeDataRebuildTest, AnUnexplainedRevisionStillRebuildsTheNode)
{
    vine::vsg::SceneBridge bridge;
    bridge.setShaderSet(vsg::createPhongShaderSet());
    auto root     = vsg::Group::create();
    auto geometry = triangle(0.0f);
    auto material = MaterialPtr(new Material());

    std::vector<RenderCommand>           commands;
    std::vector<vsg::ref_ptr<vsg::Node>> created;
    commands.emplace_back(geometry, material, Mat4d());
    bridge.syncRenderCommands(commands, root.get(), &created);
    auto* data_node_before = findDataNode(root.get());
    ASSERT_NE(data_node_before, nullptr);

    // The geometry announces a revision, but no stream's identity changed: the refresh path cannot explain
    // it, so the node is rebuilt from the model instead of the frame being treated as a no-op.
    geometry->setRevision(geometry->revision() + 1u);
    created.clear();
    bridge.syncRenderCommands(commands, root.get(), &created);

    EXPECT_NE(findDataNode(root.get()), data_node_before)
        << "an unexplained revision must be answered by re-reading the model, not by doing nothing";
}

/**
 * @brief An out-of-range index edit is REJECTED, not swapped in.
 *
 * The index fast path must not be a way around the builder's bounds check: an index beyond the vertex count
 * reads out of bounds on the GPU, and the rejection (reported once per revision) is the caller's only signal
 * that the mesh is not drawable. So a stream that fails the check takes the rebuild, which rejects it.
 */
TEST(SceneBridgeDataRebuildTest, AnOutOfRangeIndexSwapIsRejectedInsteadOfUploaded)
{
    vine::vsg::SceneBridge bridge;
    bridge.setShaderSet(vsg::createPhongShaderSet());
    auto root     = vsg::Group::create();
    auto geometry = indexedTriangle(0.0f);
    auto material = MaterialPtr(new Material());

    std::vector<RenderCommand>           commands;
    std::vector<vsg::ref_ptr<vsg::Node>> created;
    commands.emplace_back(geometry, material, Mat4d());
    bridge.syncRenderCommands(commands, root.get(), &created);
    ASSERT_EQ(root->children.size(), 1u) << "the drawable mesh is retained";

    // Index 9 does not exist in a three-vertex mesh: same count, so only the bounds check can tell.
    geometry->setIndices(packedIndices({ 0u, 1u, 9u }));
    geometry->setRevision(geometry->revision() + 1u);
    created.clear();
    bridge.syncRenderCommands(commands, root.get(), &created);

    EXPECT_TRUE(root->children.empty())
        << "an out-of-range index stream must be rejected, not swapped into the retained bind";
}

/**
 * @brief Authored normals and UVs each refresh on their own.
 *
 * The canonical channels are independent streams, so an edit to one must leave the others' arrays — and
 * their uploads — untouched, whatever they are (here: authored, not derived).
 */
TEST(SceneBridgeDataRebuildTest, AuthoredChannelEditsRefreshOnlyThatChannel)
{
    vine::vsg::SceneBridge bridge;
    bridge.setShaderSet(vsg::createPhongShaderSet());
    auto root     = vsg::Group::create();
    auto geometry = triangle(0.0f);
    auto material = MaterialPtr(new Material());

    geometry->setNormals(packed({ 0.0f, 0.0f, 1.0f, 0.0f, 0.0f, 1.0f, 0.0f, 0.0f, 1.0f }));
    geometry->setTexcoords(packed({ 0.0f, 0.0f, 1.0f, 0.0f, 0.0f, 1.0f }));

    std::vector<RenderCommand>           commands;
    std::vector<vsg::ref_ptr<vsg::Node>> created;
    commands.emplace_back(geometry, material, Mat4d());
    bridge.syncRenderCommands(commands, root.get(), &created);
    auto* data_node_before = findDataNode(root.get());
    auto* positions_before = findBoundData(root.get(), kBindingPositions);
    auto* normals_before   = findBoundData(root.get(), kBindingNormals);
    auto* texcoord_before  = findBoundData(root.get(), kBindingTexcoords);
    ASSERT_NE(data_node_before, nullptr);
    ASSERT_NE(normals_before, nullptr);
    ASSERT_NE(texcoord_before, nullptr);

    // UVs only.
    geometry->setTexcoords(packed({ 0.0f, 1.0f, 1.0f, 1.0f, 0.5f, 0.0f }));
    geometry->setRevision(geometry->revision() + 1u);
    bridge.syncRenderCommands(commands, root.get(), &created);

    EXPECT_NE(findBoundData(root.get(), kBindingTexcoords), texcoord_before) << "the new UVs must reach the GPU";
    EXPECT_EQ(findBoundData(root.get(), kBindingPositions), positions_before) << "positions were not touched";
    EXPECT_EQ(findBoundData(root.get(), kBindingNormals), normals_before) << "authored normals were not touched";

    // Authored normals only.
    geometry->setNormals(packed({ 0.0f, 1.0f, 0.0f, 0.0f, 1.0f, 0.0f, 0.0f, 1.0f, 0.0f }));
    geometry->setRevision(geometry->revision() + 1u);
    bridge.syncRenderCommands(commands, root.get(), &created);

    EXPECT_NE(findBoundData(root.get(), kBindingNormals), normals_before) << "the new normals must reach the GPU";
    EXPECT_EQ(findDataNode(root.get()), data_node_before) << "no shape changed";
}
