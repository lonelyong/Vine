#include <gtest/gtest.h>

#include <type_traits>
#include <variant>

#include <vine/geometry/Primitive.hpp>
#include <vine/geometry/Ray.hpp>
#include <vine/geometry/Solid.hpp>
#include <vine/math/Math.hpp>
#include <vine/math/Point3.hpp>
#include <vine/math/Vector3.hpp>

namespace
{

using vn::geometry::BoxSolidd;
using vn::geometry::BoxSolidf;
using vn::geometry::ConeSolidf;
using vn::geometry::EllipsoidSolidd;
using vn::geometry::EllipsoidSolidf;
using vn::geometry::ShapeType;
using vn::geometry::Solidd;
using vn::geometry::Solidf;
using vn::geometry::SolidShaped;
using vn::geometry::SolidShapef;
using vn::geometry::SphereSolidd;
using vn::geometry::SphereSolidf;

/**
 * @brief A primitive that is deliberately not a solid, standing in for a profile or a swept face.
 *
 * @details
 * - The base holds no geometry, so this class compiles and behaves without touching `Solid`: that is the property the
 *   modelling side relies on, and the two dispatchers below must say so instead of guessing a solid.
 */
class Profile final : public vn::geometry::Primitive {
  public:
    Profile()
      : Primitive(ShapeType::Unknown)
    {}

  public:
    [[nodiscard]] bool isValid() const override
    {
        return true;
    }

