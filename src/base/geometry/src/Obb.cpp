#include <vine/geometry/Obb.hpp>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <limits>

VN_GEOMETRY_NS_BEGIN

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

#undef TMPL_PREFIX

template class VN_GEOMETRY_API Obb<float>;
template class VN_GEOMETRY_API Obb<double>;

VN_GEOMETRY_NS_END
