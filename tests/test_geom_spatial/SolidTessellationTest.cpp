#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <map>
#include <type_traits>
#include <utility>
#include <vector>

#include <vine/geom/spatial/BoxSolid.hpp>
#include <vine/geom/spatial/ConeSolid.hpp>
#include <vine/geom/spatial/CylinderSolid.hpp>
#include <vine/geom/spatial/EllipsoidSolid.hpp>
#include <vine/geom/spatial/SolidTessellation.hpp>
#include <vine/geom/spatial/SphereSolid.hpp>
#include <vine/math/Vector3.hpp>
#include <vine/models/PrimitiveDispatch.hpp>
#include <vine/models/SolidTessellation.hpp>
#include <vine/models/TriangleMesh.hpp>

namespace
{

using vn::geom::spatial::BoxSolidf;
using vn::geom::spatial::BoxSolidd;
using vn::geom::spatial::ConeSolidf;
using vn::geom::spatial::CylinderSolidf;
using vn::geom::spatial::EllipsoidSolidf;
using vn::geom::spatial::SphereSolidf;
using vn::geom::spatial::SphereSolidd;
using vn::geom::spatial::tessellateSolid;
using vn::geom::spatial::TessellationQuality;
using vn::geom::spatial::Triangulationd;
using vn::geom::spatial::Triangulationf;
using vn::math::Vec3f;
using vn::models::meshFromTriangulation;

/// One vertex position rounded to a tenth of a millimetre, so duplicated vertices at one place collapse to one key.
using PositionKey = std::array<long long, 3>;

PositionKey keyOf(const Vec3f& position)
{
    return { std::llround(static_cast<double>(position.x) * 10000.0),
             std::llround(static_cast<double>(position.y) * 10000.0),
             std::llround(static_cast<double>(position.z) * 10000.0) };
}

/// Asserts that every index names a stored vertex and that every geometric edge is shared by exactly two triangles.
void expectClosed(const Triangulationf& triangles)
{
    ASSERT_EQ(triangles.indices.size() % 3u, 0u);
    ASSERT_EQ(triangles.normals.size(), triangles.positions.size());

    for (const std::uint32_t index : triangles.indices) {
        ASSERT_LT(index, triangles.positions.size()) << "an index names a vertex that is not stored";
    }

    std::map<std::pair<PositionKey, PositionKey>, int> edges;
    for (std::size_t corner = 0; corner + 2 < triangles.indices.size(); corner += 3) {
        const std::uint32_t triangle[3] = { triangles.indices[corner], triangles.indices[corner + 1], triangles.indices[corner + 2] };
        for (int edge = 0; edge < 3; ++edge) {
            auto first  = keyOf(triangles.positions[triangle[edge]]);
            auto second = keyOf(triangles.positions[triangle[(edge + 1) % 3]]);
            if (second < first) {
                std::swap(first, second);
            }
            ++edges[{ first, second }];
        }
    }

    for (const auto& [edge, count] : edges) {
        (void)edge;
        EXPECT_EQ(count, 2) << "an edge is not shared by exactly two triangles";
    }
}

/// Asserts that every normal is a unit vector and points away from the origin, which is inside every tested solid.
void expectOutwardUnitNormals(const Triangulationf& triangles)
{
    ASSERT_EQ(triangles.normals.size(), triangles.positions.size());

    for (std::size_t index = 0; index < triangles.positions.size(); ++index) {
        EXPECT_NEAR(static_cast<double>(triangles.normals[index].length()), 1.0, 1e-5);
        EXPECT_GT(triangles.normals[index].dot(triangles.positions[index]), 0.0f) << "a normal points into the solid";
    }
}

} // namespace

TEST(SolidTessellation, aBoxBecomesSixClosedQuads)
{
    // Six faces, four corners each because every face keeps its own normal: 24 vertices, 12 triangles.
    const auto triangles = tessellateSolid(BoxSolidf{ 1.0f, 2.0f, 3.0f });

    ASSERT_FALSE(triangles.empty());
    EXPECT_EQ(triangles.vertexCount(), 24U);
    EXPECT_EQ(triangles.triangleCount(), 12U);

    Vec3f low  = triangles.positions.front();
    Vec3f high = low;
    for (const auto& position : triangles.positions) {
        low  = Vec3f(std::min(low.x, position.x), std::min(low.y, position.y), std::min(low.z, position.z));
        high = Vec3f(std::max(high.x, position.x), std::max(high.y, position.y), std::max(high.z, position.z));
    }
    EXPECT_NEAR(low.x, -1.0f, 1e-6f);
    EXPECT_NEAR(high.x, 1.0f, 1e-6f);
    EXPECT_NEAR(low.y, -2.0f, 1e-6f);
    EXPECT_NEAR(high.y, 2.0f, 1e-6f);
    EXPECT_NEAR(low.z, -3.0f, 1e-6f);
    EXPECT_NEAR(high.z, 3.0f, 1e-6f);

    expectOutwardUnitNormals(triangles);
    expectClosed(triangles);
}

