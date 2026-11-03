#include <gtest/gtest.h>

#include <cstddef>
#include <string>

#include <vine/brep/Construction.hpp>
#include <vine/brep/Tessellation.hpp>
#include <vine/geometry/Aabb.hpp>
#include <vine/geometry/Box.hpp>
#include <vine/geometry/Cone.hpp>
#include <vine/geometry/Cylinder.hpp>
#include <vine/geometry/Ellipsoid.hpp>
#include <vine/geometry/Sphere.hpp>
#include <vine/geometry/TriangleMesh.hpp>

namespace {

/// A primitive that reports no ShapeType of its own: the closed dispatch must not guess.
class StrayPrimitive : public vn::geometry::Primitive {
    VN_OBJECT_META(StrayPrimitive, Primitive);

  public:
    [[nodiscard]]
    bool isValid() const override
    {
        return true;
    }
};

/// How far a tessellated curved face may sit inside the analytic bounds it stands in for.
constexpr float kBoundsTolerance = 0.03f;

/**
 * @brief Builds @p primitive, triangulates the solid, and checks the triangles fill @p expected.
 *
 * @details
 * - The trimesh is inscribed in the solid, so the measured bounds are inside the analytic ones by at most the
 *   tessellation deflection; the tolerance leaves room for that and for the float storage.
 *
 * @param primitive The primitive to build.
 * @param expected  The box the primitive describes in its local space.
 */
void expectRoundTrip(const vn::geometry::Primitive& primitive, const vn::geometry::Aabbf& expected)
{
    const auto solid = vn::brep::makeBrep(primitive);
    ASSERT_TRUE(solid);
    EXPECT_TRUE(solid->isValid());

    const vn::brep::TessellationOptions options{ 0.01, 0.2, false };
    const auto                          mesh = vn::brep::tessellate(*solid, options);
    ASSERT_TRUE(mesh);
    ASSERT_GT(mesh->vertexCount(), std::size_t{ 0 });

    const auto bounds = vn::geometry::Aabbf::compute(mesh->positions());

    EXPECT_NEAR(bounds.min().x, expected.min().x, kBoundsTolerance);
    EXPECT_NEAR(bounds.min().y, expected.min().y, kBoundsTolerance);
    EXPECT_NEAR(bounds.min().z, expected.min().z, kBoundsTolerance);
    EXPECT_NEAR(bounds.max().x, expected.max().x, kBoundsTolerance);
    EXPECT_NEAR(bounds.max().y, expected.max().y, kBoundsTolerance);
    EXPECT_NEAR(bounds.max().z, expected.max().z, kBoundsTolerance);
}

} // namespace

TEST(Construction, aBuildWithoutABackendProducesNoSolid)
{
    if (vn::brep::hasBackend()) {
        GTEST_SKIP() << "this build has the backend";
    }

    // The module carries the contract without a backend: the capability query says so, and a call produces nothing.
    EXPECT_FALSE(vn::brep::makeBrep(vn::geometry::Box{}));
}

TEST(Construction, anInvalidPrimitiveProducesNoSolid)
{
    // Holds in every build: a dimension of zero or less describes no solid, backend or not.
    EXPECT_FALSE(vn::brep::makeBrep(vn::geometry::Box(0.0f, 1.0f, 1.0f)));
    EXPECT_FALSE(vn::brep::makeBrep(vn::geometry::Sphere(-1.0f)));
}

TEST(Construction, aPrimitiveOutsideTheFiveProducesNoSolid)
{
    const StrayPrimitive stray;
    EXPECT_FALSE(vn::brep::makeBrep(stray));
}

TEST(Construction, everyPrimitiveFillsItsAnalyticBounds)
{
    if (!vn::brep::hasBackend()) {
        GTEST_SKIP() << "the bounds of a built solid are a backend property";
    }

    expectRoundTrip(vn::geometry::Box(2.0f, 4.0f, 6.0f), vn::geometry::Aabbf(-1.0f, -2.0f, -3.0f, 1.0f, 2.0f, 3.0f));
    expectRoundTrip(vn::geometry::Sphere(2.0f), vn::geometry::Aabbf(-2.0f, -2.0f, -2.0f, 2.0f, 2.0f, 2.0f));
    expectRoundTrip(vn::geometry::Cylinder(1.0f, 4.0f), vn::geometry::Aabbf(-1.0f, -1.0f, -2.0f, 1.0f, 1.0f, 2.0f));
    expectRoundTrip(vn::geometry::Cone(1.0f, 4.0f), vn::geometry::Aabbf(-1.0f, -1.0f, -2.0f, 1.0f, 1.0f, 2.0f));
    expectRoundTrip(vn::geometry::Ellipsoid(1.0f, 2.0f, 3.0f), vn::geometry::Aabbf(-1.0f, -2.0f, -3.0f, 1.0f, 2.0f, 3.0f));
}

TEST(Construction, theSolidCarriesThePrimitiveName)
{
    if (!vn::brep::hasBackend()) {
        GTEST_SKIP() << "there is no solid to name without the backend";
    }

    vn::geometry::Cylinder cylinder(1.0f, 2.0f);
    cylinder.setName(vn::String::fromUtf8("link"));

    const auto solid = vn::brep::makeBrep(cylinder);
    ASSERT_TRUE(solid);
    EXPECT_EQ(solid->name().as_std_str(), std::string("link"));
    EXPECT_EQ(solid->revision(), 0u);
}
