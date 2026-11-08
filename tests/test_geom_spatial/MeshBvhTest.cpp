#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory>
#include <utility>
#include <vector>

#include <vine/geom/spatial/Bvh.hpp>
#include <vine/geom/spatial/BvhTopology.hpp>
#include <vine/geom/spatial/Ray.hpp>
#include <vine/geom/spatial/RayHit.hpp>
#include <vine/intrusive_ptr.hpp>
#include <vine/math/Vector3.hpp>
#include <vine/models/Mesh.hpp>
#include <vine/models/MixedMesh.hpp>
#include <vine/models/QuadMesh.hpp>
#include <vine/models/TriangleMesh.hpp>

using vn::math::Point3d;
using vn::math::Vec3f;
using vn::models::Mesh;
using vn::models::MixedMesh;
using vn::models::QuadMesh;
using vn::models::TriangleMesh;

namespace {

/**
 * @brief Returns a `side x side` grid of unit quads in the z = 0 plane, as two triangles per cell.
 *
 * @param side Cells per axis.
 * @return Indexed triangle mesh with `2 * side * side` triangles.
 */
vn::intrusive_ptr<TriangleMesh> gridMesh(std::size_t side)
{
    vn::models::Vec3fArray positions;
    positions.reserve((side + 1u) * (side + 1u));
    for (std::size_t row = 0; row <= side; ++row) {
        for (std::size_t column = 0; column <= side; ++column) {
            positions.push_back(Vec3f(static_cast<float>(column), static_cast<float>(row), 0.0f));
        }
    }

    const std::uint32_t stride = static_cast<std::uint32_t>(side + 1u);
    vn::models::UInt32Array indices;
    indices.reserve(side * side * 6u);
    for (std::uint32_t row = 0; row < side; ++row) {
        for (std::uint32_t column = 0; column < side; ++column) {
            const std::uint32_t a = row * stride + column;
            const std::uint32_t b = a + 1u;
            const std::uint32_t c = a + stride + 1u;
            const std::uint32_t d = a + stride;
            indices.insert(indices.end(), { a, b, c, a, c, d });
        }
    }

    auto mesh = vn::make_intrusive<TriangleMesh>();
    mesh->setPositions(std::move(positions));
    mesh->setIndices(std::move(indices));
    return mesh;
}

/**
 * @brief Closest triangle a ray finds through a mesh's cached tree, and how many leaves the traversal offered.
 */
struct Pick
{
    std::uint32_t triangle{ std::numeric_limits<std::uint32_t>::max() };
    double        distance{ std::numeric_limits<double>::infinity() };
    std::size_t   visits{ 0 };
};

/**
 * @brief Picks the closest triangle by querying the cached tree and testing each candidate exactly.
 *
 * @details
 * - This is the shape a real picking caller has: the tree offers candidates, `intersectTriangle` decides, and the
 *   closest hit is written back as the new distance limit so the traversal prunes the rest.
 *
 * @param mesh Mesh whose tree is built.
 * @param ray  Ray in the mesh's local space.
 * @return Closest hit; `triangle` is unchanged when nothing was hit.
 */
Pick pickClosest(const Mesh& mesh, const vn::geom::spatial::Rayd& ray)
{
    Pick result;
    mesh.bvh().forEachRay(
        ray,
        [&](const std::uint32_t id, const vn::geom::spatial::Aabbd&, double& max_t) {
            ++result.visits;
            std::array<std::uint32_t, 3> corners{};
            if (!mesh.triangleCorners(id, corners)) {
                return true;
            }
            const auto positions = mesh.positions();
            const auto corner = [&](std::uint32_t index) {
                const auto& position = positions[index];
                return Point3d(static_cast<double>(position.x), static_cast<double>(position.y),
                               static_cast<double>(position.z));
            };
            vn::geom::spatial::RayHitd hit;
            if (!vn::geom::spatial::intersectTriangle(ray, corner(corners[0]), corner(corners[1]), corner(corners[2]),
                                                      hit)) {
                return true;
            }
            if (hit.t < result.distance) {
                result.distance = hit.t;
                result.triangle = id;
                max_t           = hit.t;
            }
            return true;
        });
    return result;
}

} // namespace

