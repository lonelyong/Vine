#include <vine/geom/spatial/Obb.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <limits>
#include <span>

#include <vine/math/EigenDecomposition.hpp>
#include <vine/math/Matrix3x3.hpp>
#include <vine/math/Transform3.hpp>

VN_GEOM_SPATIAL_NS_BEGIN

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

#undef TMPL_PREFIX

template class VN_GEOM_API Obb<float>;
template class VN_GEOM_API Obb<double>;

VN_GEOM_SPATIAL_NS_END
