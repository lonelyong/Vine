#include <gtest/gtest.h>

#include <cmath>
#include <type_traits>

#include <vine/math/Math.hpp>
#include <vine/math/Point3.hpp>
#include <vine/math/Vector3.hpp>

#include <vine/geometry/Circle.hpp>
#include <vine/geometry/Line.hpp>
#include <vine/geometry/LineSegment.hpp>
#include <vine/geometry/Plane.hpp>
#include <vine/geometry/Primitive.hpp>
#include <vine/geometry/Ray.hpp>
#include <vine/geometry/SphericalSurface.hpp>

namespace {

using vn::geometry::Box;
using vn::geometry::Circle;
using vn::geometry::Circled;
using vn::geometry::Cone;
using vn::geometry::Cylinder;
using vn::geometry::Ellipsoid;
using vn::geometry::Line;
using vn::geometry::Lined;
using vn::geometry::LineSegment;
using vn::geometry::LineSegmentd;
using vn::geometry::Plane;
using vn::geometry::Planed;
using vn::geometry::Ray;
using vn::geometry::Rayd;
using vn::geometry::Sphere;
using vn::geometry::SphericalSurface;
using vn::geometry::SphericalSurfaced;
using vn::math::Point3d;
using vn::math::Vec3d;

/// The families are classifications, so membership is a compile-time fact.
static_assert(std::is_base_of_v<vn::geometry::Curve, Lined>);
static_assert(std::is_base_of_v<vn::geometry::Curve, Rayd>);
static_assert(std::is_base_of_v<vn::geometry::Curve, LineSegmentd>);
static_assert(std::is_base_of_v<vn::geometry::Curve, Circled>);

static_assert(std::is_base_of_v<vn::geometry::Surface, Planed>);

/// A surface is either an unbounded analytic sheet or a bounded patch; the implemented ones sit under the right one.
static_assert(std::is_base_of_v<vn::geometry::ElementarySurface, Planed>);
static_assert(std::is_base_of_v<vn::geometry::ElementarySurface, SphericalSurfaced>);
static_assert(!std::is_base_of_v<vn::geometry::BoundedSurface, Planed>);

/// The not-yet-implemented surfaces are in the tree, and cannot be constructed by accident.
static_assert(std::is_base_of_v<vn::geometry::ElementarySurface, vn::geometry::CylindricalSurface>);
static_assert(std::is_base_of_v<vn::geometry::ElementarySurface, vn::geometry::ConicalSurface>);
static_assert(std::is_base_of_v<vn::geometry::ElementarySurface, vn::geometry::ToroidalSurface>);
static_assert(std::is_base_of_v<vn::geometry::BoundedSurface, vn::geometry::BezierSurface>);
static_assert(std::is_base_of_v<vn::geometry::BoundedSurface, vn::geometry::BSplineSurface>);
static_assert(std::is_base_of_v<vn::geometry::BoundedSurface, vn::geometry::RectangularTrimmedSurface>);
static_assert(!std::is_default_constructible_v<vn::geometry::CylindricalSurface>);

/// The parametric geometries are modeling types, not algorithm families: they are `Primitive` subclasses whose only
/// geometry answers are their own bounds and validity.
static_assert(std::is_base_of_v<vn::geometry::Primitive, Box>);
static_assert(std::is_base_of_v<vn::geometry::Primitive, Sphere>);
static_assert(std::is_base_of_v<vn::geometry::Primitive, Cylinder>);
static_assert(std::is_base_of_v<vn::geometry::Primitive, Cone>);
static_assert(std::is_base_of_v<vn::geometry::Primitive, Ellipsoid>);
static_assert(!std::is_base_of_v<vn::geometry::Surface, Box>);

/// The unbounded spherical sheet is the surface counterpart: it is a surface, not a `Primitive`.
static_assert(std::is_base_of_v<vn::geometry::Surface, SphericalSurfaced>);
static_assert(!std::is_base_of_v<vn::geometry::Primitive, SphericalSurfaced>);

/// A curve is not a surface: the two families do not overlap.
static_assert(!std::is_base_of_v<vn::geometry::Surface, Lined>);

/// The classification adds no state: empty-base optimisation keeps every type the size of its own members.
static_assert(sizeof(Rayd) == 2 * sizeof(Point3d));
static_assert(sizeof(Lined) == 2 * sizeof(Point3d));
static_assert(sizeof(LineSegmentd) == 2 * sizeof(Point3d));
static_assert(sizeof(Circled) == 2 * sizeof(Point3d) + sizeof(double));
static_assert(sizeof(SphericalSurfaced) == sizeof(Point3d) + sizeof(double));

/// A curve is trivially copyable, so it travels by value in a hot loop without a vptr or a destructor.
static_assert(std::is_trivially_copyable_v<Rayd>);
static_assert(std::is_trivially_copyable_v<Lined>);

TEST(GeometryKinds, aLineSegmentSpansItsTwoEndPoints)
{
    const LineSegmentd segment(Point3d(1.0, 2.0, 3.0), Point3d(3.0, 4.0, 3.0));

    EXPECT_TRUE(segment.isValid());
    EXPECT_DOUBLE_EQ(segment.pointAt(0.0).x, 1.0);
    EXPECT_DOUBLE_EQ(segment.pointAt(1.0).y, 4.0);
    EXPECT_DOUBLE_EQ(segment.length(), std::sqrt(8.0));
    EXPECT_DOUBLE_EQ(segment.direction().x, 1.0 / std::sqrt(2.0));

    const auto bounds = segment.bounds();
    EXPECT_DOUBLE_EQ(bounds.xmin, 1.0);
    EXPECT_DOUBLE_EQ(bounds.xmax, 3.0);
    EXPECT_DOUBLE_EQ(bounds.zmin, 3.0);
    EXPECT_DOUBLE_EQ(bounds.zmax, 3.0);
}

TEST(GeometryKinds, aDegenerateLineSegmentHasNoDirection)
{
    const LineSegmentd segment(Point3d(1.0, 1.0, 1.0), Point3d(1.0, 1.0, 1.0));

    EXPECT_FALSE(segment.isValid());
    EXPECT_DOUBLE_EQ(segment.direction().length(), 0.0);
}

TEST(GeometryKinds, aLineAnswersThePointAtItsParameter)
{
    const Lined line(Point3d(0.0, 0.0, 0.0), Vec3d(2.0, 0.0, 0.0));

    const auto point = line.pointAt(1.5);
    EXPECT_DOUBLE_EQ(point.x, 3.0);
}

TEST(GeometryKinds, aCircleWalksItsOwnPlane)
{
    const Circled circle(Point3d(1.0, 2.0, 3.0), Vec3d(0.0, 0.0, 1.0), 2.0);

    EXPECT_TRUE(circle.isValid());
    EXPECT_DOUBLE_EQ(circle.circumference(), 4.0 * vn::math::PI);

    const auto origin_point = circle.pointAt(0.0);
    EXPECT_NEAR(origin_point.z, 3.0, 1e-12);
    EXPECT_NEAR((origin_point - circle.center).length(), 2.0, 1e-12);

    const auto bounds = circle.bounds();
    EXPECT_NEAR(bounds.xmin, -1.0, 1e-12);
    EXPECT_NEAR(bounds.xmax, 3.0, 1e-12);
    EXPECT_NEAR(bounds.zmin, 3.0, 1e-12);
    EXPECT_NEAR(bounds.zmax, 3.0, 1e-12);
}

TEST(GeometryKinds, aCircleWithoutANormalIsInvalid)
{
    const Circled unit; // default: XY plane, radius 1
    EXPECT_TRUE(unit.isValid());

    const Circled flat(Point3d(0.0, 0.0, 0.0), Vec3d(0.0, 0.0, 0.0), 1.0);
    EXPECT_FALSE(flat.isValid());
}

TEST(GeometryKinds, aSphericalSheetPutsPolarZeroAtThePole)
{
    const SphericalSurfaced sheet(Point3d(1.0, 2.0, 3.0), 2.0);

    EXPECT_TRUE(sheet.isValid());

    const auto pole = sheet.pointAt(0.0, 0.0);
    EXPECT_NEAR(pole.z, 5.0, 1e-12);

    const auto equator = sheet.pointAt(0.0, vn::math::PI / 2.0);
    EXPECT_NEAR(equator.x, 3.0, 1e-12);
    EXPECT_NEAR(equator.z, 3.0, 1e-12);
    EXPECT_NEAR((equator - sheet.centre).length(), 2.0, 1e-12);
    EXPECT_NEAR(sheet.normalAt(0.0, 0.0).z, 1.0, 1e-12);

    const auto bounds = sheet.bounds();
    EXPECT_NEAR(bounds.xmin, -1.0, 1e-12);
    EXPECT_NEAR(bounds.zmax, 5.0, 1e-12);
}

} // namespace
