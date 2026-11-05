#pragma once

#include "geom3d_global.hpp"

#include <vine/math/Point3.hpp>
#include <vine/math/Types.hpp>

#include "Curve.hpp"

VN_GEOM3D_NS_BEGIN

/**
 * @brief A curve whose parameter range is finite: it always has two end points.
 *
 * @details
 * - `firstParameter()` / `lastParameter()` stay pure virtual, because only the concrete curve knows its range; the
 *   end points are derived from them here so every bounded curve answers them the same way.
 * - `startPoint()` / `endPoint()` are not virtual: they are one definition over `evaluate` and the range, and a
 *   concrete curve that could answer them more cheaply than by evaluating would be a different contract.
 *
 * @tparam T Floating-point scalar type.
 */
template <vn::math::FP T>
class BoundedCurve : public Curve<T> {
  public:
    using typename Curve<T>::Point;

  public:
    /**
     * @brief Returns the point at the first parameter.
     *
     * @return `evaluate(firstParameter())`.
     */
    [[nodiscard]] Point startPoint() const
    {
        return this->evaluate(this->firstParameter());
    }

    /**
     * @brief Returns the point at the last parameter.
     *
     * @return `evaluate(lastParameter())`.
     */
    [[nodiscard]] Point endPoint() const
    {
        return this->evaluate(this->lastParameter());
    }
};

VN_GEOM3D_NS_END
