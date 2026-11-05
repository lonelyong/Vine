#pragma once

#include "geometry_global.hpp"

#include <optional>

#include <vine/math/Math.hpp>
#include <vine/math/Types.hpp>

#include "Aabb.hpp"
#include "Shape.hpp"
#include "Solid.hpp"

VN_GEOMETRY_NS_BEGIN

/**
 * @brief A modelling shape: identity plus whatever describes it.
 *
 * @details
 * - The modelling side is `Shape`: a name, a revision and the tag the family dispatches on. A primitive is **not required
 *   to be an analytic solid**, so this base holds no geometry at all: each subclass states its own data and answers
 *   `isValid()`, `hasVolume()` and `bounds()` from it.
 * - `Box` / `Sphere` / `Cylinder` / `Cone` / `Ellipsoid` are the five subclasses this repository models. They are solids,
 *   so each one also answers `solid()` with its concrete analytic kind (`BoxSolidf`, …), which is what the geometry
 *   algorithms take. A primitive that is not a solid derives from this base without touching `Solid` at all.
 * - The tag is handed to the base by the subclass constructor, so `shapeType()` cannot name a different kind than the
 *   class that was written.
 * - The canonical local space belongs to the subclass: box, sphere and ellipsoid are centred on the origin, and cylinder
 *   and cone span `[-height / 2, +height / 2]` on +Z with the cone's base at the lower end. Placement belongs to the node
 *   transform that holds the shape, never here.
 *
 * @warning
 * - A dimension at or below 0 makes a shape unusable: `isValid()` reports it and the queries answer nothing.
 *   `bounds()` stays a well formed box either way.
 *
 * @note
 * - `bounds()` is pure virtual here because every primitive can answer it; `solid()` is not, because being a solid is a
 *   property of those five subclasses rather than of the concept.
 */
class VN_GEOMETRY_API Primitive : public Shape {
    VN_OBJECT_META_DECL;

  protected:
    /**
     * @brief Constructs a primitive that reports @p type.
     *
     * @param type The concrete tag; every subclass passes its own and nothing else.
     */
    explicit Primitive(ShapeType type) noexcept;

  public:
    /**
     * @brief Returns the axis-aligned bounds of this primitive in its own local space.
     *
     * @return The bounds; multiply by the node transform for a world box.
     */
    [[nodiscard]] virtual Aabbf bounds() const = 0;
};

/**
 * @brief A box with identity: three full extents in its own local space, centred on the origin.
 *
 * @details
 * - `width()` / `height()` / `depth()` are the full extents, the way a description file states them; `solid()` halves
 *   them for the analytic box it returns, and nothing stores the halves.
 * - The box stores its own extents, so a setter is visible to `bounds()` on the next call and no cache can go stale.
 * - A setter does not advance the revision: the holder reports the edit once it is complete (`Shape::bumpRevision`).
 *
 * @warning
 * - A dimension at or below 0 is accepted here and reported by `isValid()`, so a partially built shape never throws.
 */
class VN_GEOMETRY_API Box final : public Primitive {
    VN_OBJECT_META_DECL;

  public:
    /**
     * @brief Constructs a unit box (1 x 1 x 1) centred on the origin.
     */
    Box();

    /**
     * @brief Constructs a box from its full extents.
     *
     * @param width  Extent along X.
     * @param height Extent along Y.
     * @param depth  Extent along Z.
     */
    Box(float width, float height, float depth);

  public:
    /**
     * @brief Returns the extent along X.
     * @return The full extent along X.
     */
    [[nodiscard]] float width() const noexcept
    {
        return width_;
    }

    /**
     * @brief Returns the extent along Y.
     * @return The full extent along Y.
     */
    [[nodiscard]] float height() const noexcept
    {
        return height_;
    }

    /**
     * @brief Returns the extent along Z.
     * @return The full extent along Z.
     */
    [[nodiscard]] float depth() const noexcept
    {
        return depth_;
    }

    /**
     * @brief Sets the extent along X.
     * @param width The new full extent along X; a value at or below 0 leaves the shape invalid.
     */
    void setWidth(float width) noexcept
    {
        width_ = width;
    }

    /**
     * @brief Sets the extent along Y.
     * @param height The new full extent along Y; a value at or below 0 leaves the shape invalid.
     */
    void setHeight(float height) noexcept
    {
        height_ = height;
    }

    /**
     * @brief Sets the extent along Z.
     * @param depth The new full extent along Z; a value at or below 0 leaves the shape invalid.
     */
    void setDepth(float depth) noexcept
    {
        depth_ = depth;
    }

