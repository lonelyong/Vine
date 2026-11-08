#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <cstdint>
#include <limits>
#include <span>
#include <vector>

#include <vine/geom/spatial/BvhBuilder.hpp>
#include <vine/geom/spatial/TriangleBvh.hpp>
#include <vine/math/Vector3.hpp>
#include <vine/models/TriangleMesh.hpp>

namespace
{

using vn::geom::spatial::BvhTopologyView;
using vn::geom::spatial::TriangleSource;

using Vec3f = vn::math::Vec3f;

/**
 * @brief A triangle set that is nothing but the three facts a triangle source owes.
 *
 * @details
 * - It exists to prove the contract is the SMALL one: no mesh, no draw, no topology enum, no buffers - just vertices,
 *   a count and the corners of one triangle. If this type models `TriangleSource`, the concept describes triangles and
 *   not some library class.
 */
struct CornerList {
    std::vector<Vec3f>          vertices;
    std::vector<std::uint32_t> corners;

    [[nodiscard]] std::span<const Vec3f> positions() const { return vertices; }

    [[nodiscard]] std::size_t triangleCount() const { return corners.size() / 3u; }

    [[nodiscard]] bool triangleCorners(std::size_t triangle, std::array<std::uint32_t, 3>& out) const
    {
        if (triangle >= triangleCount()) {
            return false;
        }
        for (std::size_t corner = 0; corner < 3u; ++corner) {
            out[corner] = corners[triangle * 3u + corner];
        }
        return true;
    }
};

static_assert(TriangleSource<CornerList>, "a plain vertex set and a corner list is all a triangle tree needs");
static_assert(TriangleSource<vn::models::Mesh>, "a mesh states the same three facts, so it is a triangle source too");

/**
 * @brief A flat strip of @p count quads in the z=0 plane, two triangles each, in triangle order.
 *
 * @param count Quads along x.
 * @return The triangle set.
 */
CornerList strip(std::size_t count)
{
    CornerList list;
    for (std::size_t quad = 0; quad <= count; ++quad) {
        const float x = static_cast<float>(quad) * 2.0f;
        list.vertices.emplace_back(x, 0.0f, 0.0f);
        list.vertices.emplace_back(x, 1.0f, 0.0f);
    }
    for (std::size_t quad = 0; quad < count; ++quad) {
        const auto a = static_cast<std::uint32_t>(quad * 2u);
        const auto b = static_cast<std::uint32_t>(quad * 2u + 1u);
        list.corners.insert(list.corners.end(), { a, b, a + 2u, b, b + 2u, a + 2u });
    }
    return list;
}

/// Every primitive id a ray visits in @p tree, in visit order.
std::vector<std::uint32_t> visitedBy(const vn::geom::spatial::Bvh<double>& tree, const vn::geom::spatial::Rayd& ray)
{
    std::vector<std::uint32_t> ids;
    tree.forEachRay(ray, [&ids](std::uint32_t id, const vn::geom::spatial::Aabbd&, double&) {
        ids.push_back(id);
        return true;
    });
    return ids;
}

} // namespace

TEST(TriangleBvhTest, ThePrimitiveIdIsTheTriangleOrdinal)
{
    const CornerList source = strip(3u);
    ASSERT_EQ(source.triangleCount(), 6u);

    vn::geom::spatial::Bvh<double> tree;
    ASSERT_TRUE(vn::geom::spatial::registerTriangles(tree, source));
    EXPECT_EQ(tree.primCount(), source.triangleCount());
    EXPECT_FALSE(tree.isBuilt()) << "registration is data; the tree is built by buildTriangleBvh() or not at all";

    vn::geom::spatial::BvhBuilder{}.build(tree);
    ASSERT_TRUE(tree.isBuilt());
    ASSERT_TRUE(tree.validate());

    // A ray straight down onto the first triangle's midpoint: the tree must report THAT ordinal, not merely a leaf.
    const vn::geom::spatial::Rayd ray(vn::math::Point3d(1.0, 0.5, 5.0), vn::math::Vector3<double>(0.0, 0.0, -1.0));
    const std::vector<std::uint32_t> ids = visitedBy(tree, ray);
    EXPECT_NE(std::find(ids.begin(), ids.end(), 0u), ids.end()) << "primitive 0 is the first triangle of the strip";
}

