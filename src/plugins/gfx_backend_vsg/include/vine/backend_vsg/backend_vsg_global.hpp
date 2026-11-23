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
// The mechanisms both backends use live in libGraphics (`sdk/vine/graphics/`) and include no vsg header; this
// backend's files name them `core::X`, so `core` nominates the SDK namespace for lookup. What that nomination
// supplies, and what it costs, is written at the directive itself (see below).
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
//
// WHAT IT DOES AND DOES NOT DO (audit A3-06, corrected). A using-directive adds the nominated names to
// UNQUALIFIED lookup here, and to QUALIFIED lookup inside `core` (`core::DeviceClock` works,
// [namespace.qual]). It does NOT add `vn::graphics` to the associated namespaces of a `core::X` argument,
// so ADL is unaffected ([basic.lookup.argdep]) - the audit's "ADL also considers vn::graphics" is not what
// the standard says.
// The real cost is that a NEW SDK name becomes visible unqualified here without a line changing in this
// plugin. Measured 2026-11-18 (remove the directive, build `gfx_backend_vsg`, read the compiler's errors):
// the surface it supplies is 29 names - DrawKind / LoadOp / StoreOp / DynamicState /
// PipelineKey / PipelineKeyHash / RenderPassCompatibility / VertexLayoutKey / FrameToken /
// FrameTimeline / RetirementQueue / DeviceClock / DepthFacts / DepthPlan / TargetAction / TargetDecision /
// TargetDesc / TargetInstance / depthPlan / planTarget / ReadbackAnswer / ReadbackFormat / ReadbackKind /
// ReadbackRequest / colorReadbackOf / depthReadbackOf / decodeDepth / readbackOf / Diagnostics.
// (`ImageLayout` and `LoadOpVariantKey` left this list on 2026-11-23: they are this backend's own types now,
// in `internal/LoadOpVariantKey.hpp` - backlog T15, review 8 §4.7/§4.8.)
// Closing the set to exactly these names would need them at PARSE time: a using-DECLARATION requires its
// target to exist when it is written, so either five SDK headers would have to be included in this global
// header, or a vocabulary header would have to be included by all 36 `core` files. Accepted as-is: `core`
// defines no name that competes with an SDK one (the compiler would report the ambiguity), and every one of
// the 31 is an SDK concept `core` actually builds on. Trigger to revisit: `core` gains a name that collides
// with an SDK name, or the unqualified SDK surface grows beyond the list above.
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
