#pragma once

#include <vine/geom/geom_global.hpp>

#include "Surface.hpp"

VN_GEOM_SPATIAL_NS_BEGIN

/**
 * @brief A surface whose parameterisation covers its whole analytic sheet: an unbounded or semi-infinite sheet.
 *
 * @details
 * - Elementary surfaces are the analytic support of a face, not the face itself: a plane, a cylinder, a cone, a
 * sphere, a torus.
 * - The family is a classification only: it adds no state and no virtual function of its own, so it costs nothing on
 *   top of the interface it inherits.
 *
 * @tparam T Floating-point scalar type.
 */
template <vn::math::FP T>
class ElementarySurface : public Surface<T>
{
};

VN_GEOM_SPATIAL_NS_END
