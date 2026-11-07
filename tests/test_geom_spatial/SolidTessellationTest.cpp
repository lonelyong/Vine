#include <gtest/gtest.h>

#include <array>
#include <cmath>
#include <cstdint>
#include <map>
#include <utility>
#include <vector>

#include <vine/models/Primitive.hpp>
#include <vine/models/PrimitiveDispatch.hpp>
#include <vine/geom/spatial/SolidShape.hpp>
#include <vine/models/SolidTessellation.hpp>
#include <vine/models/TriangleMesh.hpp>
#include <vine/math/Math.hpp>
#include <vine/math/Vector3.hpp>

namespace
{

using vn::geom::spatial::BoxSolidf;
using vn::geom::spatial::ConeSolidf;
using vn::geom::spatial::CylinderSolidf;
using vn::geom::spatial::EllipsoidSolidf;
using vn::models::Primitive;
using vn::geom::spatial::SphereSolidf;
using vn::models::tessellateSolid;
using vn::models::TessellationQuality;
using vn::models::TriangleMesh;
using vn::math::Vec3f;

/// One vertex position rounded to a tenth of a millimetre, so duplicated vertices at one place collapse to one key.
using PositionKey = std::array<long long, 3>;

PositionKey keyOf(const Vec3f& position)
{
    return { std::llround(static_cast<double>(position.x) * 10000.0),
             std::llround(static_cast<double>(position.y) * 10000.0),
             std::llround(static_cast<double>(position.z) * 10000.0) };
}

/// Asserts that every geometric edge is shared by exactly two triangles: the mesh is closed, with no boundary.
void expectClosed(const TriangleMesh& mesh)
{
    std::map<std::pair<PositionKey, PositionKey>, int> edges;
    const auto                                         positions = mesh.positions();
    const auto                                         indices   = mesh.indices();
    ASSERT_EQ(indices.size() % 3u, 0u);

    for (std::size_t corner = 0; corner + 2 < indices.size(); corner += 3) {
        const std::uint32_t triangle[3] = { indices[corner], indices[corner + 1], indices[corner + 2] };
        for (int edge = 0; edge < 3; ++edge) {
            auto first  = keyOf(positions[triangle[edge]]);
            auto second = keyOf(positions[triangle[(edge + 1) % 3]]);
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
void expectOutwardUnitNormals(const TriangleMesh& mesh)
{
    const auto positions = mesh.positions();
    const auto normals   = mesh.normals();
    ASSERT_EQ(normals.size(), positions.size());

    for (std::size_t index = 0; index < positions.size(); ++index) {
        EXPECT_NEAR(static_cast<double>(normals[index].length()), 1.0, 1e-5);
        EXPECT_GT(normals[index].dot(positions[index]), 0.0f) << "a normal points into the solid";
    }
}

} // namespace

TEST(SolidTessellation, aBoxBecomesSixClosedQuads)
{
    // Six faces, four corners each because every face keeps its own normal: 24 vertices, 12 triangles.
    const auto mesh = tessellateSolid(BoxSolidf{ 1.0f, 2.0f, 3.0f });

    ASSERT_TRUE(mesh);
    ASSERT_TRUE(mesh->isValid());
    EXPECT_TRUE(mesh->hasIndices());
    EXPECT_EQ(mesh->vertexCount(), 24U);
    EXPECT_EQ(mesh->triangleCount(), 12U);

    const auto bounds = mesh->aabb();
    EXPECT_NEAR(bounds.min().x, -1.0f, 1e-6f);
    EXPECT_NEAR(bounds.max().x, 1.0f, 1e-6f);
    EXPECT_NEAR(bounds.min().y, -2.0f, 1e-6f);
    EXPECT_NEAR(bounds.max().y, 2.0f, 1e-6f);
    EXPECT_NEAR(bounds.min().z, -3.0f, 1e-6f);
    EXPECT_NEAR(bounds.max().z, 3.0f, 1e-6f);

    expectOutwardUnitNormals(*mesh);
    expectClosed(*mesh);
}

TEST(SolidTessellation, aSphereBecomesAClosedBandBetweenItsPoles)
{
    // around x (along - 1) triangles per band, plus one fan of `around` triangles at each pole.
    const auto mesh = tessellateSolid(SphereSolidf{ 1.0f }, TessellationQuality{ 8, 4 });

    ASSERT_TRUE(mesh);
    ASSERT_TRUE(mesh->isValid());
    EXPECT_EQ(mesh->vertexCount(), 2U + 3U * 8U);
    EXPECT_EQ(mesh->triangleCount(), 2U * 8U * 3U);

    for (const auto& position : mesh->positions()) {
        EXPECT_NEAR(static_cast<double>(position.length()), 1.0, 1e-5);
    }
    for (const auto& normal : mesh->normals()) {
        EXPECT_NEAR(static_cast<double>(normal.length()), 1.0, 1e-5);
    }
    expectOutwardUnitNormals(*mesh);
    expectClosed(*mesh);
}

TEST(SolidTessellation, aCylinderHasASideBandAndTwoCaps)
{
    // 2 * around side vertices, two cap centres and 2 * around cap rim vertices; the rim is duplicated between the
    // side (radial normal) and the cap (axial normal).
    const auto mesh = tessellateSolid(CylinderSolidf{ 1.0f, 1.0f }, TessellationQuality{ 8, 2 });

    ASSERT_TRUE(mesh);
    ASSERT_TRUE(mesh->isValid());
    EXPECT_EQ(mesh->vertexCount(), 2U + 4U * 8U);
    EXPECT_EQ(mesh->triangleCount(), 2U * 8U + 2U * 8U);

    for (std::size_t index = 0; index < mesh->vertexCount(); ++index) {
        const auto position = mesh->positions()[index];
        const auto normal   = mesh->normals()[index];
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
    expectOutwardUnitNormals(*mesh);
    expectClosed(*mesh);
}

TEST(SolidTessellation, aConeHasASlantedSideAndABase)
{
    // The side normal is (cos, sin, r / h) before normalisation: with r = 1, h = 2 its axial component is
    // 0.5 / sqrt(1.25) = 1 / sqrt(5).
    const auto   mesh           = tessellateSolid(ConeSolidf{ 1.0f, 1.0f }, TessellationQuality{ 8, 2 });
    const double expected_axial = 1.0 / std::sqrt(5.0);

    ASSERT_TRUE(mesh);
    ASSERT_TRUE(mesh->isValid());
    EXPECT_EQ(mesh->vertexCount(), 8U + 8U + 1U + 8U);
    EXPECT_EQ(mesh->triangleCount(), 8U + 8U);

    for (std::size_t index = 0; index < mesh->vertexCount(); ++index) {
        const auto normal = mesh->normals()[index];
        if (normal.z < 0.0f) {
            EXPECT_NEAR(static_cast<double>(normal.z), -1.0, 1e-6);
        }
        else {
            EXPECT_NEAR(static_cast<double>(normal.z), expected_axial, 1e-6);
        }
    }
    expectOutwardUnitNormals(*mesh);
    expectClosed(*mesh);
}

TEST(SolidTessellation, anEllipsoidUsesItsThreeSemiAxes)
{
    // Every point satisfies (x / a)^2 + (y / b)^2 + (z / c)^2 = 1, and the normal is parallel to (x / a^2, ...).
    const auto mesh = tessellateSolid(EllipsoidSolidf{ 1.0f, 2.0f, 3.0f }, TessellationQuality{ 8, 4 });

    ASSERT_TRUE(mesh);
    ASSERT_TRUE(mesh->isValid());
    for (std::size_t index = 0; index < mesh->vertexCount(); ++index) {
        const auto   position = mesh->positions()[index];
        const double x        = static_cast<double>(position.x);
        const double y        = static_cast<double>(position.y);
        const double z        = static_cast<double>(position.z);
        EXPECT_NEAR(x * x + y * y / 4.0 + z * z / 9.0, 1.0, 1e-5);

        const vn::math::Vec3f expected(static_cast<float>(x), static_cast<float>(y / 4.0), static_cast<float>(z / 9.0));
        EXPECT_NEAR(static_cast<double>(mesh->normals()[index].dot(expected.normalized())), 1.0, 1e-5);
    }
    expectClosed(*mesh);
}

TEST(SolidTessellation, theQualityIsClampedInsteadOfTrusted)
{
    // around < 3 cannot form a ring and along < 2 leaves no band; both are clamped, so the smallest sphere is an
    // octahedron rather than an empty or degenerate mesh.
    const auto mesh = tessellateSolid(SphereSolidf{ 1.0f }, TessellationQuality{ 0, 0 });

    ASSERT_TRUE(mesh);
    ASSERT_TRUE(mesh->isValid());
    EXPECT_EQ(mesh->triangleCount(), 6U);
    expectClosed(*mesh);
}

TEST(SolidTessellation, anUnusableSolidAnswersNull)
{
    const BoxSolidf       degenerate_box{ 1.0f, 0.0f, 1.0f };
    const SphereSolidf    degenerate_sphere{ 0.0f };
    const CylinderSolidf  degenerate_cylinder{ 0.0f, 1.0f };
    const ConeSolidf      degenerate_cone{ 1.0f, 0.0f };
    const EllipsoidSolidf degenerate_ellipsoid{ 1.0f, 0.0f, 1.0f };

    EXPECT_EQ(tessellateSolid(degenerate_box).get(), nullptr);
    EXPECT_EQ(tessellateSolid(degenerate_sphere).get(), nullptr);
    EXPECT_EQ(tessellateSolid(degenerate_cylinder).get(), nullptr);
    EXPECT_EQ(tessellateSolid(degenerate_cone).get(), nullptr);
    EXPECT_EQ(tessellateSolid(degenerate_ellipsoid).get(), nullptr);
}

TEST(SolidTessellation, aModelReachesTheMeshThroughItsSolid)
{
    // The modeling type has no triangulation of its own: it holds the float solid, which is what a mesh is built from.
    const vn::models::Box box(2.0f, 4.0f, 6.0f);

    const auto solid = vn::models::solidShapeOf(box);
    ASSERT_TRUE(solid.has_value());
    const auto mesh = tessellateSolid(*solid);
    ASSERT_TRUE(mesh);
    EXPECT_EQ(mesh->triangleCount(), 12U);
    EXPECT_NEAR(mesh->aabb().max().y, 2.0f, 1e-6f);

    // A degenerate model still hands out its solid; the tessellator is what refuses it.
    const vn::models::Cylinder collapsed(0.0f, 2.0f);
    EXPECT_FALSE(collapsed.isValid());
    const auto collapsed_solid = vn::models::solidShapeOf(collapsed);
    ASSERT_TRUE(collapsed_solid.has_value());
    EXPECT_EQ(tessellateSolid(*collapsed_solid).get(), nullptr);
}
