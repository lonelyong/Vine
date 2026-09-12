/**
 * @brief Sharing of the mesh streams that ARE the model's own bytes (P9).
 *
 * A bind command OWNS its BufferInfo, and vsg turns a BufferInfo into one device buffer plus its upload
 * (`BindVertexBuffers::compile()`). So N drawables reading one mesh stream used to upload those bytes N
 * times: the vertices exist once in the model and once per drawable on the GPU. `VsgMeshResourceCache`
 * hands them ONE bind instead — the model's buffer is read from one device buffer by every drawable.
 *
 * These tests pin the two halves of that bargain:
 *
 *  1. WHAT may be shared — only the channels the builder ALIASES verbatim from a `vine::Buffer`:
 *     positions, authored normals / texcoords / loc2 colours, and the index stream. The channels the
 *     builder BUILDS stay per-drawable by construction: the white opacity carrier even carries this
 *     drawable's alpha, so sharing it would draw every peer with the first drawable's opacity;
 *  2. WHEN an entry may be used — the key carries the source buffer's REVISION and its element count, so a
 *     refilled stream is never served the bind built from the previous bytes, and a stream no drawable
 *     reads any more is released instead of keeping its upload alive for the session.
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
constexpr std::size_t kBindingTexcoords = 2u;
constexpr std::size_t kBindingColors    = 3u;

/// A unit triangle's positions, the stream every test here shares.
const std::vector<float> kTrianglePositions{ 0.0f, 0.0f, 0.0f, 1.0f, 0.0f, 0.0f, 0.0f, 1.0f, 0.0f };

/// Builds @p floats as a buffer that owns them.
vine::intrusive_ptr<const vine::Buffer<float>> packedFloats(std::vector<float> floats)
{
    return vine::intrusive_ptr<const vine::Buffer<float>>(new vine::Buffer<float>(std::move(floats)));
}

/// Builds @p floats as a buffer the caller keeps writing to (the refill case).
vine::intrusive_ptr<vine::Buffer<float>> filledBuffer(std::vector<float> floats)
{
    return vine::intrusive_ptr<vine::Buffer<float>>(new vine::Buffer<float>(std::move(floats)));
}

/// Builds @p values as an index buffer that owns them.
vine::intrusive_ptr<const vine::Buffer<std::uint32_t>> packedIndices(std::vector<std::uint32_t> values)
{
    return vine::intrusive_ptr<const vine::Buffer<std::uint32_t>>(
        new vine::Buffer<std::uint32_t>(std::move(values)));
}

/// A triangle READING @p positions: no normals, no UVs, no colour, so every optional channel is BUILT.
GeometryPtr triangleReading(vine::intrusive_ptr<const vine::Buffer<float>> positions)
{
    auto geometry = GeometryPtr(new Geometry());
    geometry->setPositions(std::move(positions));
    return geometry;
}

/// Adds a minimal valid vertex + fragment stage pair to @p program, so the bridge builds a CUSTOM program
/// variant (the path where an authored loc2 colour is what binding 3 carries).
void addTrivialStages(const ShaderProgramPtr& program)
{
    vine::graphics::ShaderStage vs;
    vs.type   = vine::graphics::ShaderStageType::Vertex;
    vs.source = u8"#version 450\n"
                u8"layout(location = 0) in vec3 vsg_Vertex;\n"
                u8"void main() { gl_Position = vec4(vsg_Vertex, 0.5); }\n";
    program->addStage(vs);
    vine::graphics::ShaderStage fs;
    fs.type   = vine::graphics::ShaderStageType::Fragment;
    fs.source = u8"#version 450\n"
                u8"layout(location = 0) out vec4 outColor;\n"
                u8"void main() { outColor = vec4(1.0); }\n";
    program->addStage(fs);
}

/// Finds the vertex bind command carrying binding @p binding under a retained subtree.
vsg::BindVertexBuffers* findBoundBind(vsg::Node* node, std::size_t binding)
{
    if (node == nullptr) {
        return nullptr;
    }
    if (auto bind = node->cast<vsg::BindVertexBuffers>()) {
        // One command per channel means the binding has to be resolved through firstBinding; a command that
        // does not carry it is not the answer.
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

/**
 * @brief Draws @p commands once through @p bridge.
 *
 * A sync publishes ONLY the drawables it was given into @p root, which is what makes a test able to talk
 * about "this drawable's bind": one drawable per sync, and the bind found afterwards is that drawable's.
 * The drawables of earlier syncs stay retained (their items hold them), so a bind captured earlier is still
 * comparable — and still the object the earlier drawable binds.
 *
 * @param bridge   Bridge to sync through.
 * @param root     Root to publish into.
 * @param commands Drawables for this frame, in the order they are drawn.
 */