TEST(TriangleBvhTest, BuildingATriangleSourceIsOneCall)
{
    const CornerList source = strip(4u);

    vn::geom::spatial::Bvh<double> tree;
    ASSERT_TRUE(vn::geom::spatial::buildTriangleBvh(tree, source));
    EXPECT_TRUE(tree.isBuilt());
    EXPECT_TRUE(tree.validate());
    EXPECT_EQ(tree.primCount(), source.triangleCount());
}

TEST(TriangleBvhTest, ATriangleSourceWithABadCornerIsRefusedRatherThanThrownFrom)
{
    CornerList source = strip(2u);
    source.corners[3] = 99u; // a corner that names no stored vertex

    vn::geom::spatial::Bvh<double> tree;
    EXPECT_FALSE(vn::geom::spatial::registerTriangles(tree, source));
    EXPECT_FALSE(tree.isBuilt()) << "a half-registered tree is never built: the caller drops the object it was filling";
}

TEST(TriangleBvhTest, ANonFiniteCornerIsNoTreeInsteadOfAnException)
{
    // The rule both frames share: a corner that is not finite yields "no tree" - the linear walk is the answer - rather
    // than a volume that Bvh::add() rejects with an exception out of a pick.
    CornerList source = strip(2u);
    source.vertices[2].x = std::numeric_limits<float>::quiet_NaN();

    vn::geom::spatial::Bvh<double> tree;
    EXPECT_FALSE(vn::geom::spatial::buildTriangleBvh(tree, source));
    EXPECT_FALSE(tree.isBuilt());
}

TEST(TriangleBvhTest, ARestoredShapeAnswersLikeABuiltOne)
{
    const CornerList source = strip(12u);

    vn::geom::spatial::Bvh<double> built;
    ASSERT_TRUE(vn::geom::spatial::buildTriangleBvh(built, source));
    const vn::geom::spatial::BvhTopology shape = built.topology();

    vn::geom::spatial::Bvh<double> restored;
    ASSERT_TRUE(vn::geom::spatial::restoreTriangleBvh(restored, source, shape.view()));
    EXPECT_TRUE(restored.validate());
    EXPECT_EQ(restored.topology(), shape) << "nodes, order and height are one value: restore keeps all three";

    // Every ray that reaches the built tree reaches the restored one with the same primitive ids.
    for (std::size_t quad = 0; quad < 12u; ++quad) {
        const vn::geom::spatial::Rayd ray(vn::math::Point3d(static_cast<double>(quad) * 2.0 + 1.0, 0.5, 5.0),
                                          vn::math::Vector3<double>(0.0, 0.0, -1.0));
        EXPECT_EQ(visitedBy(restored, ray), visitedBy(built, ray));
    }
}

TEST(TriangleBvhTest, AShapeForAnotherTriangleSetIsRefused)
{
    const CornerList twelve = strip(12u);
    const CornerList two    = strip(2u);

    vn::geom::spatial::Bvh<double> reference;
    ASSERT_TRUE(vn::geom::spatial::buildTriangleBvh(reference, twelve));
    const vn::geom::spatial::BvhTopology shape = reference.topology();

    vn::geom::spatial::Bvh<double> tree;
    EXPECT_FALSE(vn::geom::spatial::restoreTriangleBvh(tree, two, shape.view()))
        << "a grouping over another triangle set means nothing here, so no tree is built";
    EXPECT_FALSE(tree.isBuilt());
}

TEST(TriangleBvhTest, ATreeThatAlreadyHasPrimitivesIsNotRenumbered)
{
    const CornerList source = strip(2u);

    vn::geom::spatial::Bvh<double> tree;
    ASSERT_TRUE(vn::geom::spatial::registerTriangles(tree, source));
    EXPECT_FALSE(vn::geom::spatial::registerTriangles(tree, source))
        << "ids start at 0, so a second registration would shift every ordinal the first one handed out";
    EXPECT_EQ(tree.primCount(), source.triangleCount());
}
