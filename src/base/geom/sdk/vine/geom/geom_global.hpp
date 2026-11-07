#pragma once

#include <vine/vn_global.hpp>

#ifdef VN_GEOM_LIB
#    define VN_GEOM_API VN_EXPORT
#else
#    define VN_GEOM_API VN_IMPORT
#endif

/**
 * @brief Opens the geometry module namespace: `vn::geom`.
 *
 * @details
 * - `vn::geom` is the module root. It holds the smoothness vocabulary (`Continuity`), the shared implementation subtree
 *   (`detail`) and the two dimension subtrees (`planar`, `spatial`); no curve or surface type lives here.
 * - `planar` and `spatial` are **two contracts in one module**: same library, same compile definition, two namespaces. A
 *   planar curve and a space curve answer the same eleven facts, but with `Point2`/`Vector2` and `Point3`/`Vector3`;
 *   neither is a specialisation of the other (`../decisions/ADR-2026-11-05-planar-curve-contract.md`).
 * - The two subtrees must not include each other, and `detail` must not include either. That direction used to be
 *   enforced by two separate CMake targets; it is now enforced by `scripts/check_geom_layering.py`, because one target
 *   cannot express it.
 * - Names declared here are visible **unqualified** inside both subtrees, which is why `Continuity` and `detail` need no
 *   re-export: enclosing-namespace lookup finds them.
 */
#define VN_GEOM_NS_BEGIN \
    VN_ROOT_NS_BEGIN     \
    namespace geom       \
    {

#define VN_GEOM_NS_END \
    VN_ROOT_NS_END     \
    }

/**
 * @brief Opens the planar contract: `vn::geom::planar`.
 *
 * @details
 * - Planar curves only: `Curve<T>`, the conic layer, the nine concrete types and the family algorithms. There are no
 *   surfaces or solids here - a plane has no room for them.
 */
#define VN_GEOM_PLANAR_NS_BEGIN \
    VN_GEOM_NS_BEGIN            \
    namespace planar            \
    {

#define VN_GEOM_PLANAR_NS_END \
    VN_GEOM_NS_END            \
    }

/**
 * @brief Opens the space contract: `vn::geom::spatial`.
 *
 * @details
 * - Curves, surfaces, solids, rays and boxes of the ambient space. It is not the planar tree "one dimension up": it
 *   carries types a plane cannot have, and the two contracts share no base class.
 */
#define VN_GEOM_SPATIAL_NS_BEGIN \
    VN_GEOM_NS_BEGIN             \
    namespace spatial            \
    {

#define VN_GEOM_SPATIAL_NS_END \
    VN_GEOM_NS_END             \
    }

/**
 * @brief Opens the shared implementation namespace: `vn::geom::detail`.
 *
 * @details
 * - Everything here is shared between the two contracts and is **not** public contract: it exists so a helper that is
 *   identical in both dimensions has one copy (`../decisions/ADR-2026-11-06-geom-detail-subtree.md`).
 * - The public entries stay in the two contract namespaces: `planar::arcLength` and `spatial::arcLength` are the
 *   documented names, and each one resolves its own window and delegates here.
 */
#define VN_GEOM_DETAIL_NS_BEGIN \
    VN_GEOM_NS_BEGIN            \
    namespace detail            \
    {

#define VN_GEOM_DETAIL_NS_END \
    VN_GEOM_NS_END            \
    }
