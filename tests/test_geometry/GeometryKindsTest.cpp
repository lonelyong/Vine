#include <gtest/gtest.h>

#include <cmath>
#include <memory>
#include <type_traits>

#include <vine/math/Math.hpp>
#include <vine/math/Point3.hpp>
#include <vine/math/Vector3.hpp>

#include <vine/geometry/BezierCurve.hpp>
#include <vine/geometry/BezierSurface.hpp>
#include <vine/geometry/BSplineCurve.hpp>
#include <vine/geometry/BSplineSurface.hpp>
#include <vine/geometry/CircleCurve.hpp>
#include <vine/geometry/ConicalSurface.hpp>
#include <vine/geometry/CylindricalSurface.hpp>
#include <vine/geometry/EllipseCurve.hpp>
#include <vine/geometry/HyperbolaCurve.hpp>
#include <vine/geometry/LineCurve.hpp>
#include <vine/geometry/OffsetCurve.hpp>
#include <vine/geometry/ParabolaCurve.hpp>
#include <vine/geometry/Plane.hpp>
#include <vine/geometry/Primitive.hpp>
#include <vine/geometry/Ray.hpp>
#include <vine/geometry/RectangularTrimmedSurface.hpp>
#include <vine/geometry/SphericalSurface.hpp>
#include <vine/geometry/ToroidalSurface.hpp>
#include <vine/geometry/TrimmedCurve.hpp>

