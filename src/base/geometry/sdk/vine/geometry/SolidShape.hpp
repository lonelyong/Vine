#pragma once

#include "geometry_global.hpp"

#include <variant>

#include "BoxSolid.hpp"
#include "ConeSolid.hpp"
#include "CylinderSolid.hpp"
#include "EllipsoidSolid.hpp"
#include "SphereSolid.hpp"

VN_GEOMETRY_NS_BEGIN

/// The closed set of analytic solids, stored by value: what a model holds.
template <vn::math::FP T>
using SolidShape = std::variant<BoxSolid<T>, SphereSolid<T>, CylinderSolid<T>, ConeSolid<T>, EllipsoidSolid<T>>;

using SolidShapef = SolidShape<float>;
using SolidShaped = SolidShape<double>;

/**
 * @brief Returns the family view of a stored solid.
 *
 * @details
 * - The reference is how a caller asks a question without knowing the kind: `bounds()`, `closestPoint()` and
 *   `intersectWith()` are virtual, so no `switch` is involved.
 * - The reference stays valid as long as the variant does, and follows a replacement of the variant's contents.
 *
 * @param shape The stored solid.
 * @return The family interface of the object @p shape holds.
 */
template <vn::math::FP T>
[[nodiscard]] const Solid<T>& solidOf(const SolidShape<T>& shape)
{
    return std::visit([](const auto& kind) -> const Solid<T>& { return kind; }, shape);
}

/**
 * @brief Returns the mutable family view of a stored solid.
 *
 * @param shape The stored solid.
 * @return The family interface of the object @p shape holds.
 */
template <vn::math::FP T>
[[nodiscard]] Solid<T>& solidOf(SolidShape<T>& shape)
{
    return std::visit([](auto& kind) -> Solid<T>& { return kind; }, shape);
}


VN_GEOMETRY_NS_END
