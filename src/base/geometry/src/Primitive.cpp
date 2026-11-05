#include <vine/geometry/Primitive.hpp>

#include <type_traits>
#include <variant>

VN_GEOMETRY_NS_BEGIN

VN_OBJECT_META_IMPL(Primitive, Shape)

namespace
{

/**
 * @brief Projects the stored solid's kind onto the `ShapeType` the shape family dispatches on.
 *
 * @details
 * - This is a projection, not a second source of truth: the kind lives in the variant, and the tag is read back from it
 *   here. The `static_assert` is what makes a new kind fail to compile instead of silently reporting an ellipsoid.
 *
 * @param shape The stored solid.
 * @return The matching `ShapeType`.
 */
ShapeType shapeTypeOf(const SolidShapef& shape)
{
    return std::visit(
        [](const auto& kind) {
            using Kind = std::decay_t<decltype(kind)>;
            if constexpr (std::is_same_v<Kind, Boxf>) {
                return ShapeType::Box;
            }
            else if constexpr (std::is_same_v<Kind, Spheref>) {
                return ShapeType::Sphere;
            }
            else if constexpr (std::is_same_v<Kind, Cylinderf>) {
                return ShapeType::Cylinder;
            }
            else if constexpr (std::is_same_v<Kind, Conef>) {
                return ShapeType::Cone;
            }
            else {
                static_assert(std::is_same_v<Kind, Ellipsoidf>, "a new analytic solid needs a ShapeType");
                return ShapeType::Ellipsoid;
            }
        },
        shape);
}

} // namespace

Primitive::Primitive()
  : solid_(Boxf{ 0.5f, 0.5f, 0.5f })
{
    shape_type_ = ShapeType::Box;
}

Primitive::Primitive(const SolidShapef& solid)
  : solid_(solid)
{
    shape_type_ = shapeTypeOf(solid_);
}

const Solidf& Primitive::solid() const noexcept
{
    return solidOf(solid_);
}

const SolidShapef& Primitive::shape() const noexcept
{
    return solid_;
}

void Primitive::setSolid(const SolidShapef& solid) noexcept
{
    solid_      = solid;
    shape_type_ = shapeTypeOf(solid_);
}

bool Primitive::isValid() const
{
    return solid().isValid();
}

bool Primitive::hasVolume(double eps) const
{
    return solid().hasVolume(static_cast<float>(eps));
}

Aabbf Primitive::bounds() const noexcept
{
    return solid().bounds();
}

VN_GEOMETRY_NS_END
