#pragma once

/**
 * @brief A platform window that ADOPTS a host window and can move to another one.
 *
 * WHY IT EXISTS. The host hands the backend a native window it owns (see RenderBackend::setWindowHandle --
 * a Qt QWindow's handle) and replaces it when the windowing system recreates it. A window class that
 * insists on its own window cannot serve that: vsg's platform windows adopt a handle and then DESTROY it in
 * their destructor (`xcb_destroy_window` / `::DestroyWindow`), so a backend built on one would either
 * destroy the host's window or have to hand it back before letting it die.
 *
 * WHAT IT DERIVES FROM is that very platform window: vsgXcb::Xcb_Window on X11, vsgWin32::Win32_Window on
 * Win32. Everything a window IS comes from there -- its own window-system connection, the surface, the map
 * state vsg's frame path asks about (valid() / visible()), the geometry resize() and the event pump -- so
 * this class cannot forget a part of it, and the instance, the physical device, the device, the render
 * pass, the swapchain, the frames and the depth image stay vsg's own machinery, driven by its lazy
 * getOrCreate* accessors and built from the same traits as vsg's platform window would build them.
 *
 * WHAT IT CHANGES is exactly two things:
 *   - its destructor does NOT destroy the window it adopted: that window is the host's and outlives us.
 *     This is the one property vsg's platform window cannot express, and it is why this class exists.
 *   - @ref moveToHostSurface follows the host onto a NEW window: the surface is re-created on the SAME
 *     VkInstance and the swapchain is rebuilt against it, so the device, every compiled pipeline and the
 *     content stay.
 *
 * A session with no host window does NOT use this class: it keeps vsg's own window, which is also what the
 * VINE_VSG_OWN_WINDOW test hatch asks for.
 */

#include <vine/vsg/vsg_global.hpp>

#include <cstdint>

#include <vsg/app/Window.h>

#if defined(_WIN32)
#include <vsg/platform/win32/Win32_Window.h>
#elif defined(__APPLE__)
#error "VsgHostWindow has no window-system back end for this platform (the X11 one needs XCB, which the build links under UNIX AND NOT APPLE)."
#else
#include <vsg/platform/xcb/Xcb_Window.h>
#endif

V_VSG_NS_BEGIN

namespace detail
{

#if defined(_WIN32)
/** @brief The vsg platform window this class derives from, for the window system being built for. */
using VsgHostWindowBase = ::vsgWin32::Win32_Window;
/** @brief The native window handle that platform's `WindowTraits::nativeWindow` carries. */
using VsgHostHandle = HWND;
#else
/** @brief The vsg platform window this class derives from, for the window system being built for. */
using VsgHostWindowBase = ::vsgXcb::Xcb_Window;
/** @brief The native window handle that platform's `WindowTraits::nativeWindow` carries.
 *
 * It is the type vsg's platform window reads back out of the traits through `std::any`, which matches on
 * the EXACT type -- hence one name for it, used by every conversion between it and the `void*` this
 * backend passes around (see VsgRenderer::makeWindowTraits). It is also what lets the window class be
 * written ONCE: the two platforms differ in this type and in nothing else about this class.
 */
using VsgHostHandle = xcb_window_t;
#endif

/** @brief Narrows a native handle carried as `void*` back to the type the platform window expects.
 *
 * This `#if` is the ENTIRE platform difference left in converting a handle, and it exists because the handle
 * is an INTEGER on X11 (`xcb_window_t`) and a POINTER on Win32 (`HWND`): no single cast expresses both (a
 * `reinterpret_cast` straight to `xcb_window_t` is rejected for losing information, and `reinterpret_cast`
 * will not convert integer to integer). Keeping it here is what lets the window class body and the traits
 * builder be written once -- see VsgHostWindow.cpp, which has no platform branch at all.
 *
 * @param handle Handle as this backend passes it around (what `RenderBackend::setWindowHandle` is given).
 * @return The handle in the type the platform's vsg window reads out of the traits.
 */
[[nodiscard]] inline VsgHostHandle hostHandleFromVoid(void* handle) noexcept
{
#if defined(_WIN32)
    return reinterpret_cast<VsgHostHandle>(reinterpret_cast<std::uintptr_t>(handle));
#else
    // An X window id is 32 bits wide, so the value survives the narrowing.
    return static_cast<VsgHostHandle>(reinterpret_cast<std::uintptr_t>(handle));
#endif
}

/** @brief A vsg platform window that adopts the host's window and never destroys it. */
class VsgHostWindow : public ::vsg::Inherit<VsgHostWindowBase, VsgHostWindow>
{
  public:
    /** @brief Adopts the host window the traits name.
     *
     * The base class does the adopting -- that is what vsg's platform window does with the handle it is
     * handed; this class only refuses to destroy it again (see the destructor).
     *
     * @param traits Window traits whose `nativeWindow` holds the host's window handle, in the type the
     *               platform expects (an xcb_window_t / an HWND).
     */
    explicit VsgHostWindow(::vsg::ref_ptr<::vsg::WindowTraits> traits);

    /** @brief Moves the window onto the host surface @p native_handle names.
     *
     * This is what makes the host's "the windowing system recreated my window" case cheap: the surface is
     * re-created on the instance this window already has and the swapchain is rebuilt against it, so the
     * device, the render pass and every pipeline compiled against them survive.
     *
     * The caller must have made the device idle first (VsgRenderer::moveSessionToHostSurface does, through
     * the counted path), because the swapchain and the depth image being replaced may still be named by
     * work in flight.
     *
     * @param native_handle Handle of the host window to move to (null is refused).
     * @return true when the window is on @p native_handle afterwards -- it moved, or it was already there;
     *         false when this window cannot serve it (it never went as far as a device and merely follows
     *         the handle, or the new surface presents a different swapchain format, so the render pass --
     *         and with it every pipeline -- would have to be rebuilt), which the caller serves by starting
     *         a new session instead.
     */
    [[nodiscard]] bool moveToHostSurface(void* native_handle);

    /** @brief The host window this one is attached to.
     *
     * @return The handle of the adopted host window (never null once constructed).
     */
    [[nodiscard]] void* hostHandle() const noexcept;

  protected:
    /** @brief Releases this window without destroying the host's window.
     *
     * The base class' destructor destroys the window it holds -- and on Win32 also unregisters whatever
     * class it finds on it -- but the window here belongs to the host and outlives us, so the handle is
     * dropped before the base class lets go. That is the same thing the base class' own releaseWindow()
     * does; here it is this class' decision instead of a call every teardown path has to remember.
     */
    ~VsgHostWindow() override;
};

} // namespace detail

V_VSG_NS_END