TEST(MeshBvhTest, TriangleCornersFollowTheTriangleOrdinal)
{
    auto mesh = vn::make_intrusive<TriangleMesh>();
    const Vec3f a(0.0f, 0.0f, 0.0f);
    const Vec3f b(1.0f, 0.0f, 0.0f);
    const Vec3f c(0.0f, 1.0f, 0.0f);
    const Vec3f d(2.0f, 0.0f, 0.0f);
    const Vec3f e(3.0f, 0.0f, 0.0f);
    const Vec3f f(2.0f, 1.0f, 0.0f);
    mesh->setPositions({ a, b, c, d, e, f });

    ASSERT_TRUE(mesh->isValid());
    EXPECT_EQ(mesh->triangleCount(), 2u);
    std::array<std::uint32_t, 3> corners{};
    ASSERT_TRUE(mesh->triangleCorners(0, corners));
    EXPECT_EQ(corners, (std::array<std::uint32_t, 3>{ 0u, 1u, 2u }));
    ASSERT_TRUE(mesh->triangleCorners(1, corners));
    EXPECT_EQ(corners, (std::array<std::uint32_t, 3>{ 3u, 4u, 5u }));
    EXPECT_FALSE(mesh->triangleCorners(2, corners)) << "there is no third triangle";

    // The corners come from the index buffer, not from the ordinal: shuffling it changes which vertices a triangle names.
    mesh->setIndices({ 3u, 4u, 5u, 0u, 1u, 2u });
    ASSERT_TRUE(mesh->isValid());
    ASSERT_TRUE(mesh->triangleCorners(0, corners));
    EXPECT_EQ(corners, (std::array<std::uint32_t, 3>{ 3u, 4u, 5u }));
    ASSERT_TRUE(mesh->triangleCorners(1, corners));
    EXPECT_EQ(corners, (std::array<std::uint32_t, 3>{ 0u, 1u, 2u }));

    // An out-of-range corner is reported, not silently wrapped.
    mesh->setIndices({ 0u, 1u, 9u });
    EXPECT_FALSE(mesh->triangleCorners(0, corners));
}

TEST(MeshBvhTest, EveryMeshTypeKeepsTheConvertedTriangleOrder)
{
    auto quad = vn::make_intrusive<QuadMesh>();
    quad->setPositions({ Vec3f(0, 0, 0), Vec3f(1, 0, 0), Vec3f(1, 1, 0), Vec3f(0, 1, 0), Vec3f(2, 1, 0),
                         Vec3f(2, 0, 0) });
    quad->setIndices({ 0u, 1u, 2u, 3u, 3u, 2u, 4u, 5u });
    ASSERT_TRUE(quad->isValid());
    ASSERT_EQ(quad->triangleCount(), 2u * quad->quadCount());

    const auto quad_triangles = quad->toTriangleMesh();
    ASSERT_EQ(quad_triangles->triangleCount(), quad->triangleCount());
    for (std::size_t triangle = 0; triangle < quad->triangleCount(); ++triangle) {
        std::array<std::uint32_t, 3> corners{};
        ASSERT_TRUE(quad->triangleCorners(triangle, corners)) << triangle;
        for (std::size_t corner = 0; corner < 3u; ++corner) {
            EXPECT_EQ(corners[corner], quad_triangles->indices()[triangle * 3u + corner])
                << "a quad triangle must keep its ordinal across toTriangleMesh(), triangle " << triangle;
        }
    }

    auto mixed = vn::make_intrusive<MixedMesh>();
    vn::models::Vec3fArray positions;
    for (int index = 0; index < 12; ++index) {
        positions.push_back(Vec3f(static_cast<float>(index % 4), static_cast<float>(index / 4), 0.0f));
    }
    mixed->setPositions(std::move(positions));
    mixed->setIndices({ 0u, 1u, 2u, 3u, 4u, 5u, 6u, 7u, 8u, 9u, 10u, 11u });
    mixed->setFaceOffsets({ 0u, 3u, 7u, 12u }); // a triangle, a quad and a pentagon
    ASSERT_TRUE(mixed->isValid());
    ASSERT_EQ(mixed->faceCount(), 3u);
    ASSERT_EQ(mixed->triangleCount(), 1u + 2u + 3u) << "a k-corner face fans into k - 2 triangles";

    const auto mixed_triangles = mixed->toTriangleMesh();
    ASSERT_EQ(mixed_triangles->triangleCount(), mixed->triangleCount());
    for (std::size_t triangle = 0; triangle < mixed->triangleCount(); ++triangle) {
        std::array<std::uint32_t, 3> corners{};
        ASSERT_TRUE(mixed->triangleCorners(triangle, corners)) << triangle;
        for (std::size_t corner = 0; corner < 3u; ++corner) {
            EXPECT_EQ(corners[corner], mixed_triangles->indices()[triangle * 3u + corner])
                << "a fan triangle must keep its ordinal across toTriangleMesh(), triangle " << triangle;
        }
    }
}

