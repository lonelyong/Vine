#pragma once

#include "geometry_global.hpp"

#include <vine/math/Types.hpp>

#include "Aabb.hpp"
#include "Shape.hpp"

VN_GEOMETRY_NS_BEGIN

/**
 * @brief Base class for the parametric primitives a model is built from.
 *
 * @details
 * - A primitive is a **modeling type**: it carries identity, a name and a revision through `Shape`, plus the
 *   dimensions of one analytic shape. It provides no geometry algorithm beyond its own bounds and validity —
 *   a ray against a box is `Aabb::intersectWith(Ray)`, a ray against a spherical sheet is `SphericalSurface`
 *   (see `geometry-kinds.md` 2.6). Algorithms live in `geometry` values, never here.
 * - The five shapes live in **this header**, not one file each: they are five parameter records over the same base,
 *   and keeping them together also makes the DLL's export rule implicit — `Primitive.cpp` already includes this
 *   header, so the exported inline members of every subclass are emitted without a "remember to include it"
 *   convention.
 * - Each subclass passes its own `ShapeType` to the protected constructor, so the tag cannot drift from the class:
 *   there is no per-constructor assignment to get wrong.
 */
class VN_GEOMETRY_API Primitive : public Shape {
    VN_OBJECT_META_DECL;

  protected:
    /**
     * @brief Constructs a primitive of a concrete type.
     *
     * @param type The concrete `ShapeType`; each subclass passes its own so the tag matches the class by construction.
     */
    explicit Primitive(ShapeType type);
};

/**
 * @brief Axis-aligned box given by its three full edge lengths, centred on the origin of a local space.
 *
 * @details
 * - The canonical local space keeps the origin at the centre, which is the space the B-rep bridge and the robot IO
 *   assume; placement belongs to the node transform, not to the shape.
 * - A non-positive length is invalid rather than clamped: `isValid()` answers it and `bounds()` stays well formed.
 */
class VN_GEOMETRY_API Box : public Primitive {
    VN_OBJECT_META_DECL;

  public:
    /**
     * @brief Constructs a unit box (1 x 1 x 1).
     */
    Box();

    /**
     * @brief Constructs a box from its full edge lengths.
     *
     * @param width  Size along the X axis.
     * @param height Size along the Y axis.
     * @param depth  Size along the Z axis.
     */
    Box(float width, float height, float depth);

  public:
    /**
     * @brief Returns the size along the X axis.
     *
     * @return Full edge length along X.
     */
    [[nodiscard]] float width() const noexcept;

    /**
     * @brief Sets the size along the X axis.
     *
     * @param width New full edge length along X.
     */
    void setWidth(float width) noexcept;

    /**
     * @brief Returns the size along the Y axis.
     *
     * @return Full edge length along Y.
     */
    [[nodiscard]] float height() const noexcept;

    /**
     * @brief Sets the size along the Y axis.
     *
     * @param height New full edge length along Y.
     */
    void setHeight(float height) noexcept;

    /**
     * @brief Returns the size along the Z axis.
     *
     * @return Full edge length along Z.
     */
    [[nodiscard]] float depth() const noexcept;

    /**
     * @brief Sets the size along the Z axis.
     *
     * @param depth New full edge length along Z.
     */
    void setDepth(float depth) noexcept;

    [[nodiscard]] bool isValid() const override;

    [[nodiscard]] bool hasVolume(double eps = static_cast<double>(vn::math::EPS<float>())) const override;

    /**
     * @brief Returns the axis-aligned bounds in the local space.
     *
     * @return Bounds spanning the three edge lengths, half of each either side of the origin.
     */
    [[nodiscard]] Aabbf bounds() const noexcept;

  private:
    float width_{ 1.0f };
    float height_{ 1.0f };
    float depth_{ 1.0f };
};

/**
 * @brief Sphere given by its radius, centred on the origin of a local space.
 *
 * @details
 * - The canonical local space keeps the centre at the origin; placement belongs to the node transform.
 * - This is the *solid*; the unbounded spherical sheet with its own parameterisation is `SphericalSurface`.
 */
class VN_GEOMETRY_API Sphere : public Primitive {
    VN_OBJECT_META_DECL;

  public:
    /**
     * @brief Constructs a unit sphere (radius 0.5).
     */
    Sphere();

    /**
     * @brief Constructs a sphere from its radius.
     *
     * @param radius Radius of the sphere.
     */
    explicit Sphere(float radius);

  public:
    /**
     * @brief Returns the radius.
     *
     * @return Radius of the sphere.
     */
    [[nodiscard]] float radius() const noexcept;

    /**
     * @brief Sets the radius.
     *
     * @param radius New radius.
     */
    void setRadius(float radius) noexcept;

    [[nodiscard]] bool isValid() const override;

    [[nodiscard]] bool hasVolume(double eps = static_cast<double>(vn::math::EPS<float>())) const override;

    /**
     * @brief Returns the axis-aligned bounds in the local space.
     *
     * @return Cube of side 2 * radius, centred on the origin.
     */
    [[nodiscard]] Aabbf bounds() const noexcept;

  private:
    float radius_{ 0.5f };
};

/**
 * @brief Right circular cylinder given by its radius and height, its axis on +Z through the origin.
 *
 * @details
 * - The canonical local space spans `[-height / 2, +height / 2]` on +Z with the origin at the centre of the axis.
 * - This is the *solid*; the unbounded cylindrical sheet is a future `CylindricalSurface`
 *   (`../backlog/geometry.md` G6).
 */
class VN_GEOMETRY_API Cylinder : public Primitive {
    VN_OBJECT_META_DECL;