    /**
     * @brief Returns the analytic solid this box describes, built from the current extents.
     *
     * @details
     * - The solid is a value, not stored state: the extents above are the only copy, and the halves exist for the call
     *   only. A caller that needs a `Solidf&` keeps the returned value alive for that long.
     *
     * @return The box solid with half extents.
     */
    [[nodiscard]] BoxSolidf solid() const noexcept
    {
        return BoxSolidf{ width_ * 0.5f, height_ * 0.5f, depth_ * 0.5f };
    }

    /**
     * @brief Returns whether every extent is positive.
     * @return `true` when the box can be queried.
     */
    [[nodiscard]] bool isValid() const override;

    /**
     * @brief Returns whether the box encloses a volume larger than @p eps in every characteristic size.
     *
     * @param eps Numerical tolerance a dimension must exceed.
     * @return `true` when the box encloses a volume by that measure.
     */
    [[nodiscard]] bool hasVolume(double eps = static_cast<double>(vn::math::EPS<float>())) const override;

    /**
     * @brief Returns the axis-aligned bounds of the box in its local space.
     * @return The box of half extents around the origin.
     */
    [[nodiscard]] Aabbf bounds() const override;

  private:
    /// Extent along X; 1 for the unit box.
    float width_{ 1.0f };

    /// Extent along Y; 1 for the unit box.
    float height_{ 1.0f };

    /// Extent along Z; 1 for the unit box.
    float depth_{ 1.0f };
};

/**
 * @brief A sphere with identity: one radius in its own local space, centred on the origin.
 *
 * @details
 * - The sphere stores its radius; `solid()` builds the analytic sphere for the call (see `Box`).
 */
class VN_GEOMETRY_API Sphere final : public Primitive {
    VN_OBJECT_META_DECL;

  public:
    /**
     * @brief Constructs a unit sphere (radius 1) centred on the origin.
     */
    Sphere();

    /**
     * @brief Constructs a sphere of the given radius.
     *
     * @param radius The radius; a value at or below 0 leaves the shape invalid.
     */
    explicit Sphere(float radius);

  public:
    /**
     * @brief Returns the radius.
     * @return The radius.
     */
    [[nodiscard]] float radius() const noexcept
    {
        return radius_;
    }

    /**
     * @brief Sets the radius.
     * @param radius The new radius; a value at or below 0 leaves the shape invalid.
     */
    void setRadius(float radius) noexcept
    {
        radius_ = radius;
    }

    /**
     * @brief Returns the analytic solid this sphere describes.
     * @return The sphere solid with the stored radius.
     */
    [[nodiscard]] SphereSolidf solid() const noexcept
    {
        return SphereSolidf{ radius_ };
    }

    /**
     * @brief Returns whether the radius is positive.
     * @return `true` when the sphere can be queried.
     */
    [[nodiscard]] bool isValid() const override;

    /**
     * @brief Returns whether the sphere encloses a volume larger than @p eps in every characteristic size.
     *
     * @param eps Numerical tolerance a dimension must exceed.
     * @return `true` when the sphere encloses a volume by that measure.
     */
    [[nodiscard]] bool hasVolume(double eps = static_cast<double>(vn::math::EPS<float>())) const override;

    /**
     * @brief Returns the axis-aligned bounds of the sphere in its local space.
     * @return The cube of side `2 * radius` around the origin.
     */
    [[nodiscard]] Aabbf bounds() const override;

  private:
    /// Radius; 1 for the unit sphere.
    float radius_{ 1.0f };
};

/**
 * @brief A cylinder with identity: a radius and a height, axis on +Z, spanning `[-height / 2, +height / 2]`.
 *
 * @details
 * - The cylinder stores the full height a description file states; `solid()` halves it for the analytic cylinder.
 */
class VN_GEOMETRY_API Cylinder final : public Primitive {
    VN_OBJECT_META_DECL;

  public:
    /**
     * @brief Constructs a unit cylinder (radius 1, height 1) around the origin.
     */
    Cylinder();

    /**
     * @brief Constructs a cylinder from its radius and full height.
     *
     * @param radius The radius; a value at or below 0 leaves the shape invalid.
     * @param height The full height; a value at or below 0 leaves the shape invalid.
     */
    Cylinder(float radius, float height);

  public:
    /**
     * @brief Returns the radius.
     * @return The radius.
     */
    [[nodiscard]] float radius() const noexcept
    {
        return radius_;
    }

    /**
     * @brief Returns the full height.
     * @return The full height.
     */
    [[nodiscard]] float height() const noexcept
    {
        return height_;
    }