namespace {

using vn::geometry::Box;
using vn::geometry::CircleCurve;
using vn::geometry::CircleCurved;
using vn::geometry::Cone;
using vn::geometry::Cylinder;
using vn::geometry::EllipseCurved;
using vn::geometry::Ellipsoid;
using vn::geometry::HyperbolaCurved;
using vn::geometry::LineCurve;
using vn::geometry::LineCurved;
using vn::geometry::ParabolaCurved;
using vn::geometry::Plane;
using vn::geometry::Planed;
using vn::geometry::Ray;
using vn::geometry::Rayd;
using vn::geometry::Sphere;
using vn::geometry::SphericalSurface;
using vn::geometry::SphericalSurfaced;
using vn::geometry::TrimmedCurved;
using vn::math::Point3d;
using vn::math::Vec3d;

/// The curve interface is a class template, so membership is asked per precision.
static_assert(std::is_base_of_v<vn::geometry::Curve<double>, LineCurved>);
static_assert(std::is_base_of_v<vn::geometry::Curve<double>, CircleCurved>);
static_assert(std::is_base_of_v<vn::geometry::ConicCurve<double>, CircleCurved>);
static_assert(std::is_base_of_v<vn::geometry::ConicCurve<double>, EllipseCurved>);
static_assert(std::is_base_of_v<vn::geometry::ConicCurve<double>, HyperbolaCurved>);
static_assert(std::is_base_of_v<vn::geometry::ConicCurve<double>, ParabolaCurved>);

/// The three curves whose evaluation needs data the interface does not describe yet stay abstract.
static_assert(std::is_abstract_v<vn::geometry::BezierCurve<double>>);
static_assert(std::is_abstract_v<vn::geometry::BSplineCurve<double>>);
static_assert(std::is_abstract_v<vn::geometry::OffsetCurve<double>>);

/// `Ray` is deliberately not a curve: it is copied per intersection query and stays a plain value.
static_assert(!std::is_base_of_v<vn::geometry::Curve<double>, Rayd>);
static_assert(std::is_trivially_copyable_v<Rayd>);

static_assert(std::is_base_of_v<vn::geometry::Surface<double>, Planed>);

/// The surface interface is abstract and the two implemented sheets answer it.
static_assert(std::is_abstract_v<vn::geometry::Surface<double>>);
static_assert(!std::is_abstract_v<Planed>);
static_assert(!std::is_abstract_v<SphericalSurfaced>);

/// A surface is either an unbounded analytic sheet or a bounded patch; the implemented ones sit under the right one.
static_assert(std::is_base_of_v<vn::geometry::ElementarySurface<double>, Planed>);
static_assert(std::is_base_of_v<vn::geometry::ElementarySurface<double>, SphericalSurfaced>);
static_assert(!std::is_base_of_v<vn::geometry::BoundedSurface<double>, Planed>);

/// The not-yet-implemented surfaces are in the tree, and cannot be constructed by accident.
static_assert(std::is_base_of_v<vn::geometry::ElementarySurface<double>, vn::geometry::CylindricalSurface<double>>);
static_assert(std::is_base_of_v<vn::geometry::ElementarySurface<double>, vn::geometry::ConicalSurface<double>>);
static_assert(std::is_base_of_v<vn::geometry::ElementarySurface<double>, vn::geometry::ToroidalSurface<double>>);
static_assert(std::is_base_of_v<vn::geometry::BoundedSurface<double>, vn::geometry::BezierSurface<double>>);
static_assert(std::is_base_of_v<vn::geometry::BoundedSurface<double>, vn::geometry::BSplineSurface<double>>);
static_assert(std::is_base_of_v<vn::geometry::BoundedSurface<double>, vn::geometry::RectangularTrimmedSurface<double>>);
static_assert(!std::is_default_constructible_v<vn::geometry::CylindricalSurface<double>>);

/// The parametric geometries are modeling types, not algorithm families: they are `Primitive` subclasses whose only
/// geometry answers are their own bounds and validity.
static_assert(std::is_base_of_v<vn::geometry::Primitive, Box>);
static_assert(std::is_base_of_v<vn::geometry::Primitive, Sphere>);
static_assert(std::is_base_of_v<vn::geometry::Primitive, Cylinder>);
static_assert(std::is_base_of_v<vn::geometry::Primitive, Cone>);
static_assert(std::is_base_of_v<vn::geometry::Primitive, Ellipsoid>);
static_assert(!std::is_base_of_v<vn::geometry::Surface<double>, Box>);

/// The unbounded spherical sheet is the surface counterpart: it is a surface, not a `Primitive`.
static_assert(std::is_base_of_v<vn::geometry::Surface<double>, SphericalSurfaced>);
static_assert(!std::is_base_of_v<vn::geometry::Primitive, SphericalSurfaced>);

/// A curve is not a surface: the two families do not overlap.
static_assert(!std::is_base_of_v<vn::geometry::Surface<double>, LineCurved>);

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

/// Continuity is a ladder ordered by smoothness, so `>=` answers "at least this smooth".
static_assert(vn::geometry::Continuity::CN > vn::geometry::Continuity::C2);
static_assert(vn::geometry::Continuity::C1 > vn::geometry::Continuity::G1);
static_assert(vn::geometry::Continuity::G1 > vn::geometry::Continuity::C0);

/// Numeric stubs: `BezierCurve`, `BSplineCurve` and `OffsetCurve` define their contract but leave `evaluate()` and the
/// two derivatives pure, so nothing of theirs can be constructed yet. A derived stub supplies those three and lets the
/// rest of the contract - domain, periodicity, continuity, validity - be tested today.
template <typename CurveBase>
struct NumericStub : CurveBase {
    using CurveBase::CurveBase;
    using Point  = typename CurveBase::Point;
    using Vector = typename CurveBase::Vector;

    [[nodiscard]] Point evaluate(double) const override
    {
        return Point();
    }

    [[nodiscard]] Vector derivative(double) const override
    {
        return Vector(0.0, 0.0, 0.0);
    }

    [[nodiscard]] Vector secondDerivative(double) const override
    {
        return Vector(0.0, 0.0, 0.0);
    }
};

using BezierStub  = NumericStub<vn::geometry::BezierCurve<double>>;
using OffsetStub  = NumericStub<vn::geometry::OffsetCurve<double>>;

/// The spline leaves one more answer pure than the other two: closure is a geometric fact for a spline, not a data
/// fact, so its stub has to answer that as well.
struct BSplineStub : NumericStub<vn::geometry::BSplineCurve<double>> {
    using NumericStub::NumericStub;

