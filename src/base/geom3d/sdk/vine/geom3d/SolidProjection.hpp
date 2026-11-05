#pragma once

#include "geom3d_global.hpp"

#include <vine/math/Point3.hpp>
#include <vine/math/Types.hpp>

VN_GEOM3D_NS_BEGIN

/**
 * @brief The nearest point of a solid to a query point, and the distance to it.
 *
 * @details
 * - `point` is the nearest point of the **solid**, not only of its boundary: a query inside the solid answers the query
 *   itself, and `distance` is therefore 0 there.
 * - `distance` is `|point - query|`, so one result answers both "where is the nearest point" and "how far is it".
 *
 * @tparam T Floating-point scalar type.
 */
template <vn::math::FP T>
struct SolidProjection {
    /// The nearest point of the solid; the query itself when the query is inside.
    vn::math::Point3<T> point{};

    /// Distance from the query to @p point; 0 inside the solid.
    T distance{ T(0) };
};

VN_GEOM3D_NS_END
