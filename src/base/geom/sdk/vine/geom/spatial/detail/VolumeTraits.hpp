#pragma once

#include <vine/geom/geom_global.hpp>

#include <array>
#include <cmath>

#include <vine/math/Math.hpp>
#include <vine/math/Point3.hpp>
#include <vine/math/Types.hpp>
#include <vine/math/Vector3.hpp>

#include <vine/geom/spatial/Aabb.hpp>
#include <vine/geom/spatial/Ray.hpp>
#include <vine/geom/spatial/RayHit.hpp>
#include <vine/geom/spatial/detail/RaySlab.hpp>

VN_GEOM_SPATIAL_NS_BEGIN

namespace detail
{

/**
 * @brief The operations a bounding-volume type must provide to be usable as a BVH's `Volume`.
 *
 * @details
 * - One specialization per volume type, so a volume type needs no new members: the tree's needs are stated here and
 *   the existing types are adapted, rather than the other way round.
 * - `halfArea` rather than volume on purpose: the surface-area heuristic costs splits by surface area, and a box that
 *   degenerates to a rectangle has zero volume but a meaningful area.
 * - `merged` may be conservative - it must contain both inputs but need not be their exact union - so the tree's
 *   containment invariant is the only thing a new volume type has to guarantee.
 * - Only `Aabb` is specialised today. Merging two oriented boxes has no exact closed form, and the obvious conservative
 *   merge - fitting a PCA box around the sixteen corners of a pair - was measured to give leaves about twice the
 *   surface area of an `Aabb` tree's on long thin parts, because one fitted orientation explains neither child.
 *   It also leaves corners a rounding step outside the box just fitted, so containment needed a tolerance. An `Obb`
 *   volume needs a merge that keeps both children's frames, not one fitted box per merge.
 *
 * @tparam Volume The volume type.
 */
template <typename Volume>
struct VolumeTraits;

/**
 * @brief `Aabb` as a BVH volume: exact merges, six-comparison overlap tests.
 *
 * @tparam T Floating-point scalar type.
 */
template <vn::math::FP T>
struct VolumeTraits<Aabb<T>>
{
    using Scalar  = T;
    using VolumeType = Aabb<T>;

    /**
     * @brief Merges two boxes into the smallest box containing both.
     *
     * @param lhs First box.
     * @param rhs Second box.
     * @return The union; exact and associative for boxes.
     */
    static VolumeType merged(const VolumeType& lhs, const VolumeType& rhs)
    {
        VolumeType out = lhs;
        out.expandBy(rhs);
        return out;
    }

    /**
     * @brief Returns the centre of a box.
     *
     * @param box The box.
     * @return Its centre; an inverted box still yields a finite point.
     */
    static vn::math::Point3<T> center(const VolumeType& box)
    {
        return vn::math::Point3<T>((box.xmin + box.xmax) / T(2), (box.ymin + box.ymax) / T(2),
                                   (box.zmin + box.zmax) / T(2));
    }

    /**
     * @brief Returns half the surface area of a box, the SAH cost of one primitive.
     *
     * @param box The box.
     * @return `dx·dy + dy·dz + dz·dx`; zero for a point or a segment.
     */
    static T halfArea(const VolumeType& box)
    {
        const T dx = box.xmax - box.xmin;
        const T dy = box.ymax - box.ymin;
        const T dz = box.zmax - box.zmin;
        return dx * dy + dy * dz + dz * dx;
    }

    /**
     * @brief Reports whether two boxes share any space, touching included.
     *
     * @param lhs First box.
     * @param rhs Second box.
     * @return True when they overlap on all three axes.
     */
    static bool overlaps(const VolumeType& lhs, const VolumeType& rhs)
    {
        return lhs.xmin <= rhs.xmax && rhs.xmin <= lhs.xmax && lhs.ymin <= rhs.ymax && rhs.ymin <= lhs.ymax &&
               lhs.zmin <= rhs.zmax && rhs.zmin <= lhs.zmax;
    }

    /**
     * @brief Reports whether a point lies inside a box, boundary included.
     *
     * @param box   The box.
     * @param point The point.
     * @return True when every coordinate is within the box.
     */
    static bool contains(const VolumeType& box, const vn::math::Point3<T>& point)
    {
        return box.contains(point);
    }

    /**
     * @brief Returns the eight corners of a box in a stable order.
     *
     * @param box The box.
     * @return The corners; used to test one box against another.
     */
    static std::array<vn::math::Point3<T>, 8> corners(const VolumeType& box)
    {
        return { vn::math::Point3<T>(box.xmin, box.ymin, box.zmin), vn::math::Point3<T>(box.xmax, box.ymin, box.zmin),
                 vn::math::Point3<T>(box.xmin, box.ymax, box.zmin), vn::math::Point3<T>(box.xmax, box.ymax, box.zmin),
                 vn::math::Point3<T>(box.xmin, box.ymin, box.zmax), vn::math::Point3<T>(box.xmax, box.ymin, box.zmax),
                 vn::math::Point3<T>(box.xmin, box.ymax, box.zmax), vn::math::Point3<T>(box.xmax, box.ymax, box.zmax) };
    }

    /**
     * @brief Reports whether a box is usable: finite on every axis and not inverted.
     *
     * @param box The box.
     * @return False for NaN, infinite or inverted boxes.
     */
    static bool isFinite(const VolumeType& box)
    {
        return std::isfinite(box.xmin) && std::isfinite(box.ymin) && std::isfinite(box.zmin) &&
               std::isfinite(box.xmax) && std::isfinite(box.ymax) && std::isfinite(box.zmax) && box.xmin <= box.xmax &&
               box.ymin <= box.ymax && box.zmin <= box.zmax;
    }

    /**
     * @brief Reports whether a prepared ray enters a box, and where.
     *
     * @param box   The box.
     * @param ray   The prepared ray.
     * @param enter Receives the entry distance, clamped at zero when the origin is inside.
     * @return True when the box is in front of the origin or around it.
     */
    static bool hitByRay(const VolumeType& box, const PreparedRay<T>& ray, T& enter)
    {
        RayBoxInterval<T> interval;
        if (!preparedRayBoxInterval(ray, box.xmin, box.ymin, box.zmin, box.xmax, box.ymax, box.zmax,
                                    vn::math::EPS<T>(), T(0), interval)) {
            return false;
        }
        enter = interval.enter_t > T(0) ? interval.enter_t : T(0);
        return true;
    }
};

} // namespace detail

VN_GEOM_SPATIAL_NS_END
