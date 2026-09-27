#pragma once

#include <vine/vine_global.hpp>

#ifdef VN_VSG_LIB
#    define VN_VSG_API VN_EXPORT
#else
#    define VN_VSG_API VN_IMPORT
#endif

// The backend half of graphics lives in libGraphics now (sdk/vine/graphics/backend/): it includes no
// vsg header, so every backend can share it instead of linking this plugin or copying the layer.
// This backend's files name its types `core::X` (or `vsg::core::X`), so the half is aliased into the
// backend's namespace here - one alias, and every existing spelling keeps resolving. A namespace
// alias cannot be extended, which is exactly the point: the half has ONE definition, in the library.
namespace VN_ROOT_NS
{
namespace graphics
{
namespace backend
{
}
}  // namespace graphics
namespace vsg
{
namespace core = ::VN_ROOT_NS::graphics::backend;
}  // namespace vsg
}  // namespace VN_ROOT_NS

#define VN_VSG_NS_BEGIN                                                                                                                                    \
    namespace VN_ROOT_NS                                                                                                                                   \
    {                                                                                                                                                     \
    namespace vsg                                                                                                                                         \
    {

#define VN_VSG_NS_END                                                                                                                                      \
    }                                                                                                                                                     \
    }