    [[nodiscard]] vn::geometry::Aabbf bounds() const override
    {
        return vn::geometry::Aabbf{};
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
    static_assert(std::is_same_v<SolidShapef, std::variant<BoxSolidf, SphereSolidf, vn::geometry::CylinderSolidf, ConeSolidf, EllipsoidSolidf>>);

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
    static_assert(std::is_abstract_v<vn::geometry::Primitive>);
    static_assert(std::is_base_of_v<vn::geometry::Primitive, vn::geometry::Box>);
    static_assert(std::is_base_of_v<vn::geometry::Shape, vn::geometry::Ellipsoid>);
    static_assert(std::is_base_of_v<vn::geometry::Shape, Profile>);

    // A primitive that is not a solid is a complete class: no `Solid` value, no solid query, no cast that pretends
    // otherwise.
    Profile profile;
    EXPECT_EQ(profile.shapeType(), ShapeType::Unknown);
    EXPECT_FALSE(vn::geometry::solidShapeOf(profile).has_value());
    EXPECT_FALSE(vn::geometry::visitPrimitive(profile, [](const auto&) {}));
}

TEST(Solid, aSubclassStatesFullExtentsAndBuildsItsSolidOnDemand)
{
    vn::geometry::Box box(2.0f, 4.0f, 6.0f);
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

    const vn::geometry::Shape& shape = box;
    const auto* const          cast  = vn::obj_cast<vn::geometry::Box>(&shape);
    ASSERT_NE(cast, nullptr);
    EXPECT_FLOAT_EQ(cast->depth(), 6.0f);
}

TEST(Solid, everySubclassReportsItsOwnTag)
{
    const vn::geometry::Sphere    sphere(2.0f);
    const vn::geometry::Cylinder  cylinder(1.0f, 3.0f);
    const vn::geometry::Cone      cone(1.0f, 3.0f);
    const vn::geometry::Ellipsoid ellipsoid(1.0f, 2.0f, 3.0f);

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
    const vn::geometry::Box box;
    EXPECT_FLOAT_EQ(box.width(), 1.0f);
    EXPECT_FLOAT_EQ(box.bounds().min().x, -0.5f);
    EXPECT_TRUE(box.hasVolume(vn::math::EPS<double>()));

    EXPECT_FLOAT_EQ(vn::geometry::Sphere{}.radius(), 1.0f);
    EXPECT_FLOAT_EQ(vn::geometry::Cylinder{}.height(), 1.0f);
    EXPECT_FLOAT_EQ(vn::geometry::Cone{}.radius(), 1.0f);
    EXPECT_FLOAT_EQ(vn::geometry::Ellipsoid{}.radiusZ(), 1.0f);
}

TEST(Solid, anUnusablePrimitiveStillAnswersItsBounds)
{
    // A dimension at or below 0 is reported instead of being refused at construction: the shape still exists, its
    // queries refuse it, and the bounds stay a well formed box.
    const vn::geometry::Sphere collapsed(0.0f);
    EXPECT_FALSE(collapsed.isValid());
    EXPECT_EQ(collapsed.shapeType(), ShapeType::Sphere);
    EXPECT_FALSE(collapsed.hasVolume(vn::math::EPS<double>()));
    EXPECT_FALSE(collapsed.bounds().isNeg());
    EXPECT_FALSE(collapsed.solid().closestPoint(vn::math::Point3f(1.0f, 1.0f, 1.0f)).has_value());
    EXPECT_TRUE(collapsed.solid().intersectWith(vn::geometry::Rayf(vn::math::Point3f(-2.0f, 0.0f, 0.0f), vn::math::Vec3f(1.0f, 0.0f, 0.0f))).empty());

    const vn::geometry::Cylinder degenerate_cylinder(1.0f, 0.0f);
    EXPECT_FALSE(degenerate_cylinder.isValid());
    EXPECT_EQ(degenerate_cylinder.shapeType(), ShapeType::Cylinder);
}

TEST(Solid, solidShapeOfConvertsOnlyTheFiveSolids)
{
    const vn::geometry::Cone cone(1.0f, 2.0f);
    const auto               solid = vn::geometry::solidShapeOf(cone);
    ASSERT_TRUE(solid.has_value());
    EXPECT_TRUE(std::holds_alternative<ConeSolidf>(*solid));
    EXPECT_FLOAT_EQ(std::get<ConeSolidf>(*solid).half_height, 1.0f);

    EXPECT_FALSE(vn::geometry::solidShapeOf(Profile{}).has_value());
}

TEST(Solid, visitPrimitiveReachesTheConcreteSubclass)
{
    const vn::geometry::Ellipsoid ellipsoid(1.0f, 2.0f, 3.0f);
    float                        seen = 0.0f;
    EXPECT_TRUE(vn::geometry::visitPrimitive(ellipsoid, [&seen](const auto& shaped) { seen = shaped.bounds().max().z; }));
    EXPECT_FLOAT_EQ(seen, 3.0f) << "the visitor is handed the concrete subclass, not the base";
}

TEST(Solid, theBoundsAreTheAnalyticExtentOfEachKind)
{
    // A cone's bounds are its base disk's box: the base is the widest circle the solid has.
    const BoxSolidd                    box{ 1.0, 2.0, 3.0 };
    const SphereSolidd                 sphere{ 2.0 };
    const vn::geometry::CylinderSolidd cylinder{ 2.0, 3.0 };
    const vn::geometry::ConeSolidd     cone{ 2.0, 3.0 };
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
    EXPECT_TRUE(box.intersectWith(vn::geometry::Rayd(vn::math::Point3d(-5.0, 0.0, 0.0), vn::math::Vec3d(1.0, 0.0, 0.0))).empty());
}

TEST(Solid, everyKindAnswersItsOwnVolume)
{
    // A box is measured by its full edges, a cylinder by the radius and the full height: the half extents are doubled
    // rather than compared at half scale.
    const BoxSolidf                    cube{ 1.0f, 1.0f, 1.0f };
    const vn::geometry::CylinderSolidf cylinder{ 1.0f, 1.0f };
    const EllipsoidSolidf              ellipsoid{ 1.0f, 2.0f, 3.0f };

    EXPECT_TRUE(cube.hasVolume(1.9f));
    EXPECT_FALSE(cube.hasVolume(2.1f));
    EXPECT_TRUE(cylinder.hasVolume(0.9f));
    EXPECT_FALSE(cylinder.hasVolume(1.1f));
    EXPECT_TRUE(ellipsoid.hasVolume(0.9f));
    EXPECT_FALSE(ellipsoid.hasVolume(1.1f));
}