TEST(MeshBvhTest, BuildCachesOnePrimitivePerTriangle)
{
    auto mesh = gridMesh(4u);
    ASSERT_TRUE(mesh->isValid());
    EXPECT_FALSE(mesh->hasBvh()) << "nothing builds the tree by itself";
    EXPECT_EQ(mesh->sharedBvh(), nullptr) << "nothing is published before a build";
    EXPECT_FALSE(mesh->bvh().isBuilt()) << "the unbuilt tree is the empty one";

    ASSERT_TRUE(mesh->buildBvh());
    EXPECT_TRUE(mesh->hasBvh());
    ASSERT_NE(mesh->sharedBvh(), nullptr);
    EXPECT_EQ(&mesh->bvh(), mesh->sharedBvh().get()) << "bvh() reads the published object; it is not a second copy";
    EXPECT_EQ(mesh->bvh().primCount(), mesh->triangleCount());
    EXPECT_TRUE(mesh->bvh().validate()) << "the tree must satisfy its own containment and ordering invariants";

    // A ray straight down the middle of cell (1, 2) reaches the two triangles of that cell and nothing else.
    const vn::geom::spatial::Rayd ray(Point3d(1.25, 2.5, 5.0), vn::math::Vector3<double>(0.0, 0.0, -1.0));
    const Pick pick = pickClosest(*mesh, ray);
    EXPECT_DOUBLE_EQ(pick.distance, 5.0) << "the plane sits at z = 0 and the ray starts at z = 5";
    EXPECT_LT(pick.visits, 8u) << "a single-cell ray must not walk the whole mesh";

    std::array<std::uint32_t, 3> corners{};
    ASSERT_TRUE(mesh->triangleCorners(pick.triangle, corners));
    const auto positions = mesh->positions();
    for (const std::uint32_t index : corners) {
        EXPECT_GE(positions[index].x, 1.0f);
        EXPECT_LE(positions[index].x, 2.0f);
        EXPECT_GE(positions[index].y, 2.0f);
        EXPECT_LE(positions[index].y, 3.0f);
    }
}

TEST(MeshBvhTest, ASetterThatMovesGeometryDropsTheTreeAndASetterThatDoesNotKeepsIt)
{
    auto mesh = gridMesh(3u);
    ASSERT_TRUE(mesh->buildBvh());

    mesh->setNormals(vn::models::Vec3fArray(static_cast<std::size_t>(mesh->vertexCount()), Vec3f(0.0f, 0.0f, 1.0f)));
    EXPECT_TRUE(mesh->hasBvh()) << "normals do not move a corner, so the tree still describes the triangles";

    mesh->setPositions(vn::models::Vec3fArray(static_cast<std::size_t>(mesh->vertexCount()), Vec3f(0.0f, 0.0f, 0.0f)));
    EXPECT_FALSE(mesh->hasBvh()) << "new positions invalidate every leaf's box";

    ASSERT_TRUE(mesh->buildBvh());
    mesh->setIndices(vn::models::UInt32Array{ 0u, 1u, 2u });
    EXPECT_FALSE(mesh->hasBvh()) << "new indices re-name the corners the leaves were built from";

    auto mixed = vn::make_intrusive<MixedMesh>();
    mixed->setPositions({ Vec3f(0, 0, 0), Vec3f(1, 0, 0), Vec3f(0, 1, 0) });
    mixed->setFaceOffsets({ 0u, 3u });
    ASSERT_TRUE(mixed->isValid());
    ASSERT_TRUE(mixed->buildBvh());
    EXPECT_TRUE(mixed->hasBvh());
    mixed->setFaceOffsets({ 0u, 3u });
    EXPECT_FALSE(mixed->hasBvh()) << "the face boundaries are what turn corners into triangles";
}

