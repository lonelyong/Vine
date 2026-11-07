#include <gtest/gtest.h>

#include <type_traits>
#include <variant>

#include <vine/models/Primitive.hpp>
#include <vine/models/PrimitiveDispatch.hpp>
#include <vine/geom/spatial/Ray.hpp>
#include <vine/geom/spatial/SolidShape.hpp>
#include <vine/math/Math.hpp>
#include <vine/math/Point3.hpp>
#include <vine/math/Vector3.hpp>

namespace
{

using vn::geom::spatial::BoxSolidd;
using vn::geom::spatial::BoxSolidf;
using vn::geom::spatial::ConeSolidf;
using vn::geom::spatial::EllipsoidSolidd;
using vn::geom::spatial::EllipsoidSolidf;
using vn::models::ShapeType;
using vn::geom::spatial::Solidd;
using vn::geom::spatial::Solidf;
using vn::geom::spatial::SolidShaped;
using vn::geom::spatial::SolidShapef;
using vn::geom::spatial::SphereSolidd;
using vn::geom::spatial::SphereSolidf;

/**
 * @brief A primitive that is deliberately not a solid, standing in for a profile or a swept face.
 *
 * @details
 * - The base holds no geometry, so this class compiles and behaves without touching `Solid`: that is the property the
 *   modelling side relies on, and the two dispatchers below must say so instead of guessing a solid.
 */
class Profile final : public vn::models::Primitive {
  public:
    Profile()
      : Primitive(ShapeType::Unknown)
    {}

  public:
    [[nodiscard]] bool isValid() const override
    {
        return true;
    }

    [[nodiscard]] vn::geom::spatial::Aabbf bounds() const override
    {
        return vn::geom::spatial::Aabbf{};
    }
};

} // namespace

TEST(Solid, theFamilyIsAnInterfaceAndTheKindsAreValues)
{
    // Two levels, two jobs: `Solid<T>` is what a consumer accepts when the kind does not matter, and the five kinds are
    // the values a model stores. Neither level is a pointer, so nothing allocates.
    static_assert(std::is_abstract_v<Solidf>, "the family interface has no state of its own");
    static_assert(std::is_abstract_v<Solidd>);
    static_assert(std::is_base_of_v<Solidf, BoxSolidf>);
    static_assert(std::is_base_of_v<Solidf, SphereSolidf>);
    static_assert(!std::is_abstract_v<BoxSolidf>, "a kind is a value, not an interface");
    static_assert(std::is_same_v<SolidShapef, std::variant<BoxSolidf, SphereSolidf, vn::geom::spatial::CylinderSolidf, ConeSolidf, EllipsoidSolidf>>);

    // Each kind keeps its own dimensions; the two precisions are distinct types, so nothing converts behind a caller's back.
    static_assert(std::is_same_v<decltype(BoxSolidf{}.half_x), float>);
    static_assert(!std::is_same_v<SolidShapef, SolidShaped>);

    const SphereSolidd sphere{ 2.0 };
    EXPECT_TRUE(sphere.isValid());
    EXPECT_TRUE(sphere.hasVolume(1.9));
    EXPECT_FALSE(sphere.hasVolume(2.1));
}

TEST(Solid, thePrimitiveBaseHoldsNoGeometry)
{
    // A primitive is identity plus whatever its subclass states: being a solid is a property of the five subclasses, not
    // of the concept. The base is abstract because `bounds()` is the only thing every primitive can answer.
    static_assert(std::is_abstract_v<vn::models::Primitive>);
    static_assert(std::is_base_of_v<vn::models::Primitive, vn::models::Box>);
    static_assert(std::is_base_of_v<vn::models::Shape, vn::models::Ellipsoid>);
    static_assert(std::is_base_of_v<vn::models::Shape, Profile>);

    // A primitive that is not a solid is a complete class: no `Solid` value, no solid query, no cast that pretends
    // otherwise.
    Profile profile;
    EXPECT_EQ(profile.shapeType(), ShapeType::Unknown);
    EXPECT_FALSE(vn::models::solidShapeOf(profile).has_value());
    EXPECT_FALSE(vn::models::visitPrimitive(profile, [](const auto&) {}));
}

