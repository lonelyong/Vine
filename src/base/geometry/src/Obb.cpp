#include <vine/geometry/Obb.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <limits>
#include <span>

#include <vine/math/EigenDecomposition.hpp>
#include <vine/math/Matrix3x3.hpp>
#include <vine/math/Transform3.hpp>

VN_GEOMETRY_NS_BEGIN

namespace {

/**
 * @brief Builds the box enclosing @p vertices in the orthonormal frame (x, y, z).
 *
 * @param vertices Vertex positions in the box's parent space.
 * @param x          Unit local X direction.
 * @param y          Unit local Y direction, perpendicular to @p x.
 * @param z          Unit local Z direction, perpendicular to @p x and @p y.
 * @return Box with those axes whose extents cover every vertex.
 */
template <typename T>
Obb<T> boxAlongFrame(std::span<const math::Vector3<T>>  vertices,
                     const math::Vector3<T>&            x,
                     const math::Vector3<T>&            y,
                     const math::Vector3<T>&            z)
{
    T min_x = vertices.front().dot(x);
    T min_y = vertices.front().dot(y);
    T min_z = vertices.front().dot(z);
    T max_x = min_x;
    T max_y = min_y;
    T max_z = min_z;

    for (const auto& vertex : vertices) {
        const T projected_x = vertex.dot(x);
        const T projected_y = vertex.dot(y);
        const T projected_z = vertex.dot(z);
        min_x               = std::min(min_x, projected_x);
        min_y               = std::min(min_y, projected_y);
        min_z               = std::min(min_z, projected_z);
        max_x               = std::max(max_x, projected_x);
        max_y               = std::max(max_y, projected_y);
        max_z               = std::max(max_z, projected_z);
    }

    const auto midpoint = [](T low, T high) { return (low + high) / T(2); };

    const math::Vector3<T> center_vector = x * midpoint(min_x, max_x) + y * midpoint(min_y, max_y) + z * midpoint(min_z, max_z);
    const math::Vector3<T> half_extents((max_x - min_x) / T(2), (max_y - min_y) / T(2), (max_z - min_z) / T(2));

    // The axes are the columns of the rotation matrix.
    const math::Matrix3x3<T> frame(x.x, y.x, z.x, x.y, y.y, z.y, x.z, y.z, z.z);

    return Obb<T>(math::Point3<T>(center_vector.x, center_vector.y, center_vector.z), half_extents, math::toQuaternion(frame));
}

} // namespace

#define TMPL_PREFIX template <vn::math::FP T>

TMPL_PREFIX auto Obb<T>::corners() const -> std::array<Point, 8>
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

TMPL_PREFIX bool Obb<T>::contains(const Point& point) const
{
    const auto local     = orientation_.conj() * (point - center_);
    const T    tolerance = std::numeric_limits<T>::epsilon() * T(8);
    const auto within_extent = [tolerance](T coordinate, T extent) {
        return std::abs(coordinate) <= extent + tolerance * std::max({ T(1), std::abs(coordinate), extent });
    };
    return within_extent(local.x, half_extents_.x) && within_extent(local.y, half_extents_.y) &&
           within_extent(local.z, half_extents_.z);
}