    /**
     * @brief Sets the radius.
     * @param radius The new radius; a value at or below 0 leaves the shape invalid.
     */
    void setRadius(float radius) noexcept
    {
        radius_ = radius;
    }

    /**
     * @brief Sets the height.
     * @param height The new full height; a value at or below 0 leaves the shape invalid.
     */
    void setHeight(float height) noexcept
    {
        height_ = height;
    }

    /**
     * @brief Returns the analytic solid this cylinder describes.
     * @return The cylinder solid with the stored radius and half height.
     */
    [[nodiscard]] CylinderSolidf solid() const noexcept
    {
        return CylinderSolidf{ radius_, height_ * 0.5f };
    }

    /**
     * @brief Returns whether the radius and height are positive.
     * @return `true` when the cylinder can be queried.
     */
    [[nodiscard]] bool isValid() const override;

    /**
     * @brief Returns whether the cylinder encloses a volume larger than @p eps in every characteristic size.
     *
     * @param eps Numerical tolerance a dimension must exceed.
     * @return `true` when the cylinder encloses a volume by that measure.
     */
    [[nodiscard]] bool hasVolume(double eps = static_cast<double>(vn::math::EPS<float>())) const override;

    /**
     * @brief Returns the axis-aligned bounds of the cylinder in its local space.
     * @return The box of the full diameter around the axis.
     */
    [[nodiscard]] Aabbf bounds() const override;

  private:
    /// Radius; 1 for the unit cylinder.
    float radius_{ 1.0f };

    /// Full height; 1 for the unit cylinder.
    float height_{ 1.0f };
};

/**
 * @brief A cone with identity: a base radius and a height, axis on +Z, base at `-height / 2`, apex at `+height / 2`.
 *
 * @details
 * - The cone stores the full height a description file states; `solid()` halves it for the analytic cone.
 */
class VN_GEOMETRY_API Cone final : public Primitive {
    VN_OBJECT_META_DECL;

  public:
    /**
     * @brief Constructs a unit cone (base radius 1, height 1) around the origin.
     */
    Cone();

    /**
     * @brief Constructs a cone from its base radius and full height.
     *
     * @param radius The base radius; a value at or below 0 leaves the shape invalid.
     * @param height The full height; a value at or below 0 leaves the shape invalid.
     */
    Cone(float radius, float height);

  public:
    /**
     * @brief Returns the base radius.
     * @return The base radius.
     */
    [[nodiscard]] float radius() const noexcept
    {
        return radius_;
    }

    /**
     * @brief Returns the full height.
     * @return The full height.
     */
    [[nodiscard]] float height() const noexcept
    {
        return height_;
    }

    /**
     * @brief Sets the base radius.
     * @param radius The new base radius; a value at or below 0 leaves the shape invalid.
     */
    void setRadius(float radius) noexcept
    {
        radius_ = radius;
    }

    /**
     * @brief Sets the height.
     * @param height The new full height; a value at or below 0 leaves the shape invalid.
     */
    void setHeight(float height) noexcept
    {
        height_ = height;
    }

    /**
     * @brief Returns the analytic solid this cone describes.
     * @return The cone solid with the stored base radius and half height.
     */
    [[nodiscard]] ConeSolidf solid() const noexcept
    {
        return ConeSolidf{ radius_, height_ * 0.5f };
    }

    /**
     * @brief Returns whether the radius and height are positive.
     * @return `true` when the cone can be queried.
     */
    [[nodiscard]] bool isValid() const override;

    /**
     * @brief Returns whether the cone encloses a volume larger than @p eps in every characteristic size.
     *
     * @param eps Numerical tolerance a dimension must exceed.
     * @return `true` when the cone encloses a volume by that measure.
     */
    [[nodiscard]] bool hasVolume(double eps = static_cast<double>(vn::math::EPS<float>())) const override;

    /**
     * @brief Returns the axis-aligned bounds of the cone in its local space.
     * @return The box of the base disk around the axis.
     */
    [[nodiscard]] Aabbf bounds() const override;

  private:
    /// Base radius; 1 for the unit cone.
    float radius_{ 1.0f };

    /// Full height; 1 for the unit cone.
    float height_{ 1.0f };
};

/**
 * @brief An axis-aligned ellipsoid with identity: three semi-axes, centred on the origin.
 *
 * @details
 * - The ellipsoid stores semi-axes, which is what its analytic kind states too, so `solid()` passes them through.
 */
class VN_GEOMETRY_API Ellipsoid final : public Primitive {
    VN_OBJECT_META_DECL;

  public:
    /**
     * @brief Constructs a unit sphere-shaped ellipsoid (all three radii 1).
     */
    Ellipsoid();

