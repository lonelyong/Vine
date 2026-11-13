#pragma once

#include <vine/vn_global.hpp>

#ifdef VN_BACKEND_VSG_LIB
#    define VN_BACKEND_VSG_API VN_EXPORT
#else
#    define VN_BACKEND_VSG_API VN_IMPORT
#endif

// WHAT THE NAME SAYS. `backend_vsg` has two parts and each part carries one fact:
//   * `backend` - this is an IMPLEMENTATION of the render seam (`vine/graphics/backend/` is the seam itself), not a
//     wrapper library around VulkanSceneGraph and not part of the host SDK;
//   * `vsg`     - it is the VulkanSceneGraph one: it drives `::vsg` (the vendored library's own namespace, spelled with
//     a leading `::` wherever this module names it) instead of creating Vulkan objects itself, which is what the
//     `vk` backend does.
// The namespace mirrors the include path one for one (`vine/backend_vsg/` -> `vn::backend_vsg`), so `vn::backend_vsg`
// and the library's `::vsg` never read as the same thing.
//
// The backend half of graphics lives in libGraphics (`sdk/vine/graphics/backend/`) and includes no vsg header; this
// backend's files name its types `core::X`, so the half is aliased into the backend's namespace here - one alias, and
// every existing spelling keeps resolving. A namespace alias cannot be extended, which is exactly the point: the half
// has ONE definition, in the library.
namespace VN_ROOT_NS
{
namespace graphics
{
namespace backend
{}

} // namespace graphics

namespace backend_vsg
{
namespace core = ::VN_ROOT_NS::graphics::backend;

} // namespace backend_vsg
} // namespace VN_ROOT_NS

#define VN_BACKEND_VSG_NS_BEGIN \
    namespace VN_ROOT_NS        \
    {                           \
    namespace backend_vsg       \
    {

#define VN_BACKEND_VSG_NS_END \
    }                         \
    }
