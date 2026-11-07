#pragma once

#include <vine/geom/geom_global.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <limits>
#include <span>

#include <vine/math/Point3.hpp>
#include <vine/math/Quaternion.hpp>
#include <vine/math/Types.hpp>
#include <vine/math/Vector3.hpp>

#include "Aabb.hpp"
#include "Ray.hpp"

VN_GEOM_SPATIAL_NS_BEGIN

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
     * - Only the NEAREST hit is reported, the same contract as `Aabb`: an oriented box is a bound, not a solid.
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

template <vn::math::FP T>
std::array<typename Obb<T>::Point, 8> Obb<T>::corners() const
{
    std::array<Point, 8> result;
    for (std::size_t i = 0; i < result.size(); ++i) {
        const Vector local((i & 1u) ? half_extents_.x : -half_extents_.x,
                           (i & 2u) ? half_extents_.y : -half_extents_.y,
                           (i & 4u) ? half_extents_.z : -half_extents_.z);
        result[i] = center_ + orientation_ * local;
    }
    return result;
}

template <vn::math::FP T>
bool Obb<T>::contains(const Point& point) const
{
    const auto local         = orientation_.conj() * (point - center_);
    const T    tolerance     = std::numeric_limits<T>::epsilon() * T(8);
    const auto within_extent = [tolerance](T coordinate, T extent) {
        return std::abs(coordinate) <= extent + tolerance * std::max({ T(1), std::abs(coordinate), extent });
    };
    return within_extent(local.x, half_extents_.x) && within_extent(local.y, half_extents_.y) &&
           within_extent(local.z, half_extents_.z);
}

template <vn::math::FP T>
bool Obb<T>::intersects(const Obb<T>& other) const
{
    const auto                      a_axes       = axes();
    const auto                      b_axes       = other.axes();
    const auto                      displacement = other.center_ - center_;
    const std::array<T, 3>          a_extent{ half_extents_.x, half_extents_.y, half_extents_.z };
    const std::array<T, 3>          b_extent{ other.half_extents_.x, other.half_extents_.y, other.half_extents_.z };
    std::array<std::array<T, 3>, 3> rotation{};
    std::array<std::array<T, 3>, 3> abs_rotation{};
    std::array<T, 3>                translated{};
    const T                         tolerance = std::numeric_limits<T>::epsilon() * T(8);

    for (std::size_t i = 0; i < 3; ++i) {
        translated[i] = displacement.dot(a_axes[i]);
        for (std::size_t j = 0; j < 3; ++j) {
            rotation[i][j]     = a_axes[i].dot(b_axes[j]);
            abs_rotation[i][j] = std::abs(rotation[i][j]) + tolerance;
        }
    }

    for (std::size_t i = 0; i < 3; ++i) {
        T projected_radius = T(0);
        for (std::size_t j = 0; j < 3; ++j) {
            projected_radius += b_extent[j] * abs_rotation[i][j];
        }
        if (std::abs(translated[i]) > a_extent[i] + projected_radius) {
            return false;
        }
    }

    for (std::size_t j = 0; j < 3; ++j) {
        const T projected_center =
          std::abs(translated[0] * rotation[0][j] + translated[1] * rotation[1][j] + translated[2] * rotation[2][j]);
        T projected_radius = T(0);
        for (std::size_t i = 0; i < 3; ++i) {
            projected_radius += a_extent[i] * abs_rotation[i][j];
        }
        if (projected_center > b_extent[j] + projected_radius) {
            return false;
        }
    }

    for (std::size_t i = 0; i < 3; ++i) {
        for (std::size_t j = 0; j < 3; ++j) {
            const std::size_t i1 = (i + 1) % 3;
            const std::size_t i2 = (i + 2) % 3;
            const std::size_t j1 = (j + 1) % 3;
            const std::size_t j2 = (j + 2) % 3;
            const T projected_center = std::abs(translated[i2] * rotation[i1][j] - translated[i1] * rotation[i2][j]);
            const T a_radius         = a_extent[i1] * abs_rotation[i2][j] + a_extent[i2] * abs_rotation[i1][j];
            const T b_radius         = b_extent[j1] * abs_rotation[i][j2] + b_extent[j2] * abs_rotation[i][j1];
            if (projected_center > a_radius + b_radius) {
                return false;
            }
        }
    }

    return true;
}

template <vn::math::FP T>
bool Obb<T>::intersectWith(const Ray<T>& ray, RayHit<T>& hit, T eps) const
{
    // Into the box's local space, where the test is the axis-aligned one. The orientation is a rotation, so a unit
    // direction stays unit and the distance along it survives the round trip unchanged.
    const Quaternion inverse = orientation_.conj();
    const Vector     to_origin(ray.origin.x - center_.x, ray.origin.y - center_.y, ray.origin.z - center_.z);
    const Vector     local_origin    = inverse * to_origin;
    const Vector     local_direction = inverse * ray.direction;

    const Ray<T>  local_ray(math::Point3<T>(local_origin.x, local_origin.y, local_origin.z), local_direction);
    const Aabb<T> local_box(math::Point3<T>(-half_extents_.x, -half_extents_.y, -half_extents_.z),
                            math::Point3<T>(half_extents_.x, half_extents_.y, half_extents_.z));

    RayHit<T> local_hit;
    if (!local_box.intersectWith(local_ray, local_hit, eps)) {
        return false;
    }

    const math::Point3<T> world_point = orientation_ * local_hit.point;

    hit.t      = local_hit.t;
    hit.point  = math::Point3<T>(center_.x + world_point.x, center_.y + world_point.y, center_.z + world_point.z);
    hit.normal = orientation_ * local_hit.normal;
    return true;
}

template <vn::math::FP T>
Aabb<T> Obb<T>::toAabb() const
{
    const auto   box_axes = axes();
    const Vector radius(std::abs(box_axes[0].x) * half_extents_.x + std::abs(box_axes[1].x) * half_extents_.y +
                          std::abs(box_axes[2].x) * half_extents_.z,
                        std::abs(box_axes[0].y) * half_extents_.x + std::abs(box_axes[1].y) * half_extents_.y +
                          std::abs(box_axes[2].y) * half_extents_.z,
                        std::abs(box_axes[0].z) * half_extents_.x + std::abs(box_axes[1].z) * half_extents_.y +
                          std::abs(box_axes[2].z) * half_extents_.z);
    return Aabb<T>(center_.x - radius.x,
                   center_.y - radius.y,
                   center_.z - radius.z,
                   center_.x + radius.x,
                   center_.y + radius.y,
                   center_.z + radius.z);
}

using Obbf = Obb<float>;
using Obbd = Obb<double>;

VN_GEOM_SPATIAL_NS_END