TEST(SolidTessellation, aSphereBecomesAClosedBandBetweenItsPoles)
{
    // around x (along - 1) triangles per band, plus one fan of `around` triangles at each pole.
    const auto triangles = tessellateSolid(SphereSolidf{ 1.0f }, TessellationQuality{ 8, 4 });

    ASSERT_FALSE(triangles.empty());
    EXPECT_EQ(triangles.vertexCount(), 2U + 3U * 8U);
    EXPECT_EQ(triangles.triangleCount(), 2U * 8U * 3U);

    for (const auto& position : triangles.positions) {
        EXPECT_NEAR(static_cast<double>(position.length()), 1.0, 1e-5);
    }
    for (const auto& normal : triangles.normals) {
        EXPECT_NEAR(static_cast<double>(normal.length()), 1.0, 1e-5);
    }
    expectOutwardUnitNormals(triangles);
    expectClosed(triangles);
}

TEST(SolidTessellation, aCylinderHasASideBandAndTwoCaps)
{
    // 2 * around side vertices, two cap centres and 2 * around cap rim vertices; the rim is duplicated between the
    // side (radial normal) and the cap (axial normal).
    const auto triangles = tessellateSolid(CylinderSolidf{ 1.0f, 1.0f }, TessellationQuality{ 8, 2 });

    ASSERT_FALSE(triangles.empty());
    EXPECT_EQ(triangles.vertexCount(), 2U + 4U * 8U);
    EXPECT_EQ(triangles.triangleCount(), 2U * 8U + 2U * 8U);

    for (std::size_t index = 0; index < triangles.vertexCount(); ++index) {
        const auto position = triangles.positions[index];
        const auto normal   = triangles.normals[index];
        if (std::abs(normal.z) < 1e-6f) {
            // A side vertex: radial normal, and the point is on the radius.
            EXPECT_NEAR(static_cast<double>(std::hypot(position.x, position.y)), 1.0, 1e-5);
            EXPECT_NEAR(static_cast<double>(normal.z), 0.0, 1e-6);
        }
        else {
            // A cap vertex: axial normal, at the end of the axis.
            EXPECT_NEAR(static_cast<double>(std::abs(normal.z)), 1.0, 1e-6);
            EXPECT_NEAR(static_cast<double>(position.z), normal.z > 0.0f ? 1.0 : -1.0, 1e-6);
        }
    }
    expectOutwardUnitNormals(triangles);
    expectClosed(triangles);
}

TEST(SolidTessellation, aConeHasASlantedSideAndABase)
{
    // The side normal is (cos, sin, r / h) before normalisation: with r = 1, h = 2 its axial component is
    // 0.5 / sqrt(1.25) = 1 / sqrt(5).
    const auto   triangles      = tessellateSolid(ConeSolidf{ 1.0f, 1.0f }, TessellationQuality{ 8, 2 });
    const double expected_axial = 1.0 / std::sqrt(5.0);

    ASSERT_FALSE(triangles.empty());
    EXPECT_EQ(triangles.vertexCount(), 8U + 8U + 1U + 8U);
    EXPECT_EQ(triangles.triangleCount(), 8U + 8U);

    for (std::size_t index = 0; index < triangles.vertexCount(); ++index) {
        const auto normal = triangles.normals[index];
        if (normal.z < 0.0f) {
            EXPECT_NEAR(static_cast<double>(normal.z), -1.0, 1e-6);
        }
        else {
            EXPECT_NEAR(static_cast<double>(normal.z), expected_axial, 1e-6);
        }
    }
    expectOutwardUnitNormals(triangles);
    expectClosed(triangles);
}

