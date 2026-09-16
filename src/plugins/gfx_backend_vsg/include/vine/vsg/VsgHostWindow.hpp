#pragma once

/**
 * @brief A vsg::Window that ADOPTS a host window and can move to another one.
 *
 * WHY IT EXISTS. The host can hand the backend a native window it owns (see
 * RenderBackend::setWindowHandle -- a Qt QWindow's handle) and can replace that window with a new one when
 * the windowing system recreates it. vsg's own platform window cannot serve that: it adopts the handle by
 * copying it into its private `_window` and then DESTROYS it in its destructor
 * (`xcb_destroy_window` / `::DestroyWindow`), so a backend built on it can neither let the host keep its
 * window nor follow it to the next one. It has to tear the whole session down (device, every compiled
 * pipeline, every target and cache) and start again -- which is why this backend used to raise vsg's
 * VSG_MAX_DEVICES and call releaseWindow() before letting its window die.
 *
 * WHAT IT REPLACES: the platform back end vsg's own window would install -- the surface creation, the map
 * state the frame path asks about, and the resize. The instance, the physical device, the device, the
 * render pass, the swapchain, the frames and the depth image are the base class' own protected machinery,
 * driven by its lazy getOrCreate* accessors, built by the same code and from the same traits as vsg's
 * platform window would build them -- which is what keeps the picture a session draws byte-for-byte the
 * same.
 *
 * WHAT IT ADDS is the lifetime the host needs: its OWN window system connection (the host's is none of
 * its business), a destructor that never destroys the window it adopted, and @ref moveToHostSurface,
 * which re-creates the surface on the SAME VkInstance and rebuilds the swapchain against it -- so a
 * session follows a recreated host window and keeps its device, its pipelines and its content.
 *
 * THE TWO QUESTIONS IT HAS TO ANSWER ITSELF are @ref valid and @ref visible: vsg's frame path SKIPS a
 * window whose `visible()` is false, and the base class answers false for a window no platform class
 * claimed -- so forgetting them makes the whole frame vanish (window black, off-screen targets unwritten)
 * with no validation error to show for it. They come from the adopted window's own state.
 *
 * A session with no host window does NOT use this class: it keeps vsg's own window (which is also what
 * the VINE_VSG_OWN_WINDOW test hatch asks for).
 */

#include <vine/vsg/vsg_global.hpp>

#include <atomic>
#include <cstdint>

#include <vsg/app/Window.h>

V_VSG_NS_BEGIN

namespace detail
{

class VsgHostWindow : public ::vsg::Inherit<::vsg::Window, VsgHostWindow>
{
  public:
    /** @brief Adopts the host window the traits name.
     *
     * @param traits Window traits whose `nativeWindow` holds the host's window handle, in the type the
     *               platform expects (an xcb_window_t / an HWND).
     */
    explicit VsgHostWindow(::vsg::ref_ptr<::vsg::WindowTraits> traits);

    /** @brief The window-system surface extension this window's surface comes from.
     *
     * The base class asks for this while it creates its instance, so it is what makes the instance able to
     * present on the host's window at all (see vsg::Window::_initInstance).
     *
     * @return The Vulkan instance extension name (never null).
     */
    const char* instanceExtensionSurfaceName() const override;

    /** @brief Moves the window onto the host surface @p native_handle names.
     *
     * This is what makes the host's "the windowing system recreated my window" case cheap: the surface is
     * re-created on the instance this window already has and the swapchain is rebuilt against it (the
     * path resize() already drives), so the device, the render pass and every pipeline compiled against
     * it survive.
     *
     * The caller must have made the device idle first (VsgRenderer::setWindowHandle does, through the
     * counted path), because the swapchain and the depth image being replaced may still be named by work
     * in flight.
     *
     * @param native_handle Handle of the host window to move to (null is refused).
     * @return true when the move happened; false when the new surface cannot serve this session (it
     *         presents a different swapchain format, so the render pass -- and with it every pipeline --
     *         would have to be rebuilt), which the caller serves by starting a new session instead.
     */
    [[nodiscard]] bool moveToHostSurface(void* native_handle);

    /** @brief The host window this one is attached to.
     *
     * @return The handle of the adopted host window (never null once constructed).
     */
    [[nodiscard]] void* hostHandle() const noexcept { return reinterpret_cast<void*>(host_window_); }

    /** @brief Whether the host window is there at all.
     *
     * The base class answers false for a window no platform class claimed, and vsg's frame path treats a
     * window that is not valid as one to skip (see visible()).
     *
     * @return true when this window holds a host handle and a live connection.
     */
    bool valid() const override;

    /** @brief Whether the host window is mapped and can be presented to.
     *
     * This is the question vsg's frame path really asks: `CommandGraph::record()` and the viewer's frame
     * loop both SKIP a window whose `visible()` is false, so answering false makes the whole frame -- every
     * pass, the off-screen ones included -- disappear without a single validation error to show for it.
     *
     * The answer is the ADOPTED window's map state. We see none of the host's map/unmap events, so it is
     * re-read while it reads as unmapped; the mapped case is the steady one and costs no round trip.
     *
     * @return true when the host window is mapped.
     */
    bool visible() const override;

  protected:
    void _initSurface() override;

    /** @brief Re-reads the host window's geometry and rebuilds the swapchain for it.
     *
     * The base class' contract for an embedded surface: the SIZE belongs to the host window, so the
     * backend follows it rather than announcing one (see RenderBackend::resize).
     */
    void resize() override;

    ~VsgHostWindow() override;

  private:
    /** @brief Re-reads the host window's map state, the one fact visible() cannot see by itself.
     *
     * An adopted window shows us none of the host's map/unmap events, so the state is re-read wherever we
     * are already talking to the server (attach, resize, move) and lazily again while it reads as unmapped.
     * The cached value is atomic: visible() runs on the frame path's thread while attach/resize/move come
     * from the host's.
     */
    void refreshHostWindowState() const;

    void*                     connection_  = nullptr;  ///< The connection this window opened (never the host's).
    std::uintptr_t            host_window_ = 0;        ///< The adopted host window (never created, never destroyed).
    mutable std::atomic<bool> window_mapped_{ false }; ///< Last seen state of that window's map.
};

} // namespace detail

V_VSG_NS_END
