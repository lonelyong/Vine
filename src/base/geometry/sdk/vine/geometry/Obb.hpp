#pragma once

#include "geometry_global.hpp"

#include <array>
#include <cmath>
#include <span>

#include <vine/math/Point3.hpp>
#include <vine/math/Quaternion.hpp>
#include <vine/math/Types.hpp>
#include <vine/math/Vector3.hpp>

#include "Aabb.hpp"
#include "Ray.hpp"

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
     * @brief Intersects the box with a ray.
     *
     * @details
     * - The ray is moved into the box's local space, where the test is the axis-aligned slab test; the orientation is a
     *   rotation, so the returned `t` is a distance in the space the ray was given in.
     *
     * @param ray The ray, whose direction is unit length (see Ray).
     * @param hit Receives the nearest hit when the call returns true; untouched otherwise.
     * @param eps Tolerance for "the direction runs parallel to this slab".
     * @return True when the box lies in front of the ray's origin or around it.
     */
    [[nodiscard]]
    bool intersectWith(const Ray<T>& ray, RayHit<T>& hit, T eps = math::EPS<T>()) const;

    /**
     * @brief Returns the smallest axis-aligned box enclosing this oriented box.
     *
     * @return World-space axis-aligned bounds.
     */
    [[nodiscard]]
    Aabb<T> toAabb() const;

    /**
     * @brief Fits an oriented box whose local X and Y axes follow the given directions.
     *
     * @details
     * - @p y_dir is orthogonalized against @p x_dir before use; z completes a right-handed frame.
     * - Every returned box encloses all vertices: the axes only decide how tightly.
     * - A zero-length @p x_dir falls back to world X.
     * - A @p y_dir parallel to @p x_dir falls back to the world axis least aligned with @p x_dir, so the caller's X
     *   direction is kept.
     * - An empty vertex set returns the default box.
     *
     * @param vertices Vertex positions in the box's parent space.
     * @param x_dir    Local X direction; need not be unit length, but must not be degenerate unless a fallback is intended.
     * @param y_dir    Local Y direction; need not be unit length and need not be perpendicular to @p x_dir.
     * @return Box enclosing every vertex.
     */
    [[nodiscard]]
    static Obb<T> computeFixedOrientation(std::span<const math::Vector3<T>> vertices,
                                          const math::Vector3<T>& x_dir,
                                          const math::Vector3<T>& y_dir);

    /**
     * @brief Fits an oriented box along the principal axes of the vertex set (PCA).
     *
     * @details
     * - The axes are the eigenvectors of the vertex covariance, ordered by decreasing variance: local X holds the
     *   largest spread, local Z the smallest. Extents are measured after that ordering, so they are not sorted.
     * - Every returned box encloses all vertices; a degenerate point set only decides how arbitrary the axes are:
     *   a collinear set keeps the major axis and the remaining directions are an arbitrary but deterministic
     *   orthonormal pair.
     * - An empty vertex set returns the default box; vertices that all share one coordinate return a zero-size box
     *   at that coordinate with identity orientation, because no direction can be inferred.
     *
     * @param vertices Vertex positions in the box's parent space.
     * @return Box enclosing every vertex, with the axes of largest spread.
     */
    [[nodiscard]]
    static Obb<T> computePca(std::span<const math::Vector3<T>> vertices);

  private:
    Point center_{};
    Vector half_extents_{};
    Quaternion orientation_{};
};

using Obbf = Obb<float>;
using Obbd = Obb<double>;

VN_GEOMETRY_NS_END