void sync(vine::vsg::SceneBridge& bridge, vsg::Group& root, std::vector<RenderCommand> commands)
{
    bridge.syncRenderCommands(commands, &root, nullptr);
}

}  // namespace

/**
 * @brief One stream, one bind: two drawables reading the same positions share the very same command.
 *
 * This is the whole point of the cache. A bind owns its BufferInfo, so two binds mean two device buffers
 * holding the same vertices and two uploads of them; one bind means the model's bytes are uploaded once and
 * read by both drawables.
 */
TEST(MeshResourceCacheTest, TwoDrawablesReadingOneStreamShareOneBind)
{
    vine::vsg::VsgMeshResourceCache cache;
    vine::vsg::SceneBridge           bridge;
    bridge.setShaderSet(vsg::createPhongShaderSet());
    bridge.setMeshResourceCache(&cache);
    auto root     = vsg::Group::create();
    auto positions = packedFloats(kTrianglePositions);
    auto material  = MaterialPtr(new Material());

    const auto first  = triangleReading(positions);
    const auto second = triangleReading(positions);

    sync(bridge, *root, { RenderCommand(first, material, Mat4d()) });
    auto* const first_bind = findBoundBind(root.get(), kBindingPositions);
    auto* const first_data = findBoundData(root.get(), kBindingPositions);
    ASSERT_NE(first_bind, nullptr) << "the positions must be bound";

    sync(bridge, *root, { RenderCommand(second, material, Mat4d()) });
    auto* const second_bind = findBoundBind(root.get(), kBindingPositions);
    EXPECT_EQ(second_bind, first_bind) << "one stream must mean one bind, and therefore one upload";
    EXPECT_EQ(findBoundData(root.get(), kBindingPositions), first_data)
        << "the same array, so the second drawable adds no copy of the vertices";
    EXPECT_EQ(cache.count(), 1u) << "one stream, one entry";
}

/**
 * @brief Different streams do not share a bind.
 *
 * The cache is keyed by the buffer (and its revision), so two meshes that happen to have the same shape and
 * the same element count still upload separately — sharing must never hand one mesh another's vertices.
 */
TEST(MeshResourceCacheTest, DifferentStreamsDoNotShareABind)
{
    vine::vsg::VsgMeshResourceCache cache;
    vine::vsg::SceneBridge           bridge;
    bridge.setShaderSet(vsg::createPhongShaderSet());
    bridge.setMeshResourceCache(&cache);
    auto root     = vsg::Group::create();
    auto material = MaterialPtr(new Material());

    const auto first  = triangleReading(packedFloats(kTrianglePositions));
    const auto second = triangleReading(packedFloats({ 0.5f, 0.0f, 0.0f, 1.5f, 0.0f, 0.0f, 0.5f, 1.0f, 0.0f }));

    sync(bridge, *root, { RenderCommand(first, material, Mat4d()) });
    auto* const first_bind = findBoundBind(root.get(), kBindingPositions);

    sync(bridge, *root, { RenderCommand(second, material, Mat4d()) });
    auto* const second_bind = findBoundBind(root.get(), kBindingPositions);
    EXPECT_NE(second_bind, first_bind) << "two buffers are two streams";
    EXPECT_EQ(cache.count(), 2u) << "and two entries";
}

