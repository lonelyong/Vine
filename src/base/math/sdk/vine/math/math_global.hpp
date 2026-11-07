#pragma once

#include <vine/vn_global.hpp>

#ifdef VN_MATH_LIB
#    define VN_MATH_API VN_EXPORT
#else
#    define VN_MATH_API VN_IMPORT
#endif

/**
 * @brief Opens the math namespace `vn::math`, which is the repository's base layer.
 *
 * @details
 * - Every module may depend on `math`, and `math` depends on nothing but `global` / `core`; nothing here may name a
 *   curve, a surface or a renderer.
 * - Two sub-namespaces live inside it and are opened by hand in their own headers rather than by a macro here:
 *   `numeric` holds numerical *processes* (the caller supplies the accuracy target and may be answered
 *   `std::nullopt`), `spline` holds the Bernstein evaluation formulas - a value sequence and a scalar, with no model of
 *   ours in the signature.
 * - Nothing geometric lives here, not even a representation convention: the B-spline knot model and the smoothness
 *   ladder sit in geometry, because their only consumers are curves and surfaces.
 * - Three headers do not earn three macros: this level offers one namespace, while `geom`'s global offers three because
 *   sixty-odd headers write them in alternating flavours.
 *
 * @note
 * - `symmetricEigen()` is the one process left in the root namespace: its sweep budget is internal, so no caller
 *   supplies an accuracy target - the split is "who sets the target", not "does it iterate".
 */
#define VN_MATH_NS_BEGIN                                                                                                                                       \
    VN_ROOT_NS_BEGIN                                                                                                                                           \
    namespace math                                                                                                                                             \
    {

#define VN_MATH_NS_END                                                                                                                                         \
    VN_ROOT_NS_END                                                                                                                                             \
    }
