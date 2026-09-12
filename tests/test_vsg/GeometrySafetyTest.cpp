#include <gtest/gtest.h>

#include <vine/graphics/Geometry.hpp>
#include <vine/graphics/Material.hpp>
#include <vine/graphics/RenderCommand.hpp>
#include <vine/graphics/StateNode.hpp>
#include <vine/vsg/SceneBridge.hpp>

#include <vsg/commands/BindIndexBuffer.h>
#include <vsg/commands/BindVertexBuffers.h>
#include <vsg/commands/Commands.h>
#include <vsg/commands/DrawIndexed.h>
#include <vsg/core/Array.h>
#include <vsg/io/Options.h>
#include <vsg/nodes/Group.h>
#include <vsg/nodes/MatrixTransform.h>
#include <vsg/utils/ShaderSet.h>

#include <cmath>

using namespace vine::graphics;
using vine::math::Mat4d;

namespace
{

/// Builds a shared float payload for an AttributeBuffer.
std::shared_ptr<std::vector<float>> packedFloats(const std::vector<float>& floats)
{
    return std::make_shared<std::vector<float>>(floats);
}

/**
 * @brief Builds a geometry whose location-0 buffer carries @p pos_floats with
 * the given components stride (positions are never added through the Vec3
 * convenience API so tests can drive arbitrary component counts).
 */
GeometryPtr makePackedGeometry(const std::vector<float>& pos_floats,
                               std::uint32_t             pos_components,
                               std::shared_ptr<vine::geometry::UInt32Array> indices = nullptr)
{
    auto geom = GeometryPtr(new Geometry());
    AttributeBuffer buf;
    buf.components = pos_components;
    buf.data       = packedFloats(pos_floats);
    geom->addBuffer(0, buf);
    if (indices != nullptr) {
        geom->setIndices(indices);
    }
    return geom;
}

/// Attaches a location-1 (normal) channel with the given components.
void attachNormal(Geometry* geom, const std::vector<float>& floats, std::uint32_t components)
{
    AttributeBuffer buf;
    buf.components = components;
    buf.data       = packedFloats(floats);
    geom->addBuffer(1, buf);
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

/// Finds the vertex Data bound at binding @p binding under a retained subtree.
vsg::Data* findBoundData(vsg::Node* node, std::size_t binding)
{
    if (node == nullptr) {
        return nullptr;
    }
    if (auto bvb = node->cast<vsg::BindVertexBuffers>()) {
        if (binding < bvb->arrays.size() && bvb->arrays[binding] != nullptr &&
            bvb->arrays[binding]->data != nullptr) {
            return bvb->arrays[binding]->data;
        }
        return nullptr;
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

/// Every normal in a retained subtree is finite (no NaN from derivation).
bool normalsAreFinite(vsg::Node* node)
{
    auto* normals = findBoundData(node, 1u);
    if (normals == nullptr) {
        return false;
    }
    auto* arr = normals->cast<vsg::vec3Array>();
    if (arr == nullptr) {
        return false;
    }
    for (const auto& n : *arr) {
        if (!std::isfinite(n.x) || !std::isfinite(n.y) || !std::isfinite(n.z)) {
            return false;
        }
    }
    return true;
}

/// The standard right-facing unit triangle (positions only, no normals).
std::vector<float> triangleFloats(float x = 0.0f)
{
    return { x, 0.0f, 0.0f, x, 1.0f, 0.0f, x, 0.0f, 1.0f };
}

}  // namespace

/**
 * @brief Verifies the SceneBridge rejects vertex data that is unsafe to draw:
 * malformed attribute strides or out-of-range indices must not reach the GPU
 * (they would read OOB in the CPU normal derivation and validation-fault in
 * DrawIndexed). The retained root stays empty for the rejected geometry.
 */
TEST(GeometrySafetyTest, OutOfRangeIndexRejectsGeometry)
{
    vine::vsg::SceneBridge bridge;
    bridge.setShaderSet(vsg::createPhongShaderSet());
    auto root     = vsg::Group::create();
    auto material = MaterialPtr(new Material());

    // One triangle's positions, but an index pointing past the 3rd vertex.
    auto bad = makePackedGeometry(triangleFloats(), 3u,
                                  std::make_shared<vine::geometry::UInt32Array>(
                                      vine::geometry::UInt32Array{ 0u, 1u, 5u }));

    std::vector<vsg::ref_ptr<vsg::Node>> created;
    bridge.syncRenderCommands(std::vector<RenderCommand>{ RenderCommand(bad, material, Mat4d()) },
                              root.get(), &created);

    EXPECT_EQ(root->children.size(), 0u); // nothing drawable, nothing recorded
    EXPECT_EQ(created.size(), 0u);
}

/**
 * @brief An index count that is not a multiple of three has its trailing
 * partial triangle dropped: the GPU draw count is clamped to whole triangles,
 * so the CPU normal derivation and the draw agree (no partial primitive).
 */
TEST(GeometrySafetyTest, NonTriangleMultipleIndexCountIsDrawnVerbatim)
{
    vine::vsg::SceneBridge bridge;
    bridge.setShaderSet(vsg::createPhongShaderSet());
    auto root     = vsg::Group::create();
    auto material = MaterialPtr(new Material());

    // Two full triangles (6 vertices) + one stray trailing index.
    std::vector<float> positions;
    for (int i = 0; i < 6; ++i) {
        const float x = static_cast<float>(i);
        positions.push_back(x);
        positions.push_back(0.0f);
        positions.push_back(0.0f);
    }
    auto geom = makePackedGeometry(
        positions, 3u,
        std::make_shared<vine::geometry::UInt32Array>(
            vine::geometry::UInt32Array{ 0u, 1u, 2u, 3u, 4u, 5u, 0u }));

    std::vector<vsg::ref_ptr<vsg::Node>> created;
    bridge.syncRenderCommands(std::vector<RenderCommand>{ RenderCommand(geom, material, Mat4d()) },
                              root.get(), &created);

    ASSERT_EQ(root->children.size(), 1u); // still drawable
    auto* draw = findDrawIndexed(root.get());
    ASSERT_NE(draw, nullptr);
    // The index stream is drawn verbatim: a trailing partial triangle simply
    // rasterises nothing, so no index is ever truncated by the data builder.
    EXPECT_EQ(draw->indexCount, 7u);
    EXPECT_TRUE(normalsAreFinite(root.get()));
}

/**
 * @brief An index buffer too short for one triangle is still a legal no-op
 * draw (a partial primitive rasterises nothing): the data path keeps it and
 * never rejects on COUNT — only an out-of-range index rejects a geometry.
 */
TEST(GeometrySafetyTest, ShortIndexBufferStillDrawsNoOp)
{
    vine::vsg::SceneBridge bridge;
    bridge.setShaderSet(vsg::createPhongShaderSet());
    auto root     = vsg::Group::create();
    auto material = MaterialPtr(new Material());

    auto geom = makePackedGeometry(triangleFloats(), 3u,
                                   std::make_shared<vine::geometry::UInt32Array>(
                                       vine::geometry::UInt32Array{ 0u, 1u }));

    std::vector<vsg::ref_ptr<vsg::Node>> created;
    bridge.syncRenderCommands(std::vector<RenderCommand>{ RenderCommand(geom, material, Mat4d()) },
                              root.get(), &created);

    ASSERT_EQ(root->children.size(), 1u); // drawn (a no-op partial primitive)
    auto* draw = findDrawIndexed(root.get());
    ASSERT_NE(draw, nullptr);
    EXPECT_EQ(draw->indexCount, 2u); // kept verbatim
}

/**
 * @brief A vec4 position channel (components = 4) is unpacked with the 4-float
 * stride and only its xyz used: the extra w must not shift the following
 * vertex's position.
 */
TEST(GeometrySafetyTest, Vec4PositionUsesXyzSkipsW)
{
    vine::vsg::SceneBridge bridge;
    bridge.setShaderSet(vsg::createPhongShaderSet());
    auto root     = vsg::Group::create();
    auto material = MaterialPtr(new Material());

    // Three vec4 vertices: the w component (99/98/97) is large and distinct so
    // any stride misread would corrupt the xyz read.
    auto geom = makePackedGeometry(
        { 1.0f, 2.0f, 3.0f, 99.0f,
          4.0f, 5.0f, 6.0f, 98.0f,
          7.0f, 8.0f, 9.0f, 97.0f },
        4u,
        std::make_shared<vine::geometry::UInt32Array>(
            vine::geometry::UInt32Array{ 0u, 1u, 2u }));

    std::vector<vsg::ref_ptr<vsg::Node>> created;
    bridge.syncRenderCommands(std::vector<RenderCommand>{ RenderCommand(geom, material, Mat4d()) },
                              root.get(), &created);

    ASSERT_EQ(root->children.size(), 1u);
    auto* positions = findBoundData(root.get(), 0u)->cast<vsg::vec3Array>();
    ASSERT_NE(positions, nullptr);
    ASSERT_EQ(positions->size(), 3u);
    EXPECT_FLOAT_EQ((*positions)[0].x, 1.0f);
    EXPECT_FLOAT_EQ((*positions)[0].y, 2.0f);
    EXPECT_FLOAT_EQ((*positions)[0].z, 3.0f);
    EXPECT_FLOAT_EQ((*positions)[1].x, 4.0f);
    EXPECT_FLOAT_EQ((*positions)[1].y, 5.0f);
    EXPECT_FLOAT_EQ((*positions)[1].z, 6.0f);
    auto* draw = findDrawIndexed(root.get());
    ASSERT_NE(draw, nullptr);
    EXPECT_EQ(draw->indexCount, 3u);
    EXPECT_TRUE(normalsAreFinite(root.get()));
}

/**
 * @brief A position channel with fewer than three components (no xyz) is
 * unusable and rejects the geometry instead of misreading it.
 */
TEST(GeometrySafetyTest, ShortPositionComponentsRejectGeometry)
{
    vine::vsg::SceneBridge bridge;
    bridge.setShaderSet(vsg::createPhongShaderSet());
    auto root     = vsg::Group::create();
    auto material = MaterialPtr(new Material());

    auto geom = makePackedGeometry({ 1.0f, 2.0f, 3.0f, 4.0f, 5.0f, 6.0f }, 2u);

    std::vector<vsg::ref_ptr<vsg::Node>> created;
    bridge.syncRenderCommands(std::vector<RenderCommand>{ RenderCommand(geom, material, Mat4d()) },
                              root.get(), &created);

    EXPECT_EQ(root->children.size(), 0u);
}

/**
 * @brief A position payload whose size is not divisible by its components
 * stride is rejected (it cannot be unpacked vertex-by-vertex).
 */
TEST(GeometrySafetyTest, UndivisiblePositionDataRejectsGeometry)
{
    vine::vsg::SceneBridge bridge;
    bridge.setShaderSet(vsg::createPhongShaderSet());
    auto root     = vsg::Group::create();
    auto material = MaterialPtr(new Material());

    // 8 floats with a 3-component stride: 2 complete vertices + 2 trailing.
    auto geom = makePackedGeometry({ 0.0f, 0.0f, 0.0f, 1.0f, 0.0f, 0.0f, 1.0f, 1.0f }, 3u);

    std::vector<vsg::ref_ptr<vsg::Node>> created;
    bridge.syncRenderCommands(std::vector<RenderCommand>{ RenderCommand(geom, material, Mat4d()) },
                              root.get(), &created);

    EXPECT_EQ(root->children.size(), 0u);
}

/**
 * @brief A vec4 normal channel is unpacked with the 4-float stride and only
 * its xyz used (extra w skipped); the geometry still draws.
 */
TEST(GeometrySafetyTest, Vec4NormalSkipsW)
{
    vine::vsg::SceneBridge bridge;
    bridge.setShaderSet(vsg::createPhongShaderSet());
    auto root     = vsg::Group::create();
    auto material = MaterialPtr(new Material());

    auto geom = makePackedGeometry(triangleFloats(), 3u,
                                   std::make_shared<vine::geometry::UInt32Array>(
                                       vine::geometry::UInt32Array{ 0u, 1u, 2u }));
    attachNormal(geom.get(),
                 { 0.0f, 0.0f, 1.0f, 55.0f,
                   0.0f, 0.0f, 1.0f, 55.0f,
                   0.0f, 0.0f, 1.0f, 55.0f },
                 4u);

    std::vector<vsg::ref_ptr<vsg::Node>> created;
    bridge.syncRenderCommands(std::vector<RenderCommand>{ RenderCommand(geom, material, Mat4d()) },
                              root.get(), &created);

    ASSERT_EQ(root->children.size(), 1u);
    auto* normals = findBoundData(root.get(), 1u)->cast<vsg::vec3Array>();
    ASSERT_NE(normals, nullptr);
    ASSERT_EQ(normals->size(), 3u);
    EXPECT_FLOAT_EQ((*normals)[0].x, 0.0f);
    EXPECT_FLOAT_EQ((*normals)[0].y, 0.0f);
    EXPECT_FLOAT_EQ((*normals)[0].z, 1.0f);
    EXPECT_TRUE(normalsAreFinite(root.get()));
}

/**
 * @brief A malformed OPTIONAL normal channel (non-divisible payload) is
 * reported and ignored: the mesh still draws, deriving normals from positions.
 */
TEST(GeometrySafetyTest, MalformedOptionalNormalStillDraws)
{
    vine::vsg::SceneBridge bridge;
    bridge.setShaderSet(vsg::createPhongShaderSet());
    auto root     = vsg::Group::create();
    auto material = MaterialPtr(new Material());

    auto geom = makePackedGeometry(triangleFloats(), 3u,
                                   std::make_shared<vine::geometry::UInt32Array>(
                                       vine::geometry::UInt32Array{ 0u, 1u, 2u }));
    // 5 floats with a 4-component stride is not divisible -> normal ignored.
    attachNormal(geom.get(), { 0.0f, 0.0f, 1.0f, 0.0f, 0.0f }, 4u);

    std::vector<vsg::ref_ptr<vsg::Node>> created;
    bridge.syncRenderCommands(std::vector<RenderCommand>{ RenderCommand(geom, material, Mat4d()) },
                              root.get(), &created);

    ASSERT_EQ(root->children.size(), 1u);
    EXPECT_TRUE(normalsAreFinite(root.get())); // derived, not NaN
}

/**
 * @brief A too-short normal channel (components < 3, e.g. a UV misplaced on
 * location 1) is ignored rather than killing the mesh; normals are derived.
 */
TEST(GeometrySafetyTest, ShortNormalChannelDerivesNormals)
{
    vine::vsg::SceneBridge bridge;
    bridge.setShaderSet(vsg::createPhongShaderSet());
    auto root     = vsg::Group::create();
    auto material = MaterialPtr(new Material());

    auto geom = makePackedGeometry(triangleFloats(), 3u,
                                   std::make_shared<vine::geometry::UInt32Array>(
                                       vine::geometry::UInt32Array{ 0u, 1u, 2u }));
    attachNormal(geom.get(), { 0.1f, 0.2f, 0.1f, 0.2f, 0.1f, 0.2f }, 2u);

    std::vector<vsg::ref_ptr<vsg::Node>> created;
    bridge.syncRenderCommands(std::vector<RenderCommand>{ RenderCommand(geom, material, Mat4d()) },
                              root.get(), &created);

    ASSERT_EQ(root->children.size(), 1u);
    EXPECT_TRUE(normalsAreFinite(root.get())); // derived from positions
}

/**
 * @brief A rejected geometry is only re-evaluated when its data revision
 * changes (the rejection diagnostic is emitted once per revision, not every
 * frame): after the indices are fixed the same geometry draws again.
 */
TEST(GeometrySafetyTest, FixedDataRevisionRebuildsRejectedGeometry)
{
    vine::vsg::SceneBridge bridge;
    bridge.setShaderSet(vsg::createPhongShaderSet());
    auto root     = vsg::Group::create();
    auto material = MaterialPtr(new Material());

    auto geom = makePackedGeometry(triangleFloats(), 3u,
                                   std::make_shared<vine::geometry::UInt32Array>(
                                       vine::geometry::UInt32Array{ 0u, 1u, 5u }));

    std::vector<vsg::ref_ptr<vsg::Node>> created;
    bridge.syncRenderCommands(std::vector<RenderCommand>{ RenderCommand(geom, material, Mat4d()) },
                              root.get(), &created);
    ASSERT_EQ(root->children.size(), 0u); // rejected on first sync

    // Second frame, data unchanged: still rejected, no rebuild churn.
    created.clear();
    bridge.syncRenderCommands(std::vector<RenderCommand>{ RenderCommand(geom, material, Mat4d()) },
                              root.get(), &created);
    ASSERT_EQ(root->children.size(), 0u);
    EXPECT_EQ(created.size(), 0u);

    // Fix the indices (bumps the revision): the geometry is rebuilt and drawn.
    geom->setIndices(vine::geometry::UInt32Array{ 0u, 1u, 2u });
    created.clear();
    bridge.syncRenderCommands(std::vector<RenderCommand>{ RenderCommand(geom, material, Mat4d()) },
                              root.get(), &created);
    ASSERT_EQ(root->children.size(), 1u);
    ASSERT_NE(findDrawIndexed(root.get()), nullptr);
    EXPECT_EQ(findDrawIndexed(root.get())->indexCount, 3u);
}

/**
 * @brief An indexed Topology::Points draw keeps EVERY index: the data builder
 * must not truncate a point stream to whole triangles (a point needs only one
 * index). Index count equals the source buffer length.
 */
TEST(GeometrySafetyTest, IndexedPointsKeepAllIndices)
{
    vine::vsg::SceneBridge bridge;
    bridge.setShaderSet(vsg::createPhongShaderSet());
    auto root     = vsg::Group::create();
    auto material = MaterialPtr(new Material());

    auto geom = makePackedGeometry(
        triangleFloats(), 3u,
        std::make_shared<vine::geometry::UInt32Array>(
            vine::geometry::UInt32Array{ 0u, 1u, 2u, 0u, 1u, 2u, 0u }));

    RenderCommand cmd(geom, material, Mat4d());
    cmd.renderState.topology = Topology::Points;
    std::vector<vsg::ref_ptr<vsg::Node>> created;
    bridge.syncRenderCommands(std::vector<RenderCommand>{ cmd }, root.get(), &created);

    ASSERT_EQ(root->children.size(), 1u);
    auto* draw = findDrawIndexed(root.get());
    ASSERT_NE(draw, nullptr);
    EXPECT_EQ(draw->indexCount, 7u); // every point index kept
    EXPECT_TRUE(normalsAreFinite(root.get())); // defaulted, not triangle-derived
}

/**
 * @brief An indexed Topology::Lines draw keeps EVERY index (two indices per
 * line); a 4-index line stream is never truncated to 3 by triangle rules.
 */
TEST(GeometrySafetyTest, IndexedLinesKeepAllIndices)
{
    vine::vsg::SceneBridge bridge;
    bridge.setShaderSet(vsg::createPhongShaderSet());
    auto root     = vsg::Group::create();
    auto material = MaterialPtr(new Material());

    // Four vertices -> indices {0,1,2,3} are two lines.
    auto geom = makePackedGeometry(
        { 0.0f, 0.0f, 0.0f, 1.0f, 0.0f, 0.0f, 0.0f, 1.0f, 0.0f, 1.0f, 1.0f, 0.0f },
        3u,
        std::make_shared<vine::geometry::UInt32Array>(
            vine::geometry::UInt32Array{ 0u, 1u, 2u, 3u }));

    RenderCommand cmd(geom, material, Mat4d());
    cmd.renderState.topology = Topology::Lines;
    std::vector<vsg::ref_ptr<vsg::Node>> created;
    bridge.syncRenderCommands(std::vector<RenderCommand>{ cmd }, root.get(), &created);

    ASSERT_EQ(root->children.size(), 1u);
    auto* draw = findDrawIndexed(root.get());
    ASSERT_NE(draw, nullptr);
    EXPECT_EQ(draw->indexCount, 4u); // two full lines, nothing truncated
    EXPECT_TRUE(normalsAreFinite(root.get())); // defaulted, not triangle-derived
}


// ============ Retained-cache identity and in-flight lifetime ============

namespace
{

/**
 * @brief Geometry that counts live instances.
 *
 * Used to observe who owns a geometry: the bridge's retained cache must hold a
 * reference (that is what keeps its pointer key valid), and must release it
 * once nothing else references the geometry any more.
 */
class TrackedGeometry : public Geometry
{
  public:
    TrackedGeometry() { ++alive; }
    ~TrackedGeometry() override { --alive; }

    /// Instances currently alive.
    static inline int alive = 0;
};

}  // namespace

/**
 * @brief The retained cache owns the geometry it is keyed by.
 *
 * The per-geometry cache is keyed by the geometry's address, but it cannot
 * observe a geometry being destroyed. If it merely borrowed the key, an entry
 * would outlive its geometry and a later geometry allocated at the recycled
 * address would be served the dead entry's retained node (drawing the old mesh,
 * or being skipped by a stale rejection record). Holding the reference keeps
 * the key stable, and the entry is dropped as soon as the cache is the only
 * owner left, so an abandoned geometry is still released promptly.
 */
TEST(GeometrySafetyTest, RetainedCacheOwnsTheGeometryItIsKeyedBy)
{
    vine::vsg::SceneBridge bridge;
    bridge.setShaderSet(vsg::createPhongShaderSet());
    auto root     = vsg::Group::create();
    auto material = MaterialPtr(new Material());

    TrackedGeometry::alive = 0;
    {
        GeometryPtr geom(new TrackedGeometry());
        geom->setPositions(vine::geometry::Vec3fArray{ vine::math::Vec3f(0.0f, 0.0f, 0.0f),
                                                        vine::math::Vec3f(1.0f, 0.0f, 0.0f),
                                                        vine::math::Vec3f(0.0f, 1.0f, 0.0f) });
        bridge.syncRenderCommands(std::vector<RenderCommand>{ RenderCommand(geom, material, Mat4d()) },
                                  root.get(), nullptr);
        ASSERT_EQ(root->children.size(), 1u);
        EXPECT_EQ(TrackedGeometry::alive, 1);
    }

    // The app dropped its reference: the cache still owns the geometry, so the
    // address cannot be recycled under the entry's key.
    EXPECT_EQ(TrackedGeometry::alive, 1);

    // A frame that no longer draws it drops the abandoned entry (the cache was
    // the last owner) and releases the geometry with it.
    bridge.syncRenderCommands(std::vector<RenderCommand>{}, root.get(), nullptr);
    EXPECT_EQ(TrackedGeometry::alive, 0);
    EXPECT_TRUE(root->children.empty());
}

/**
 * @brief A replaced retained node is parked, then released after the ring.
 *
 * Rebuilding a geometry's vertex data replaces its retained data node, whose
 * VkBuffers may still be referenced by a command buffer the GPU is executing.
 * The old node must therefore stay alive until every command-buffer slot that
 * could reference it has been re-recorded (each re-record waits on its fence),
 * which is what SceneBridge::advanceRetireRing() accounts for.
 */
TEST(GeometrySafetyTest, ReplacedDataNodeIsParkedUntilTheRingAdvances)
{
    vine::vsg::SceneBridge bridge;
    bridge.setShaderSet(vsg::createPhongShaderSet());
    auto root     = vsg::Group::create();
    auto material = MaterialPtr(new Material());

    auto geom = makePackedGeometry(triangleFloats(), 3u);
    bridge.syncRenderCommands(std::vector<RenderCommand>{ RenderCommand(geom, material, Mat4d()) },
                              root.get(), nullptr);
    ASSERT_EQ(root->children.size(), 1u);

    // transform -> state wrapper -> retained data node.
    auto* transform = root->children.front()->cast<vsg::MatrixTransform>();
    ASSERT_NE(transform, nullptr);
    ASSERT_FALSE(transform->children.empty());
    auto* wrapper = transform->children.front()->cast<vsg::StateGroup>();
    ASSERT_NE(wrapper, nullptr);
    ASSERT_FALSE(wrapper->children.empty());
    vsg::ref_ptr<vsg::Node> old_data = wrapper->children.front();
    ASSERT_NE(old_data, nullptr);

    // New vertex data (bumps the revision): the data node is rebuilt, and the
    // replaced one is parked rather than destroyed.
    geom->setPositions(vine::geometry::Vec3fArray{ vine::math::Vec3f(0.0f, 0.0f, 0.0f),
                                                    vine::math::Vec3f(2.0f, 0.0f, 0.0f),
                                                    vine::math::Vec3f(0.0f, 2.0f, 0.0f) });
    bridge.syncRenderCommands(std::vector<RenderCommand>{ RenderCommand(geom, material, Mat4d()) },
                              root.get(), nullptr);
    ASSERT_FALSE(wrapper->children.empty());
    EXPECT_NE(wrapper->children.front().get(), old_data.get());
    EXPECT_GT(old_data->referenceCount(), 1u); // parked: still owned by the ring

    // Released once every slot that could reference it has been re-recorded.
    for (std::size_t i = 0; i < vine::vsg::VsgRetireRing::kRetireRingDepth; ++i) {
        bridge.advanceRetireRing();
    }
    EXPECT_EQ(old_data->referenceCount(), 1u); // only this test still holds it
}

// ============ Diagnostics channel (failures must not be silent) ============

namespace
{

/// Collects the diagnostics a bridge (or backend) reports.
struct CapturedDiagnostics
{
    std::vector<RenderDiagnostic> items;

    /// Installs this collector as the sink of @p bridge.
    void installOn(vine::vsg::SceneBridge& bridge)
    {
        bridge.setDiagnosticSink([this](const RenderDiagnostic& diagnostic) {
            items.push_back(diagnostic);
        });
    }

    /// Number of captured diagnostics in @p category.
    std::size_t count(DiagnosticCategory category) const
    {
        std::size_t n = 0;
        for (const auto& item : items) {
            if (item.category == category) {
                ++n;
            }
        }
        return n;
    }
};

}  // namespace

/**
 * @brief A rejected geometry is reported to the host's sink, once per revision.
 *
 * The backend used to only print to stderr: a host had no way to learn that
 * content it asked to draw is missing. The rejection must reach the sink with
 * the right category/severity, exactly once per data revision (not per frame,
 * so a broken mesh in a live scene does not flood the log), and stop once the
 * data is fixed.
 */
TEST(DiagnosticsTest, RejectedGeometryIsReportedOncePerRevision)
{
    vine::vsg::SceneBridge bridge;
    bridge.setShaderSet(vsg::createPhongShaderSet());
    auto root     = vsg::Group::create();
    auto material = MaterialPtr(new Material());

    CapturedDiagnostics captured;
    captured.installOn(bridge);

    auto geom = makePackedGeometry(triangleFloats(), 3u,
                                   std::make_shared<vine::geometry::UInt32Array>(
                                       vine::geometry::UInt32Array{ 0u, 1u, 5u }));

    // First sync: rejected -> one Error/GeometryRejected diagnostic.
    bridge.syncRenderCommands(std::vector<RenderCommand>{ RenderCommand(geom, material, Mat4d()) },
                              root.get(), nullptr);
    ASSERT_EQ(root->children.size(), 0u);
    ASSERT_EQ(captured.items.size(), 1u);
    EXPECT_EQ(captured.items[0].severity, DiagnosticSeverity::Error);
    EXPECT_EQ(captured.items[0].category, DiagnosticCategory::GeometryRejected);
    EXPECT_FALSE(captured.items[0].message.empty());
    EXPECT_EQ(bridge.diagnosticCount(), 1u);
    EXPECT_EQ(bridge.diagnosticCount(DiagnosticCategory::GeometryRejected), 1u);

    // Same revision, another frame: still rejected, but not reported again.
    bridge.syncRenderCommands(std::vector<RenderCommand>{ RenderCommand(geom, material, Mat4d()) },
                              root.get(), nullptr);
    EXPECT_EQ(captured.items.size(), 1u);
    EXPECT_EQ(bridge.diagnosticCount(), 1u);

    // Fixed data (bumps the revision): drawn again, and no further diagnostic.
    geom->setIndices(vine::geometry::UInt32Array{ 0u, 1u, 2u });
    bridge.syncRenderCommands(std::vector<RenderCommand>{ RenderCommand(geom, material, Mat4d()) },
                              root.get(), nullptr);
    ASSERT_EQ(root->children.size(), 1u);
    EXPECT_EQ(captured.items.size(), 1u);

    // Broken again with a NEW revision: reported again.
    geom->setIndices(vine::geometry::UInt32Array{ 0u, 1u, 9u });
    bridge.syncRenderCommands(std::vector<RenderCommand>{ RenderCommand(geom, material, Mat4d()) },
                              root.get(), nullptr);
    ASSERT_EQ(captured.items.size(), 2u);
    EXPECT_EQ(bridge.diagnosticCount(DiagnosticCategory::GeometryRejected), 2u);
}

/**
 * @brief A dropped channel is a Warning, and the mesh is still drawn.
 *
 * The distinction matters to a host: ChannelIgnored means "looks wrong", while
 * GeometryRejected means "missing entirely".
 */
TEST(DiagnosticsTest, DroppedChannelWarnsButStillDraws)
{
    vine::vsg::SceneBridge bridge;
    bridge.setShaderSet(vsg::createPhongShaderSet());
    auto root     = vsg::Group::create();
    auto material = MaterialPtr(new Material());

    CapturedDiagnostics captured;
    captured.installOn(bridge);

    auto geom = makePackedGeometry(triangleFloats(), 3u);
    // A malformed custom channel (2 vertices where the mesh has 3): dropped.
    AttributeBuffer bad;
    bad.components = 4u;
    bad.data       = packedFloats({ 0.0f, 0.0f, 0.0f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f });
    geom->addBuffer(4u, bad);

    bridge.syncRenderCommands(std::vector<RenderCommand>{ RenderCommand(geom, material, Mat4d()) },
                              root.get(), nullptr);

    ASSERT_EQ(root->children.size(), 1u); // still drawn
    ASSERT_EQ(captured.items.size(), 1u);
    EXPECT_EQ(captured.items[0].severity, DiagnosticSeverity::Warning);
    EXPECT_EQ(captured.items[0].category, DiagnosticCategory::ChannelIgnored);
    EXPECT_EQ(bridge.diagnosticCount(DiagnosticCategory::ChannelIgnored), 1u);
}

/**
 * @brief A clean frame reports nothing, and clearing the sink stops delivery.
 *
 * The channel must be usable as a "did anything unexpected happen" gate: a
 * valid scene produces no diagnostics at all (no per-frame noise), and a host
 * that clears the sink stops receiving them without the counting stopping.
 */
TEST(DiagnosticsTest, CleanFrameIsSilentAndSinkCanBeCleared)
{
    vine::vsg::SceneBridge bridge;
    bridge.setShaderSet(vsg::createPhongShaderSet());
    auto root     = vsg::Group::create();
    auto material = MaterialPtr(new Material());

    CapturedDiagnostics captured;
    captured.installOn(bridge);

    // A well-formed mesh, a material and a custom channel: nothing to report.
    auto geom = makePackedGeometry(triangleFloats(), 3u);
    AttributeBuffer colour;
    colour.components = 3u;
    colour.data       = packedFloats({ 1, 0, 0, 0, 1, 0, 0, 0, 1 });
    geom->addBuffer(3u, colour);

    for (int frame = 0; frame < 3; ++frame) {
        bridge.syncRenderCommands(std::vector<RenderCommand>{ RenderCommand(geom, material, Mat4d()) },
                                  root.get(), nullptr);
    }
    EXPECT_EQ(root->children.size(), 1u);
    EXPECT_TRUE(captured.items.empty());
    EXPECT_EQ(bridge.diagnosticCount(), 0u);

    // A bad mesh still counts after the sink is cleared, but is no longer
    // delivered (and the bridge falls back to its stderr trace).
    geom->setIndices(vine::geometry::UInt32Array{ 0u, 1u, 7u });
    bridge.setDiagnosticSink({});
    bridge.syncRenderCommands(std::vector<RenderCommand>{ RenderCommand(geom, material, Mat4d()) },
                              root.get(), nullptr);
    EXPECT_TRUE(captured.items.empty());
    EXPECT_EQ(bridge.diagnosticCount(DiagnosticCategory::GeometryRejected), 1u);
}