/**
 * @brief Only the aliased channel is shared; the BUILT ones stay per drawable.
 *
 * Two drawables reading one positions stream share binding 0 — and nothing else. Binding 1 (normals derived
 * from those positions), binding 2 (zero UVs) and binding 3 (the white opacity carrier) are built per
 * drawable: the carrier even carries THIS drawable's alpha, so sharing it would draw both drawables with the
 * first one's opacity.
 */
TEST(MeshResourceCacheTest, BuiltChannelsAreNeverShared)
{
    vine::vsg::VsgMeshResourceCache cache;
    vine::vsg::SceneBridge           bridge;
    bridge.setShaderSet(vsg::createPhongShaderSet());
    bridge.setMeshResourceCache(&cache);
    auto root      = vsg::Group::create();
    auto positions = packedFloats(kTrianglePositions);
    auto material  = MaterialPtr(new Material());

    const auto first  = triangleReading(positions);
    const auto second = triangleReading(positions);

    sync(bridge, *root, { RenderCommand(first, material, Mat4d()) });
    auto* const positions_bind = findBoundBind(root.get(), kBindingPositions);
    auto* const normals_bind   = findBoundBind(root.get(), kBindingNormals);
    auto* const texcoord_bind  = findBoundBind(root.get(), kBindingTexcoords);
    auto* const color_bind     = findBoundBind(root.get(), kBindingColors);
    ASSERT_NE(normals_bind, nullptr);
    ASSERT_NE(texcoord_bind, nullptr);
    ASSERT_NE(color_bind, nullptr);

    sync(bridge, *root, { RenderCommand(second, material, Mat4d()) });
    EXPECT_EQ(findBoundBind(root.get(), kBindingPositions), positions_bind);
    EXPECT_NE(findBoundBind(root.get(), kBindingNormals), normals_bind) << "derived normals are per drawable";
    EXPECT_NE(findBoundBind(root.get(), kBindingTexcoords), texcoord_bind) << "zero UVs are per drawable";
    EXPECT_NE(findBoundBind(root.get(), kBindingColors), color_bind) << "the white carrier is per drawable";
    EXPECT_EQ(cache.count(), 1u) << "only the aliased stream entered the table";
}

/**
 * @brief The index stream shares like a vertex stream, and only when it IS one buffer.
 *
 * The index bind is one more stream of the model's own bytes, so two drawables indexing the same buffer must
 * share it too — that is one index upload instead of two, and (for a mesh read by many drawables) the larger
 * saving of the two.
 */
TEST(MeshResourceCacheTest, TheIndexStreamSharesTheSameWay)
{
    vine::vsg::VsgMeshResourceCache cache;
    vine::vsg::SceneBridge           bridge;
    bridge.setShaderSet(vsg::createPhongShaderSet());
    bridge.setMeshResourceCache(&cache);
    auto root       = vsg::Group::create();
    auto positions  = packedFloats(kTrianglePositions);
    auto indices    = packedIndices({ 0u, 1u, 2u });
    auto material   = MaterialPtr(new Material());

    const auto first  = triangleReading(positions);
    const auto second = triangleReading(positions);
    for (const auto& geometry : { first, second }) {
        geometry->setIndices(indices);
    }

    sync(bridge, *root, { RenderCommand(first, material, Mat4d()) });
    auto* const first_bind = findBindIndex(root.get());
    ASSERT_NE(first_bind, nullptr);

    sync(bridge, *root, { RenderCommand(second, material, Mat4d()) });
    EXPECT_EQ(findBindIndex(root.get()), first_bind) << "one index stream must mean one bind";
    EXPECT_EQ(cache.count(), 2u) << "the positions and the indices, and nothing else";

    // A drawable with its OWN index buffer is a different stream even when its indices are the same numbers.
    const auto third = triangleReading(positions);
    third->setIndices(packedIndices({ 0u, 1u, 2u }));
    sync(bridge, *root, { RenderCommand(third, material, Mat4d()) });
    EXPECT_NE(findBindIndex(root.get()), first_bind) << "different bytes are a different stream";
}

