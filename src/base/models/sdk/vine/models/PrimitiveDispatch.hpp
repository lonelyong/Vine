#pragma once

#include <vine/models/models_global.hpp>

#include <vine/math/Math.hpp>
#include <vine/math/Types.hpp>

#include <vine/models/Box.hpp>
#include <vine/models/Cone.hpp>
#include <vine/models/Cylinder.hpp>
#include <vine/models/Ellipsoid.hpp>
#include <vine/models/Primitive.hpp>
#include <vine/models/Sphere.hpp>

VN_MODELS_NS_BEGIN

/**
 * @brief Calls @p visitor with the concrete subclass @p primitive is.
 *
 * @details
 * - The five subclasses are a closed set, so a consumer that must answer per kind gets the concrete type without a
 *   `dynamic_cast` of its own and without a `default:` branch that silently skips a new kind.
 * - The tag checked here and the cast that follows agree by construction: every subclass hands its own tag to the base.
 *
 * @tparam Visitor Callable taking `const Box&`, `const Sphere&`, `const Cylinder&`, `const Cone&` or `const Ellipsoid&`.
 * @param primitive The primitive to visit.
 * @param visitor The callable to call with the concrete primitive.
 * @return `true` when the visitor was called; `false` when the tag is not one of the five.
 */
template <class Visitor>
bool visitPrimitive(const Primitive& primitive, Visitor&& visitor)
{
    switch (primitive.shapeType()) {
    case ShapeType::Box: {
        visitor(vn::obj_cast<Box>(primitive));
        return true;
    }
    case ShapeType::Sphere: {
        visitor(vn::obj_cast<Sphere>(primitive));
        return true;
    }
    case ShapeType::Cylinder: {
        visitor(vn::obj_cast<Cylinder>(primitive));
        return true;
    }
    case ShapeType::Cone: {
        visitor(vn::obj_cast<Cone>(primitive));
        return true;
    }
    case ShapeType::Ellipsoid: {
        visitor(vn::obj_cast<Ellipsoid>(primitive));
        return true;
    }
    default:
        return false;
    }
}

VN_MODELS_NS_END
