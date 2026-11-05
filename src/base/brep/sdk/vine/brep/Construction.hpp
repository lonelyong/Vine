#pragma once

#include "brep_global.hpp"

#include <vine/models/BrepShape.hpp>
#include <vine/models/Primitive.hpp>
#include <vine/intrusive_ptr.hpp>

#include "Backend.hpp"

VN_BREP_NS_BEGIN

/**
 * @brief Builds a closed B-rep solid from a parametric primitive.
 *
 * @details
 * - The solid occupies the space the primitive describes in its own local space: a box, sphere or ellipsoid is
 *   centered at the origin, a cylinder and a cone run from -height/2 to +height/2 along +Z, with the cone's base at
 *   -height/2 and its apex at +height/2.
 * - Nothing is placed, scaled or converted between units: the dimensions are model units, widened from float to
 *   double, and the caller applies its own transform to the result.
 * - The result carries the primitive's name and starts at revision 0.
 * - Two calls on the same primitive return two independent solids; nothing is cached or shared between them.
 *
 * @warning
 * - The ellipsoid is the only approximate one: the backend has no tri-axial ellipsoid primitive, so a unit sphere
 *   is scaled non-uniformly and its face becomes a B-spline. Its bounds and volume match the analytic values only
 *   within the transform tolerance; the other four primitives build exact solids.
 * - BrepShape::hasVolume() answers false for the result, because closure is never queried here: a primitive that
 *   answered true reports false once it has become a BrepShape.
 *
 * @note
 * - The backend body is compiled only when the backend is found (`VN_BREP_HAS_OCCT`); a build without it returns
 *   null and reports false from hasBackend(). That branch still needs a build with the backend to be verified (see
 *   `.ai/backlog/brep.md`).
 *
 * @param primitive The primitive to build; it must be valid, that is, every dimension positive.
 * @return The solid, or null when this build has no backend, the primitive is invalid, or its type is not one of
 *         the five the geometry module defines. Ask hasBackend() to tell a build-level cause from an input-level
 *         one.
 */
[[nodiscard]] VN_BREP_API vn::intrusive_ptr<vn::models::BrepShape> makeBrep(const vn::models::Primitive& primitive);

VN_BREP_NS_END
