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

using vn::geometry::Boxd;
using vn::geometry::Boxf;
using vn::geometry::Conef;
using vn::geometry::Ellipsoidd;
using vn::geometry::Ellipsoidf;
using vn::geometry::Primitive;
using vn::geometry::ShapeType;
using vn::geometry::Solidd;
using vn::geometry::Solidf;
using vn::geometry::SolidShaped;
using vn::geometry::SolidShapef;
using vn::geometry::Sphered;
using vn::geometry::Spheref;

} // namespace

TEST(Solid, theFamilyIsAnInterfaceAndTheKindsAreValues)
{
    // Two levels, two jobs: `Solid<T>` is what a consumer accepts when the kind does not matter, and the five kinds are
    // the values a model stores. Neither level is a pointer, so nothing allocates.
    static_assert(std::is_abstract_v<Solidf>, "the family interface has no state of its own");
    static_assert(std::is_abstract_v<Solidd>);
    static_assert(std::is_base_of_v<Solidf, Boxf>);
    static_assert(std::is_base_of_v<Solidf, Spheref>);
    static_assert(!std::is_abstract_v<Boxf>, "a kind is a value, not an interface");
    static_assert(std::is_same_v<SolidShapef, std::variant<Boxf, Spheref, vn::geometry::Cylinderf, Conef, Ellipsoidf>>);

    // Each kind keeps its own dimensions; the two precisions are distinct types, so nothing converts behind a caller's back.
    static_assert(std::is_same_v<decltype(Boxf{}.half_x), float>);
    static_assert(!std::is_same_v<SolidShapef, SolidShaped>);

    const Sphered sphere{ 2.0 };
    EXPECT_TRUE(sphere.isValid());
    EXPECT_TRUE(sphere.hasVolume(1.9));
    EXPECT_FALSE(sphere.hasVolume(2.1));
}

TEST(Solid, theTwoViewsOfOneSolidAgree)
{
    // `solid()` is the family view and `shape()` is the stored variant; they describe the same object, which is what
    // lets a query go through the interface while a per-kind consumer visits the variant.
    const Primitive box(Boxf{ 1.0f, 2.0f, 3.0f });

    EXPECT_EQ(&box.solid(), static_cast<const Solidf*>(&std::get<Boxf>(box.shape())));
    EXPECT_FLOAT_EQ(box.solid().bounds().max().y, 2.0f);
    EXPECT_TRUE(box.solid().isValid());
}

TEST(Solid, theModelHoldsTheGeometryAndDerivesItsTag)
{
    const Primitive box(Boxf{ 1.0f, 2.0f, 3.0f });
    EXPECT_EQ(box.shapeType(), ShapeType::Box);
    EXPECT_TRUE(std::holds_alternative<Boxf>(box.shape()));
    EXPECT_TRUE(box.isValid());
    EXPECT_FLOAT_EQ(box.bounds().max().y, 2.0f);

    Primitive shape(Spheref{ 2.0f });
    EXPECT_EQ(shape.shapeType(), ShapeType::Sphere);
    EXPECT_EQ(box.shapeType(), ShapeType::Box) << "the tag belongs to the object, not to the kind";

    shape.setSolid(Conef{ 1.0f, 1.0f });
    EXPECT_EQ(shape.shapeType(), ShapeType::Cone);
    EXPECT_FLOAT_EQ(shape.bounds().max().z, 1.0f);
}

TEST(Solid, theDefaultPrimitiveIsAUnitBox)
{
    const Primitive primitive;
    EXPECT_EQ(primitive.shapeType(), ShapeType::Box);
    EXPECT_FLOAT_EQ(primitive.bounds().min().x, -0.5f);
    EXPECT_FLOAT_EQ(primitive.bounds().max().x, 0.5f);
    EXPECT_TRUE(primitive.hasVolume(vn::math::EPS<double>()));
}

TEST(Solid, anUnusableModelStillCarriesItsGeometry)
{
    // A dimension at or below 0 is reported instead of being refused at construction: the model still has a geometry,
    // the queries refuse it, and the bounds stay a well formed box.
    const Primitive collapsed(Spheref{ 0.0f });
    EXPECT_FALSE(collapsed.isValid());
    EXPECT_EQ(collapsed.shapeType(), ShapeType::Sphere);
    EXPECT_FALSE(collapsed.hasVolume(vn::math::EPS<double>()));
    EXPECT_FALSE(collapsed.bounds().isNeg());
    EXPECT_FALSE(collapsed.solid().closestPoint(vn::math::Point3f(1.0f, 1.0f, 1.0f)).has_value());
    EXPECT_TRUE(collapsed.solid().intersectWith(vn::geometry::Rayf(vn::math::Point3f(-2.0f, 0.0f, 0.0f), vn::math::Vec3f(1.0f, 0.0f, 0.0f))).empty());
}

TEST(Solid, theBoundsAreTheAnalyticExtentOfEachKind)
{
    // A cone's bounds are its base disk's box: the base is the widest circle the solid has.
    const Boxd                    box{ 1.0, 2.0, 3.0 };
    const Sphered                 sphere{ 2.0 };
    const vn::geometry::Cylinderd cylinder{ 2.0, 3.0 };
    const vn::geometry::Coned     cone{ 2.0, 3.0 };
    const Ellipsoidd              ellipsoid{ 1.0, 2.0, 3.0 };

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
    const Boxd box{ 1.0, 0.0, 1.0 };
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
    const Boxf                    cube{ 1.0f, 1.0f, 1.0f };
    const vn::geometry::Cylinderf cylinder{ 1.0f, 1.0f };
    const Ellipsoidf              ellipsoid{ 1.0f, 2.0f, 3.0f };

    EXPECT_TRUE(cube.hasVolume(1.9f));
    EXPECT_FALSE(cube.hasVolume(2.1f));
    EXPECT_TRUE(cylinder.hasVolume(0.9f));
    EXPECT_FALSE(cylinder.hasVolume(1.1f));
    EXPECT_TRUE(ellipsoid.hasVolume(0.9f));
    EXPECT_FALSE(ellipsoid.hasVolume(1.1f));
}
