/**
 * @brief Channels that read a SEGMENT of one buffer (the arena case, P7).
 *
 * "One big buffer, one segment per geometry" is what `AttributeChannel::offset` / `scalarCount` express: a
 * geometry points a channel at its own scalars inside a buffer several geometries share, and nothing is
 * repacked. These tests pin the two things that make it work:
 *
 *  1. the aliased array is built FROM THE SEGMENT — its element 0 is the segment's first vertex, in the
 *     model's own memory, so a slice never silently draws the arena from its start (the failure mode of an
 *     offset the backend forgot to apply is a quiet one: the previous geometry's vertices);
 *  2. the SEGMENT IS PART OF THE STREAM'S IDENTITY — the shared-bind cache and the derived-channel cache both
 *     key on it, so two segments of one buffer never share a bind, and moving a channel to another segment
 *     re-derives what depended on it. The INDEX stream is the intentional exception: the bind aliases the
 *     whole buffer and the DRAW states the span, which is what lets an index arena share one index upload.
 */

#include <gtest/gtest.h>

#include <vine/Buffer.hpp>
#include <vine/graphics/Geometry.hpp>
#include <vine/graphics/Material.hpp>
#include <vine/graphics/RenderCommand.hpp>
#include <vine/graphics/ShaderProgram.hpp>
#include <vine/vsg/SceneBridge.hpp>
#include <vine/vsg/VsgMeshResourceCache.hpp>

#include <vsg/commands/BindIndexBuffer.h>
#include <vsg/commands/BindVertexBuffers.h>
#include <vsg/commands/Commands.h>
#include <vsg/commands/DrawIndexed.h>
#include <vsg/core/Array.h>
#include <vsg/core/Data.h>
#include <vsg/maths/vec3.h>
#include <vsg/nodes/Group.h>
#include <vsg/nodes/StateGroup.h>
#include <vsg/utils/ShaderSet.h>

#include <cstddef>
#include <cstdint>
#include <utility>
#include <vector>

using namespace vine::graphics;
using vine::math::Mat4d;