  public:
    /**
     * @brief Constructs a cylinder of radius 0.5 and height 1.
     */
    Cylinder();

    /**
     * @brief Constructs a cylinder from its radius and height.
     *
     * @param radius Radius of the circular faces.
     * @param height Full height along +Z.
     */
    Cylinder(float radius, float height);

  public:
    /**
     * @brief Returns the radius of the circular faces.
     *
     * @return Radius.
     */
    [[nodiscard]] float radius() const noexcept;

    /**
     * @brief Sets the radius of the circular faces.
     *
     * @param radius New radius.
     */
    void setRadius(float radius) noexcept;

    /**
     * @brief Returns the full height along +Z.
     *
     * @return Height.
     */
    [[nodiscard]] float height() const noexcept;

    /**
     * @brief Sets the full height along +Z.
     *
     * @param height New height.
     */
    void setHeight(float height) noexcept;

    [[nodiscard]] bool isValid() const override;

    [[nodiscard]] bool hasVolume(double eps = static_cast<double>(vn::math::EPS<float>())) const override;

    /**
     * @brief Returns the axis-aligned bounds in the local space.
     *
     * @return Bounds spanning +/- radius on X and Y, +/- height / 2 on Z.
     */
    [[nodiscard]] Aabbf bounds() const noexcept;

  private:
    float radius_{ 0.5f };
    float height_{ 1.0f };
};

/**
 * @brief Right circular cone given by its base radius and height, its axis on +Z through the origin.
 *
 * @details
 * - The canonical local space puts the base at `-height / 2` and the apex at `+height / 2` on +Z, radius measured at
 *   the base.
 * - This is the *solid*; the unbounded conical sheet is a future `ConicalSurface`
 *   (`../backlog/geometry.md` G6).
 */
class VN_GEOMETRY_API Cone : public Primitive {
    VN_OBJECT_META_DECL;

  public:
    /**
     * @brief Constructs a cone of base radius 0.5 and height 1.
     */
    Cone();

    /**
     * @brief Constructs a cone from its base radius and height.
     *
     * @param radius Radius of the base.
     * @param height Full height along +Z.
     */
    Cone(float radius, float height);

  public:
    /**
     * @brief Returns the radius of the base.
     *
     * @return Radius.
     */
    [[nodiscard]] float radius() const noexcept;

    /**
     * @brief Sets the radius of the base.
     *
     * @param radius New radius.
     */
    void setRadius(float radius) noexcept;

    /**
     * @brief Returns the full height along +Z.
     *
     * @return Height.
     */
    [[nodiscard]] float height() const noexcept;

    /**
     * @brief Sets the full height along +Z.
     *
     * @param height New height.
     */
    void setHeight(float height) noexcept;

    [[nodiscard]] bool isValid() const override;

    [[nodiscard]] bool hasVolume(double eps = static_cast<double>(vn::math::EPS<float>())) const override;

    /**
     * @brief Returns the axis-aligned bounds in the local space.
     *
     * @return Bounds spanning +/- radius on X and Y, +/- height / 2 on Z.
     */
    [[nodiscard]] Aabbf bounds() const noexcept;

  private:
    float radius_{ 0.5f };
    float height_{ 1.0f };
};

/**
 * @brief Tri-axial ellipsoid given by its three semi-axes, centred on the origin of a local space.
 *
 * @details
 * - The semi-axes lie on the local X, Y and Z axes; the canonical local space keeps the centre at the origin.
 */
class VN_GEOMETRY_API Ellipsoid : public Primitive {
    VN_OBJECT_META_DECL;

  public:
    /**
     * @brief Constructs a unit sphere (all three semi-axes 0.5).
     */
    Ellipsoid();

    /**
     * @brief Constructs an ellipsoid from its three semi-axes.
     *
     * @param radius_x Semi-axis along X.
     * @param radius_y Semi-axis along Y.
     * @param radius_z Semi-axis along Z.
     */
    Ellipsoid(float radius_x, float radius_y, float radius_z);

  public:
    /**
     * @brief Returns the semi-axis along X.
     *
     * @return Semi-axis along X.
     */
    [[nodiscard]] float radiusX() const noexcept;

    /**
     * @brief Sets the semi-axis along X.
     *
     * @param radius New semi-axis along X.
     */
    void setRadiusX(float radius) noexcept;

    /**
     * @brief Returns the semi-axis along Y.
     *
     * @return Semi-axis along Y.
     */
    [[nodiscard]] float radiusY() const noexcept;

    /**
     * @brief Sets the semi-axis along Y.
     *
     * @param radius New semi-axis along Y.
     */
    void setRadiusY(float radius) noexcept;

    /**
     * @brief Returns the semi-axis along Z.
     *
     * @return Semi-axis along Z.
     */
    [[nodiscard]] float radiusZ() const noexcept;

    /**
     * @brief Sets the semi-axis along Z.
     *
     * @param radius New semi-axis along Z.
     */
    void setRadiusZ(float radius) noexcept;

    [[nodiscard]] bool isValid() const override;

    [[nodiscard]] bool hasVolume(double eps = static_cast<double>(vn::math::EPS<float>())) const override;

    /**
     * @brief Returns the axis-aligned bounds in the local space.
     *
     * @return Bounds spanning +/- each semi-axis, centred on the origin.
     */
    [[nodiscard]] Aabbf bounds() const noexcept;

  private:
    float radius_x_{ 0.5f };
    float radius_y_{ 0.5f };
    float radius_z_{ 0.5f };
};

VN_GEOMETRY_NS_END
