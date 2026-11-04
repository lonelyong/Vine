// Explicit instantiation of every geometry class template at the precision the library ships.
//
// Why one central file instead of one line per header-owning .cpp:
// - A type whose members are all defined in its header has no .cpp of its own, so without this file the library would
//   export no symbol for it and every consumer would silently get a private instantiation of its own. Listing them
//   here makes the shipped set reviewable in one diff, and makes this translation unit a compile-time check that each
//   type is complete and instantiable.
// - `LineCurve`, `Obb` and `Plane` stay in their own .cpp instead: their out-of-line members
//   (`intersectWith`, `computePca`, `computeFixedOrientation`) are defined there, and an explicit instantiation can
//   only instantiate what it can see.
//
// No `extern template` declarations accompany these on purpose. The value types are short-query heavy and must stay
// inlinable at the call site - the picking loop's measured win is recorded in `geometry-primitives.md` 2.8 - so
// exporting the symbols is a fallback for callers that cannot inline, not a replacement for the header definitions.
//
// Both precisions are instantiated: `float` is not a fallback for the consumers that cannot afford `double`, it is a
// supported choice (real-time and GPU-side callers work in float, see `geometry-primitives.md` 2.8), so the library
// ships the same symbol set for both. Adding a new geometry type means adding two lines here.

#include <vine/geometry/Aabb.hpp>
#include <vine/geometry/BezierCurve.hpp>
#include <vine/geometry/BezierSurface.hpp>
#include <vine/geometry/BoundedCurve.hpp>
#include <vine/geometry/BoundedSurface.hpp>
#include <vine/geometry/BSplineCurve.hpp>
#include <vine/geometry/BSplineSurface.hpp>
#include <vine/geometry/CircleCurve.hpp>
#include <vine/geometry/ConicalSurface.hpp>
#include <vine/geometry/ConicCurve.hpp>
#include <vine/geometry/Curve.hpp>
#include <vine/geometry/CylindricalSurface.hpp>
#include <vine/geometry/ElementarySurface.hpp>
#include <vine/geometry/EllipseCurve.hpp>
#include <vine/geometry/HyperbolaCurve.hpp>
#include <vine/geometry/OffsetCurve.hpp>
#include <vine/geometry/ParabolaCurve.hpp>
#include <vine/geometry/Placement.hpp>
#include <vine/geometry/Ray.hpp>
#include <vine/geometry/RayHit.hpp>
#include <vine/geometry/RectangularTrimmedSurface.hpp>
#include <vine/geometry/Surface.hpp>
#include <vine/geometry/ToroidalSurface.hpp>
#include <vine/geometry/TrimmedCurve.hpp>

VN_GEOMETRY_NS_BEGIN

template class VN_GEOMETRY_API Aabb<float>;
template class VN_GEOMETRY_API Aabb<double>;
template class VN_GEOMETRY_API Ray<float>;
template class VN_GEOMETRY_API Ray<double>;
template class VN_GEOMETRY_API RayHit<float>;
template class VN_GEOMETRY_API RayHit<double>;
template class VN_GEOMETRY_API Placement<float>;
template class VN_GEOMETRY_API Placement<double>;

template class VN_GEOMETRY_API Curve<float>;
template class VN_GEOMETRY_API Curve<double>;
template class VN_GEOMETRY_API ConicCurve<float>;
template class VN_GEOMETRY_API ConicCurve<double>;
template class VN_GEOMETRY_API BoundedCurve<float>;
template class VN_GEOMETRY_API BoundedCurve<double>;
template class VN_GEOMETRY_API CircleCurve<float>;
template class VN_GEOMETRY_API CircleCurve<double>;
template class VN_GEOMETRY_API EllipseCurve<float>;
template class VN_GEOMETRY_API EllipseCurve<double>;
template class VN_GEOMETRY_API HyperbolaCurve<float>;
template class VN_GEOMETRY_API HyperbolaCurve<double>;
template class VN_GEOMETRY_API ParabolaCurve<float>;
template class VN_GEOMETRY_API ParabolaCurve<double>;

template class VN_GEOMETRY_API Surface<float>;
template class VN_GEOMETRY_API Surface<double>;
template class VN_GEOMETRY_API ElementarySurface<float>;
template class VN_GEOMETRY_API ElementarySurface<double>;
template class VN_GEOMETRY_API BoundedSurface<float>;
template class VN_GEOMETRY_API BoundedSurface<double>;
template class VN_GEOMETRY_API CylindricalSurface<float>;
template class VN_GEOMETRY_API CylindricalSurface<double>;
template class VN_GEOMETRY_API ConicalSurface<float>;
template class VN_GEOMETRY_API ConicalSurface<double>;
template class VN_GEOMETRY_API ToroidalSurface<float>;
template class VN_GEOMETRY_API ToroidalSurface<double>;

// These hold standard containers and handles, whose types have no dll-interface. That is expected: the members
// are read through the inline accessors in the header, and the export exists for the vtable and the out-of-line
// answers, not to let a consumer link against `std::vector`.
#if defined(_MSC_VER)
#    pragma warning(push)
#    pragma warning(disable : 4251)
#endif

template class VN_GEOMETRY_API RectangularTrimmedSurface<float>;
template class VN_GEOMETRY_API RectangularTrimmedSurface<double>;
template class VN_GEOMETRY_API BezierSurface<float>;
template class VN_GEOMETRY_API BezierSurface<double>;
template class VN_GEOMETRY_API BSplineSurface<float>;
template class VN_GEOMETRY_API BSplineSurface<double>;
template class VN_GEOMETRY_API TrimmedCurve<float>;
template class VN_GEOMETRY_API TrimmedCurve<double>;
template class VN_GEOMETRY_API OffsetCurve<float>;
template class VN_GEOMETRY_API OffsetCurve<double>;
template class VN_GEOMETRY_API BezierCurve<float>;
template class VN_GEOMETRY_API BezierCurve<double>;
template class VN_GEOMETRY_API BSplineCurve<float>;
template class VN_GEOMETRY_API BSplineCurve<double>;

#if defined(_MSC_VER)
#    pragma warning(pop)
#endif

VN_GEOMETRY_NS_END
