#pragma once

#include <vine/geom/geom_global.hpp>

#include <cstddef>
#include <type_traits>
#include <utility>
#include <vector>

#include <vine/math/Types.hpp>

VN_GEOM_DETAIL_NS_BEGIN

/**
 * @brief The type a difference of @p Value produces: `Point - Point` is a vector, everything else stays what it is.
 *
 * @tparam Value Element type of the array being differentiated.
 */
template <typename Value>
using DifferenceOf = std::decay_t<decltype(std::declval<const Value&>() - std::declval<const Value&>())>;

/**
 * @brief Returns the de Casteljau blend of @p values at parameter @p t, working on the copy it is given.
 *
 * @details
 * - The blend is `V[0] + (V[1] - V[0]) * t` applied level by level, which is why it works for points, for difference
 *   vectors and for homogeneous control points alike: all three answer `operator-` with their own type (points answer
 *   with a vector) and `operator+` with the blend.
 * - @p t is used as given, so a parameter outside `[0, 1]` is the polynomial's own extension rather than an error.
 * - The blend needs one scratch value per input, which is why it takes the array by value instead of keeping mutable
 *   state on the curve or patch.
 *
 * @param values Control values in order; a single value is returned unchanged.
 * @param t      Parameter to evaluate at.
 * @return The blended value; a default-constructed @p Value for an empty array.
 *
 * @tparam Value Element type: a point, a vector or a homogeneous control point.
 * @tparam T     Floating-point scalar type.
 */
template <typename Value, vn::math::FP T>
[[nodiscard]] Value deCasteljau(std::vector<Value> values, T t)
{
    for (std::size_t level = values.size(); level > 1; --level) {
        for (std::size_t index = 0; index + 1 < level; ++index) {
            values[index] = values[index] + (values[index + 1] - values[index]) * t;
        }
    }

    return values.empty() ? Value() : values.front();
}

/**
 * @brief Returns one level of the finite-difference triangle of @p values.
 *
 * @details
 * - `Point - Point` yields a vector, `Vector - Vector` a vector and a homogeneous control point its own kind, so this
 *   one function serves the Bezier curve's first and second differences and the patch's row and column differences.
 *
 * @param values Values in order.
 * @return One fewer value than the input; empty for fewer than two values.
 *
 * @tparam Value Element type.
 */
template <typename Value>
[[nodiscard]] std::vector<DifferenceOf<Value>> differences(const std::vector<Value>& values)
{
    std::vector<DifferenceOf<Value>> result;
    if (values.size() < 2) {
        return result;
    }

    result.reserve(values.size() - 1);
    for (std::size_t index = 0; index + 1 < values.size(); ++index) {
        result.push_back(values[index + 1] - values[index]);
    }

    return result;
}

VN_GEOM_DETAIL_NS_END
