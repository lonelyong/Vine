#pragma once

#include "geometry_global.hpp"

VN_GEOMETRY_NS_BEGIN

/**
 * @brief Classifies a geometry as a one-dimensional locus: a point on it needs one parameter.
 *
 * @details
 * - This is a **classification, not an interface**: no virtual function, no state, no vtable. Empty-base
 *   optimisation makes a concrete curve the same size as its members, so a `Ray` copied per intersection query
 *   stays a plain value and its queries stay inlineable (`G10`).
 * - Concrete curves derive from it (`Line`, `Ray`, `LineSegment`, `Circle`), which is what makes
 *   `std::is_base_of_v<Curve, Rayd>` the answer to "is this a curve?" without a runtime tag.
 * - Runtime polymorphism is `std::variant` + `std::visit` over the concrete types, or a pointer to a concrete
 *   type. It is deliberately **not** inheritance from this class.
 * - The meaning of the parameter belongs to the concrete type (a `Ray` measures distance, a `Circle` measures an
 *   angle); it is documented there, not here.
 *
 * @warning
 * - Adding a virtual function to this class would put a vptr into every curve, including the ones copied inside
 *   hot loops. A shared algorithm goes in a free function constrained on the concrete types.
 */
struct Curve {
    /// Number of parameters a point on the geometry needs.
    static constexpr int kDimension = 1;
};

VN_GEOMETRY_NS_END