/**
 * @brief A refilled stream is never served the bind built from the previous bytes.
 *
 * The key carries the buffer's revision, which is the contract a consumer that cached bytes compares
 * against (the same rule Texture and ShaderProgram follow). Refilling a buffer in place and announcing it
 * therefore produces a bind over the NEW bytes — the old bind is not re-pointed even though the buffer's
 * address never changed.
 */
TEST(MeshResourceCacheTest, ARefilledStreamIsNeverServedTheStaleBind)
{
    vine::vsg::VsgMeshResourceCache cache;
    vine::vsg::SceneBridge           bridge;
    bridge.setShaderSet(vsg::createPhongShaderSet());
    bridge.setMeshResourceCache(&cache);
    auto root     = vsg::Group::create();
    auto material = MaterialPtr(new Material());

    // A buffer the model keeps writing into, which is what a morph / skinning / editing app does.
    auto       positions = filledBuffer(kTrianglePositions);
    const auto geometry  = triangleReading(positions);

    sync(bridge, *root, { RenderCommand(geometry, material, Mat4d()) });
    auto* const       stale_bind = findBoundBind(root.get(), kBindingPositions);
    const std::size_t entry_count = cache.count();
    ASSERT_NE(stale_bind, nullptr);

    // The model refills the stream in place and announces it (Buffer's contract), then says so to the
    // geometry: without the revision in the key the cache would hand back the bind over the old bytes.
    const std::uint64_t revision = positions->revision();
    for (float& scalar : positions->view()) {
        scalar += 10.0f;
    }
    positions->setRevision(revision + 1u);
    geometry->setRevision(geometry->revision() + 1u);

    sync(bridge, *root, { RenderCommand(geometry, material, Mat4d()) });
    auto* const fresh_bind = findBoundBind(root.get(), kBindingPositions);
    EXPECT_NE(fresh_bind, stale_bind) << "new bytes must not reuse the bind over the old ones";
    EXPECT_EQ(cache.count(), entry_count) << "the abandoned revision's entry is swept, not accumulated";
    auto* const bound = findBoundData(root.get(), kBindingPositions);
    ASSERT_NE(bound, nullptr);
    const auto* const vertices = bound->cast<vsg::vec3Array>();
    ASSERT_NE(vertices, nullptr);
    EXPECT_FLOAT_EQ(vertices->at(0).x, 10.0f) << "the bind must read the refilled bytes";
}

/**
 * @brief Refreshing a shared stream leaves the drawables that share it alone.
 *
 * Two drawables reading one stream were built with the SAME bind command, so refreshing one of them may not
 * re-point that command: `assignArrays()` on it would replace the array every peer is reading, silently
 * swapping their stream for this drawable's. The refresh therefore takes the bind the cache holds for the
 * NEW stream and swaps it into its own command list, which is what these assertions trace — the peer keeps
 * both the command and the array it was built with.
 */
TEST(MeshResourceCacheTest, ARefreshOfASharedStreamLeavesItsPeersAlone)
{
    vine::vsg::VsgMeshResourceCache cache;
    vine::vsg::SceneBridge           bridge;
    bridge.setShaderSet(vsg::createPhongShaderSet());
    bridge.setMeshResourceCache(&cache);
    auto root     = vsg::Group::create();
    auto material = MaterialPtr(new Material());

    auto       positions = filledBuffer(kTrianglePositions);
    const auto first     = triangleReading(positions);
    const auto second    = triangleReading(positions);

    sync(bridge, *root, { RenderCommand(first, material, Mat4d()) });
    auto* const shared_bind = findBoundBind(root.get(), kBindingPositions);

    sync(bridge, *root, { RenderCommand(second, material, Mat4d()) });
    auto* const peer_bind = findBoundBind(root.get(), kBindingPositions);
    auto* const peer_data = findBoundData(root.get(), kBindingPositions);
    ASSERT_EQ(peer_bind, shared_bind) << "the premise: both drawables read one bind";

    // The app refills the stream and announces the edit to ONE of the two drawables.
    const std::uint64_t revision = positions->revision();
    for (float& scalar : positions->view()) {
        scalar += 1.0f;
    }
    positions->setRevision(revision + 1u);
    first->setRevision(first->revision() + 1u);

    sync(bridge, *root, { RenderCommand(first, material, Mat4d()) });
    EXPECT_NE(findBoundBind(root.get(), kBindingPositions), peer_bind)
        << "the refreshed drawable must not re-point the bind its peer is reading";
    EXPECT_EQ(cache.count(), 2u) << "the peer's revision and the refreshed one both have a live entry";

    sync(bridge, *root, { RenderCommand(second, material, Mat4d()) });
    EXPECT_EQ(findBoundBind(root.get(), kBindingPositions), peer_bind) << "the peer's command is untouched";
    EXPECT_EQ(findBoundData(root.get(), kBindingPositions), peer_data) << "and so is the array it reads";
}

