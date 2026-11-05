#pragma once

#include <vine/vn_global.hpp>

#include <vine/math/Continuity.hpp>

#ifdef VN_GEOM2D_LIB
#    define VN_GEOM2D_API VN_EXPORT
#else
#    define VN_GEOM2D_API VN_IMPORT
#endif

#define VN_GEOM2D_NS_BEGIN \
    VN_ROOT_NS_BEGIN        \
    namespace geom2d        \
    {

#define VN_GEOM2D_NS_END \
    VN_ROOT_NS_END        \
    }

VN_GEOM2D_NS_BEGIN
/**
 * @brief Re-exports the smoothness vocabulary, which is defined once in `math` and shared by every geometry module.
 *
 * @details
 * - `Continuity` is dimension-free - a planar curve answers the same ladder as a space curve - so its one definition
 *   lives in `math`, where every geometry module can reach it.
 * - Importing the name here lets this module's declarations write `Continuity` unqualified, and keeps
 *   `geom2d::Continuity` naming the same type as `math::Continuity`.
 */
using math::Continuity;
VN_GEOM2D_NS_END