TEST(SolidTessellation, anEllipsoidUsesItsThreeSemiAxes)
{
    // Every point satisfies (x / a)^2 + (y / b)^2 + (z / c)^2 = 1, and the normal is parallel to (x / a^2, ...).
    const auto triangles = tessellateSolid(EllipsoidSolidf{ 1.0f, 2.0f, 3.0f }, TessellationQuality{ 8, 4 });

    ASSERT_FALSE(triangles.empty());
    for (std::size_t index = 0; index < triangles.vertexCount(); ++index) {
        const auto   position = triangles.positions[index];
        const double x        = static_cast<double>(position.x);
        const double y        = static_cast<double>(position.y);
        const double z        = static_cast<double>(position.z);
        EXPECT_NEAR(x * x + y * y / 4.0 + z * z / 9.0, 1.0, 1e-5);

        const vn::math::Vec3f expected(static_cast<float>(x), static_cast<float>(y / 4.0), static_cast<float>(z / 9.0));
        EXPECT_NEAR(static_cast<double>(triangles.normals[index].dot(expected.normalized())), 1.0, 1e-5);
    }
    expectClosed(triangles);
}

TEST(SolidTessellation, theQualityIsClampedInsteadOfTrusted)
{
    // around < 3 cannot form a ring and along < 2 leaves no band; both are clamped, so the smallest sphere is an
    // octahedron rather than an empty or degenerate result.
    const auto triangles = tessellateSolid(SphereSolidf{ 1.0f }, TessellationQuality{ 0, 0 });

    ASSERT_FALSE(triangles.empty());
    EXPECT_EQ(triangles.triangleCount(), 6U);
    expectClosed(triangles);
}

TEST(SolidTessellation, anUnusableSolidAnswersAnEmptyValue)
{
    const BoxSolidf       degenerate_box{ 1.0f, 0.0f, 1.0f };
    const SphereSolidf    degenerate_sphere{ 0.0f };
    const CylinderSolidf  degenerate_cylinder{ 0.0f, 1.0f };
    const ConeSolidf      degenerate_cone{ 1.0f, 0.0f };
    const EllipsoidSolidf degenerate_ellipsoid{ 1.0f, 0.0f, 1.0f };

    EXPECT_TRUE(tessellateSolid(degenerate_box).empty());
    EXPECT_TRUE(tessellateSolid(degenerate_sphere).empty());
    EXPECT_TRUE(tessellateSolid(degenerate_cylinder).empty());
    EXPECT_TRUE(tessellateSolid(degenerate_cone).empty());
    EXPECT_TRUE(tessellateSolid(degenerate_ellipsoid).empty());
}

TEST(SolidTessellation, thePrecisionOfTheSolidIsThePrecisionOfTheTriangles)
{
    // The value type and the entries follow the solid's precision, the way every other geometry value does: a `Solidd`
    // answers `Triangulationd`, and nothing narrows it behind the caller's back.
    static_assert(std::is_same_v<decltype(tessellateSolid(BoxSolidd{ 1.0, 2.0, 3.0 })), Triangulationd>);
    static_assert(!std::is_same_v<Triangulationd, Triangulationf>);

    const auto triangles = tessellateSolid(SphereSolidd{ 2.0 }, TessellationQuality{ 8, 4 });
    ASSERT_FALSE(triangles.empty());
    EXPECT_EQ(triangles.triangleCount(), 2U * 8U * 3U);
    for (const auto& position : triangles.positions) {
        EXPECT_NEAR(position.length(), 2.0, 1e-12);
    }
}

TEST(SolidTessellation, aModelReachesTheMeshThroughItsSolid)
{
    // The modeling type has no triangulation of its own: it hands out its concrete kind, which is what the triangles are
    // built from, and the model side is what turns those triangles into a mesh.
    const vn::models::Box box(2.0f, 4.0f, 6.0f);

    vn::geom::spatial::Triangulationf triangles;
    const bool visited = vn::models::visitPrimitive(box, [&triangles](const auto& shape) {
        triangles = tessellateSolid(shape.solid());
    });
    ASSERT_TRUE(visited);
    const auto mesh = meshFromTriangulation(std::move(triangles));
    ASSERT_TRUE(mesh);
    EXPECT_EQ(mesh->triangleCount(), 12U);
    EXPECT_NEAR(mesh->aabb().max().y, 2.0f, 1e-6f);

    // A degenerate model still hands out its solid; the tessellator is what refuses it, and the adapter passes the
    // refusal on as a null mesh.
    const vn::models::Cylinder collapsed(0.0f, 2.0f);
    EXPECT_FALSE(collapsed.isValid());
    vn::geom::spatial::Triangulationf collapsed_triangles;
    const bool collapsed_visited = vn::models::visitPrimitive(collapsed, [&collapsed_triangles](const auto& shape) {
        collapsed_triangles = tessellateSolid(shape.solid());
    });
    ASSERT_TRUE(collapsed_visited);
    EXPECT_EQ(meshFromTriangulation(std::move(collapsed_triangles)).get(), nullptr);
}