/**
 * @brief A revision no stream explains takes its own binds instead of a shared one.
 *
 * The geometry's revision is the only announcement a caller makes — the whole gate a retained node rebuilds
 * on — and it is what reports a change made behind the renderer's back (a model rebuilt underneath the
 * geometry, or a buffer written through a pointer the buffer cannot see, see Geometry::setRevision). A
 * shared bind holds a copy of the bytes as of ITS insertion and its key cannot tell such a change apart, so
 * serving it would keep drawing the previous mesh, silently. A rebuild that cannot attribute the annotation
 * to a stream therefore RE-READS the model into its own binds — which is also what keeps the peer's stream
 * what it was.
 */
TEST(MeshResourceCacheTest, ARevisionNoStreamExplainsTakesItsOwnBinds)
{
    vine::vsg::VsgMeshResourceCache cache;
    vine::vsg::SceneBridge           bridge;
    bridge.setShaderSet(vsg::createPhongShaderSet());
    bridge.setMeshResourceCache(&cache);
    auto root     = vsg::Group::create();
    auto material = MaterialPtr(new Material());

    auto       positions = packedFloats(kTrianglePositions);
    const auto first     = triangleReading(positions);
    const auto second    = triangleReading(positions);

    sync(bridge, *root, { RenderCommand(first, material, Mat4d()) });
    auto* const shared_bind = findBoundBind(root.get(), kBindingPositions);
    ASSERT_NE(shared_bind, nullptr);
    sync(bridge, *root, { RenderCommand(second, material, Mat4d()) });
    ASSERT_EQ(findBoundBind(root.get(), kBindingPositions), shared_bind) << "the premise: one shared stream";

    // A revision NO stream accounts for: every channel still reads the same buffer at the same revision.
    first->setRevision(first->revision() + 1u);
    sync(bridge, *root, { RenderCommand(first, material, Mat4d()) });
    EXPECT_NE(findBoundBind(root.get(), kBindingPositions), shared_bind)
        << "an unattributable revision must re-read the model, not reuse the bytes it already has";

    sync(bridge, *root, { RenderCommand(second, material, Mat4d()) });
    EXPECT_EQ(findBoundBind(root.get(), kBindingPositions), shared_bind)
        << "and the peer keeps reading the stream it vouched for";
}

/**
 * @brief A stream no drawable reads any more is released instead of keeping its upload alive.
 *
 * An entry holds the bind, the bind holds the array and the array holds the model's buffer, so a shared
 * stream keeps its bytes alive. When the last drawable reading it moves on (here: the geometry is pointed at
 * a different buffer, which a review of the retained node can absorb), the entry's last user is gone and the
 * frame sweep drops it — otherwise a session that keeps editing meshes would grow its table without limit
 * until the FIFO bound pushed something live out.
 */