TEST(Solid, aSubclassStatesFullExtentsAndBuildsItsSolidOnDemand)
{
    vn::models::Box box(2.0f, 4.0f, 6.0f);
    EXPECT_EQ(box.shapeType(), ShapeType::Box);
    EXPECT_FLOAT_EQ(box.width(), 2.0f);
    EXPECT_FLOAT_EQ(box.height(), 4.0f);
    EXPECT_FLOAT_EQ(box.depth(), 6.0f);
    EXPECT_FLOAT_EQ(box.bounds().max().z, 3.0f) << "the analytic box states halves; the subclass states extents";

    // The solid is a value built from those extents, so there is one copy of the numbers and no cache to invalidate.
    const auto solid = box.solid();
    EXPECT_FLOAT_EQ(solid.half_x, 1.0f);
    EXPECT_FLOAT_EQ(solid.bounds().max().y, 2.0f);

    box.setWidth(10.0f);
    EXPECT_FLOAT_EQ(box.solid().half_x, 5.0f);
    EXPECT_FLOAT_EQ(box.bounds().max().x, 5.0f) << "a setter is visible on the next call";

    const vn::models::Shape& shape = box;
    const auto* const          cast  = vn::obj_cast<vn::models::Box>(&shape);
    ASSERT_NE(cast, nullptr);
    EXPECT_FLOAT_EQ(cast->depth(), 6.0f);
}

TEST(Solid, everySubclassReportsItsOwnTag)
{
    const vn::models::Sphere    sphere(2.0f);
    const vn::models::Cylinder  cylinder(1.0f, 3.0f);
    const vn::models::Cone      cone(1.0f, 3.0f);
    const vn::models::Ellipsoid ellipsoid(1.0f, 2.0f, 3.0f);

    EXPECT_EQ(sphere.shapeType(), ShapeType::Sphere);
    EXPECT_EQ(cylinder.shapeType(), ShapeType::Cylinder);
    EXPECT_EQ(cone.shapeType(), ShapeType::Cone);
    EXPECT_EQ(ellipsoid.shapeType(), ShapeType::Ellipsoid);

    EXPECT_FLOAT_EQ(cylinder.height(), 3.0f) << "the height is stated in full, the solid stores half of it";
    EXPECT_FLOAT_EQ(cylinder.solid().half_height, 1.5f);
    EXPECT_FLOAT_EQ(cone.height(), 3.0f);
    EXPECT_FLOAT_EQ(ellipsoid.radiusY(), 2.0f);
}

TEST(Solid, theDefaultSubclassIsTheUnitShape)
{
    const vn::models::Box box;
    EXPECT_FLOAT_EQ(box.width(), 1.0f);
    EXPECT_FLOAT_EQ(box.bounds().min().x, -0.5f);
    EXPECT_TRUE(box.hasVolume(vn::math::EPS<double>()));

    EXPECT_FLOAT_EQ(vn::models::Sphere{}.radius(), 1.0f);
    EXPECT_FLOAT_EQ(vn::models::Cylinder{}.height(), 1.0f);
    EXPECT_FLOAT_EQ(vn::models::Cone{}.radius(), 1.0f);
    EXPECT_FLOAT_EQ(vn::models::Ellipsoid{}.radiusZ(), 1.0f);
}

TEST(Solid, anUnusablePrimitiveStillAnswersItsBounds)
{
    // A dimension at or below 0 is reported instead of being refused at construction: the shape still exists, its
    // queries refuse it, and the bounds stay a well formed box.
    const vn::models::Sphere collapsed(0.0f);
    EXPECT_FALSE(collapsed.isValid());
    EXPECT_EQ(collapsed.shapeType(), ShapeType::Sphere);
    EXPECT_FALSE(collapsed.hasVolume(vn::math::EPS<double>()));
    EXPECT_FALSE(collapsed.bounds().isNeg());
    EXPECT_FALSE(collapsed.solid().closestPoint(vn::math::Point3f(1.0f, 1.0f, 1.0f)).has_value());
    EXPECT_TRUE(collapsed.solid().intersectWith(vn::geom::spatial::Rayf(vn::math::Point3f(-2.0f, 0.0f, 0.0f), vn::math::Vec3f(1.0f, 0.0f, 0.0f))).empty());

    const vn::models::Cylinder degenerate_cylinder(1.0f, 0.0f);
    EXPECT_FALSE(degenerate_cylinder.isValid());
    EXPECT_EQ(degenerate_cylinder.shapeType(), ShapeType::Cylinder);
}

