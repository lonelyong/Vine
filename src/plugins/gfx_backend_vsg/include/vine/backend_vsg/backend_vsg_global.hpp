#pragma once

#include <vine/vn_global.hpp>

#ifdef VN_BACKEND_VSG_LIB
#    define VN_BACKEND_VSG_API VN_EXPORT
#else
#    define VN_BACKEND_VSG_API VN_IMPORT
#endif

// WHAT THE NAME SAYS. `backend_vsg` has two parts and each part carries one fact:
//   * `backend` - this is an IMPLEMENTATION of the render seam (`vine/graphics/` is the seam itself), not a
//     wrapper library around VulkanSceneGraph and not part of the host SDK;
//   * `vsg`     - it is the VulkanSceneGraph one: it drives `::vsg` (the vendored library's own namespace, spelled with
//     a leading `::` wherever this module names it) instead of creating Vulkan objects itself, which is what the
//     `vk` backend does.
// The namespace mirrors the include path one for one (`vine/backend_vsg/` -> `vn::backend_vsg`), so `vn::backend_vsg`
// and the library's `::vsg` never read as the same thing.
//
// The mechanisms both backends use live in libGraphics (`sdk/vine/graphics/`) and include no vsg header;
// this backend's files name them `core::X`, so a namespace alias points at them - one alias, and every
// existing spelling keeps resolving. A namespace alias cannot be extended, which is exactly the point:
// the mechanisms have ONE definition, in the library.
namespace VN_ROOT_NS
{
namespace graphics
{} // Declared so the using-directive below names an existing namespace without pulling a graphics header in here.

namespace backend_vsg
{
// THIS BACKEND'S OWN LAYER. `core` is where this plugin's private types live (Observe, VariantPool, the
// frame tables, the arenas), and the using-directive is what lets `core::DeviceClock` name the SDK's
// shared vocabulary while `core::Observe` resolves in here. The SDK's namespace stays free of one
// particular backend's internals.
namespace core
{
using namespace ::VN_ROOT_NS::graphics;
} // namespace core
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

// A file whose types belong to THIS BACKEND's own layer opens `core` (inside `vn::backend_vsg`) instead of
// the module namespace alone: the type's home is the backend that owns it, and `core::` still reaches
// the SDK's shared vocabulary through the using-directive above.
#define VN_BACKEND_VSG_CORE_NS_BEGIN \
    VN_BACKEND_VSG_NS_BEGIN          \
    namespace core                   \
    {

#define VN_BACKEND_VSG_CORE_NS_END \
    }                              \
    VN_BACKEND_VSG_NS_END
