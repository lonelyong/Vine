#include <gtest/gtest.h>

#include <cmath>
#include <limits>
#include <memory>
#include <type_traits>

#include <vine/math/Math.hpp>
#include <vine/math/Point3.hpp>
#include <vine/math/Vector3.hpp>

#include <vine/geom/spatial/BSplineCurve.hpp>
#include <vine/geom/spatial/BSplineSurface.hpp>
#include <vine/geom/spatial/BezierCurve.hpp>
#include <vine/geom/spatial/BezierSurface.hpp>
#include <vine/geom/spatial/CircleCurve.hpp>
#include <vine/geom/spatial/ConicalSurface.hpp>
#include <vine/geom/spatial/CylindricalSurface.hpp>
#include <vine/geom/spatial/EllipseCurve.hpp>
#include <vine/geom/spatial/HyperbolaCurve.hpp>
#include <vine/geom/spatial/LineCurve.hpp>
#include <vine/geom/spatial/OffsetCurve.hpp>
#include <vine/geom/spatial/ParabolaCurve.hpp>
#include <vine/geom/spatial/Placement.hpp>
#include <vine/geom/spatial/Plane.hpp>
#include <vine/models/Primitive.hpp>
#include <vine/geom/spatial/SolidShape.hpp>
#include <vine/geom/spatial/Ray.hpp>
#include <vine/geom/spatial/RectangularTrimmedSurface.hpp>
#include <vine/geom/spatial/SphericalSurface.hpp>
#include <vine/geom/spatial/ToroidalSurface.hpp>
#include <vine/geom/spatial/TrimmedCurve.hpp>

namespace
{

using vn::geom::spatial::BezierCurved;
using vn::geom::spatial::BezierSurfaced;
using vn::geom::spatial::BSplineCurved;
using vn::geom::spatial::BSplineSurfaced;
using vn::geom::spatial::CircleCurve;
using vn::geom::spatial::CircleCurved;
using vn::geom::spatial::ConicalSurfaced;
using vn::geom::spatial::CylindricalSurfaced;
using vn::geom::spatial::EllipseCurved;
using vn::geom::spatial::HyperbolaCurved;
using vn::geom::spatial::LineCurve;
using vn::geom::spatial::LineCurved;
using vn::geom::spatial::OffsetCurved;
using vn::geom::spatial::ParabolaCurved;
using vn::geom::spatial::Plane;
using vn::geom::spatial::Planed;
using vn::models::Primitive;
using vn::geom::spatial::Ray;
using vn::geom::spatial::Rayd;
using vn::geom::spatial::RectangularTrimmedSurfaced;
using vn::geom::spatial::SphericalSurface;
using vn::geom::spatial::SphericalSurfaced;
using vn::geom::spatial::ToroidalSurfaced;
using vn::geom::spatial::TrimmedCurved;
using vn::math::Point3d;
using vn::math::Vec3d;

/// The curve interface is a class template, so membership is asked per precision.
static_assert(std::is_base_of_v<vn::geom::spatial::Curve<double>, LineCurved>);
static_assert(std::is_base_of_v<vn::geom::spatial::Curve<double>, CircleCurved>);
static_assert(std::is_base_of_v<vn::geom::spatial::ConicCurve<double>, CircleCurved>);
static_assert(std::is_base_of_v<vn::geom::spatial::ConicCurve<double>, EllipseCurved>);
static_assert(std::is_base_of_v<vn::geom::spatial::ConicCurve<double>, HyperbolaCurved>);
static_assert(std::is_base_of_v<vn::geom::spatial::ConicCurve<double>, ParabolaCurved>);

/// `BezierCurve` and `BSplineCurve` answer all ten curve properties, so they are concrete values.
static_assert(!std::is_abstract_v<vn::geom::spatial::BezierCurve<double>>);
static_assert(!std::is_abstract_v<vn::geom::spatial::BSplineCurve<double>>);

/// `OffsetCurve` answers all of them now: its second derivative comes from the basis' third, through the capability.
static_assert(!std::is_abstract_v<vn::geom::spatial::OffsetCurve<double>>);

/// `Ray` is deliberately not a curve: it is copied per intersection query and stays a plain value.
static_assert(!std::is_base_of_v<vn::geom::spatial::Curve<double>, Rayd>);
static_assert(std::is_trivially_copyable_v<Rayd>);

static_assert(std::is_base_of_v<vn::geom::spatial::Surface<double>, Planed>);

/// The surface interface is abstract and the two implemented sheets answer it.
static_assert(std::is_abstract_v<vn::geom::spatial::Surface<double>>);
static_assert(!std::is_abstract_v<Planed>);
static_assert(!std::is_abstract_v<SphericalSurfaced>);

/// A surface is either an unbounded analytic sheet or a bounded patch; the implemented ones sit under the right one.
static_assert(std::is_base_of_v<vn::geom::spatial::ElementarySurface<double>, Planed>);
static_assert(std::is_base_of_v<vn::geom::spatial::ElementarySurface<double>, SphericalSurfaced>);
static_assert(!std::is_base_of_v<vn::geom::spatial::BoundedSurface<double>, Planed>);

/// The axis-symmetric elementary surfaces carry a `Placement` value: the frame is a value type, not a base class.
static_assert(std::is_base_of_v<vn::geom::spatial::ElementarySurface<double>, CylindricalSurfaced>);
static_assert(std::is_base_of_v<vn::geom::spatial::ElementarySurface<double>, ConicalSurfaced>);
static_assert(std::is_base_of_v<vn::geom::spatial::ElementarySurface<double>, ToroidalSurfaced>);
static_assert(!std::is_abstract_v<CylindricalSurfaced>);
static_assert(!std::is_abstract_v<ConicalSurfaced>);
static_assert(!std::is_abstract_v<ToroidalSurfaced>);
static_assert(!std::is_default_constructible_v<CylindricalSurfaced>);

/// A default-constructed sheet only exists when its default is meaningful geometry or a recognisable empty state: the
/// sphere defaults to a radius-1 sheet and an empty patch to something `isValid()` refuses, while a plane or an axis
/// surface has no defensible default at all - "a unit cylinder" would look perfectly legal while carrying no caller
/// input.
static_assert(!std::is_default_constructible_v<Planed>);
static_assert(!std::is_default_constructible_v<ConicalSurfaced>);
static_assert(!std::is_default_constructible_v<ToroidalSurfaced>);
static_assert(std::is_default_constructible_v<SphericalSurfaced>);
static_assert(std::is_default_constructible_v<BezierSurfaced>);
static_assert(std::is_default_constructible_v<BSplineSurfaced>);

/// The `Placement` value type is three vectors and nothing else.
static_assert(sizeof(vn::geom::spatial::Placement<double>) == 3 * sizeof(Point3d));
static_assert(std::is_trivially_copyable_v<vn::geom::spatial::Placement<double>>);

/// The rectangular trim is a bounded view over a basis surface, like `TrimmedCurve` over a basis curve.
static_assert(std::is_base_of_v<vn::geom::spatial::BoundedSurface<double>, RectangularTrimmedSurfaced>);
static_assert(!std::is_abstract_v<RectangularTrimmedSurfaced>);

/// The two patch surfaces answer the surface interface now: the net, the degrees and the knots are their data model.
static_assert(std::is_base_of_v<vn::geom::spatial::BoundedSurface<double>, BezierSurfaced>);
static_assert(std::is_base_of_v<vn::geom::spatial::BoundedSurface<double>, BSplineSurfaced>);
static_assert(!std::is_abstract_v<BezierSurfaced>);
static_assert(!std::is_abstract_v<BSplineSurfaced>);

/// A parametric shape is one modeling class holding an analytic solid: the five bodies are kinds of `Solid`, not five
/// modeling classes, and the shape is a `Shape` rather than a surface or a curve.
static_assert(std::is_base_of_v<vn::models::Shape, Primitive>);
static_assert(!std::is_base_of_v<vn::geom::spatial::Surface<double>, Primitive>);
static_assert(!std::is_base_of_v<vn::geom::spatial::Curve<double>, Primitive>);

/// The analytic solid is a family plus a closed value set: `Solid<T>` is the interface (abstract, no state), the five
/// kinds are its values, and a model stores the variant of them.
static_assert(std::is_abstract_v<vn::geom::spatial::Solidf>);
static_assert(std::is_base_of_v<vn::geom::spatial::Solidf, vn::geom::spatial::BoxSolidf>);
static_assert(!std::is_abstract_v<vn::geom::spatial::BoxSolidf>);
static_assert(std::is_same_v<decltype(vn::geom::spatial::BoxSolidf{}.half_x), float>);
static_assert(!std::is_same_v<vn::geom::spatial::SolidShapef, vn::geom::spatial::SolidShaped>);

/// The unbounded spherical sheet is the surface counterpart: it is a surface, not a modeling primitive.
static_assert(std::is_base_of_v<vn::geom::spatial::Surface<double>, SphericalSurfaced>);
static_assert(!std::is_base_of_v<vn::models::Primitive, SphericalSurfaced>);

/// A curve is not a surface: the two families do not overlap.
static_assert(!std::is_base_of_v<vn::geom::spatial::Surface<double>, LineCurved>);

/// Empty-base optimisation still keeps the value types the size of their own members; the two evaluable curves pay
/// one vptr for the interface, which is why the hot `Ray` is not one of them.
static_assert(sizeof(Rayd) == 2 * sizeof(Point3d));
static_assert(sizeof(SphericalSurfaced) == sizeof(Point3d) + sizeof(double) + sizeof(void*));
static_assert(sizeof(Planed) == 2 * sizeof(Point3d) + sizeof(void*));
static_assert(sizeof(LineCurved) == 2 * sizeof(Point3d) + sizeof(void*));
static_assert(sizeof(CircleCurved) == 3 * sizeof(Point3d) + sizeof(double) + sizeof(void*));
static_assert(sizeof(EllipseCurved) == 3 * sizeof(Point3d) + 2 * sizeof(double) + sizeof(void*));
static_assert(sizeof(HyperbolaCurved) == 3 * sizeof(Point3d) + 2 * sizeof(double) + sizeof(void*));
static_assert(sizeof(ParabolaCurved) == 3 * sizeof(Point3d) + sizeof(double) + sizeof(void*));

/// The axis surfaces differ from the sphere only by the frame they store, and that frame is three vectors.
static_assert(sizeof(CylindricalSurfaced) == 3 * sizeof(Point3d) + sizeof(double) + sizeof(void*));
static_assert(sizeof(ConicalSurfaced) == 3 * sizeof(Point3d) + 2 * sizeof(double) + sizeof(void*));
static_assert(sizeof(ToroidalSurfaced) == 3 * sizeof(Point3d) + 2 * sizeof(double) + sizeof(void*));

/// The interface is the contract, so validity is askable through it: a caller holding `const Curve<T>&` or
/// `const Surface<T>&` must not have to know the concrete type to find out whether the state is usable.
static_assert(std::is_same_v<decltype(std::declval<const vn::geom::spatial::Curve<double>&>().isValid()), bool>);
static_assert(std::is_same_v<decltype(std::declval<const vn::geom::spatial::Surface<double>&>().isValid()), bool>);

/// Continuity is a ladder ordered by smoothness, so `>=` answers "at least this smooth".
static_assert(vn::geom::Continuity::CN > vn::geom::Continuity::C2);
static_assert(vn::geom::Continuity::C1 > vn::geom::Continuity::G1);
static_assert(vn::geom::Continuity::G1 > vn::geom::Continuity::C0);

/// `OffsetCurve` leaves one answer pure - the second derivative needs the basis' third derivative - so this stub
/// supplies exactly that one and lets the implemented point, velocity and domain answers be exercised.
struct OffsetStub : vn::geom::spatial::OffsetCurved {
    using Base = vn::geom::spatial::OffsetCurved;
    using Base::Base;