TEST(Solid, solidShapeOfConvertsOnlyTheFiveSolids)
{
    const vn::models::Cone cone(1.0f, 2.0f);
    const auto               solid = vn::models::solidShapeOf(cone);
    ASSERT_TRUE(solid.has_value());
    EXPECT_TRUE(std::holds_alternative<ConeSolidf>(*solid));
    EXPECT_FLOAT_EQ(std::get<ConeSolidf>(*solid).half_height, 1.0f);

    EXPECT_FALSE(vn::models::solidShapeOf(Profile{}).has_value());
}

TEST(Solid, visitPrimitiveReachesTheConcreteSubclass)
{
    const vn::models::Ellipsoid ellipsoid(1.0f, 2.0f, 3.0f);
    float                        seen = 0.0f;
    EXPECT_TRUE(vn::models::visitPrimitive(ellipsoid, [&seen](const auto& shaped) { seen = shaped.bounds().max().z; }));
    EXPECT_FLOAT_EQ(seen, 3.0f) << "the visitor is handed the concrete subclass, not the base";
}

TEST(Solid, theBoundsAreTheAnalyticExtentOfEachKind)
{
    // A cone's bounds are its base disk's box: the base is the widest circle the solid has.
    const BoxSolidd                    box{ 1.0, 2.0, 3.0 };
    const SphereSolidd                 sphere{ 2.0 };
    const vn::geom::spatial::CylinderSolidd cylinder{ 2.0, 3.0 };
    const vn::geom::spatial::ConeSolidd     cone{ 2.0, 3.0 };
    const EllipsoidSolidd              ellipsoid{ 1.0, 2.0, 3.0 };

    EXPECT_DOUBLE_EQ(box.bounds().min().x, -1.0);
    EXPECT_DOUBLE_EQ(box.bounds().max().z, 3.0);
    EXPECT_DOUBLE_EQ(sphere.bounds().min().y, -2.0);
    EXPECT_DOUBLE_EQ(cylinder.bounds().max().z, 3.0);
    EXPECT_DOUBLE_EQ(cone.bounds().max().x, 2.0);
    EXPECT_DOUBLE_EQ(cone.bounds().max().z, 3.0);
    EXPECT_DOUBLE_EQ(ellipsoid.bounds().min().y, -2.0);
    EXPECT_DOUBLE_EQ(ellipsoid.bounds().max().z, 3.0);
}

TEST(Solid, aDegenerateKindIsUnusableButStillWellFormed)
{
    // A dimension of zero is reported instead of asserted: the bounds stay a valid box and the queries answer nothing.
    const BoxSolidd box{ 1.0, 0.0, 1.0 };
    EXPECT_FALSE(box.isValid());
    EXPECT_FALSE(box.hasVolume(0.0));
    EXPECT_FALSE(box.bounds().isNeg());
    EXPECT_FALSE(box.closestPoint(vn::math::Point3d(5.0, 5.0, 5.0)).has_value());
    EXPECT_TRUE(box.intersectWith(vn::geom::spatial::Rayd(vn::math::Point3d(-5.0, 0.0, 0.0), vn::math::Vec3d(1.0, 0.0, 0.0))).empty());
}

TEST(Solid, everyKindAnswersItsOwnVolume)
{
    // A box is measured by its full edges, a cylinder by the radius and the full height: the half extents are doubled
    // rather than compared at half scale.
    const BoxSolidf                    cube{ 1.0f, 1.0f, 1.0f };
    const vn::geom::spatial::CylinderSolidf cylinder{ 1.0f, 1.0f };
    const EllipsoidSolidf              ellipsoid{ 1.0f, 2.0f, 3.0f };

    EXPECT_TRUE(cube.hasVolume(1.9f));
    EXPECT_FALSE(cube.hasVolume(2.1f));
    EXPECT_TRUE(cylinder.hasVolume(0.9f));
    EXPECT_FALSE(cylinder.hasVolume(1.1f));
    EXPECT_TRUE(ellipsoid.hasVolume(0.9f));
    EXPECT_FALSE(ellipsoid.hasVolume(1.1f));
}