    [[nodiscard]] bool isClosed() const override
    {
        return false;
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
    const auto basis = std::make_shared<LineCurved>(Point3d(0.0, 0.0, 0.0), Vec3d(1.0, 0.0, 0.0));
    const vn::geometry::TrimmedCurved trimmed(basis, 1.0, 3.0);

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
    EXPECT_EQ(line.continuity(), vn::geometry::Continuity::CN);
    EXPECT_DOUBLE_EQ(line.reversedParameter(2.0), -2.0);
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

    EXPECT_EQ(circle.continuity(), vn::geometry::Continuity::CN);
    EXPECT_NEAR(circle.reversedParameter(0.25), 2.0 * vn::math::PI - 0.25, 1e-12);
}

TEST(GeometryKinds, aTrimmedCurveMirrorsInsideItsOwnRange)
{
    const auto basis = std::make_shared<CircleCurved>(Point3d(0.0, 0.0, 0.0), Vec3d(0.0, 0.0, 1.0), 1.0);
    const vn::geometry::TrimmedCurved quarter(basis, 0.0, vn::math::PI / 2.0);

    // The reflection belongs to the piece, not to the basis: reflecting on the basis would answer 2*pi - u.
    EXPECT_NEAR(quarter.reversedParameter(0.25), vn::math::PI / 2.0 - 0.25, 1e-12);

    // Trimming does not reparameterise, and the answers are reachable through the interface.
    const vn::geometry::Curve<double>& as_curve = quarter;
    EXPECT_NEAR(as_curve.derivative(0.0).y, 1.0, 1e-12);
    EXPECT_NEAR(as_curve.secondDerivative(0.0).y, 0.0, 1e-12);
    EXPECT_EQ(as_curve.continuity(), vn::geometry::Continuity::CN);
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
    EXPECT_EQ(hyperbola.continuity(), vn::geometry::Continuity::CN);
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

    // A cubic with an internal knot of multiplicity 2 is C1: one continuous derivative, no more.
    const BSplineStub kinked(3,
                             { Point3d(0.0, 0.0, 0.0), Point3d(1.0, 1.0, 0.0), Point3d(2.0, 0.0, 0.0),
                               Point3d(3.0, 1.0, 0.0) },
                             { 0.0, 0.5, 1.0 }, { 3, 2, 3 });
    EXPECT_TRUE(kinked.isCN(0));
    EXPECT_TRUE(kinked.isCN(1));
    EXPECT_FALSE(kinked.isCN(2));
}

TEST(GeometryKinds, aBezierCurveDescribesItselfWithoutEvaluating)
{
    const BezierStub open({ Point3d(0.0, 0.0, 0.0), Point3d(1.0, 1.0, 0.0), Point3d(2.0, 0.0, 0.0) });

    // The degree is the pole count, not a stored number; the domain is the type's, not the data's.
    EXPECT_EQ(open.degree(), 2);
    EXPECT_TRUE(open.isValid());
    EXPECT_FALSE(open.isRational());
    EXPECT_FALSE(open.isClosed());
    EXPECT_DOUBLE_EQ(open.firstParameter(), 0.0);
    EXPECT_DOUBLE_EQ(open.lastParameter(), 1.0);
    EXPECT_FALSE(open.isPeriodic());
    EXPECT_DOUBLE_EQ(open.period(), 0.0);
    EXPECT_EQ(open.continuity(), vn::geometry::Continuity::CN);
    EXPECT_DOUBLE_EQ(open.reversedParameter(0.25), 0.75);

    // Closed means the two end poles coincide; weights make the curve rational.
    const BezierStub closed({ Point3d(0.0, 0.0, 0.0), Point3d(1.0, 1.0, 0.0), Point3d(0.0, 0.0, 0.0) });
    EXPECT_TRUE(closed.isClosed());

    const BezierStub rational({ Point3d(0.0, 0.0, 0.0), Point3d(1.0, 0.0, 0.0) }, { 1.0, 2.0 });
    EXPECT_TRUE(rational.isRational());
    EXPECT_TRUE(rational.isValid());
}

TEST(GeometryKinds, aBSplineCurveDerivesItsContinuityFromItsMultiplicities)
{
    const auto poles = vn::geometry::BSplineCurve<double>::PoleArray{ Point3d(0.0, 0.0, 0.0), Point3d(1.0, 1.0, 0.0),
                                                                     Point3d(2.0, 0.0, 0.0), Point3d(3.0, 1.0, 0.0) };

    // Cubic with one span: the only joins are the two clamps, so the curve is CN throughout.
    const BSplineStub smooth(3, poles, { 0.0, 1.0 }, { 4, 4 });
    EXPECT_TRUE(smooth.isValid());
    EXPECT_FALSE(smooth.isRational());
    EXPECT_EQ(smooth.continuity(), vn::geometry::Continuity::CN);

    // An internal multiplicity of 2 costs one order, multiplicity 3 costs three.
    const BSplineStub kinked(3, poles, { 0.0, 0.5, 1.0 }, { 3, 2, 3 });
    EXPECT_TRUE(kinked.isValid());
    EXPECT_EQ(kinked.continuity(), vn::geometry::Continuity::C1);

    // Multiplicities that do not rebuild a flat knot vector of poles + degree + 1 are refused.
    const BSplineStub broken(3, poles, { 0.0, 0.5, 1.0 }, { 3, 3, 3 });
    EXPECT_FALSE(broken.isValid());
    EXPECT_EQ(broken.continuity(), vn::geometry::Continuity::C0);
}

TEST(GeometryKinds, anOffsetCurveInheritsTheBasisDomain)
{
    const auto basis  = std::make_shared<LineCurved>(Point3d(0.0, 0.0, 0.0), Vec3d(1.0, 0.0, 0.0));
    const OffsetStub offset(basis, 2.0, Vec3d(0.0, 0.0, 1.0));

    // The offset is evaluated at the basis' parameter, so its range, closure and period are the basis'.
    EXPECT_TRUE(offset.isValid());
    EXPECT_TRUE(std::isinf(offset.firstParameter()));
    EXPECT_TRUE(std::isinf(offset.lastParameter()));
    EXPECT_FALSE(offset.isClosed());
    EXPECT_FALSE(offset.isPeriodic());
    EXPECT_DOUBLE_EQ(offset.period(), 0.0);
    EXPECT_EQ(offset.continuity(), vn::geometry::Continuity::CN);
    EXPECT_DOUBLE_EQ(offset.reversedParameter(3.0), -3.0);

    const OffsetStub periodic(std::make_shared<CircleCurved>(), 1.0, Vec3d(0.0, 0.0, 1.0));
    EXPECT_TRUE(periodic.isClosed());
    EXPECT_TRUE(periodic.isPeriodic());
    EXPECT_NEAR(periodic.period(), 2.0 * vn::math::PI, 1e-12);
}

TEST(GeometryKinds, aPlaneAnswersTheSurfaceInterface)
{
    const Planed                          plane(Point3d(1.0, 2.0, 3.0), Vec3d(0.0, 0.0, 1.0));
    const vn::geometry::Surface<double>& as_surface = plane;

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
    EXPECT_EQ(as_surface.continuity(), vn::geometry::Continuity::CN);
    EXPECT_TRUE(as_surface.isCN(2));
}

TEST(GeometryKinds, aSphericalSheetClosesOnlyAroundItsAxis)
{
    const SphericalSurfaced              sheet(Point3d(0.0, 0.0, 0.0), 2.0);
    const vn::geometry::Surface<double>& as_surface = sheet;

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
    EXPECT_EQ(as_surface.continuity(), vn::geometry::Continuity::CN);
}

} // namespace