TEST(MeshBvhTest, AnInvalidMeshIsNotBuilt)
{
    auto empty = vn::make_intrusive<TriangleMesh>();
    EXPECT_FALSE(empty->buildBvh());
    EXPECT_FALSE(empty->hasBvh());

    auto broken = vn::make_intrusive<TriangleMesh>();
    broken->setPositions({ Vec3f(0, 0, 0), Vec3f(1, 0, 0), Vec3f(0, 1, 0) });
    broken->setIndices({ 0u, 1u, 7u });
    ASSERT_FALSE(broken->isValid());
    EXPECT_FALSE(broken->buildBvh()) << "a tree over out-of-range corners would describe something else";
    EXPECT_FALSE(broken->hasBvh());
}

TEST(MeshBvhTest, AnAdoptedTreeIsUsedAsIs)
{
    auto mesh = vn::make_intrusive<TriangleMesh>();
    mesh->setPositions({ Vec3f(0, 0, 0), Vec3f(1, 0, 0), Vec3f(0, 1, 0) });
    ASSERT_TRUE(mesh->isValid());

    Mesh::Bvh adopted;
    adopted.add(vn::geom::spatial::Aabbd::compute(std::array<vn::math::Vector3<double>, 3>{
        vn::math::Vector3<double>(0.0, 0.0, 0.0), vn::math::Vector3<double>(1.0, 0.0, 0.0),
        vn::math::Vector3<double>(0.0, 1.0, 0.0) }));
    vn::geom::spatial::BvhBuilder{}.leafSize(1u).build(adopted);
    ASSERT_TRUE(adopted.isBuilt());

    EXPECT_TRUE(mesh->setBvh(std::move(adopted)));
    EXPECT_TRUE(mesh->hasBvh()) << "a caller that configures its own builder adopts the result";

    const Pick pick = pickClosest(*mesh, vn::geom::spatial::Rayd(Point3d(0.2, 0.2, 4.0),
                                                                vn::math::Vector3<double>(0.0, 0.0, -1.0)));
    EXPECT_EQ(pick.triangle, 0u);
    EXPECT_DOUBLE_EQ(pick.distance, 4.0);

    mesh->clearBvh();
    EXPECT_FALSE(mesh->hasBvh()) << "clearing drops the tree";
}

TEST(MeshBvhTest, AnAdoptedTreeThatCoversAnotherTriangleCountIsRefused)
{
    // The mesh frame's half of the admission rule a draw applies in Geometry::adoptBvh(): a tree over another triangle
    // set would report hits on triangles this mesh does not have, so it is refused rather than cached.
    auto mesh = gridMesh(4u);
    ASSERT_GT(mesh->triangleCount(), 1u);

    const auto one_triangle_tree = [] {
        Mesh::Bvh tree;
        tree.add(vn::geom::spatial::Aabbd::compute(std::array<vn::math::Vector3<double>, 3>{
            vn::math::Vector3<double>(0.0, 0.0, 0.0), vn::math::Vector3<double>(1.0, 0.0, 0.0),
            vn::math::Vector3<double>(0.0, 1.0, 0.0) }));
        vn::geom::spatial::BvhBuilder{}.leafSize(1u).build(tree);
        return tree;
    };

    Mesh::Bvh foreign = one_triangle_tree();
    ASSERT_TRUE(foreign.isBuilt());
    EXPECT_FALSE(mesh->setBvh(std::move(foreign)));
    EXPECT_FALSE(mesh->hasBvh()) << "a refused tree does not enter the cache";

    Mesh::Bvh unbuilt;
    unbuilt.add(vn::geom::spatial::Aabbd::compute(std::array<vn::math::Vector3<double>, 3>{
        vn::math::Vector3<double>(0.0, 0.0, 0.0), vn::math::Vector3<double>(1.0, 0.0, 0.0),
        vn::math::Vector3<double>(0.0, 1.0, 0.0) }));
    EXPECT_FALSE(mesh->setBvh(std::move(unbuilt))) << "an unbuilt tree answers nothing, so caching it would lie";
    EXPECT_FALSE(mesh->hasBvh());
}

