#pragma once

#include "geometry_global.hpp"

#include <array>
#include <cmath>

#include <vine/math/Point3.hpp>
#include <vine/math/Quaternion.hpp>
#include <vine/math/Types.hpp>
#include <vine/math/Vector3.hpp>

#include "Aabb.hpp"

VN_GEOMETRY_NS_BEGIN

/**
 * @brief Three-dimensional oriented box defined by its center, half-extents and rotation.
 *
 * The local axes are the world axes rotated by a normalized orientation quaternion.
 * Half-extents are stored as non-negative magnitudes.
 *
 * @tparam T Floating-point scalar type.
 */
template <vn::math::FP T>
class Obb {
  public:
    using value_type = T;
    using Point = vn::math::Point3<T>;
    using Vector = vn::math::Vector3<T>;
    using Quaternion = vn::math::Quaternion<T>;

  public:
    /** @brief Constructs a zero-sized box at the origin with identity orientation. */
    Obb() = default;

    /**
     * @brief Constructs an oriented box and canonicalizes its extent and rotation.
     *
     * @param center World-space center.
     * @param half_extents Non-negative half-size along each local axis; magnitudes are stored.
     * @param orientation Rotation from local axes to world axes; normalized before storage.
     */
    Obb(const Point& center,
        const Vector& half_extents,
        const Quaternion& orientation = Quaternion())
      : center_(center)
      , half_extents_(std::abs(half_extents.x), std::abs(half_extents.y), std::abs(half_extents.z))
      , orientation_(orientation.normalized())
    {}

  public:
    /**
     * @brief Returns the world-space center.
     *
     * @return The stored center point.
     */
    [[nodiscard]]
    Point center() const
    {
        return center_;
    }

    /**
     * @brief Returns the non-negative half-size along each local axis.
     *
     * @return The stored half-extents, each component non-negative.
     */
    [[nodiscard]]
    Vector halfExtents() const
    {
        return half_extents_;
    }

    /**
     * @brief Returns the normalized rotation from local axes to world axes.
     *
     * @return The stored orientation quaternion, unit length.
     */
    [[nodiscard]]
    Quaternion orientation() const
    {
        return orientation_;
    }

    /**
     * @brief Sets the world-space center.
     *
     * @param center New world-space center.
     */
    void setCenter(const Point& center)
    {
        center_ = center;
    }

    /**
     * @brief Sets the half-extents, storing each component as a non-negative magnitude.
     *
     * @param half_extents Half-size along each local axis.
     */
    void setHalfExtents(const Vector& half_extents)
    {
        half_extents_ = Vector(std::abs(half_extents.x), std::abs(half_extents.y), std::abs(half_extents.z));
    }

    /**
     * @brief Sets the local-to-world rotation, normalizing it before storage.
     *
     * A zero-length quaternion is converted to the identity rotation.
     *
     * @param orientation Rotation from local axes to world axes.
     */
    void setOrientation(const Quaternion& orientation)
    {
        orientation_ = orientation.normalized();
    }

    /**
     * @brief Returns the three local unit axes expressed in world space.
     *
     * @return Axes ordered as local X, Y and Z.
     */
    [[nodiscard]]
    std::array<Vector, 3> axes() const
    {
        return { orientation_ * Vector(T(1), T(0), T(0)),
                 orientation_ * Vector(T(0), T(1), T(0)),
                 orientation_ * Vector(T(0), T(0), T(1)) };
    }

    /**
     * @brief Returns the eight corners in a stable sign-bit order.
     *
     * Bit 0 selects the X sign, bit 1 the Y sign and bit 2 the Z sign; a set bit means positive.
     *
     * @return Corners transformed into world space.
     */
    [[nodiscard]]
    std::array<Point, 8> corners() const;

    /**
     * @brief Checks whether a point is inside or on the box, within round-off tolerance.
     *
     * @param point World-space point.
     * @return True when the point lies within all three local half-extents.
     */
    [[nodiscard]]
    bool contains(const Point& point) const;

    /**
     * @brief Checks whether this box intersects another oriented box.
     *
     * Touching boxes count as intersecting. A machine-epsilon tolerance is applied to the
     * separating-axis projections to avoid false separation from round-off.
     *
     * @param other Box to test.
     * @return True when the boxes overlap or touch.
     */
    [[nodiscard]]
    bool intersects(const Obb<T>& other) const;

    /**
     * @brief Returns the smallest axis-aligned box enclosing this oriented box.
     *
     * @return World-space axis-aligned bounds.
     */
    [[nodiscard]]
    Aabb<T> toAabb() const;

  private:
    Point center_{};
    Vector half_extents_{};
    Quaternion orientation_{};
};

using Obbf = Obb<float>;
using Obbd = Obb<double>;

VN_GEOMETRY_NS_END