    [[nodiscard]] Base::Vector secondDerivative(double) const override
    {
        return Base::Vector(0.0, 0.0, 0.0);
    }
};

TEST(GeometryKinds, aLineCurveAnswersThePointAtItsParameter)
{
    const LineCurved line(Point3d(0.0, 0.0, 0.0), Vec3d(2.0, 0.0, 0.0));

    const auto point = line.pointAt(1.5);
    EXPECT_DOUBLE_EQ(point.x, 3.0);
}

TEST(GeometryKinds, aCircleCurveWalksItsOwnPlane)
{
    const CircleCurved circle(Point3d(1.0, 2.0, 3.0), Vec3d(0.0, 0.0, 1.0), 2.0);

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

TEST(GeometryKinds, aCircleCurveWithoutANormalIsInvalid)
{
    const CircleCurved unit; // default: XY plane, radius 1
    EXPECT_TRUE(unit.isValid());

    const CircleCurved flat(Point3d(0.0, 0.0, 0.0), Vec3d(0.0, 0.0, 0.0), 1.0);
    EXPECT_FALSE(flat.isValid());
}

TEST(GeometryKinds, aConicWithADegenerateFrameCollapsesOntoItsAxis)
{
    const CircleCurved flat(Point3d(1.0, 2.0, 3.0), Vec3d(0.0, 0.0, 0.0), 2.0);

    // No plane means no circle: `ConicCurve` falls back to `x_direction = +X` and leaves `yAxis()` zero, so the
    // parameterisation runs along the world X axis through the centre - finite, and reported by `isValid()`.
    EXPECT_FALSE(flat.isValid());
    EXPECT_NEAR((flat.evaluate(0.0) - Point3d(3.0, 2.0, 3.0)).length(), 0.0, 1e-12);
    EXPECT_NEAR(flat.evaluate(vn::math::PI / 2.0).x, 1.0, 1e-12);

    // `bounds()` follows the same collapse: the box of that segment, not a cube of radius 2.
    const auto bounds = flat.bounds();
    EXPECT_NEAR(bounds.xmin, -1.0, 1e-12);
    EXPECT_NEAR(bounds.xmax, 3.0, 1e-12);
    EXPECT_NEAR(bounds.ymin, 2.0, 1e-12);
    EXPECT_NEAR(bounds.ymax, 2.0, 1e-12);
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

TEST(GeometryKinds, aCurveAnswersItsDomainAndPeriod)
{
    const LineCurved line(Point3d(0.0, 0.0, 0.0), Vec3d(1.0, 0.0, 0.0));
    EXPECT_FALSE(line.isClosed());
    EXPECT_FALSE(line.isPeriodic());
    EXPECT_DOUBLE_EQ(line.period(), 0.0);
    EXPECT_TRUE(std::isinf(line.firstParameter()));

    const CircleCurved circle; // default: XY plane, radius 1
    EXPECT_TRUE(circle.isClosed());
    EXPECT_TRUE(circle.isPeriodic());
    EXPECT_NEAR(circle.period(), 2.0 * vn::math::PI, 1e-12);
    EXPECT_NEAR(circle.evaluate(vn::math::PI).y, 0.0, 1e-12);
}

TEST(GeometryKinds, aTrimmedCurveForwardsToItsBasis)
{
    const auto                        basis = std::make_shared<LineCurved>(Point3d(0.0, 0.0, 0.0), Vec3d(1.0, 0.0, 0.0));
    const vn::geom::spatial::TrimmedCurved trimmed(basis, 1.0, 3.0);

    EXPECT_DOUBLE_EQ(trimmed.firstParameter(), 1.0);
    EXPECT_DOUBLE_EQ(trimmed.startPoint().x, 1.0);
    EXPECT_DOUBLE_EQ(trimmed.endPoint().x, 3.0);
    EXPECT_DOUBLE_EQ(trimmed.evaluate(2.0).x, 2.0);
    EXPECT_FALSE(trimmed.isClosed());
    EXPECT_FALSE(trimmed.isPeriodic());
}

TEST(GeometryKinds, aLineCurveHasAConstantVelocity)
{
    const LineCurved line(Point3d(1.0, 2.0, 3.0), Vec3d(2.0, 0.0, 0.0));

    // The derivative is the direction vector itself, whatever the parameter.
    EXPECT_DOUBLE_EQ(line.derivative(0.0).x, 2.0);
    EXPECT_DOUBLE_EQ(line.derivative(7.5).x, 2.0);
    EXPECT_DOUBLE_EQ(line.secondDerivative(7.5).length(), 0.0);
    EXPECT_EQ(line.continuity(), vn::geom::Continuity::CN);
    EXPECT_DOUBLE_EQ(line.reversedParameter(2.0), -2.0);
}

TEST(GeometryKinds, aLineCurveWithoutADirectionIsInvalid)
{
    const LineCurved usable(Point3d(0.0, 0.0, 0.0), Vec3d(1.0, 0.0, 0.0));
    const LineCurved degenerate(Point3d(1.0, 2.0, 3.0), Vec3d(0.0, 0.0, 0.0));

    EXPECT_TRUE(usable.isValid());
    EXPECT_FALSE(degenerate.isValid());

    // A zero direction leaves a point, not a line: there is no closest-point pair, and the answer must not be a NaN
    // that a caller only discovers when it is already used.
    const auto intersection = usable.intersectWith(degenerate);
    EXPECT_FALSE(intersection.is_intersected);
    EXPECT_FALSE(intersection.is_parallel);
    EXPECT_FALSE(intersection.is_collinear);
    EXPECT_FALSE(std::isnan(intersection.params.distance));
}

TEST(GeometryKinds, aCircleCurveTurnsTowardsItsCentre)
{
    const CircleCurved circle(Point3d(1.0, 2.0, 3.0), Vec3d(0.0, 0.0, 1.0), 2.0);

    // Velocity is tangential and as long as the radius; acceleration points at the centre and is as long as it too.
    for (const double angle : { 0.0, vn::math::PI / 4.0, 1.5 }) {
        const auto point    = circle.evaluate(angle);
        const auto velocity = circle.derivative(angle);
        const auto accel    = circle.secondDerivative(angle);

        EXPECT_NEAR(velocity.length(), 2.0, 1e-12);
        EXPECT_NEAR(velocity.dot(point - circle.center), 0.0, 1e-12);
        EXPECT_NEAR((accel - (circle.center - point)).length(), 0.0, 1e-12);
        EXPECT_NEAR(accel.length(), 2.0, 1e-12);
    }

    EXPECT_EQ(circle.continuity(), vn::geom::Continuity::CN);
    EXPECT_NEAR(circle.reversedParameter(0.25), 2.0 * vn::math::PI - 0.25, 1e-12);
}

TEST(GeometryKinds, aTrimmedCurveMirrorsInsideItsOwnRange)
{
    const auto                        basis = std::make_shared<CircleCurved>(Point3d(0.0, 0.0, 0.0), Vec3d(0.0, 0.0, 1.0), 1.0);
    const vn::geom::spatial::TrimmedCurved quarter(basis, 0.0, vn::math::PI / 2.0);

    // The reflection belongs to the piece, not to the basis: reflecting on the basis would answer 2*pi - u.
    EXPECT_NEAR(quarter.reversedParameter(0.25), vn::math::PI / 2.0 - 0.25, 1e-12);

    // Trimming does not reparameterise, and the answers are reachable through the interface.
    const vn::geom::spatial::Curve<double>& as_curve = quarter;
    EXPECT_NEAR(as_curve.derivative(0.0).y, 1.0, 1e-12);
    EXPECT_NEAR(as_curve.secondDerivative(0.0).y, 0.0, 1e-12);
    EXPECT_EQ(as_curve.continuity(), vn::geom::Continuity::CN);
}

TEST(GeometryKinds, aTrimmedCurveWithoutABasisOrWithAReversedRangeIsInvalid)
{
    const auto basis = std::make_shared<LineCurved>(Point3d(0.0, 0.0, 0.0), Vec3d(1.0, 0.0, 0.0));

    EXPECT_TRUE(vn::geom::spatial::TrimmedCurved(basis, 1.0, 3.0).isValid());

    // A trim is a view over a basis: without one there is nothing to evaluate, and a reversed range describes a
    // different piece than the caller asked for.
    EXPECT_FALSE(vn::geom::spatial::TrimmedCurved(nullptr, 0.0, 1.0).isValid());
    EXPECT_FALSE(vn::geom::spatial::TrimmedCurved(basis, 3.0, 1.0).isValid());
}

TEST(GeometryKinds, aTrimOfAPeriodicBasisReportsAPeriodOnlyWhenItRepeats)
{
    const auto basis = std::make_shared<CircleCurved>(Point3d(0.0, 0.0, 0.0), Vec3d(0.0, 0.0, 1.0), 1.0);

    // The period belongs to the interface (`0` for a non-periodic curve), not to the basis: a quarter turn is a piece
    // with two ends, so it answers no period even though the circle it was cut from repeats every 2*pi.
    const TrimmedCurved quarter(basis, 0.0, vn::math::PI / 2.0);
    EXPECT_FALSE(quarter.isPeriodic());
    EXPECT_DOUBLE_EQ(quarter.period(), 0.0);

    // Spanning one full turn is what makes the piece periodic, and then the basis' period is the answer.
    const TrimmedCurved full_turn(basis, 0.0, 2.0 * vn::math::PI);
    EXPECT_TRUE(full_turn.isPeriodic());
    EXPECT_NEAR(full_turn.period(), 2.0 * vn::math::PI, 1e-12);
}

TEST(GeometryKinds, aTrimThatOvershootsTheBasisIsNotClosed)
{
    const auto basis = std::make_shared<CircleCurved>(Point3d(0.0, 0.0, 0.0), Vec3d(0.0, 0.0, 1.0), 1.0);

    // Covering the basis' range is necessary but not sufficient: this range ends a third of a radian later, so the
    // two end points differ and the piece has two ends.
    const TrimmedCurved overshooting(basis, 0.0, 2.0 * vn::math::PI + 0.3);
    EXPECT_FALSE(overshooting.isClosed());
    EXPECT_GT((overshooting.evaluate(overshooting.firstParameter())
               - overshooting.evaluate(overshooting.lastParameter()))
                  .length(),
              0.1);

    // A range that ends exactly one whole turn later does close.
    const TrimmedCurved full_turn(basis, 0.0, 2.0 * vn::math::PI);
    EXPECT_TRUE(full_turn.isClosed());
}

TEST(GeometryKinds, aTrimWithoutABasisAnswersInsteadOfDereferencingIt)
{
    const TrimmedCurved empty(nullptr, 0.0, 1.0);

    // `isValid()` reports the mistake; asking anyway answers the degenerate value of the family rather than crashing.
    EXPECT_FALSE(empty.isValid());
    EXPECT_FALSE(empty.isPeriodic());
    EXPECT_FALSE(empty.isClosed());
    EXPECT_DOUBLE_EQ(empty.period(), 0.0);
    EXPECT_EQ(empty.continuity(), vn::geom::Continuity::C0);
    EXPECT_DOUBLE_EQ((empty.evaluate(0.5) - Point3d()).length(), 0.0);
    EXPECT_DOUBLE_EQ((empty.startPoint() - Point3d()).length(), 0.0);
    EXPECT_DOUBLE_EQ(empty.derivative(0.5).length(), 0.0);
    EXPECT_DOUBLE_EQ(empty.secondDerivative(0.5).length(), 0.0);
}

TEST(GeometryKinds, anEllipseCurveWalksItsTwoAxes)
{
    const EllipseCurved ellipse(Point3d(1.0, 2.0, 3.0), Vec3d(0.0, 0.0, 1.0), Vec3d(1.0, 0.0, 0.0), 4.0, 2.0);

    EXPECT_TRUE(ellipse.isValid());

    // u = 0 is the end of the major axis, pi/2 the end of the minor axis.
    EXPECT_NEAR((ellipse.pointAt(0.0) - Point3d(5.0, 2.0, 3.0)).length(), 0.0, 1e-12);
    EXPECT_NEAR((ellipse.pointAt(vn::math::PI / 2.0) - Point3d(1.0, 4.0, 3.0)).length(), 0.0, 1e-12);

    // The speed is the semi-axis being travelled along, not a constant; the acceleration points at the centre.
    EXPECT_NEAR(ellipse.derivative(0.0).length(), 2.0, 1e-12);
    EXPECT_NEAR(ellipse.derivative(vn::math::PI / 2.0).length(), 4.0, 1e-12);
    EXPECT_NEAR((ellipse.secondDerivative(0.7) - (ellipse.center - ellipse.pointAt(0.7))).length(), 0.0, 1e-12);

    // The bounds are the exact per-axis support of the ellipse: 4 along X and 2 along Y.
    const auto bounds = ellipse.bounds();
    EXPECT_NEAR(bounds.xmax, 5.0, 1e-12);
    EXPECT_NEAR(bounds.ymax, 4.0, 1e-12);

    EXPECT_TRUE(ellipse.isClosed());
    EXPECT_NEAR(ellipse.reversedParameter(0.3), 2.0 * vn::math::PI - 0.3, 1e-12);
}

TEST(GeometryKinds, aHyperbolaBranchWalksAwayFromItsCentre)
{
    const HyperbolaCurved hyperbola(Point3d(0.0, 0.0, 0.0), Vec3d(0.0, 0.0, 1.0), Vec3d(1.0, 0.0, 0.0), 2.0, 3.0);

    EXPECT_TRUE(hyperbola.isValid());

    // u = 0 is the vertex, and the speed there is the conjugate semi-axis.
    EXPECT_NEAR((hyperbola.pointAt(0.0) - Point3d(2.0, 0.0, 0.0)).length(), 0.0, 1e-12);
    EXPECT_NEAR(hyperbola.derivative(0.0).length(), 3.0, 1e-12);
    EXPECT_NEAR((hyperbola.secondDerivative(1.0) - (hyperbola.pointAt(1.0) - hyperbola.center)).length(), 0.0, 1e-12);

    EXPECT_FALSE(hyperbola.isClosed());
    EXPECT_FALSE(hyperbola.isPeriodic());
    EXPECT_DOUBLE_EQ(hyperbola.period(), 0.0);
    EXPECT_TRUE(std::isinf(hyperbola.firstParameter()));
    EXPECT_EQ(hyperbola.continuity(), vn::geom::Continuity::CN);
    EXPECT_DOUBLE_EQ(hyperbola.reversedParameter(1.5), -1.5);
}

TEST(GeometryKinds, aParabolaIsEquidistantFromFocusAndDirectrix)
{
    const double         focal_distance = 2.0;
    const ParabolaCurved parabola(Point3d(0.0, 0.0, 0.0), Vec3d(0.0, 0.0, 1.0), Vec3d(1.0, 0.0, 0.0), focal_distance);

    EXPECT_TRUE(parabola.isValid());
    EXPECT_NEAR((parabola.pointAt(0.0) - parabola.center).length(), 0.0, 1e-12);

    const Point3d focus(2.0, 0.0, 0.0);
    const auto    point = parabola.pointAt(3.0);
    EXPECT_NEAR((point - focus).length(), point.x + focal_distance, 1e-12);

    EXPECT_NEAR(parabola.derivative(0.0).y, 1.0, 1e-12);
    EXPECT_NEAR(parabola.secondDerivative(5.0).x, 0.25, 1e-12);
    EXPECT_FALSE(parabola.isPeriodic());
    EXPECT_TRUE(std::isinf(parabola.lastParameter()));
    EXPECT_DOUBLE_EQ(parabola.reversedParameter(2.0), -2.0);
}

TEST(GeometryKinds, continuityAnswersTheDerivativeOrderQuestion)
{
    const LineCurved line(Point3d(0.0, 0.0, 0.0), Vec3d(1.0, 0.0, 0.0));

    // CN answers every finite order; a negative order is refused rather than read as C0.
    EXPECT_TRUE(line.isCN(0));
    EXPECT_TRUE(line.isCN(3));
    EXPECT_TRUE(line.isCN(8));
    EXPECT_FALSE(line.isCN(-1));

    // A cubic with an internal knot of multiplicity 2 is C1: one continuous derivative, no more. The end
    // multiplicities are the clamp, so they are `degree + 1` and not joins.
    const BSplineCurved kinked(
        3,
        { Point3d(0.0, 0.0, 0.0), Point3d(1.0, 1.0, 0.0), Point3d(2.0, 0.0, 0.0), Point3d(2.0, 2.0, 0.0), Point3d(3.0, 1.0, 0.0), Point3d(3.0, 0.0, 0.0) },
        { 0.0, 0.5, 1.0 },
        { 4, 2, 4 });
    EXPECT_TRUE(kinked.isCN(0));
    EXPECT_TRUE(kinked.isCN(1));
    EXPECT_FALSE(kinked.isCN(2));
}

TEST(GeometryKinds, aBezierCurveDescribesItself)
{
    const BezierCurved open({ Point3d(0.0, 0.0, 0.0), Point3d(1.0, 1.0, 0.0), Point3d(2.0, 0.0, 0.0) });

    // The degree is the pole count, not a stored number; the domain is the type's, not the data's.
    EXPECT_EQ(open.degree(), 2);
    EXPECT_TRUE(open.isValid());
    EXPECT_FALSE(open.isRational());
    EXPECT_FALSE(open.isClosed());
    EXPECT_DOUBLE_EQ(open.firstParameter(), 0.0);
    EXPECT_DOUBLE_EQ(open.lastParameter(), 1.0);
    EXPECT_FALSE(open.isPeriodic());
    EXPECT_DOUBLE_EQ(open.period(), 0.0);
    EXPECT_EQ(open.continuity(), vn::geom::Continuity::CN);
    EXPECT_DOUBLE_EQ(open.reversedParameter(0.25), 0.75);

    // Closed means the two end poles coincide; weights make the curve rational.
    const BezierCurved closed({ Point3d(0.0, 0.0, 0.0), Point3d(1.0, 1.0, 0.0), Point3d(0.0, 0.0, 0.0) });
    EXPECT_TRUE(closed.isClosed());

    const BezierCurved rational({ Point3d(0.0, 0.0, 0.0), Point3d(1.0, 0.0, 0.0) }, { 1.0, 2.0 });
    EXPECT_TRUE(rational.isRational());
    EXPECT_TRUE(rational.isValid());

    // A non-positive weight makes the rational denominator vanish inside the domain, so it is refused.
    const BezierCurved negative({ Point3d(0.0, 0.0, 0.0), Point3d(1.0, 0.0, 0.0) }, { 1.0, -1.0 });
    EXPECT_FALSE(negative.isValid());
}

TEST(GeometryKinds, aBezierCurveBlendsItsPoles)
{
    const BezierCurved arch({ Point3d(0.0, 0.0, 0.0), Point3d(1.0, 1.0, 0.0), Point3d(2.0, 0.0, 0.0) });

    // The end poles are the end points; a quadratic at u = 1/2 sits half way between its middle pole and the chord.
    EXPECT_NEAR((arch.evaluate(0.0) - Point3d(0.0, 0.0, 0.0)).length(), 0.0, 1e-12);
    EXPECT_NEAR((arch.evaluate(1.0) - Point3d(2.0, 0.0, 0.0)).length(), 0.0, 1e-12);
    EXPECT_NEAR((arch.evaluate(0.5) - Point3d(1.0, 0.5, 0.0)).length(), 0.0, 1e-12);

    // P' is the degree times the curve of the first differences: 2 * (P1 - P0) and 2 * (P2 - P1) at the ends.
    EXPECT_NEAR((arch.derivative(0.0) - Vec3d(2.0, 2.0, 0.0)).length(), 0.0, 1e-12);
    EXPECT_NEAR((arch.derivative(1.0) - Vec3d(2.0, -2.0, 0.0)).length(), 0.0, 1e-12);

    // A quadratic has a constant second derivative: n * (n - 1) times the second difference.
    EXPECT_NEAR((arch.secondDerivative(0.3) - Vec3d(0.0, -4.0, 0.0)).length(), 0.0, 1e-12);

    // The answers are reachable through the interface, so they are overrides rather than hidden members.
    const vn::geom::spatial::Curve<double>& as_curve = arch;
    EXPECT_NEAR(as_curve.evaluate(0.5).y, 0.5, 1e-12);
    EXPECT_NEAR(as_curve.derivative(0.0).y, 2.0, 1e-12);
}

TEST(GeometryKinds, aRationalBezierCurveIsPulledTowardsItsHeavierPole)
{
    const BezierCurved weighted({ Point3d(0.0, 0.0, 0.0), Point3d(1.0, 0.0, 0.0) }, { 1.0, 3.0 });

    // P(u) = 3u / (1 + 2u), so the midpoint runs past the half-way point towards the heavy pole.
    EXPECT_NEAR(weighted.evaluate(0.5).x, 0.75, 1e-12);
    EXPECT_NEAR(weighted.derivative(0.5).x, 0.75, 1e-12);
    EXPECT_NEAR(weighted.secondDerivative(0.5).x, -1.5, 1e-12);

    // The same answers through the interface, which is where the quotient rule lives.
    const vn::geom::spatial::Curve<double>& as_curve = weighted;
    EXPECT_NEAR(as_curve.evaluate(0.25).x, 0.5, 1e-12);
}

TEST(GeometryKinds, aBSplineCurveDerivesItsContinuityFromItsMultiplicities)
{
    const auto poles =
        vn::geom::spatial::BSplineCurve<double>::PoleArray{ Point3d(0.0, 0.0, 0.0), Point3d(1.0, 1.0, 0.0), Point3d(2.0, 0.0, 0.0), Point3d(3.0, 1.0, 0.0) };

    // Cubic with one span: the only joins are the two clamps, so the curve is CN throughout.
    const BSplineCurved smooth(3, poles, { 0.0, 1.0 }, { 4, 4 });
    EXPECT_TRUE(smooth.isValid());
    EXPECT_FALSE(smooth.isRational());
    EXPECT_EQ(smooth.continuity(), vn::geom::Continuity::CN);

    // A clamped cubic with an internal knot of multiplicity 2 needs 6 poles (4 + 2 + 4 = poles + degree + 1) and is
    // C1 there.
    const BSplineCurved kinked(
        3,
        { Point3d(0.0, 0.0, 0.0), Point3d(1.0, 1.0, 0.0), Point3d(2.0, 0.0, 0.0), Point3d(2.0, 2.0, 0.0), Point3d(3.0, 1.0, 0.0), Point3d(3.0, 0.0, 0.0) },
        { 0.0, 0.5, 1.0 },
        { 4, 2, 4 });
    EXPECT_TRUE(kinked.isValid());
    EXPECT_EQ(kinked.continuity(), vn::geom::Continuity::C1);

    // Multiplicities that do not rebuild a flat knot vector of poles + degree + 1 are refused.
    const BSplineCurved broken(3, poles, { 0.0, 0.5, 1.0 }, { 4, 1, 4 });
    EXPECT_FALSE(broken.isValid());
}

TEST(GeometryKinds, aKnotVectorThatBreaksTheCurveIsNotACurve)
{
    // An internal multiplicity of `degree + 1` makes the two sides independent: the parameterisation jumps instead of
    // meeting. That is not one curve, so `isValid()` refuses it rather than letting `continuity()` claim `C0`
    // ("the pieces meet at a point") about a curve whose pieces do not meet at all.
    const BSplineCurved split(
        2,
        { Point3d(0.0, 0.0, 0.0), Point3d(1.0, 0.0, 0.0), Point3d(2.0, 0.0, 0.0), Point3d(5.0, 1.0, 0.0), Point3d(6.0, 1.0, 0.0), Point3d(7.0, 1.0, 0.0) },
        { 0.0, 0.5, 1.0 },
        { 3, 3, 3 });
    EXPECT_FALSE(split.isValid());

    // The same rule holds in each direction of a patch: a break in `u` is not a sheet either.
    const BSplineSurfaced split_patch({ Point3d(0.0, 0.0, 0.0),
                                        Point3d(1.0, 0.0, 0.0),
                                        Point3d(2.0, 0.0, 0.0),
                                        Point3d(5.0, 1.0, 0.0),
                                        Point3d(6.0, 1.0, 0.0),
                                        Point3d(7.0, 1.0, 0.0),
                                        Point3d(0.0, 0.0, 2.0),
                                        Point3d(1.0, 0.0, 2.0),
                                        Point3d(2.0, 0.0, 2.0),
                                        Point3d(5.0, 1.0, 2.0),
                                        Point3d(6.0, 1.0, 2.0),
                                        Point3d(7.0, 1.0, 2.0) },
                                      6,
                                      2,
                                      2,
                                      1,
                                      { 0.0, 0.5, 1.0 },
                                      { 3, 3, 3 },
                                      { 0.0, 1.0 },
                                      { 2, 2 });
    EXPECT_FALSE(split_patch.isValid());

    // A multiplicity equal to the degree still joins at a point, so it is a (kinked) curve and answers `C0`.
    const BSplineCurved kinked(2,
                               { Point3d(0.0, 0.0, 0.0), Point3d(1.0, 0.0, 0.0), Point3d(2.0, 0.0, 0.0), Point3d(5.0, 1.0, 0.0), Point3d(6.0, 1.0, 0.0) },
                               { 0.0, 0.5, 1.0 },
                               { 3, 2, 3 });
    EXPECT_TRUE(kinked.isValid());
    EXPECT_EQ(kinked.continuity(), vn::geom::Continuity::C0);
    EXPECT_TRUE(kinked.isCN(0));
    EXPECT_FALSE(kinked.isCN(1));
}

TEST(GeometryKinds, aBSplineCurveBlendsItsClampedSpan)
{
    const BSplineCurved arc(3, { Point3d(0.0, 0.0, 0.0), Point3d(1.0, 2.0, 0.0), Point3d(2.0, 2.0, 0.0), Point3d(3.0, 0.0, 0.0) }, { 0.0, 1.0 }, { 4, 4 });

    EXPECT_TRUE(arc.isValid());
    EXPECT_FALSE(arc.isClosed());

    // Both ends are clamped onto the end poles, and one span is a Bernstein combination of the four poles.
    EXPECT_NEAR((arc.evaluate(0.0) - Point3d(0.0, 0.0, 0.0)).length(), 0.0, 1e-12);
    EXPECT_NEAR((arc.evaluate(1.0) - Point3d(3.0, 0.0, 0.0)).length(), 0.0, 1e-12);
    EXPECT_NEAR((arc.evaluate(0.5) - Point3d(1.5, 1.5, 0.0)).length(), 0.0, 1e-12);

    // One span means the answers must agree with the degree-3 Bezier curve of the same poles.
    EXPECT_NEAR((arc.derivative(0.0) - Vec3d(3.0, 6.0, 0.0)).length(), 0.0, 1e-12);
    EXPECT_NEAR((arc.derivative(0.5) - Vec3d(3.0, 0.0, 0.0)).length(), 0.0, 1e-12);
    EXPECT_NEAR((arc.secondDerivative(0.5) - Vec3d(0.0, -12.0, 0.0)).length(), 0.0, 1e-12);

    // Outside the knot domain the curve continues as its end point instead of extrapolating a span that is not there.
    EXPECT_NEAR((arc.evaluate(2.5) - Point3d(3.0, 0.0, 0.0)).length(), 0.0, 1e-12);

    // Closure is a geometric fact here: it is the two end points that must meet, not the two end poles.
    const BSplineCurved loop(3, { Point3d(0.0, 0.0, 0.0), Point3d(1.0, 1.0, 0.0), Point3d(1.0, -1.0, 0.0), Point3d(0.0, 0.0, 0.0) }, { 0.0, 1.0 }, { 4, 4 });
    EXPECT_TRUE(loop.isClosed());
}

TEST(GeometryKinds, aRationalBSplineCurveTracesAQuarterCircle)
{
    // The classic exact quarter circle: degree 2, weights 1 / sqrt(2) / 1.
    const BSplineCurved quarter(2,
                                { Point3d(1.0, 0.0, 0.0), Point3d(1.0, 1.0, 0.0), Point3d(0.0, 1.0, 0.0) },
                                { 0.0, 1.0 },
                                { 3, 3 },
                                { 1.0, std::sqrt(0.5), 1.0 });

    EXPECT_TRUE(quarter.isValid());
    EXPECT_TRUE(quarter.isRational());

    for (const double u : { 0.0, 0.25, 0.5, 0.75, 1.0 }) {
        EXPECT_NEAR((quarter.evaluate(u) - Point3d(0.0, 0.0, 0.0)).length(), 1.0, 1e-12);
    }
}

TEST(GeometryKinds, anOffsetCurveInheritsTheBasisDomain)
{
    const auto       basis = std::make_shared<LineCurved>(Point3d(0.0, 0.0, 0.0), Vec3d(1.0, 0.0, 0.0));
    const OffsetStub offset(basis, 2.0, Vec3d(0.0, 0.0, 1.0));

    // The offset is evaluated at the basis' parameter, so its range, closure and period are the basis'.
    EXPECT_TRUE(offset.isValid());
    EXPECT_TRUE(std::isinf(offset.firstParameter()));
    EXPECT_TRUE(std::isinf(offset.lastParameter()));
    EXPECT_FALSE(offset.isClosed());
    EXPECT_FALSE(offset.isPeriodic());
    EXPECT_DOUBLE_EQ(offset.period(), 0.0);
    EXPECT_EQ(offset.continuity(), vn::geom::Continuity::CN);
    EXPECT_DOUBLE_EQ(offset.reversedParameter(3.0), -3.0);

    // The displacement is reference_direction x tangent = (0, 0, 1) x (1, 0, 0) = (0, 1, 0); a straight basis keeps
    // its velocity, because the offset direction does not turn.
    EXPECT_NEAR((offset.evaluate(3.0) - Point3d(3.0, 2.0, 0.0)).length(), 0.0, 1e-12);
    EXPECT_NEAR((offset.derivative(3.0) - Vec3d(1.0, 0.0, 0.0)).length(), 0.0, 1e-12);

    // A closed periodic basis keeps both answers, and the offset of a circle is a circle.
    const OffsetStub periodic(std::make_shared<CircleCurved>(), 0.5, Vec3d(0.0, 0.0, 1.0));
    EXPECT_TRUE(periodic.isClosed());
    EXPECT_TRUE(periodic.isPeriodic());
    EXPECT_NEAR(periodic.period(), 2.0 * vn::math::PI, 1e-12);
    EXPECT_NEAR((periodic.evaluate(0.0) - Point3d(0.0, 0.0, 0.0)).length(), 0.5, 1e-12);
    EXPECT_NEAR(periodic.derivative(0.0).length(), 0.5, 1e-12);
}

TEST(GeometryKinds, anOffsetCurveAtASingularPointStaysOnTheBasis)
{
    // The basis runs along the reference direction, so there is no perpendicular to offset along: the offset falls
    // back to the basis itself instead of normalising a zero vector.
    const auto       basis = std::make_shared<LineCurved>(Point3d(0.0, 0.0, 0.0), Vec3d(0.0, 0.0, 1.0));
    const OffsetStub offset(basis, 2.0, Vec3d(0.0, 0.0, 1.0));

    EXPECT_TRUE(offset.isValid());
    EXPECT_NEAR((offset.evaluate(1.5) - Point3d(0.0, 0.0, 1.5)).length(), 0.0, 1e-12);
    EXPECT_NEAR((offset.derivative(1.5) - Vec3d(0.0, 0.0, 1.0)).length(), 0.0, 1e-12);
}

TEST(GeometryKinds, anOffsetCurveWithoutABasisAnswersInsteadOfDereferencingIt)
{
    // A null basis is what `isValid()` reports; the queries below would all be dereferences without the guard.
    const OffsetStub unbound(nullptr, 1.0, Vec3d(0.0, 0.0, 1.0));

    EXPECT_FALSE(unbound.isValid());
    EXPECT_FALSE(unbound.isClosed());
    EXPECT_FALSE(unbound.isPeriodic());
    EXPECT_DOUBLE_EQ(unbound.period(), 0.0);
    EXPECT_DOUBLE_EQ(unbound.firstParameter(), 0.0);
    EXPECT_DOUBLE_EQ(unbound.lastParameter(), 0.0);
    EXPECT_DOUBLE_EQ(unbound.reversedParameter(2.5), 2.5);
    EXPECT_EQ(unbound.continuity(), vn::geom::Continuity::C0);
    EXPECT_DOUBLE_EQ((unbound.evaluate(0.5) - Point3d()).length(), 0.0);
    EXPECT_DOUBLE_EQ(unbound.derivative(0.5).length(), 0.0);

    // The other invalid input is a reference direction of zero length: there is no perpendicular to offset along.
    const auto basis = std::make_shared<LineCurved>(Point3d(0.0, 0.0, 0.0), Vec3d(1.0, 0.0, 0.0));
    EXPECT_FALSE(OffsetStub(basis, 1.0, Vec3d(0.0, 0.0, 0.0)).isValid());
}

TEST(GeometryKinds, aPlacementCompletesAndNormalisesItsFrame)
{
    const vn::geom::spatial::Placementd world;
    EXPECT_NEAR(world.axis.z, 1.0, 1e-12);
    EXPECT_NEAR(world.x_direction.x, 1.0, 1e-12);
    EXPECT_NEAR((world.yAxis() - Vec3d(0.0, 1.0, 0.0)).length(), 0.0, 1e-12);
    EXPECT_TRUE(world.hasUsableAxis());

    // A preferred X that is not unit and not in plane is projected and normalised; the third axis is derived.
    const vn::geom::spatial::Placementd tilted(Point3d(1.0, 2.0, 3.0), Vec3d(0.0, 1.0, 0.0), Vec3d(2.0, 5.0, 0.0));
    EXPECT_NEAR(tilted.axis.length(), 1.0, 1e-12);
    EXPECT_NEAR(tilted.x_direction.length(), 1.0, 1e-12);
    EXPECT_NEAR(tilted.axis.dot(tilted.x_direction), 0.0, 1e-12);
    EXPECT_NEAR(tilted.yAxis().length(), 1.0, 1e-12);

    // A degenerate main direction is reported, and the X falls back exactly as the conics and the plane do.
    const vn::geom::spatial::Placementd degenerate(Point3d(0.0, 0.0, 0.0), Vec3d(0.0, 0.0, 0.0));
    EXPECT_FALSE(degenerate.hasUsableAxis());
    EXPECT_NEAR(degenerate.x_direction.x, 1.0, 1e-12);
}

TEST(GeometryKinds, aCylindricalSurfaceWrapsItsAxis)
{
    const CylindricalSurfaced            cylinder(Point3d(1.0, 2.0, 3.0), Vec3d(0.0, 0.0, 1.0), 2.0);
    const vn::geom::spatial::Surface<double>& as_surface = cylinder;

    // u = 0 starts at x_direction = +X; v is the signed height along the axis.
    EXPECT_NEAR((as_surface.evaluate(0.0, 5.0) - Point3d(3.0, 2.0, 8.0)).length(), 0.0, 1e-12);
    EXPECT_NEAR((as_surface.evaluate(vn::math::PI / 2.0, -1.0) - Point3d(1.0, 4.0, 2.0)).length(), 0.0, 1e-12);

    // The parallel's speed is the radius; the axis itself is the v tangent.
    EXPECT_NEAR(as_surface.derivativeU(0.0, 0.0).length(), 2.0, 1e-12);
    EXPECT_NEAR((as_surface.derivativeV(0.0, 0.0) - Vec3d(0.0, 0.0, 1.0)).length(), 0.0, 1e-12);

    EXPECT_TRUE(as_surface.isUClosed());
    EXPECT_TRUE(as_surface.isUPeriodic());
    EXPECT_FALSE(as_surface.isVClosed());
    EXPECT_FALSE(as_surface.isVPeriodic());
    EXPECT_NEAR(as_surface.uPeriod(), 2.0 * vn::math::PI, 1e-12);
    EXPECT_DOUBLE_EQ(as_surface.vPeriod(), 0.0);
    EXPECT_TRUE(std::isinf(as_surface.firstVParameter()));
    EXPECT_EQ(as_surface.continuity(), vn::geom::Continuity::CN);

    EXPECT_NEAR((cylinder.normalAt(0.0) - Vec3d(1.0, 0.0, 0.0)).length(), 0.0, 1e-12);
    EXPECT_TRUE(cylinder.isValid());
    EXPECT_FALSE(CylindricalSurfaced(Point3d(0.0, 0.0, 0.0), Vec3d(0.0, 0.0, 0.0), 2.0).isValid());
    EXPECT_FALSE(CylindricalSurfaced(Point3d(0.0, 0.0, 0.0), Vec3d(0.0, 0.0, 1.0), 0.0).isValid());
}

TEST(GeometryKinds, aConicalSurfaceWalksItsGeneratrix)
{
    const double          semi_angle = vn::math::PI / 4.0;
    const ConicalSurfaced cone(Point3d(0.0, 0.0, 0.0), Vec3d(0.0, 0.0, 1.0), 1.0, semi_angle);

    // v is arc length along the generatrix: the radius grows by v * sin(alpha) and the height by v * cos(alpha).
    const double offset = 2.0;
    const double grown  = 1.0 + offset * std::sin(semi_angle);
    const double height = offset * std::cos(semi_angle);
    EXPECT_NEAR((cone.evaluate(0.0, offset) - Point3d(grown, 0.0, height)).length(), 0.0, 1e-12);

    // That is why dP/dv is a unit vector, while dP/du has the local radius as its speed.
    EXPECT_NEAR(cone.derivativeV(0.0, offset).length(), 1.0, 1e-12);
    EXPECT_NEAR(cone.derivativeU(0.0, offset).length(), grown, 1e-12);

    EXPECT_TRUE(cone.isUClosed());
    EXPECT_TRUE(cone.isUPeriodic());
    EXPECT_FALSE(cone.isVClosed());
    EXPECT_FALSE(cone.isVPeriodic());
    EXPECT_TRUE(std::isinf(cone.lastVParameter()));

    // The normal tilts away from the axis by the semi-angle.
    EXPECT_NEAR((cone.normalAt(0.0) - Vec3d(std::cos(semi_angle), 0.0, -std::sin(semi_angle))).length(), 0.0, 1e-12);

    // A plane is not a cone: the semi-angle must stay strictly inside the open first quadrant.
    EXPECT_FALSE(ConicalSurfaced(Point3d(0.0, 0.0, 0.0), Vec3d(0.0, 0.0, 1.0), 1.0, vn::math::PI / 2.0).isValid());
    EXPECT_FALSE(ConicalSurfaced(Point3d(0.0, 0.0, 0.0), Vec3d(0.0, 0.0, 1.0), 1.0, 0.0).isValid());
}

TEST(GeometryKinds, aToroidalSurfaceClosesInBothDirections)
{
    const ToroidalSurfaced               torus(Point3d(0.0, 0.0, 1.0), Vec3d(0.0, 0.0, 1.0), 3.0, 1.0);
    const vn::geom::spatial::Surface<double>& as_surface = torus;

    // v = 0 is the outer equator, v = pi / 2 the top of the tube.
    EXPECT_NEAR((as_surface.evaluate(0.0, 0.0) - Point3d(4.0, 0.0, 1.0)).length(), 0.0, 1e-12);
    EXPECT_NEAR((as_surface.evaluate(0.0, vn::math::PI / 2.0) - Point3d(3.0, 0.0, 2.0)).length(), 0.0, 1e-12);

    EXPECT_TRUE(as_surface.isUClosed());
    EXPECT_TRUE(as_surface.isVClosed());
    EXPECT_TRUE(as_surface.isUPeriodic());
    EXPECT_TRUE(as_surface.isVPeriodic());
    EXPECT_NEAR(as_surface.uPeriod(), 2.0 * vn::math::PI, 1e-12);
    EXPECT_NEAR(as_surface.vPeriod(), 2.0 * vn::math::PI, 1e-12);

    // The tube is parameterised by arc length, so the v speed is the minor radius everywhere.
    EXPECT_NEAR(as_surface.derivativeV(0.0, 0.0).length(), 1.0, 1e-12);
    EXPECT_NEAR((torus.normalAt(0.0, 0.0) - Vec3d(1.0, 0.0, 0.0)).length(), 0.0, 1e-12);

    // A tube that swallows the axis is not a torus.
    EXPECT_FALSE(ToroidalSurfaced(Point3d(0.0, 0.0, 0.0), Vec3d(0.0, 0.0, 1.0), 1.0, 1.0).isValid());
    EXPECT_FALSE(ToroidalSurfaced(Point3d(0.0, 0.0, 0.0), Vec3d(0.0, 0.0, 1.0), 3.0, 4.0).isValid());
}

TEST(GeometryKinds, aRectangularTrimmedSurfaceForwardsToItsBasis)
{
    const auto                       basis = std::make_shared<CylindricalSurfaced>(Point3d(0.0, 0.0, 0.0), Vec3d(0.0, 0.0, 1.0), 2.0);
    const RectangularTrimmedSurfaced patch(basis, 0.0, vn::math::PI / 2.0, -1.0, 1.0);

    EXPECT_TRUE(patch.isValid());
    EXPECT_NEAR((patch.evaluate(0.0, 0.0) - Point3d(2.0, 0.0, 0.0)).length(), 0.0, 1e-12);
    EXPECT_DOUBLE_EQ(patch.firstVParameter(), -1.0);
    EXPECT_DOUBLE_EQ(patch.lastVParameter(), 1.0);

    // A quarter of the cylinder neither closes nor repeats, although its basis does both.
    EXPECT_FALSE(patch.isUClosed());
    EXPECT_FALSE(patch.isVClosed());
    EXPECT_FALSE(patch.isUPeriodic());
    EXPECT_FALSE(patch.isVPeriodic());
    EXPECT_EQ(patch.continuity(), vn::geom::Continuity::CN);
    EXPECT_NEAR(patch.derivativeU(0.0, 0.0).length(), 2.0, 1e-12);

    // Covering the whole basis does keep both answers.
    const RectangularTrimmedSurfaced full(basis, 0.0, 2.0 * vn::math::PI, -std::numeric_limits<double>::infinity(), std::numeric_limits<double>::infinity());
    EXPECT_TRUE(full.isUClosed());
    EXPECT_TRUE(full.isUPeriodic());

    // Without a basis, or with a reversed rectangle, the trim is unusable.
    EXPECT_FALSE(RectangularTrimmedSurfaced(nullptr, 0.0, 1.0, 0.0, 1.0).isValid());
    EXPECT_FALSE(RectangularTrimmedSurfaced(basis, 1.0, 0.0, 0.0, 1.0).isValid());

    // The period is the piece's own answer: a quarter neither repeats nor reports the basis' period.
    EXPECT_DOUBLE_EQ(patch.uPeriod(), 0.0);
    EXPECT_NEAR(full.uPeriod(), 2.0 * vn::math::PI, 1e-12);

    // A trim without a basis answers the degenerate values instead of dereferencing the handle.
    const RectangularTrimmedSurfaced unbound(nullptr, 0.0, 1.0, 0.0, 1.0);
    EXPECT_FALSE(unbound.isUClosed());
    EXPECT_FALSE(unbound.isVPeriodic());
    EXPECT_DOUBLE_EQ(unbound.uPeriod(), 0.0);
    EXPECT_DOUBLE_EQ((unbound.evaluate(0.5, 0.5) - Point3d()).length(), 0.0);
    EXPECT_DOUBLE_EQ(unbound.derivativeU(0.5, 0.5).length(), 0.0);
    EXPECT_DOUBLE_EQ(unbound.derivativeV(0.5, 0.5).length(), 0.0);
    EXPECT_EQ(unbound.continuity(), vn::geom::Continuity::C0);
}

TEST(GeometryKinds, aTrimmedSurfaceClosesOnlyWhenItsEdgesMeet)
{
    const auto basis = std::make_shared<CylindricalSurfaced>(Point3d(0.0, 0.0, 0.0), Vec3d(0.0, 0.0, 1.0), 2.0);

    // Covering the range but ending a fraction of a turn later leaves two different edges, even though the rectangle
    // still repeats with the basis' period.
    const RectangularTrimmedSurfaced overshooting(basis, 0.0, 2.0 * vn::math::PI + 0.3, -1.0, 1.0);
    EXPECT_TRUE(overshooting.isUPeriodic());
    EXPECT_FALSE(overshooting.isUClosed());
    EXPECT_GT((overshooting.evaluate(overshooting.firstUParameter(), 0.0)
               - overshooting.evaluate(overshooting.lastUParameter(), 0.0))
                  .length(),
              0.1);

    // Two whole turns later the two edges land on the same points again.
    const RectangularTrimmedSurfaced two_turns(basis, 0.0, 4.0 * vn::math::PI, -1.0, 1.0);
    EXPECT_TRUE(two_turns.isUPeriodic());
    EXPECT_TRUE(two_turns.isUClosed());

    // A whole turn that starts away from the basis' seam meets at its edges but does not cover the natural range.
    const RectangularTrimmedSurfaced shifted(basis, 0.3, 0.3 + 2.0 * vn::math::PI, -1.0, 1.0);
    EXPECT_FALSE(shifted.isUClosed());

    // A non-periodic closed basis has no repetition to land on: only its own natural range closes, an extrapolated
    // rectangle ends on different points. The net's first and last columns coincide, so the patch itself is u-closed.
    const BezierSurfaced closed_patch({ Point3d(0.0, 0.0, 0.0), Point3d(0.5, 1.0, 0.0), Point3d(0.0, 0.0, 0.0),
                                        Point3d(0.0, 0.0, 1.0), Point3d(0.5, 1.0, 1.0), Point3d(0.0, 0.0, 1.0) },
                                      3, 2);
    EXPECT_TRUE(closed_patch.isUClosed());
    const auto closed_basis = std::make_shared<const BezierSurfaced>(closed_patch);
    EXPECT_TRUE(RectangularTrimmedSurfaced(closed_basis, 0.0, 1.0, 0.0, 1.0).isUClosed());
    EXPECT_FALSE(RectangularTrimmedSurfaced(closed_basis, 0.0, 1.5, 0.0, 1.0).isUClosed());
}

TEST(GeometryKinds, aBezierPatchBlendsItsNet)
{
    // Bilinear net, u contiguous: pole(iu, iv) = poles[iu + 2 * iv].
    const BezierSurfaced patch({ Point3d(0.0, 0.0, 0.0), Point3d(1.0, 0.0, 0.0), Point3d(0.0, 1.0, 1.0), Point3d(1.0, 1.0, 1.0) }, 2, 2);

    EXPECT_TRUE(patch.isValid());
    EXPECT_EQ(patch.degreeU(), 1);
    EXPECT_EQ(patch.degreeV(), 1);

    // The four corners are the four poles, and the middle is their average.
    EXPECT_NEAR((patch.evaluate(0.0, 0.0) - patch.poleAt(0, 0)).length(), 0.0, 1e-12);
    EXPECT_NEAR((patch.evaluate(1.0, 1.0) - patch.poleAt(1, 1)).length(), 0.0, 1e-12);
    EXPECT_NEAR((patch.evaluate(0.5, 0.5) - Point3d(0.5, 0.5, 0.5)).length(), 0.0, 1e-12);

    // A bilinear patch is linear in both parameters: the tangents are the net's differences.
    EXPECT_NEAR((patch.derivativeU(0.5, 0.5) - Vec3d(1.0, 0.0, 0.0)).length(), 0.0, 1e-12);
    EXPECT_NEAR((patch.derivativeV(0.5, 0.5) - Vec3d(0.0, 1.0, 1.0)).length(), 0.0, 1e-12);

    EXPECT_FALSE(patch.isUClosed());
    EXPECT_FALSE(patch.isVClosed());
    EXPECT_FALSE(patch.isUPeriodic());
    EXPECT_FALSE(patch.isVPeriodic());
    EXPECT_EQ(patch.continuity(), vn::geom::Continuity::CN);

    // Closure is a data test: matching columns close the patch along u.
    const BezierSurfaced loop({ Point3d(0.0, 0.0, 0.0), Point3d(0.0, 0.0, 0.0), Point3d(0.0, 1.0, 0.0), Point3d(0.0, 1.0, 0.0) }, 2, 2);
    EXPECT_TRUE(loop.isUClosed());

    // The answers are reachable through the interface, so they are overrides rather than hidden members.
    const vn::geom::spatial::Surface<double>& as_surface = patch;
    EXPECT_NEAR(as_surface.evaluate(0.5, 0.5).z, 0.5, 1e-12);
}

TEST(GeometryKinds, aRationalBezierPatchPullsTowardsItsHeavierCorner)
{
    const BezierSurfaced patch({ Point3d(0.0, 0.0, 0.0), Point3d(1.0, 0.0, 0.0), Point3d(0.0, 1.0, 0.0), Point3d(1.0, 1.0, 0.0) },
                               2,
                               2,
                               { 1.0, 1.0, 1.0, 3.0 });

    EXPECT_TRUE(patch.isRational());
    EXPECT_TRUE(patch.isValid());

    // Weights 1, 1, 1, 3 put the centre at (2/3, 2/3) instead of (1/2, 1/2).
    EXPECT_NEAR(patch.evaluate(0.5, 0.5).x, 2.0 / 3.0, 1e-12);
    EXPECT_NEAR(patch.evaluate(0.5, 0.5).y, 2.0 / 3.0, 1e-12);
}

TEST(GeometryKinds, aBSplinePatchSumsItsTwoBasisFamilies)
{
    // Degree 1 in both directions with clamped knots is exactly the bilinear Bezier patch above.
    const BSplineSurfaced bilinear({ Point3d(0.0, 0.0, 0.0), Point3d(1.0, 0.0, 0.0), Point3d(0.0, 1.0, 0.0), Point3d(1.0, 1.0, 0.0) },
                                   2,
                                   2,
                                   1,
                                   1,
                                   { 0.0, 1.0 },
                                   { 2, 2 },
                                   { 0.0, 1.0 },
                                   { 2, 2 });

    EXPECT_TRUE(bilinear.isValid());
    EXPECT_NEAR((bilinear.evaluate(0.5, 0.5) - Point3d(0.5, 0.5, 0.0)).length(), 0.0, 1e-12);
    EXPECT_NEAR((bilinear.derivativeU(0.0, 0.0) - Vec3d(1.0, 0.0, 0.0)).length(), 0.0, 1e-12);
    EXPECT_NEAR((bilinear.derivativeV(0.0, 0.0) - Vec3d(0.0, 1.0, 0.0)).length(), 0.0, 1e-12);
    EXPECT_EQ(bilinear.continuity(), vn::geom::Continuity::CN);

    // Quadratic in u, linear in v: the u tangent at the start is the degree times the first net difference.
    const BSplineSurfaced apron(
        { Point3d(0.0, 0.0, 0.0), Point3d(1.0, 1.0, 0.0), Point3d(2.0, 0.0, 0.0), Point3d(0.0, 0.0, 2.0), Point3d(1.0, 1.0, 2.0), Point3d(2.0, 0.0, 2.0) },
        3,
        2,
        2,
        1,
        { 0.0, 1.0 },
        { 3, 3 },
        { 0.0, 1.0 },
        { 2, 2 });

    EXPECT_TRUE(apron.isValid());
    EXPECT_NEAR((apron.evaluate(0.0, 0.0) - Point3d(0.0, 0.0, 0.0)).length(), 0.0, 1e-12);
    EXPECT_NEAR((apron.evaluate(1.0, 1.0) - Point3d(2.0, 0.0, 2.0)).length(), 0.0, 1e-12);
    EXPECT_NEAR((apron.derivativeU(0.0, 0.0) - Vec3d(2.0, 2.0, 0.0)).length(), 0.0, 1e-12);

    // Outside a knot domain the patch continues as its boundary instead of extrapolating.
    EXPECT_NEAR((apron.evaluate(1.5, 0.0) - apron.evaluate(1.0, 0.0)).length(), 0.0, 1e-12);

    // An internal multiplicity costs an order of continuity, and the worse direction wins: a quadratic in u with an
    // internal double knot is C0 there, so the 5x2 net (5 + 2 + 1 = 3 + 2 + 3 knots) is the one that fits.
    const BSplineSurfaced kinked({ Point3d(0.0, 0.0, 0.0),
                                   Point3d(1.0, 1.0, 0.0),
                                   Point3d(2.0, 0.0, 0.0),
                                   Point3d(3.0, 1.0, 0.0),
                                   Point3d(4.0, 0.0, 0.0),
                                   Point3d(0.0, 0.0, 2.0),
                                   Point3d(1.0, 1.0, 2.0),
                                   Point3d(2.0, 0.0, 2.0),
                                   Point3d(3.0, 1.0, 2.0),
                                   Point3d(4.0, 0.0, 2.0) },
                                 5,
                                 2,
                                 2,
                                 1,
                                 { 0.0, 0.5, 1.0 },
                                 { 3, 2, 3 },
                                 { 0.0, 1.0 },
                                 { 2, 2 });
    EXPECT_TRUE(kinked.isValid());
    EXPECT_EQ(kinked.continuity(), vn::geom::Continuity::C0);
}

TEST(GeometryKinds, aPlaneAnswersTheSurfaceInterface)
{
    const Planed                         plane(Point3d(1.0, 2.0, 3.0), Vec3d(0.0, 0.0, 1.0));
    const vn::geom::spatial::Surface<double>& as_surface = plane;

    // The in-plane axes are derived from the normal, so a Z normal keeps the world X and Y axes.
    EXPECT_NEAR((as_surface.evaluate(1.0, 2.0) - Point3d(2.0, 4.0, 3.0)).length(), 0.0, 1e-12);
    EXPECT_NEAR(as_surface.derivativeU(0.0, 0.0).x, 1.0, 1e-12);
    EXPECT_NEAR(as_surface.derivativeV(0.0, 0.0).y, 1.0, 1e-12);

    EXPECT_TRUE(std::isinf(as_surface.firstUParameter()));
    EXPECT_TRUE(std::isinf(as_surface.lastVParameter()));
    EXPECT_FALSE(as_surface.isUClosed());
    EXPECT_FALSE(as_surface.isVClosed());
    EXPECT_FALSE(as_surface.isUPeriodic());
    EXPECT_DOUBLE_EQ(as_surface.uPeriod(), 0.0);
    EXPECT_EQ(as_surface.continuity(), vn::geom::Continuity::CN);
    EXPECT_TRUE(as_surface.isCN(2));
}

TEST(GeometryKinds, aSphericalSheetClosesOnlyAroundItsAxis)
{
    const SphericalSurfaced              sheet(Point3d(0.0, 0.0, 0.0), 2.0);
    const vn::geom::spatial::Surface<double>& as_surface = sheet;

    EXPECT_DOUBLE_EQ(as_surface.firstUParameter(), 0.0);
    EXPECT_NEAR(as_surface.lastUParameter(), 2.0 * vn::math::PI, 1e-12);
    EXPECT_DOUBLE_EQ(as_surface.firstVParameter(), 0.0);
    EXPECT_NEAR(as_surface.lastVParameter(), vn::math::PI, 1e-12);

    // The azimuth wraps around the axis; the polar range ends at the two poles, which is not a seam.
    EXPECT_TRUE(as_surface.isUClosed());
    EXPECT_FALSE(as_surface.isVClosed());
    EXPECT_TRUE(as_surface.isUPeriodic());
    EXPECT_FALSE(as_surface.isVPeriodic());
    EXPECT_NEAR(as_surface.uPeriod(), 2.0 * vn::math::PI, 1e-12);
    EXPECT_DOUBLE_EQ(as_surface.vPeriod(), 0.0);

    // A meridian moves at the radius' speed; a parallel slows to nothing at the poles.
    EXPECT_NEAR(as_surface.derivativeV(0.7, 1.1).length(), 2.0, 1e-12);
    EXPECT_NEAR(as_surface.derivativeU(0.7, vn::math::PI / 2.0).length(), 2.0, 1e-12);
    EXPECT_NEAR(as_surface.derivativeU(0.7, 0.0).length(), 0.0, 1e-12);

    EXPECT_NEAR((as_surface.evaluate(0.0, vn::math::PI / 2.0) - Point3d(2.0, 0.0, 0.0)).length(), 0.0, 1e-12);
    EXPECT_EQ(as_surface.continuity(), vn::geom::Continuity::CN);
}

/// The promises `Curve<T>` itself makes, asked of every implementation instead of one type at a time: a curve that
/// answers `isPeriodic()` without a period, or a reversal that is not an involution, breaks every caller at once.
template <vn::math::FP T>
void expectCurveInvariants(const vn::geom::spatial::Curve<T>& curve)
{
    const T first     = curve.firstParameter();
    const T last      = curve.lastParameter();
    const T tolerance = T(1000) * std::numeric_limits<T>::epsilon();
    EXPECT_TRUE(curve.isValid());
    EXPECT_LE(first, last);

    if (curve.isPeriodic()) {
        EXPECT_GT(curve.period(), T(0));
    }
    else {
        EXPECT_EQ(curve.period(), T(0));
    }

    // A negative order is refused rather than read as C0, and only `CN` answers an order past the ladder's top.
    EXPECT_FALSE(curve.isCN(-1));
    EXPECT_EQ(curve.isCN(4), curve.continuity() == vn::geom::Continuity::CN);

    if (!std::isfinite(first) || !std::isfinite(last)) {
        return;
    }

    // Closure is a necessary condition on the geometry, not only a structural answer: a curve that claims to be
    // closed must meet at its own ends.
    if (curve.isClosed()) {
        EXPECT_NEAR((curve.evaluate(last) - curve.evaluate(first)).length(), T(0), tolerance);
    }

    const T middle = first + (last - first) * T(0.3);
    EXPECT_NEAR(curve.reversedParameter(curve.reversedParameter(middle)), middle, tolerance);
    if (!curve.isPeriodic()) {
        // A bounded piece reflects inside its own range; a periodic curve reflects inside one period and answers
        // `period - u` instead, so this direction of the check only applies here.
        EXPECT_NEAR(curve.reversedParameter(first), last, tolerance);
    }

    const auto point = curve.evaluate(middle);
    EXPECT_TRUE(std::isfinite(point.x) && std::isfinite(point.y) && std::isfinite(point.z));
    EXPECT_TRUE(std::isfinite(curve.derivative(middle).length()));
    EXPECT_TRUE(std::isfinite(curve.secondDerivative(middle).length()));

    // Derivative order is a capability: the two named orders are always answerable and agree with `derivativeAt()`,
    // and asking past the reported capability answers empty rather than a fabricated zero.
    const int max_order = curve.maxDerivativeOrder();
    EXPECT_GE(max_order, 2);

    const auto order_one = curve.derivativeAt(middle, 1);
    const auto order_two = curve.derivativeAt(middle, 2);
    ASSERT_TRUE(order_one.has_value());
    ASSERT_TRUE(order_two.has_value());
    EXPECT_NEAR((*order_one - curve.derivative(middle)).length(), T(0), tolerance);
    EXPECT_NEAR((*order_two - curve.secondDerivative(middle)).length(), T(0), tolerance);
    EXPECT_FALSE(curve.derivativeAt(middle, 0).has_value());
    EXPECT_FALSE(curve.derivativeAt(middle, -1).has_value());
    EXPECT_FALSE(curve.derivativeAt(middle, max_order + 1).has_value());

    if (curve.isPeriodic()) {
        // The period's own promise is that the curve repeats. Closure is a different fact: a piece that covers the
        // basis' range and ends a fraction of a period later repeats without closing.
        EXPECT_NEAR((curve.evaluate(middle + curve.period()) - point).length(), T(0), tolerance);
    }
}

/// The surface counterpart: the domain is ordered, a periodic direction repeats with its own period, closure and the
/// period value are the surface's own answers, and both partial derivatives exist inside a finite rectangle.
template <vn::math::FP T>
void expectSurfaceInvariants(const vn::geom::spatial::Surface<T>& surface)
{
    const T first_u   = surface.firstUParameter();
    const T last_u    = surface.lastUParameter();
    const T first_v   = surface.firstVParameter();
    const T last_v    = surface.lastVParameter();
    const T tolerance = T(1000) * std::numeric_limits<T>::epsilon();
    EXPECT_TRUE(surface.isValid());
    EXPECT_LE(first_u, last_u);
    EXPECT_LE(first_v, last_v);

    if (surface.isUPeriodic()) {
        EXPECT_GT(surface.uPeriod(), T(0));
    }
    else {
        EXPECT_EQ(surface.uPeriod(), T(0));
    }

    if (surface.isVPeriodic()) {
        EXPECT_GT(surface.vPeriod(), T(0));
    }
    else {
        EXPECT_EQ(surface.vPeriod(), T(0));
    }

    EXPECT_FALSE(surface.isCN(-1));
    EXPECT_EQ(surface.isCN(4), surface.continuity() == vn::geom::Continuity::CN);

    if (!std::isfinite(first_u) || !std::isfinite(last_u) || !std::isfinite(first_v) || !std::isfinite(last_v)) {
        return;
    }

    // A closed direction means the two boundary edges coincide, which is the necessary condition the interface
    // states; a structural answer without it would lie to every caller that welds a seam.
    if (surface.isUClosed()) {
        const T v = first_v + (last_v - first_v) * T(0.5);
        EXPECT_NEAR((surface.evaluate(last_u, v) - surface.evaluate(first_u, v)).length(), T(0), tolerance);
    }
    if (surface.isVClosed()) {
        const T u = first_u + (last_u - first_u) * T(0.5);
        EXPECT_NEAR((surface.evaluate(u, last_v) - surface.evaluate(u, first_v)).length(), T(0), tolerance);
    }

    const T    middle_u = first_u + (last_u - first_u) * T(0.3);
    const T    middle_v = first_v + (last_v - first_v) * T(0.3);
    const auto point    = surface.evaluate(middle_u, middle_v);
    EXPECT_TRUE(std::isfinite(point.x) && std::isfinite(point.y) && std::isfinite(point.z));
    EXPECT_TRUE(std::isfinite(surface.derivativeU(middle_u, middle_v).length()));
    EXPECT_TRUE(std::isfinite(surface.derivativeV(middle_u, middle_v).length()));

    // The period's own promise is repetition; closure additionally needs the range to cover the natural domain and
    // to end a whole number of periods later, so a trim may repeat without closing.
    if (surface.isUPeriodic()) {
        EXPECT_NEAR((surface.evaluate(middle_u + surface.uPeriod(), middle_v) - point).length(), T(0), tolerance);
    }
    if (surface.isVPeriodic()) {
        EXPECT_NEAR((surface.evaluate(middle_u, middle_v + surface.vPeriod()) - point).length(), T(0), tolerance);
    }
}

/// Builds one instance of every curve implementation at precision @p T and asks each of them the interface's own
/// promises. The library exports both precisions (ADR-2026-11-04-geometry-precision-and-instantiation), so a sweep
/// that only ran in `double` would leave the float instantiations unchecked.
template <vn::math::FP T>
void sweepCurves()
{
    using Point  = vn::math::Point3<T>;
    using Vector = vn::math::Vector3<T>;
    using Curve  = vn::geom::spatial::Curve<T>;

    const auto point  = [](double x, double y, double z) { return Point(T(x), T(y), T(z)); };
    const auto vector = [](double x, double y, double z) { return Vector(T(x), T(y), T(z)); };

    const vn::geom::spatial::LineCurve<T>      line(point(0.0, 0.0, 0.0), vector(1.0, 1.0, 0.0));
    const vn::geom::spatial::CircleCurve<T>    circle(point(0.0, 0.0, 0.0), vector(0.0, 0.0, 1.0), T(2.0));
    const vn::geom::spatial::EllipseCurve<T>   ellipse(point(0.0, 0.0, 0.0), vector(0.0, 0.0, 1.0), vector(1.0, 0.0, 0.0), T(3.0), T(1.0));
    const vn::geom::spatial::HyperbolaCurve<T> hyperbola(point(0.0, 0.0, 0.0), vector(0.0, 0.0, 1.0), vector(1.0, 0.0, 0.0), T(2.0), T(1.0));
    const vn::geom::spatial::ParabolaCurve<T>  parabola(point(0.0, 0.0, 0.0), vector(0.0, 0.0, 1.0), vector(1.0, 0.0, 0.0), T(1.0));
    const vn::geom::spatial::BezierCurve<T>    bezier({ point(0.0, 0.0, 0.0), point(1.0, 2.0, 0.0), point(2.0, 0.0, 1.0) });
    const vn::geom::spatial::BSplineCurve<T>   spline(2, { point(0.0, 0.0, 0.0), point(1.0, 1.0, 0.0), point(2.0, 0.0, 1.0) }, { T(0.0), T(1.0) }, { 3, 3 });
    const auto                            basis = std::make_shared<vn::geom::spatial::CircleCurve<T>>(point(0.0, 0.0, 0.0), vector(0.0, 0.0, 1.0), T(1.0));
    const vn::geom::spatial::TrimmedCurve<T>   trim(basis, T(0.5), T(2.0));

    const Curve* const curves[] = { &line, &circle, &ellipse, &hyperbola, &parabola, &bezier, &spline, &trim };
    for (const Curve* curve : curves) {
        expectCurveInvariants<T>(*curve);
    }

    // The offset is a value type now, so it takes part in the same sweep: its ability to answer the second derivative
    // comes from the basis' third.
    const auto offset_basis = std::make_shared<vn::geom::spatial::CircleCurve<T>>(point(0.0, 0.0, 0.0), vector(0.0, 0.0, 1.0), T(2.0));
    const vn::geom::spatial::OffsetCurve<T> offset(offset_basis, T(0.5), vector(0.0, 0.0, 1.0));
    expectCurveInvariants<T>(offset);
}

TEST(GeometryKinds, everyCurveKeepsTheInterfacePromises)
{
    sweepCurves<double>();
}

TEST(GeometryKinds, everyCurveKeepsTheInterfacePromisesInFloat)
{
    sweepCurves<float>();
}

/// Every space curve answers a third derivative, and it agrees with differentiating the second one numerically.
TEST(GeometryKinds, everySpaceCurveAnswersItsThirdDerivative)
{
    const Point3d origin(0.0, 0.0, 0.0);
    const Vec3d   z_axis(0.0, 0.0, 1.0);
    const Vec3d   x_axis(1.0, 0.0, 0.0);
    const double  h         = 1e-5;
    const double  tolerance = 1e-3;

    const LineCurved      line(origin, Vec3d(1.0, 1.0, 0.0));
    const CircleCurved    circle(origin, z_axis, 2.0);
    const EllipseCurved   ellipse(origin, z_axis, x_axis, 3.0, 1.0);
    const HyperbolaCurved hyperbola(origin, z_axis, x_axis, 2.0, 1.0);
    const ParabolaCurved  parabola(origin, z_axis, x_axis, 1.0);
    const BezierCurved    bezier({ Point3d(0.0, 0.0, 0.0), Point3d(1.0, 2.0, 0.0), Point3d(2.0, 0.0, 1.0),
                                   Point3d(3.0, 1.0, 2.0) });
    const BSplineCurved   spline(3, { Point3d(0.0, 0.0, 0.0), Point3d(1.0, 2.0, 0.0), Point3d(2.0, 0.0, 1.0),
                                      Point3d(3.0, 1.0, 2.0) }, { 0.0, 1.0 }, { 4, 4 });
    const BezierCurved    rational({ Point3d(1.0, 0.0, 0.0), Point3d(1.0, 1.0, 0.5), Point3d(0.0, 1.0, 1.0),
                                     Point3d(-1.0, 0.5, 1.5) }, { 1.0, std::sqrt(0.5), 1.0, 1.2 });

    const auto expect_jerk = [&](const auto& curve, double from, double to) {
        for (const double fraction : { 0.25, 0.5, 0.75 }) {
            const double u = from + (to - from) * fraction;
            const auto   jerk = curve.derivativeAt(u, 3);
            ASSERT_TRUE(jerk.has_value());
            const auto before = curve.secondDerivative(u - h);
            const auto after  = curve.secondDerivative(u + h);
            EXPECT_NEAR((*jerk - (after - before) / (2.0 * h)).length(), 0.0, tolerance);
        }
    };

    ASSERT_EQ(line.maxDerivativeOrder(), 3);
    ASSERT_EQ(circle.maxDerivativeOrder(), 3);
    ASSERT_EQ(ellipse.maxDerivativeOrder(), 3);
    ASSERT_EQ(hyperbola.maxDerivativeOrder(), 3);
    ASSERT_EQ(parabola.maxDerivativeOrder(), 3);
    ASSERT_EQ(bezier.maxDerivativeOrder(), 3);
    ASSERT_EQ(spline.maxDerivativeOrder(), 3);
    ASSERT_EQ(rational.maxDerivativeOrder(), 3);

    expect_jerk(line, -1.0, 1.0);
    expect_jerk(circle, 0.2, 6.0);
    expect_jerk(ellipse, 0.2, 6.0);
    expect_jerk(hyperbola, -1.5, 1.5);
    expect_jerk(parabola, -2.0, 2.0);
    expect_jerk(bezier, 0.05, 0.95);
    expect_jerk(spline, 0.05, 0.95);
    expect_jerk(rational, 0.05, 0.95);

    // The closed forms: a circle's jerk is the negated velocity, an ellipse's too, a hyperbola's equals its velocity,
    // and a line or a parabola has none.
    EXPECT_NEAR((circle.thirdDerivative(1.1) + circle.derivative(1.1)).length(), 0.0, 1e-12);
    EXPECT_NEAR((ellipse.thirdDerivative(1.1) + ellipse.derivative(1.1)).length(), 0.0, 1e-12);
    EXPECT_NEAR((hyperbola.thirdDerivative(1.1) - hyperbola.derivative(1.1)).length(), 0.0, 1e-12);
    EXPECT_EQ(line.thirdDerivative(1.1).length(), 0.0);
    EXPECT_EQ(parabola.thirdDerivative(1.1).length(), 0.0);
}

/// The space offset answers its second derivative exactly, and reports what the basis lets it answer.
TEST(GeometryKinds, theSpaceOffsetAnswersASecondDerivativeExactly)
{
    const Point3d origin(0.0, 0.0, 0.0);
    const Vec3d   reference(0.0, 0.0, 1.0);
    const auto    basis = std::make_shared<CircleCurved>(origin, reference, 2.0);

    const OffsetCurved inner(basis, 0.5, reference);
    ASSERT_TRUE(inner.isValid());
    EXPECT_EQ(inner.maxDerivativeOrder(), 2);
    EXPECT_TRUE(inner.derivativeAt(1.0, 2).has_value());
    EXPECT_FALSE(inner.derivativeAt(1.0, 3).has_value());

    // Radius 1.5 about the same centre: the acceleration points at the centre with that length.
    const double h = 1e-5;
    for (const double u : { 0.3, 1.4, 4.0 }) {
        const auto symbolic = inner.secondDerivative(u);
        const auto before   = inner.derivative(u - h);
        const auto after    = inner.derivative(u + h);
        EXPECT_NEAR((symbolic - (after - before) / (2.0 * h)).length(), 0.0, 1e-4);
        EXPECT_NEAR(symbolic.length(), 1.5, 1e-9);
    }

    // A zero distance is the basis itself, derivative by derivative.
    const OffsetCurved zero(basis, 0.0, reference);
    for (const double u : { 0.0, 0.7, 2.9 }) {
        EXPECT_NEAR((zero.evaluate(u) - basis->evaluate(u)).length(), 0.0, 1e-12);
        EXPECT_NEAR((zero.secondDerivative(u) - basis->secondDerivative(u)).length(), 0.0, 1e-12);
    }

    // A circle keeps `|reference x tangent|` constant, so it exercises none of the radial-rate terms; a spline does.
    const auto spline_basis = std::make_shared<BezierCurved>(
        BezierCurved{ { Point3d(0.0, 0.0, 0.0), Point3d(1.0, 2.0, 0.5), Point3d(2.0, -1.0, 1.0), Point3d(3.0, 1.0, 0.0) } });
    const OffsetCurved over_spline(spline_basis, 0.4, reference);
    ASSERT_TRUE(over_spline.isValid());
    for (const double u : { 0.15, 0.45, 0.8 }) {
        const auto symbolic = over_spline.secondDerivative(u);
        const auto before   = over_spline.derivative(u - h);
        const auto after    = over_spline.derivative(u + h);
        EXPECT_NEAR((symbolic - (after - before) / (2.0 * h)).length(), 0.0, 1e-4);
    }
}

/// The offset of an offset needs the basis' fourth derivative, so it reports one order and refuses the second.
TEST(GeometryKinds, anOffsetOfAnOffsetStopsAtTheVelocity)
{
    const Point3d origin(0.0, 0.0, 0.0);
    const Vec3d   reference(0.0, 0.0, 1.0);
    const auto    circle = std::make_shared<CircleCurved>(origin, reference, 2.0);

    auto outer_basis = std::make_shared<OffsetCurved>(circle, 0.5, reference);
    const OffsetCurved twice(outer_basis, 0.25, reference);

    EXPECT_EQ(twice.maxDerivativeOrder(), 1);
    EXPECT_FALSE(twice.isValid());
    EXPECT_TRUE(twice.derivativeAt(1.0, 1).has_value());
    EXPECT_FALSE(twice.derivativeAt(1.0, 2).has_value());
    EXPECT_EQ(twice.secondDerivative(1.0).length(), 0.0);
    // The position is still exact: only the order that needs a fourth derivative is missing.
    EXPECT_NEAR((twice.evaluate(1.0) - origin).length(), 1.25, 1e-12);
}

/// The surface counterpart of `sweepCurves()`, at the same two precisions.
template <vn::math::FP T>
void sweepSurfaces()
{
    using Point   = vn::math::Point3<T>;
    using Vector  = vn::math::Vector3<T>;
    using Surface = vn::geom::spatial::Surface<T>;

    const auto point  = [](double x, double y, double z) { return Point(T(x), T(y), T(z)); };
    const auto vector = [](double x, double y, double z) { return Vector(T(x), T(y), T(z)); };

    const vn::geom::spatial::Plane<T>              plane(point(0.0, 0.0, 0.0), vector(0.0, 0.0, 1.0));
    const vn::geom::spatial::SphericalSurface<T>   sphere(point(0.0, 0.0, 0.0), T(2.0));
    const vn::geom::spatial::CylindricalSurface<T> cylinder(point(0.0, 0.0, 0.0), vector(0.0, 0.0, 1.0), T(2.0));
    const vn::geom::spatial::ConicalSurface<T>     cone(point(0.0, 0.0, 0.0), vector(0.0, 0.0, 1.0), T(1.0), T(vn::math::PI / 4.0));
    const vn::geom::spatial::ToroidalSurface<T>    torus(point(0.0, 0.0, 0.0), vector(0.0, 0.0, 1.0), T(3.0), T(1.0));
    const auto basis = std::make_shared<vn::geom::spatial::CylindricalSurface<T>>(point(0.0, 0.0, 0.0), vector(0.0, 0.0, 1.0), T(2.0));
    const vn::geom::spatial::RectangularTrimmedSurface<T> quarter(basis, T(0.0), T(vn::math::PI / 2.0), T(-1.0), T(1.0));
    const vn::geom::spatial::BezierSurface<T>             patch({ point(0.0, 0.0, 0.0), point(1.0, 0.0, 0.0), point(0.0, 1.0, 1.0), point(1.0, 1.0, 1.0) }, 2, 2);
    const vn::geom::spatial::BSplineSurface<T>            spline({ point(0.0, 0.0, 0.0), point(1.0, 0.0, 0.0), point(0.0, 1.0, 1.0), point(1.0, 1.0, 1.0) },
                                                            2,
                                                            2,
                                                            1,
                                                            1,
                                                            { T(0.0), T(1.0) },
                                                            { 2, 2 },
                                                            { T(0.0), T(1.0) },
                                                            { 2, 2 });

    const Surface* const surfaces[] = { &plane, &sphere, &cylinder, &cone, &torus, &quarter, &patch, &spline };
    for (const Surface* surface : surfaces) {
        expectSurfaceInvariants<T>(*surface);
    }
}

TEST(GeometryKinds, everySurfaceKeepsTheInterfacePromises)
{
    sweepSurfaces<double>();
}

TEST(GeometryKinds, everySurfaceKeepsTheInterfacePromisesInFloat)
{
    sweepSurfaces<float>();
}

TEST(GeometryKinds, anEmptyPatchIsRecognisablyEmpty)
{
    // The default constructor exists because an empty net is a state `isValid()` names, not because "no data" is a
    // usable patch: queries answer the degenerate values instead of reading the empty arrays.
    const BezierSurfaced  bezier;
    const BSplineSurfaced spline;

    EXPECT_FALSE(bezier.isValid());
    EXPECT_FALSE(spline.isValid());
    EXPECT_FALSE(bezier.isUClosed());
    EXPECT_FALSE(spline.isVClosed());
    EXPECT_DOUBLE_EQ((bezier.evaluate(0.5, 0.5) - Point3d()).length(), 0.0);
    EXPECT_DOUBLE_EQ(spline.derivativeU(0.5, 0.5).length(), 0.0);
    EXPECT_DOUBLE_EQ(spline.derivativeV(0.5, 0.5).length(), 0.0);
}

TEST(GeometryKinds, validityIsAskableThroughTheInterface)
{
    // The four answers below are the ones that used to need a concrete type, a `dynamic_cast` or a guess: a curve, a
    // surface, a view and a patch, each asked through the interface they are handed over as.
    const vn::geom::spatial::Curve<double>& degenerate_line       = LineCurved(Point3d(1.0, 2.0, 3.0), Vec3d(0.0, 0.0, 0.0));
    const vn::geom::spatial::Curve<double>& trimmed_without_basis = TrimmedCurved(nullptr, 0.0, 1.0);
    const vn::geom::spatial::Curve<double>& usable_circle         = CircleCurved(Point3d(0.0, 0.0, 0.0), Vec3d(0.0, 0.0, 1.0), 1.0);
    const vn::geom::spatial::Curve<double>& empty_spline          = BSplineCurved();

    EXPECT_FALSE(degenerate_line.isValid());
    EXPECT_FALSE(trimmed_without_basis.isValid());
    EXPECT_TRUE(usable_circle.isValid());
    EXPECT_FALSE(empty_spline.isValid());

    // A plane whose normal has no length has no in-plane axes either: that is a validity answer, not a parameter
    // answer, and it is the one sheet whose answer is derived rather than stored.
    const vn::geom::spatial::Surface<double>& flat_plane       = Planed(Point3d(0.0, 0.0, 0.0), Vec3d(0.0, 0.0, 0.0));
    const vn::geom::spatial::Surface<double>& usable_plane     = Planed(Point3d(0.0, 0.0, 0.0), Vec3d(0.0, 0.0, 1.0));
    const vn::geom::spatial::Surface<double>& collapsed_sphere = SphericalSurfaced(Point3d(0.0, 0.0, 0.0), 0.0);

    EXPECT_FALSE(flat_plane.isValid());
    EXPECT_TRUE(usable_plane.isValid());
    EXPECT_FALSE(collapsed_sphere.isValid());
}

} // namespace