TEST(MeshBvhTest, ARebuildPublishesANewObjectAndLeavesTheOldOneAlive)
{
    auto mesh = gridMesh(4u);
    ASSERT_TRUE(mesh->buildBvh());
    const std::shared_ptr<const Mesh::Bvh> first = mesh->sharedBvh();
    ASSERT_NE(first, nullptr);

    ASSERT_TRUE(mesh->buildBvh());
    EXPECT_NE(mesh->sharedBvh(), first)
        << "a rebuild publishes a new object: it cannot write into the one another frame is still reading";
    ASSERT_NE(mesh->sharedBvh(), nullptr);
    EXPECT_TRUE(mesh->sharedBvh()->validate());
    EXPECT_EQ(first->primCount(), mesh->triangleCount())
        << "the tree an earlier holder kept still describes the same triangles";

    mesh->clearBvh();
    EXPECT_FALSE(mesh->hasBvh());
    EXPECT_EQ(mesh->sharedBvh(), nullptr);
    EXPECT_TRUE(first->isBuilt()) << "dropping the cache stops publishing the object; it does not touch it";
}

TEST(MeshBvhTest, ARestoredTreeAnswersLikeARebuiltOne)
{
    // The persistence path: a tree is stored as its shape (nodes, primitive order, height), and the boxes are recomputed
    // from the current positions. The restored tree must answer every query the rebuilt one answers.
    auto reference = gridMesh(6u);
    ASSERT_TRUE(reference->buildBvh());
    const vn::geom::spatial::BvhTopology shape = reference->bvh().topology();
    ASSERT_FALSE(shape.nodes.empty());

    auto restored = gridMesh(6u);
    ASSERT_TRUE(restored->restoreBvh(shape));
    EXPECT_TRUE(restored->hasBvh());
    EXPECT_TRUE(restored->bvh().validate());
    EXPECT_EQ(restored->bvh().primCount(), restored->triangleCount());
    EXPECT_EQ(restored->bvh().topology(), shape) << "same nodes, same order, same height";

    // Same leaf grouping, same candidates, same closest hit.
    const vn::geom::spatial::Rayd ray(Point3d(2.25, 3.4, 5.0), vn::math::Vec3d(0.0, 0.0, -1.0));
    const Pick                    rebuilt   = pickClosest(*reference, ray);
    const Pick                    from_disk = pickClosest(*restored, ray);
    EXPECT_EQ(from_disk.triangle, rebuilt.triangle);
    EXPECT_DOUBLE_EQ(from_disk.distance, rebuilt.distance);
    EXPECT_EQ(from_disk.visits, rebuilt.visits) << "identical topology, identical traversal";
}

TEST(MeshBvhTest, ATopologyThatDoesNotDescribeTheMeshIsRefused)
{
    auto mesh = gridMesh(4u);
    ASSERT_TRUE(mesh->buildBvh());
    const vn::geom::spatial::BvhTopology shape = mesh->bvh().topology();

    // A different triangle count: the order cannot cover this mesh, so the shape describes another one.
    auto shorter = gridMesh(3u);
    EXPECT_FALSE(shorter->restoreBvh(shape));
    EXPECT_FALSE(shorter->hasBvh()) << "a refused restore leaves no tree behind";

    // An order that repeats an id is not a permutation.
    auto repeated       = shape;
    repeated.order[1]   = repeated.order[0];
    auto other          = gridMesh(4u);
    EXPECT_FALSE(other->restoreBvh(repeated));
    EXPECT_FALSE(other->hasBvh());

    // An empty mesh has no triangles to number.
    auto empty = gridMesh(0u);
    EXPECT_FALSE(empty->restoreBvh(shape));
}
