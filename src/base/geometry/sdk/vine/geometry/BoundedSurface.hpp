#pragma once

#include "geometry_global.hpp"

#include "Surface.hpp"

VN_GEOMETRY_NS_BEGIN

/**
 * @brief A surface with finite extent: a patch with its own corners, or an elementary sheet restricted to a rectangle.
 *
 * @details
 * - The parameter domain is bounded, so a bounded surface can always answer its four parameter bounds.
 * - The family is a classification only: it adds no state and no virtual function of its own, so it costs nothing on
 *   top of the interface it inherits.
 *
 * @tparam T Floating-point scalar type.
 */
template <vn::math::FP T>
class BoundedSurface : public Surface<T>
{
};

VN_GEOMETRY_NS_END