TEST(MeshResourceCacheTest, AStreamNobodyReadsAnyMoreIsReleased)
{
    vine::vsg::VsgMeshResourceCache cache;
    vine::vsg::SceneBridge           bridge;
    bridge.setShaderSet(vsg::createPhongShaderSet());
    bridge.setMeshResourceCache(&cache);
    auto root     = vsg::Group::create();
    auto material = MaterialPtr(new Material());

    const auto geometry = triangleReading(packedFloats(kTrianglePositions));
    sync(bridge, *root, { RenderCommand(geometry, material, Mat4d()) });
    auto* const abandoned = findBoundBind(root.get(), kBindingPositions);
    ASSERT_NE(abandoned, nullptr);
    EXPECT_EQ(cache.count(), 1u);

    geometry->setPositions(packedFloats({ 0.0f, 0.0f, 0.0f, 2.0f, 0.0f, 0.0f, 0.0f, 2.0f, 0.0f }));
    geometry->setRevision(geometry->revision() + 1u);

    sync(bridge, *root, { RenderCommand(geometry, material, Mat4d()) });
    EXPECT_NE(findBoundBind(root.get(), kBindingPositions), abandoned);
    EXPECT_EQ(cache.count(), 1u) << "the abandoned stream's entry must be swept, not kept";
}

/**
 * @brief An authored colour is shared only when binding 3 binds the buffer itself.
 *
 * On the built-in path binding 3 is the backend's own white carrier, but a custom program makes the AUTHORED
 * loc2 colour the channel's source. Four components alias the model's floats verbatim (shareable); three are
 * packed into a vec4 per drawable (NOT shareable — the packed arrays differ from the model's bytes, and
 * sharing one would draw every peer with the first drawable's colours).
 */
TEST(MeshResourceCacheTest, AnAliasedColourIsSharedButAPackedOneIsNot)
{
    vine::vsg::VsgMeshResourceCache cache;
    vine::vsg::SceneBridge           bridge;
    bridge.setShaderSet(vsg::createPhongShaderSet());
    bridge.setMeshResourceCache(&cache);
    auto root      = vsg::Group::create();
    auto positions = packedFloats(kTrianglePositions);
    auto material  = MaterialPtr(new Material());
    auto program   = ShaderProgramPtr(new ShaderProgram());
    addTrivialStages(program);

    const auto draw = [&](const GeometryPtr& geometry) {
        std::vector<RenderCommand> commands{ RenderCommand(geometry, material, Mat4d()) };
        commands.back().program = program;
        sync(bridge, *root, std::move(commands));
    };

    // Four components: the array IS the model's scalars, so both drawables read one uploaded colour stream.
    auto colors4 = packedFloats({ 1.0f, 0.0f, 0.0f, 0.5f, 0.0f, 1.0f, 0.0f, 0.5f, 0.0f, 0.0f, 1.0f, 0.5f });
    auto first   = triangleReading(positions);
    auto second  = triangleReading(positions);
    first->addBuffer(2u, AttributeBuffer::shared(colors4, 4u));
    second->addBuffer(2u, AttributeBuffer::shared(colors4, 4u));

    draw(first);
    auto* const aliased_colors = findBoundBind(root.get(), kBindingColors);
    ASSERT_NE(aliased_colors, nullptr);
    draw(second);
    EXPECT_EQ(findBoundBind(root.get(), kBindingColors), aliased_colors) << "a verbatim view is shareable";
    EXPECT_EQ(cache.count(), 2u) << "the positions and the colours";

    // Three components: the builder packs them, so the bound array is not the model's bytes.
    auto colors3 = packedFloats({ 1.0f, 0.0f, 0.0f, 0.0f, 1.0f, 0.0f, 0.0f, 0.0f, 1.0f });
    auto third   = triangleReading(positions);
    auto fourth  = triangleReading(positions);
    third->addBuffer(2u, AttributeBuffer::shared(colors3, 3u));
    fourth->addBuffer(2u, AttributeBuffer::shared(colors3, 3u));

    draw(third);
    auto* const packed_colors = findBoundBind(root.get(), kBindingColors);
    ASSERT_NE(packed_colors, nullptr);
    EXPECT_NE(packed_colors, aliased_colors) << "different bytes, different element type: not the same bind";
    draw(fourth);
    EXPECT_NE(findBoundBind(root.get(), kBindingColors), packed_colors) << "a packed channel is per drawable";
    EXPECT_EQ(cache.count(), 2u) << "a packed channel never enters the table";
}

