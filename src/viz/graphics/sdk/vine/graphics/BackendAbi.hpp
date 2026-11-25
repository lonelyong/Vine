#pragma once
#include "graphics_global.hpp"

#include <cstddef>
#include <cstdint>
#include <type_traits>

/**
 * @brief ABI revision of the surface a render backend module compiles against.
 *
 * @details
 * - WHAT IT GOVERNS. A render backend is compiled as its own module (today an appfw plugin; a host may load one any
 *   way it likes) and implements virtual interfaces this SDK declares: `RenderBackendFactory`, `RenderBackend`,
 *   `RenderDevice` and `RenderFrameExecutor`.
 *   Those interfaces carry no explicit ABI, so their shape IS this number, and `1` is the shape these headers
 *   describe.
 * - BUMP IT WHENEVER A MODULE'S COMPILED VIEW OF THE SDK STOPS BEING VALID:
 *   - a virtual is ADDED, REMOVED, REORDERED or re-signed in any class a backend derives from.
 *     An added virtual is not additive: a class compiled earlier has a shorter vtable, so the host's call reads past
 *     its end;
 *   - a struct, enum or class that crosses the boundary changes layout (a field added, removed or reordered, a base
 *     class added, an enum's underlying type changed);
 *   - a function a backend calls or overrides changes signature.
 * - DO NOT BUMP IT FOR a new header, a new class, a new free function, a non-virtual member added to an SDK class, or
 *   a comment.
 *   None of those changes what a module already compiled against, and a revision that moves without cause is a
 *   recompile nobody asked for.
 * - A REVISION IS NOT A RELEASE. It says nothing about the SDK's version (that is `VN_GRAPHICS_VERSION`, diagnostic
 *   only); it says whether a module built against this SDK can be used by this host.
 * - A NEW VIRTUAL therefore costs TWO things: this number, and a default implementation that keeps the SOURCE-level
 *   cost of a recompile at zero ("not answering keeps the default").
 *   The default says what a backend that does not override gets; it never says an OLD binary keeps working - that is
 *   what the bump prevents.
 * - WHERE IT IS CHECKED: `RenderBackendRegistry::registerFactory` compares the module's revision before it calls into
 *   the factory at all, so a stale module becomes one diagnostic instead of a crash (see RenderBackendAbi below).
 *
 * 1u - the first revision, introduced with this header: the virtual sets above and the value types they exchange are
 *      frozen here.
 *      A module built before this header states no revision at all and is refused as such, which is the one
 *      compatibility event this batch carries (every in-tree backend is rebuilt with it).
 */
#define VN_RENDER_BACKEND_ABI_VERSION 1u

VN_GRAPHICS_NS_BEGIN

/**
 * @brief What a backend module states about the graphics SDK it was compiled against.
 *
 * @details
 * - THE HANDSHAKE IS A VALUE, NOT A VIRTUAL. A module reports it through `vineRenderBackendAbi()` (see
 *   VN_DECLARE_RENDER_BACKEND_ABI) and hands the same pointer to `registerFactory`.
 *   Asking a factory object for anything - a virtual, a field of `RenderBackendInfo` - is the call this check exists
 *   to avoid, because a module built against another revision has a different object layout AND a different vtable.
 * - THE LAYOUT IS PART OF THE HANDSHAKE: `abi_version` stays the first member, members are only appended, and a
 *   member is a plain integer or a `const char*` (never an SDK type whose own layout can change).
 *   The reader reads no other member until `abi_version` matches, so a newer SDK's extra members never reach an older
 *   host.
 * - The pointer must stay valid for the module's lifetime: it points at that module's own static (see
 *   VN_DECLARE_RENDER_BACKEND_ABI), never at a copy made by the host.
 */
struct VN_GRAPHICS_API RenderBackendAbi {
    std::uint32_t abi_version{ 0 };       ///< VN_RENDER_BACKEND_ABI_VERSION the module was compiled with.
    const char*   sdk_version{ nullptr }; ///< VN_GRAPHICS_VERSION it was built against; never null, UTF-8.
};