    /**
     * @brief Constructs an ellipsoid from its three semi-axes.
     *
     * @param radius_x Semi-axis along X; a value at or below 0 leaves the shape invalid.
     * @param radius_y Semi-axis along Y; a value at or below 0 leaves the shape invalid.
     * @param radius_z Semi-axis along Z; a value at or below 0 leaves the shape invalid.
     */
    Ellipsoid(float radius_x, float radius_y, float radius_z);

  public:
    /**
     * @brief Returns the semi-axis along X.
     * @return The semi-axis along X.
     */
    [[nodiscard]] float radiusX() const noexcept
    {
        return radius_x_;
    }

    /**
     * @brief Returns the semi-axis along Y.
     * @return The semi-axis along Y.
     */
    [[nodiscard]] float radiusY() const noexcept
    {
        return radius_y_;
    }

    /**
     * @brief Returns the semi-axis along Z.
     * @return The semi-axis along Z.
     */
    [[nodiscard]] float radiusZ() const noexcept
    {
        return radius_z_;
    }

    /**
     * @brief Sets the semi-axis along X.
     * @param radius The new semi-axis; a value at or below 0 leaves the shape invalid.
     */
    void setRadiusX(float radius) noexcept
    {
        radius_x_ = radius;
    }

    /**
     * @brief Sets the semi-axis along Y.
     * @param radius The new semi-axis; a value at or below 0 leaves the shape invalid.
     */
    void setRadiusY(float radius) noexcept
    {
        radius_y_ = radius;
    }

    /**
     * @brief Sets the semi-axis along Z.
     * @param radius The new semi-axis; a value at or below 0 leaves the shape invalid.
     */
    void setRadiusZ(float radius) noexcept
    {
        radius_z_ = radius;
    }

    /**
     * @brief Returns the analytic solid this ellipsoid describes.
     * @return The ellipsoid solid with the stored semi-axes.
     */
    [[nodiscard]] EllipsoidSolidf solid() const noexcept
    {
        return EllipsoidSolidf{ radius_x_, radius_y_, radius_z_ };
    }

    /**
     * @brief Returns whether every semi-axis is positive.
     * @return `true` when the ellipsoid can be queried.
     */
    [[nodiscard]] bool isValid() const override;

    /**
     * @brief Returns whether the ellipsoid encloses a volume larger than @p eps in every characteristic size.
     *
     * @param eps Numerical tolerance a dimension must exceed.
     * @return `true` when the ellipsoid encloses a volume by that measure.
     */
    [[nodiscard]] bool hasVolume(double eps = static_cast<double>(vn::math::EPS<float>())) const override;

    /**
     * @brief Returns the axis-aligned bounds of the ellipsoid in its local space.
     * @return The box of the three semi-axes around the origin.
     */
    [[nodiscard]] Aabbf bounds() const override;

  private:
    /// Semi-axis along X; 1 for the unit ellipsoid.
    float radius_x_{ 1.0f };

    /// Semi-axis along Y; 1 for the unit ellipsoid.
    float radius_y_{ 1.0f };

    /// Semi-axis along Z; 1 for the unit ellipsoid.
    float radius_z_{ 1.0f };
};

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

/**
 * @brief Returns the analytic solid @p primitive describes, for the consumers that take one.
 *
 * @details
 * - This is the one conversion a primitive that is a solid needs; the geometry algorithms take the value and know
 *   nothing about `Shape`. A primitive that is not one of the five answers nothing, which is why the result is optional
 *   rather than an empty solid.
 *
 * @param primitive The primitive to convert.
 * @return The analytic solid, or nothing when the primitive is not one of the five solids.
 */
[[nodiscard]] inline std::optional<SolidShapef> solidShapeOf(const Primitive& primitive) noexcept
{
    if (const auto* box = vn::obj_cast<Box>(&primitive)) {
        return SolidShapef{ box->solid() };
    }
    if (const auto* sphere = vn::obj_cast<Sphere>(&primitive)) {
        return SolidShapef{ sphere->solid() };
    }
    if (const auto* cylinder = vn::obj_cast<Cylinder>(&primitive)) {
        return SolidShapef{ cylinder->solid() };
    }
    if (const auto* cone = vn::obj_cast<Cone>(&primitive)) {
        return SolidShapef{ cone->solid() };
    }
    if (const auto* ellipsoid = vn::obj_cast<Ellipsoid>(&primitive)) {
        return SolidShapef{ ellipsoid->solid() };
    }
    return std::nullopt;
}

VN_GEOMETRY_NS_END
