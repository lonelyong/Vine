#pragma once

#include "geometry_global.hpp"

#include <cstdint>

#include <vine/INameable.hpp>
#include <vine/Object.hpp>
#include <vine/RefCounted.hpp>
#include <vine/math/Math.hpp>

VN_GEOMETRY_NS_BEGIN

/**
 * @brief Coarse shape family used for category-level dispatch.
 */
enum class ShapeKind
{
    /// Not a concrete shape yet.
    Unknown = 0,
    /// Parametric primitive (Primitive).
    Primitive,
    /// Polygonal mesh (Mesh).
    Mesh,
    /// Boundary-representation solid (BrepShape).
    Brep,
    /// Unconnected points (PointCloud).
    PointCloud,
};

/**
 * @brief Concrete shape type used for fine-grained dispatch.
 */
enum class ShapeType
{
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
    /// Triangle mesh; whether it is indexed depends on whether it has indices.
    TriangleMesh,
    /// Quad mesh; each face has four corners.
    QuadMesh,
    /// Mixed polygon mesh; face offsets delimit faces of three or more corners.
    MixedMesh,
    /// Boundary-representation solid (BrepShape).
    Brep,
    /// Unconnected points (PointCloud).
    PointCloud,
};

/**
 * @brief Base class for all geometric shapes.
 *
 * Every shape carries a name: a loader fills it from the source model, so a
 * shape that came out of a file can be told apart from its siblings. The name
 * is free-form and may be empty.
 */
class VN_GEOMETRY_API Shape : public vn::Object, public vn::RefCounted<Shape>, public vn::INameable {
    VN_OBJECT_META_DECL;

  protected:
    /// Protected so Shape cannot be instantiated directly.
    Shape();

  public:
    /**
     * @brief Returns the shape name.
     *
     * @return The name; empty when the shape was never named.
     */
    const String& name() const noexcept override
    {
        return name_;
    }

    /**
     * @brief Sets the shape name.
     *
     * @param name The new name; may be empty.
     */
    void setName(const String& name) override
    {
        name_ = name;
    }

    /**
     * @brief Returns whether the shape holds valid, queryable geometry.
     *
     * @return true when the shape can be used for rendering or queries.
     */
    [[nodiscard]]
    virtual bool isValid() const;

    /**
     * @brief Returns whether the shape reports the volume it encloses.
     *
     * @details
     * - Parametric primitives answer from their dimensions; meshes answer false because their closure is not tracked,
     *   and a BrepShape answers false because the backend's closure is not queried here.
     *
     * @param eps Numerical tolerance for volume significance checks.
     * @return true when the shape reports a volume, false for surfaces and for a solid whose closure is unknown.
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

    /**
     * @brief Returns the content revision of this shape.
     *
     * @details
     * - Nothing in the geometry module advances it: a setter replaces storage and an append edits it, and neither knows
     *   where an edit begins and ends. The holder reports the change once the change is complete.
     * - It is the counterpart of Buffer::revision() (one byte stream) and Geometry::revision() (one draw's layout).
     *
     * @return Monotonic counter (starts at 0).
     */
    [[nodiscard]]
    std::uint64_t revision() const noexcept
    {
        return revision_;
    }

    /**
     * @brief Reports a content revision.
     *
     * @warning Lowering it is a hazard: a consumer holding a cached revision would treat old content as current. Prefer
     *          bumpRevision(), which cannot move the counter backwards.
     *
     * @param revision Revision to report.
     */
    void setRevision(std::uint64_t revision) noexcept
    {
        revision_ = revision;
    }

    /**
     * @brief Advances the content revision by one.
     */
    void bumpRevision() noexcept
    {
        ++revision_;
    }

  protected:
    /// Name of the shape; empty when unnamed.
    String name_;

    /// Concrete shape type; assigned by derived class constructors.
    ShapeType shape_type_ = ShapeType::Unknown;

    /// Content revision; advanced only by the holder, once a change is complete.
    std::uint64_t revision_ = 0;
};

VN_GEOMETRY_NS_END