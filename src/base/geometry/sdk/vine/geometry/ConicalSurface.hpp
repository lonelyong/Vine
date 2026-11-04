#pragma once

#include "geometry_global.hpp"

#include "ElementarySurface.hpp"

VN_GEOMETRY_NS_BEGIN

/**
 * @brief Placeholder for an unbounded cone: an axis, a semi-angle and a reference radius.
 *
 * @details
 * - Declared, not implemented: the name fixes the taxonomy (`geometry-kinds.md` 2.3) while the
 *   parameters and the queries wait for a consumer (`../backlog/geometry.md` G6).
 * - The default constructor is deleted on purpose: a placeholder that could be constructed would be a
 *   silently meaningless value.
 */
template <vn::math::FP T>
class ConicalSurface : public ElementarySurface<T>
{
  public:
    ConicalSurface() = delete;
};

VN_GEOMETRY_NS_END
