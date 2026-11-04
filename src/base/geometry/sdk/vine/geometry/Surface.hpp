#pragma once

#include "geometry_global.hpp"

VN_GEOMETRY_NS_BEGIN

/**
 * @brief Classifies a geometry as a two-dimensional locus: a point on it needs two parameters.
 *
 * @details
 * - This is a **classification, not an interface**: no virtual function, no state, no vtable, and empty-base
 *   optimisation keeps the concrete type's size unchanged (`G10`), exactly as `Curve` does.
 * - Concrete surfaces derive from it (`Plane`, `Triangle`, `Quad`), so `std::is_base_of_v<Surface, Plane<T>>`
 *   answers "is this a surface?" at compile time.
 * - Runtime polymorphism is `std::variant` + `std::visit`, never inheritance from this class.
 * - The parameterisation belongs to the concrete type: three corners give barycentric `(u, v)`, four give the
 *   bilinear patch.
 *
 * @warning
 * - Adding a virtual function here would put a vptr into `Plane`, which other geometry holds by value and copies.
 */
struct Surface {
    /// Number of parameters a point on the geometry needs.
    static constexpr int kDimension = 2;
};

/**
 * @brief A surface whose parameterisation covers its whole analytic sheet: an elementary surface.
 *
 * @details
 * - Elementary surfaces are unbounded or semi-infinite in at least one parameter: a plane, a cylinder, a cone, a
 *   sphere, a torus. They are the analytic *support* of a face, not the face itself.
 * - The family exists as a classification only (no state, no virtual function); the concrete types decide their own
 *   parameterisation and answer their own queries.
 * - Currently `Plane` and `SphericalSurface` belong here; the other three are declared placeholders
 *   (`../backlog/geometry.md` G6).
 */
struct ElementarySurface : Surface {};

/**
 * @brief A surface with finite extent: a patch with its own corners, or an elementary sheet restricted to a rectangle.
 *
 * @details
 * - The parameter domain is bounded, so a bounded surface can always answer `bounds()`.
 * - Every member is currently a declared placeholder (`../backlog/geometry.md` G6): a single triangle or quad is not a
 *   type here — `intersectTriangle(ray, a, b, c, hit)` already takes the three corners it needs, and the names
 *   `TriangleSurface` / `QuadSurface` are reserved for the day a patch grows a query of its own.
 */
struct BoundedSurface : Surface {};

/**
 * @brief Placeholder for the unbounded cylindrical sheet: an axis and a radius.
 *
 * @details
 * - The type is fixed here so the taxonomy in code matches the plan (`geometry-kinds.md` 2.3); its parameters and
 *   queries are not — they arrive with a consumer (`../backlog/geometry.md` G6).
 * - The constructor is deleted on purpose: a cylindrical surface without a frame and a radius is not a value, and a
 *   placeholder that can be constructed would be a silently meaningless one.
 */
class CylindricalSurface : public ElementarySurface {
  public:
    CylindricalSurface() = delete;
};

/**
 * @brief Placeholder for the unbounded conical sheet: an axis, a semi-angle and a radius where the axis is crossed.
 *
 * @details
 * - Declared, not implemented: see `../backlog/geometry.md` G6 and `CylindricalSurface`.
 */
class ConicalSurface : public ElementarySurface {
  public:
    ConicalSurface() = delete;
};

/**
 * @brief Placeholder for the torus: an axis, a major radius and a minor radius.
 *
 * @details
 * - Declared, not implemented: see `../backlog/geometry.md` G6 and `CylindricalSurface`.
 */
class ToroidalSurface : public ElementarySurface {
  public:
    ToroidalSurface() = delete;
};

/**
 * @brief Placeholder for a Bézier patch: a rectangular control net and its degrees.
 *
 * @details
 * - Declared, not implemented: see `../backlog/geometry.md` G6 and `CylindricalSurface`.
 */
class BezierSurface : public BoundedSurface {
  public:
    BezierSurface() = delete;
};

/**
 * @brief Placeholder for a B-spline patch: a control net, knots and degrees.
 *
 * @details
 * - Declared, not implemented: see `../backlog/geometry.md` G6 and `CylindricalSurface`.
 * - The knot and control-point storage is exactly the data model this repository has not chosen yet; the name is
 *   fixed so the taxonomy does not have to be revisited when it is.
 */
class BSplineSurface : public BoundedSurface {
  public:
    BSplineSurface() = delete;
};

/**
 * @brief Placeholder for an elementary surface restricted to a parameter rectangle.
 *
 * @details
 * - This is the B-rep face's usual support: the analytic sheet plus the parameter range the face actually uses.
 *   Trimming by a wire is a separate concept and is not represented here.
 * - Declared, not implemented: see `../backlog/geometry.md` G6 and `CylindricalSurface`.
 */
class RectangularTrimmedSurface : public BoundedSurface {
  public:
    RectangularTrimmedSurface() = delete;
};

VN_GEOMETRY_NS_END