static_assert(std::is_standard_layout_v<RenderBackendAbi>, "RenderBackendAbi is read across a module boundary and must stay standard layout");
static_assert(offsetof(RenderBackendAbi, abi_version) == 0, "abi_version must stay the first member: the reader reads it before the layout is trusted");

/**
 * @brief Answers whether this host may use a backend module that reported @p abi.
 *
 * @details
 * - ONE RULE: the revisions are EQUAL.
 *   A newer or an older revision is refused the same way, because the question ("does this module's compiled view of
 *   the SDK match mine?") has no "close enough" answer.
 * - A null pointer (no handshake at all) is incompatible too: a module built before this rule existed cannot say what
 *   it expects, and guessing is exactly what the rule replaces.
 * - This is the host's half of the rule, and it is public so a host that loads backend modules itself (its own
 *   `dlopen`, a package manager, a test) asks the same question the registry asks instead of inventing a second one.
 *
 * @param abi What a module reported through `vineRenderBackendAbi()`, or null when it reported nothing.
 * @return true when the module may be used; false when it must be rebuilt against this SDK.
 */
[[nodiscard]] inline bool isRenderBackendAbiCompatible(const RenderBackendAbi* abi) noexcept
{
    return abi != nullptr && abi->abi_version == VN_RENDER_BACKEND_ABI_VERSION;
}

VN_GRAPHICS_NS_END

/**
 * @brief Export visibility for a backend module's handshake entry point.
 *
 * @details
 * - A backend module exports one C entry point whatever it was built with (`vineRenderBackendAbi`), so a loader can
 *   ask "which SDK were you built against?" before it calls anything the module declares.
 *   That is the MODULE's own export (a plugin's, not the library's), which is why it is not `VN_GRAPHICS_API`.
 */
#if defined(_WIN32) || defined(_WIN64)
#    define VN_RENDER_BACKEND_EXPORT __declspec(dllexport)
#else
#    define VN_RENDER_BACKEND_EXPORT __attribute__((visibility("default")))
#endif

extern "C"
{
    /**
     * @brief ABI handshake: what the backend module was compiled against.
     *
     * @details
     * - THE ONE ENTRY POINT A LOADER MAY CALL BEFORE IT TRUSTS THE MODULE: a C signature with no C++ type in the call,
     *   so it resolves and answers even when the module's C++ headers are older than the host's.
     *   Everything else the module declares is read only after isRenderBackendAbiCompatible() accepted the answer.
     * - Defined exactly once per backend module by VN_DECLARE_RENDER_BACKEND_ABI(); a module that does not define it
     *   is refused as "built before this handshake existed" rather than called into.
     *
     * @return The module's build description; never null.
     */
    VN_RENDER_BACKEND_EXPORT const vn::graphics::RenderBackendAbi* vineRenderBackendAbi();
}

/**
 * @brief Defines a backend module's ABI handshake.
 *
 * @details
 * - Use it EXACTLY ONCE per backend module, next to the registration that hands the factory to
 *   `RenderBackendRegistry::registerFactory`.
 *   One definition per module is what makes the answer the module's own compile-time value, rather than whatever a
 *   shared inline definition happened to resolve to at load time.
 * - The returned value is a static of the module, so it outlives every use and a loader may keep the pointer.
 * - A module that registers a backend and does not define this is refused with `engine.backend.abi_missing`, which is
 *   the answer a library built against an older SDK gets - not a crash.
 *
 * Usage:
 * @code
 * VN_DECLARE_RENDER_BACKEND_ABI()
 * @endcode
 */
#define VN_DECLARE_RENDER_BACKEND_ABI()                                                                                                                        \
    extern "C" VN_RENDER_BACKEND_EXPORT const vn::graphics::RenderBackendAbi* vineRenderBackendAbi()                                                           \
    {                                                                                                                                                          \
        static const vn::graphics::RenderBackendAbi s_abi{ VN_RENDER_BACKEND_ABI_VERSION, VN_GRAPHICS_VERSION };                                               \
        return &s_abi;                                                                                                                                         \
    }
