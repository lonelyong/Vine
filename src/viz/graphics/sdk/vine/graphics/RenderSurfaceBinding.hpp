#pragma once
#include "graphics_global.hpp"

VN_GRAPHICS_NS_BEGIN

/**
 * @brief The WINDOW half of a backend: the surface its frames are presented through - and OPTIONAL.
 *
 * WHAT IT IS FOR. A backend's frame contract (see RenderBackend::executeFrame) says what a frame IS; this says
 * where the picture goes when the backend has a window at all. The two are separate because they change for
 * different reasons and at different times: a frame is a value the engine compiles and hands over, while the
 * surface is the swapchain, the platform window and its handle - re-announced when the platform recreates the
 * window, resized while the pipeline keeps drawing, presented once per frame.
 *
 * A host that never has a window - an offline baker, a CI run with no display, a backend that only draws into
 * render targets - implements NOTHING of this: its RenderBackend::surface() answers nullptr and the four calls
 * below are not empty shells it has to carry. That is the whole point of the split: as entries of the backend
 * contract itself they were exactly that (a headless backend had to stub out four methods it had nothing to say
 * about).
 *
 * WHO DRIVES IT. The AREA, on behalf of the host: RenderSurface::bindTarget() announces the window (and moves the
 * binding onto a recreated one), RenderSurface::resize() announces the size, and the engine presents the frame it
 * just ran through it at the frame's tail (see RenderEngine::frame). A host that drives a backend itself (a tool,
 * a test) calls the same entries directly.
 *
 * THREADING and lifetime follow the backend's own rules (see RenderBackend): one thread, serialised calls, and
 * the binding lives as long as the backend's session does.
 */
class VN_GRAPHICS_API RenderSurfaceBinding
{
  public:
    virtual ~RenderSurfaceBinding() = default;

    /** @brief Binds a host native window the backend presents through.
     *
     * Called before RenderBackend::initialize() when the host has a window of its own (a Qt QWindow / HWND, an
     * X11 window) - and again when the platform recreates that window, followed by another initialize(). The
     * binding is MOVED to the new handle rather than rebuilt, so the session (its device, its caches, every
     * compiled pipeline) survives a window recreation instead of being taken down and built again.
     *
     * @param native_handle Native window handle (HWND on Windows), or nullptr when the backend should not be
     *                      attached to one - it then brings up its own surface, which is also what it does when
     *                      no host window is ever announced.
     */
    virtual void setWindowHandle(void* native_handle) = 0;

    /** @brief Gets the native handle the binding is currently attached to.
     *
     * Answers "which surface am I on" for a host that has to notice the windowing system recreating that surface
     * underneath the backend: re-announcing through setWindowHandle() is how the binding is moved to the new
     * one, and this is how a host decides whether that is necessary.
     *
     * @return The attached native handle, or nullptr (the binding owns its own surface, or is not initialized).
     */
    [[nodiscard]] virtual void* nativeHandle() const = 0;

    /** @brief Announces a change of the surface size.
     *
     * The surface itself owns its size, so the authority order is surface > announcement > default: a binding
     * whose surface belongs to a window system (an embedded host window, a swapchain) FOLLOWS THAT SURFACE and
     * uses this call to re-derive what hangs off the size, whereas one that owns its surface (headless, or it
     * created the window itself) applies the announcement. The engine records the announced numbers either way:
     * its frame context is what the passes that lay themselves out on the surface size read (see
     * RenderEngine::resize).
     *
     * @param width  Announced surface width in pixels.
     * @param height Announced surface height in pixels.
     */
    virtual void resize(int width, int height) = 0;

    /** @brief Finishes the frame the backend last ran: it becomes real work, and its image is presented.
     *
     * The frame's own body - acquiring the next presentable image, collecting, compiling - happens inside
     * RenderBackend::executeFrame; this is the half that records what was collected into the frame's graph,
     * submits it, and hands the acquired image back to the display. It is the ONE call that presents, it is
     * called once per frame the backend ran, and the backend itself must not present from executeFrame.
     */
    virtual void present() = 0;
};

VN_GRAPHICS_NS_END