namespace
{

/// Binding indices of the canonical arrays, in the order the bridge binds them.
constexpr std::size_t kBindingPositions = 0u;
constexpr std::size_t kBindingNormals   = 1u;

/// Two unit triangles, ten units apart on y: an arena whose segments are trivially distinguishable.
/// Segment 0 is the tri at y = 0 (its normal is +Z), segment 1 the tri at y = 10 standing in the yz-ish
/// plane (its normal is +Y) — the contents a test reads to see WHICH segment a bind points at.
std::vector<float> arenaFloats()
{
    return {
        0.0f, 0.0f, 0.0f, 1.0f, 0.0f, 0.0f, 0.0f, 1.0f, 0.0f,      // segment 0 (xy plane at y = 0)
        0.0f, 10.0f, 0.0f, 1.0f, 10.0f, 0.0f, 0.0f, 10.0f, -1.0f,  // segment 1 (its normal is +Y)
    };
}

/// Builds @p floats as a buffer the test keeps reading (the arena).
vine::intrusive_ptr<vine::Buffer<float>> arena(std::vector<float> floats = arenaFloats())
{
    return vine::intrusive_ptr<vine::Buffer<float>>(new vine::Buffer<float>(std::move(floats)));
}

/// Builds @p values as an index buffer that owns them.
vine::intrusive_ptr<const vine::Buffer<std::uint32_t>> packedIndices(std::vector<std::uint32_t> values)
{
    return vine::intrusive_ptr<const vine::Buffer<std::uint32_t>>(
        new vine::Buffer<std::uint32_t>(std::move(values)));
}

/// A triangle reading the three vertices of @p buffer that start at vertex @p first_vertex.
GeometryPtr segment(vine::intrusive_ptr<const vine::Buffer<float>> buffer, std::size_t first_vertex)
{
    auto geometry = GeometryPtr(new Geometry());
    geometry->addBuffer(0u, AttributeChannel::slice(std::move(buffer), 3u, first_vertex, 3u));
    return geometry;
}

/// Finds the vertex bind command carrying binding @p binding under a retained subtree.
vsg::BindVertexBuffers* findBoundBind(vsg::Node* node, std::size_t binding)
{
    if (node == nullptr) {
        return nullptr;
    }
    if (auto bind = node->cast<vsg::BindVertexBuffers>()) {
        const std::size_t first = static_cast<std::size_t>(bind->firstBinding);
        if (binding >= first && binding - first < bind->arrays.size()) {
            return bind;
        }
    }
    if (auto commands = node->cast<vsg::Commands>()) {
        for (const auto& child : commands->children) {
            if (auto* hit = findBoundBind(child.get(), binding)) {
                return hit;
            }
        }
    }
    if (auto group = node->cast<vsg::Group>()) {
        for (const auto& child : group->children) {
            if (auto* hit = findBoundBind(child.get(), binding)) {
                return hit;
            }
        }
    }
    if (auto state_group = node->cast<vsg::StateGroup>()) {
        for (const auto& command : state_group->stateCommands) {
            if (auto* hit = findBoundBind(command.get(), binding)) {
                return hit;
            }
        }
    }
    return nullptr;
}

/// The vertex Data bound at binding @p binding under a retained subtree.
vsg::Data* findBoundData(vsg::Node* node, std::size_t binding)
{
    const auto* bind = findBoundBind(node, binding);
    if (bind == nullptr) {
        return nullptr;
    }
    const std::size_t first = static_cast<std::size_t>(bind->firstBinding);
    const auto&       info  = bind->arrays[binding - first];
    return info != nullptr ? info->data.get() : nullptr;
}

/// The vec3 array bound at binding @p binding, or null when it is not one.
vsg::vec3Array* findBoundVec3(vsg::Node* node, std::size_t binding)
{
    auto* data = findBoundData(node, binding);
    return data != nullptr ? data->cast<vsg::vec3Array>() : nullptr;
}

/// Finds the index bind command under a retained subtree.
vsg::BindIndexBuffer* findBindIndex(vsg::Node* node)
{
    if (node == nullptr) {
        return nullptr;
    }
    if (auto bind = node->cast<vsg::BindIndexBuffer>()) {
        return bind;
    }
    if (auto commands = node->cast<vsg::Commands>()) {
        for (const auto& child : commands->children) {
            if (auto* hit = findBindIndex(child.get())) {
                return hit;
            }
        }
    }
    if (auto group = node->cast<vsg::Group>()) {
        for (const auto& child : group->children) {
            if (auto* hit = findBindIndex(child.get())) {
                return hit;
            }
        }
    }
    if (auto state_group = node->cast<vsg::StateGroup>()) {
        for (const auto& command : state_group->stateCommands) {
            if (auto* hit = findBindIndex(command.get())) {
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
    if (auto state_group = node->cast<vsg::StateGroup>()) {
        for (const auto& command : state_group->stateCommands) {
            if (auto* hit = findDrawIndexed(command.get())) {
                return hit;
            }
        }
    }
    return nullptr;
}

/**
 * @brief Draws @p commands once through @p bridge.
 *
 * A sync publishes only the drawables it was given into @p root, so a bind found afterwards is that
 * drawable's (the siblings of earlier syncs stay retained by their items, not by the root).
 *
 * @param bridge   Bridge to sync through.
 * @param root     Root to publish into.
 * @param commands Drawables for this frame.
 */
void sync(vine::vsg::SceneBridge& bridge, vsg::Group& root, std::vector<RenderCommand> commands)
{
    bridge.syncRenderCommands(commands, &root, nullptr);
}

}  // namespace

/**
 * @brief A sliced channel is bound FROM ITS SEGMENT, straight out of the shared buffer.
 *
 * The array the binding reads must start at the segment's first vertex and still be the model's own memory —
 * both at once is the point of an arena. An offset the backend forgot to apply would draw the arena's first
 * segment for every geometry, silently.
 */
TEST(ChannelSliceTest, ASegmentIsBoundFromItsOwnOffset)
{
    vine::vsg::SceneBridge bridge;
    bridge.setShaderSet(vsg::createPhongShaderSet());
    auto root     = vsg::Group::create();
    auto material = MaterialPtr(new Material());
    auto buffer   = arena();

    sync(bridge, *root, { RenderCommand(segment(buffer, 0u), material, Mat4d()) });
    auto* const first = findBoundVec3(root.get(), kBindingPositions);
    ASSERT_NE(first, nullptr);
    EXPECT_FLOAT_EQ(first->at(0).y, 0.0f);

    sync(bridge, *root, { RenderCommand(segment(buffer, 3u), material, Mat4d()) });
    auto* const second = findBoundVec3(root.get(), kBindingPositions);
    ASSERT_NE(second, nullptr);
    EXPECT_FLOAT_EQ(second->at(0).y, 10.0f) << "the second segment must be the one that is bound";
    EXPECT_EQ(second->size(), 3u) << "and it holds its three vertices, not the arena's six";

    // Still the model's memory: the array reads the arena at the segment's offset (3 scalars in = one vertex).
    EXPECT_EQ(second->data(), reinterpret_cast<const vsg::vec3*>(buffer->data() + 9u));
}

/**
 * @brief Two segments of one buffer are two streams and never share a bind.
 *
 * The cache key carries the slice, exactly like it carries the buffer's revision: without it, the second
 * geometry would be served the bind the first built — a device buffer holding the WRONG segment.
 */
TEST(ChannelSliceTest, TwoSegmentsOfOneArenaDoNotShareABind)
{
    vine::vsg::VsgMeshResourceCache cache;
    vine::vsg::SceneBridge           bridge;
    bridge.setShaderSet(vsg::createPhongShaderSet());
    bridge.setMeshResourceCache(&cache);
    auto root     = vsg::Group::create();
    auto material = MaterialPtr(new Material());
    auto buffer   = arena();

    sync(bridge, *root, { RenderCommand(segment(buffer, 0u), material, Mat4d()) });
    auto* const first_bind = findBoundBind(root.get(), kBindingPositions);
    auto* const first_data = findBoundVec3(root.get(), kBindingPositions);
    ASSERT_NE(first_bind, nullptr);

    sync(bridge, *root, { RenderCommand(segment(buffer, 3u), material, Mat4d()) });
    EXPECT_NE(findBoundBind(root.get(), kBindingPositions), first_bind)
        << "the same buffer at another offset is another stream";
    EXPECT_EQ(cache.count(), 2u) << "and it has its own entry";

    // The same segment DOES share: the arena's users still get one upload per segment.
    sync(bridge, *root, { RenderCommand(segment(buffer, 0u), material, Mat4d()) });
    EXPECT_EQ(findBoundBind(root.get(), kBindingPositions), first_bind);
    EXPECT_EQ(findBoundVec3(root.get(), kBindingPositions), first_data);
}

/**
 * @brief Moving a channel to another segment of the same buffer takes the refresh path with the new offset.
 *
 * The buffer, its revision and the channel's length are all unchanged — only the offset moves. The refresh
 * path has to notice, because the array it builds reads a different place in memory.
 */
TEST(ChannelSliceTest, AChannelMovedToAnotherSegmentIsRefreshedFromThere)
{
    vine::vsg::SceneBridge bridge;
    bridge.setShaderSet(vsg::createPhongShaderSet());
    auto root     = vsg::Group::create();
    auto material = MaterialPtr(new Material());
    auto buffer   = arena();
    auto geometry = segment(buffer, 0u);

    sync(bridge, *root, { RenderCommand(geometry, material, Mat4d()) });
    ASSERT_NE(findBoundVec3(root.get(), kBindingPositions), nullptr);
    EXPECT_FLOAT_EQ(findBoundVec3(root.get(), kBindingPositions)->at(0).y, 0.0f);

    geometry->addBuffer(0u, AttributeChannel::slice(buffer, 3u, 3u, 3u));
    geometry->setRevision(geometry->revision() + 1u);

    sync(bridge, *root, { RenderCommand(geometry, material, Mat4d()) });
    auto* const moved = findBoundVec3(root.get(), kBindingPositions);
    ASSERT_NE(moved, nullptr);
    EXPECT_FLOAT_EQ(moved->at(0).y, 10.0f) << "the new segment's bytes must reach the GPU";
    EXPECT_EQ(moved->data(), reinterpret_cast<const vsg::vec3*>(buffer->data() + 9u));
}

/**
 * @brief An index slice is drawn as firstIndex + count over ONE bound index buffer.
 *
 * The bind covers the whole arena, so every geometry slicing it shares one entry (one index upload); which
 * triangles each one draws is the DRAW's business. The indices are relative to the geometry's own vertices.
 */
TEST(ChannelSliceTest, AnIndexSliceIsDrawnAsFirstIndexAndCount)
{
    vine::vsg::VsgMeshResourceCache cache;
    vine::vsg::SceneBridge           bridge;
    bridge.setShaderSet(vsg::createPhongShaderSet());
    bridge.setMeshResourceCache(&cache);
    auto root     = vsg::Group::create();
    auto material = MaterialPtr(new Material());
    auto buffer   = arena();
    auto indices  = packedIndices({ 0u, 1u, 2u, 0u, 1u, 2u });  // one triangle per segment, slice-local

    const auto first_segment  = segment(buffer, 0u);
    const auto second_segment = segment(buffer, 3u);
    first_segment->setIndices(indices, 0u, 3u);
    second_segment->setIndices(indices, 3u, 3u);

    sync(bridge, *root, { RenderCommand(first_segment, material, Mat4d()) });
    auto* const index_bind = findBindIndex(root.get());
    auto* const draw       = findDrawIndexed(root.get());
    ASSERT_NE(index_bind, nullptr);
    ASSERT_NE(draw, nullptr);
    EXPECT_EQ(draw->indexCount, 3u);
    EXPECT_EQ(draw->firstIndex, 0u);
    EXPECT_EQ(index_bind->indices->data->cast<vsg::uintArray>()->size(), 6u)
        << "the bind holds the whole arena, so it can be shared";

    sync(bridge, *root, { RenderCommand(second_segment, material, Mat4d()) });
    EXPECT_EQ(findBindIndex(root.get()), index_bind) << "one index arena, one index bind";
    EXPECT_EQ(cache.count(), 3u) << "two position segments and the shared index stream";
    auto* const second_draw = findDrawIndexed(root.get());
    ASSERT_NE(second_draw, nullptr);
    EXPECT_EQ(second_draw->indexCount, 3u);
    EXPECT_EQ(second_draw->firstIndex, 3u) << "the second geometry draws the second triangle";
}

/**
 * @brief A geometry that moves to another index span gets a REBUILT draw command.
 *
 * The span lives in the draw command, not in the bind, so the in-place index swap (which only replaces the
 * bound array) cannot express it. The refresh path has to refuse and rebuild — otherwise the geometry keeps
 * drawing the triangles it drew before, which no diagnostic would report.
 *
 * Both shapes of the change are covered: the span alone (same buffer) and the span together with a new
 * buffer — the second is the one a naive "the buffer changed, so swap the bind" gate would let through.
 */
TEST(ChannelSliceTest, AChangedIndexSpanRebuildsTheDrawCommand)
{
    vine::vsg::SceneBridge bridge;
    bridge.setShaderSet(vsg::createPhongShaderSet());
    auto root     = vsg::Group::create();
    auto material = MaterialPtr(new Material());
    auto buffer   = arena();
    auto indices  = packedIndices({ 0u, 1u, 2u, 0u, 1u, 2u });
    auto geometry = segment(buffer, 3u);
    geometry->setIndices(indices, 0u, 3u);

    sync(bridge, *root, { RenderCommand(geometry, material, Mat4d()) });
    auto* const draw_before = findDrawIndexed(root.get());
    ASSERT_NE(draw_before, nullptr);
    ASSERT_EQ(draw_before->firstIndex, 0u);
    ASSERT_EQ(draw_before->indexCount, 3u);

    // Same buffer, same revision, same count: only the span moves.
    geometry->setIndices(indices, 3u, 3u);
    geometry->setRevision(geometry->revision() + 1u);

    sync(bridge, *root, { RenderCommand(geometry, material, Mat4d()) });
    auto* const draw_after = findDrawIndexed(root.get());
    ASSERT_NE(draw_after, nullptr);
    EXPECT_NE(draw_after, draw_before) << "the draw command has to be rewritten for a new span";
    EXPECT_EQ(draw_after->firstIndex, 3u);
    EXPECT_EQ(draw_after->indexCount, 3u);

    // A NEW buffer AND a new span: a changed buffer alone may swap the bind in place (the draw keeps stating
    // the same span), so this is the case a gate that only asks "did the buffer change?" would answer with a
    // stale first index — the bind would hold the new bytes while the draw reads an old part of them.
    auto other_indices = packedIndices({ 2u, 1u, 0u, 2u, 1u, 0u });
    geometry->setIndices(other_indices, 0u, 3u);
    geometry->setRevision(geometry->revision() + 1u);

    sync(bridge, *root, { RenderCommand(geometry, material, Mat4d()) });
    auto* const draw_moved = findDrawIndexed(root.get());
    ASSERT_NE(draw_moved, nullptr);
    EXPECT_NE(draw_moved, draw_after) << "a span change needs the draw command rewritten";
    EXPECT_EQ(draw_moved->firstIndex, 0u) << "the new span must reach the draw command";
    EXPECT_EQ(draw_moved->indexCount, 3u);
    auto* const moved_bind = findBindIndex(root.get());
    ASSERT_NE(moved_bind, nullptr);
    EXPECT_EQ(moved_bind->indices->data->cast<vsg::uintArray>()->at(0u), 2u) << "and the new buffer's bytes";
}

/**
 * @brief An index of the SEGMENT that is out of range for that segment is rejected, not drawn.
 *
 * The bounds check runs over the drawn span against the channel's own vertex count. An index that only
 * exists in the arena (`5` here, while the segment holds three vertices) would read a peer's vertices or run
 * off the bind.
 */
TEST(ChannelSliceTest, AnOutOfRangeIndexInTheSegmentIsRejected)
{
    vine::vsg::SceneBridge bridge;
    bridge.setShaderSet(vsg::createPhongShaderSet());
    auto root     = vsg::Group::create();
    auto material = MaterialPtr(new Material());
    auto buffer   = arena();
    auto geometry = segment(buffer, 0u);
    geometry->setIndices(packedIndices({ 0u, 1u, 5u }));

    sync(bridge, *root, { RenderCommand(geometry, material, Mat4d()) });
    EXPECT_EQ(findDrawIndexed(root.get()), nullptr) << "the geometry must not be drawn";
    EXPECT_EQ(bridge.diagnosticCount(vine::graphics::DiagnosticCategory::GeometryRejected), 1u);
}

/**
 * @brief Derived normals are keyed on the segment they were derived from.
 *
 * The derived-channel cache exists so an unrelated rebuild does not pay for the derivation again. Its key has
 * to carry the slice for the same reason the bind key does: the same buffer at another offset is another set
 * of vertices, and reusing the previous segment's normals would light the mesh with them.
 */
TEST(ChannelSliceTest, DerivedNormalsFollowTheSegmentTheyWereDerivedFrom)
{
    vine::vsg::SceneBridge bridge;
    bridge.setShaderSet(vsg::createPhongShaderSet());
    auto root     = vsg::Group::create();
    auto material = MaterialPtr(new Material());
    auto buffer   = arena();
    auto geometry = segment(buffer, 0u);

    sync(bridge, *root, { RenderCommand(geometry, material, Mat4d()) });
    auto* const first_normals = findBoundVec3(root.get(), kBindingNormals);
    ASSERT_NE(first_normals, nullptr);
    EXPECT_NEAR(first_normals->at(0).z, 1.0f, 1e-3f) << "a triangle in the xy plane faces +Z";

    geometry->addBuffer(0u, AttributeChannel::slice(buffer, 3u, 3u, 3u));
    geometry->setRevision(geometry->revision() + 1u);

    sync(bridge, *root, { RenderCommand(geometry, material, Mat4d()) });
    auto* const second_normals = findBoundVec3(root.get(), kBindingNormals);
    ASSERT_NE(second_normals, nullptr);
    EXPECT_NEAR(second_normals->at(0).y, 1.0f, 1e-3f)
        << "the second segment faces +Y, so its derived normals must be re-derived";
    EXPECT_NEAR(second_normals->at(0).z, 0.0f, 1e-3f)
        << "reusing the first segment's normals would still read +Z here";
}