TMPL_PREFIX bool Obb<T>::intersects(const Obb<T>& other) const
{
    const auto             a_axes       = axes();
    const auto             b_axes       = other.axes();
    const auto             displacement = other.center_ - center_;
    const std::array<T, 3> a_extent{ half_extents_.x, half_extents_.y, half_extents_.z };
    const std::array<T, 3> b_extent{ other.half_extents_.x, other.half_extents_.y, other.half_extents_.z };
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

TMPL_PREFIX Aabb<T> Obb<T>::toAabb() const
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

TMPL_PREFIX Obb<T> Obb<T>::computeFixedOrientation(std::span<const math::Vector3<T>> vertices,
                                                   const math::Vector3<T>&           x_dir,
                                                   const math::Vector3<T>&           y_dir)
{
    if (vertices.empty()) {
        return Obb<T>();
    }

    math::Vector3<T> x_axis = x_dir;
    if (x_axis.length() <= math::EPS<T>()) {
        x_axis = math::Vector3<T>(T(1), T(0), T(0));
    }
    x_axis = x_axis.normalized();

    math::Vector3<T> y_axis = y_dir - x_axis * x_axis.dot(y_dir);
    if (y_axis.length() <= math::EPS<T>()) {
        // Keep the caller's X direction: take the world axis least aligned with it instead of a fixed fallback axis.
        const std::array<math::Vector3<T>, 3> world{ math::Vector3<T>(T(1), T(0), T(0)),
                                                     math::Vector3<T>(T(0), T(1), T(0)),
                                                     math::Vector3<T>(T(0), T(0), T(1)) };

        std::size_t least_aligned = 0;
        for (std::size_t i = 1; i < world.size(); ++i) {
            if (std::abs(x_axis.dot(world[i])) < std::abs(x_axis.dot(world[least_aligned]))) {
                least_aligned = i;
            }
        }
        y_axis = world[least_aligned] - x_axis * x_axis.dot(world[least_aligned]);
    }
    y_axis = y_axis.normalized();

    const math::Vector3<T> z_axis = x_axis.cross(y_axis).normalized();
    return boxAlongFrame(vertices, x_axis, y_axis, z_axis);
}

TMPL_PREFIX Obb<T> Obb<T>::computePca(std::span<const math::Vector3<T>> vertices)
{
    if (vertices.empty()) {
        return Obb<T>();
    }

    // One coordinate repeated carries no direction: the box has no size, so the axes are meaningless.
    const math::Vector3<T>& first = vertices.front();
    const bool              all_same =
      std::all_of(vertices.begin(), vertices.end(), [&first](const math::Vector3<T>& vertex) {
          return vertex.x == first.x && vertex.y == first.y && vertex.z == first.z;
      });
    if (all_same) {
        return Obb<T>(math::Point3<T>(first.x, first.y, first.z), math::Vector3<T>(T(0), T(0), T(0)), math::Quaternion<T>());
    }

    math::Vector3<T> centroid;
    for (const auto& vertex : vertices) {
        centroid = centroid + vertex;
    }
    centroid = centroid * (T(1) / T(vertices.size()));

    // Covariance about the centroid; the eigenvectors are the principal axes.
    T sxx = T(0);
    T sxy = T(0);
    T sxz = T(0);
    T syy = T(0);
    T syz = T(0);
    T szz = T(0);
    for (const auto& vertex : vertices) {
        const math::Vector3<T> delta = vertex - centroid;
        sxx += delta.x * delta.x;
        sxy += delta.x * delta.y;
        sxz += delta.x * delta.z;
        syy += delta.y * delta.y;
        syz += delta.y * delta.z;
        szz += delta.z * delta.z;
    }

    const math::Matrix3x3<T> covariance(sxx, sxy, sxz, sxy, syy, syz, sxz, syz, szz);

    std::array<T, 3>   spectrum{};
    math::Matrix3x3<T> axes;
    math::symmetricEigen(covariance, spectrum, &axes);

    // Ascending spectrum: the last column is the direction of largest variance, the middle one the second.
    math::Vector3<T> x_axis(axes(0, 2), axes(1, 2), axes(2, 2));
    math::Vector3<T> y_axis(axes(0, 1), axes(1, 1), axes(2, 1));

    if (x_axis.length() <= math::EPS<T>()) {
        x_axis = math::Vector3<T>(T(1), T(0), T(0));
    }
    x_axis = x_axis.normalized();

    y_axis = y_axis - x_axis * x_axis.dot(y_axis);
    if (y_axis.length() <= math::EPS<T>()) {
        y_axis = math::Vector3<T>(T(0), T(1), T(0)) - x_axis * x_axis.dot(math::Vector3<T>(T(0), T(1), T(0)));
    }
    y_axis = y_axis.normalized();

    const math::Vector3<T> z_axis = x_axis.cross(y_axis).normalized();
    return boxAlongFrame(vertices, x_axis, y_axis, z_axis);
}

TMPL_PREFIX bool Obb<T>::intersectWith(const Ray<T>& ray, RayHit<T>& hit, T eps) const
{
    if (ray.direction.length() <= eps) {
        return false; // an aimless ray crosses nothing
    }

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

#undef TMPL_PREFIX

template class VN_GEOMETRY_API Obb<float>;
template class VN_GEOMETRY_API Obb<double>;

VN_GEOMETRY_NS_END