/**
 * @brief Two bridges without an injected cache share nothing.
 *
 * A bridge falls back to its OWN cache, which cannot know about another bridge's device: sharing across
 * bridges is exactly what the session cache exists for (VsgRendererState::mesh_cache is injected into every
 * slot's bridge). Two bridges that were never given one must therefore keep uploading separately.
 */
TEST(MeshResourceCacheTest, TwoBridgesWithoutInjectionShareNothing)
{
    auto positions = packedFloats(kTrianglePositions);
    auto material  = MaterialPtr(new Material());
    auto first_root  = vsg::Group::create();
    auto second_root = vsg::Group::create();

    vine::vsg::SceneBridge first_bridge;
    first_bridge.setShaderSet(vsg::createPhongShaderSet());
    sync(first_bridge, *first_root, { RenderCommand(triangleReading(positions), material, Mat4d()) });

    vine::vsg::SceneBridge second_bridge;
    second_bridge.setShaderSet(vsg::createPhongShaderSet());
    sync(second_bridge, *second_root, { RenderCommand(triangleReading(positions), material, Mat4d()) });

    auto* const first_bind  = findBoundBind(first_root.get(), kBindingPositions);
    auto* const second_bind = findBoundBind(second_root.get(), kBindingPositions);
    ASSERT_NE(first_bind, nullptr);
    ASSERT_NE(second_bind, nullptr);
    EXPECT_NE(first_bind, second_bind) << "only an injected session cache shares across bridges";
}

/**
 * @brief The shared table stays bounded, and a stream read by the newest drawables still shares.
 *
 * Capacity is the FIFO half of the bargain every retained cache in the bridge makes: a scene that keeps
 * producing meshes cannot grow the table without limit. A trim that threw the whole table away would be
 * worse than the bound it enforces, so the entries the live scene just inserted have to survive it.
 */
TEST(MeshResourceCacheTest, TheSharedTableStaysBounded)
{
    vine::vsg::VsgMeshResourceCache cache;
    vine::vsg::SceneBridge           bridge;
    bridge.setShaderSet(vsg::createPhongShaderSet());
    bridge.setMeshResourceCache(&cache);
    auto root     = vsg::Group::create();
    auto material = MaterialPtr(new Material());

    // One drawable per stream, more streams than the table holds: the worst case for it.
    constexpr std::size_t kDrawables = vine::vsg::VsgMeshResourceCache::kMaxEntries + 8u;
    std::vector<GeometryPtr>      geometries;
    std::vector<RenderCommand>    commands;
    geometries.reserve(kDrawables);
    commands.reserve(kDrawables);
    for (std::size_t i = 0; i < kDrawables; ++i) {
        const float x = static_cast<float>(i);
        geometries.push_back(triangleReading(packedFloats({ x, 0.0f, 0.0f, x, 1.0f, 0.0f, x, 0.0f, 1.0f })));
        commands.emplace_back(geometries.back(), material, Mat4d());
    }

    sync(bridge, *root, std::move(commands));
    EXPECT_EQ(cache.count(), vine::vsg::VsgMeshResourceCache::kMaxEntries)
        << "the table fills up to its bound and stays there, instead of growing or being cleared";

    // The table still works after the trim: two drawables built afterwards, reading one stream, share it.
    auto late_positions = packedFloats(kTrianglePositions);
    sync(bridge, *root, { RenderCommand(triangleReading(late_positions), material, Mat4d()) });
    auto* const late_bind = findBoundBind(root.get(), kBindingPositions);
    ASSERT_NE(late_bind, nullptr);
    sync(bridge, *root, { RenderCommand(triangleReading(late_positions), material, Mat4d()) });
    EXPECT_EQ(findBoundBind(root.get(), kBindingPositions), late_bind)
        << "a trimmed table must still share what the live scene reads";
}
