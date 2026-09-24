#pragma once

#include "geometry_global.hpp"

#include <vine/math/Math.hpp>
#include <vine/Object.hpp>
#include <vine/RefCounted.hpp>

VN_GEOMETRY_NS_BEGIN

/**
 * @brief Coarse shape family used for category-level dispatch.
 */
enum class ShapeKind {
    /// Not a concrete shape yet.
    Unknown = 0,
    /// Parametric primitive (Primitive).
    Primitive,
    /// Polygonal mesh (Mesh).
    Mesh,
    /// Boundary-representation solid (BrepShape).
    Brep,
};

/**
 * @brief Concrete shape type used for fine-grained dispatch.
 */
enum class ShapeType {
    /// Not a concrete shape yet.
    Unknown = 0,
    /// Axis-aligned box (Box).
    Box,
    /// Cylinder (Cylinder).
    Cylinder,
    /// Cone (Cone).
    Cone,
    /// Sphere (Sphere).
    Sphere,
    /// Ellipsoid (Ellipsoid).
    Ellipsoid,
    /// Non-indexed triangle mesh (TriangleMesh).
    TriangleMesh,
    /// Indexed triangle mesh (IndexedTriangleMesh).
    IndexedTriangleMesh,
    /// Boundary-representation solid (BrepShape).
    Brep,
};

/**
 * @brief Base class for all geometric shapes.
 */
class VN_GEOMETRY_API Shape : public vn::Object, public vn::RefCounted<Shape> {
    VN_OBJECT_META_DECL;

  protected:
    /// Protected so Shape cannot be instantiated directly.
    Shape();

  public:
    /**
     * @brief Returns whether the shape holds valid, queryable geometry.
     *
     * @return true when the shape can be used for rendering or queries.
     */
    [[nodiscard]]
    virtual bool isValid() const;

    /**
     * @brief Returns whether the shape encloses a volume.
     *
     * @param eps Numerical tolerance for volume significance checks.
     * @return true for closed solids, false for surfaces or wireframes.
     */
    [[nodiscard]]
    virtual bool hasVolume(double eps = vn::math::EPS<double>()) const;

    /**
     * @brief Returns the concrete shape type.
     *
     * @return The ShapeType of this shape.
     */
    [[nodiscard]]
    ShapeType shapeType() const
    {
        return shape_type_;
    }

    /**
     * @brief Returns the coarse shape family.
     *
     * The kind is derived from the stored shape type, so it can never be
     * inconsistent with the concrete type.
     *
     * @return The ShapeKind of this shape.
     */
    [[nodiscard]]
    ShapeKind shapeKind() const;

  protected:
    /// Concrete shape type; assigned by derived class constructors.
    ShapeType shape_type_ = ShapeType::Unknown;
};

VN_GEOMETRY_NS_END