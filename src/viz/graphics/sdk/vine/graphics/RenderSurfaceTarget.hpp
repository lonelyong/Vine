#pragma once
#include "graphics_global.hpp"

VN_GRAPHICS_NS_BEGIN

/**
 * @brief A presentable surface: the window half of a device, as an object of its own - and OPTIONAL.
 *
 * WHAT IT IS FOR. A device's frame execution (see RenderFrameExecutor::executeFrame) says what a frame IS; a target
 * says where the picture goes when there is a window at all. The two are separate because they change for different
 * reasons and at different times: a frame is a value the engine compiles and hands over, while a target is the
 * swapchain, the platform window and its handle - re-announced when the platform recreates the window, resized
 * while the pipeline keeps drawing, presented once per frame. One DEVICE may hand out one target per window (the
 * platform allows one live swapchain per window, see RenderDevice::createSurfaceTarget), so a target is shorter
 * lived than the device that made it and the caches the device holds outlive it.
 *
 * WHO MAKES ONE, WHO DRIVES ONE. The DEVICE makes it (RenderDevice::createSurfaceTarget, from the host's native
 * handle); the AREA holds it (RenderSurface::bindTarget) and is what announces the host's window and size
 * (RenderSurface::resize), and the executor presents the frame it just ran through the target that frame named
 * (see RenderFrameExecutor::executeFrame). A host that drives a device by itself (a tool, a test) calls the same
 * entries directly.
 *
 * A device that presents nothing hands out none: it answers "no presentation" instead (see
 * RenderDevice::createSurfaceTarget and Capabilities::presentation), and a frame it runs presents nowhere - which
 * is a first-class state (an offline bake, a display-less run), not an empty implementation of the four calls
 * below waiting to be written.
 *
 * THREADING and lifetime follow the device's own rules (see RenderDevice): one thread, serialised calls, and a
 * target outlives neither the window it is on nor the device that made it.
 */
class VN_GRAPHICS_API RenderSurfaceTarget
{
  public:
    virtual ~RenderSurfaceTarget() = default;

    /** @brief Binds a host native window this target presents through.
     *
     * Called when the platform recreates the window a target is already on: the binding is MOVED to the new
     * handle rather than rebuilt, so the device, its caches and every compiled pipeline survive a window
     * recreation instead of being taken down and built again (see RenderDevice::createSurfaceTarget for the
     * first window, which is a creation input rather than a move).
     *
     * @param native_handle Native window handle (HWND on Windows), or nullptr when the target's device should
     *                      not be attached to a host window - it then follows the window it created for itself,
     *                      which is also what it does when no host window is ever announced.
     */
    virtual void setWindowHandle(void* native_handle) = 0;

    /** @brief Gets the native handle the target is currently attached to.
     *
     * Answers "which surface am I on" for a host that has to notice the windowing system recreating that surface
     * underneath the target: re-announcing through setWindowHandle() is how the target is moved to the new one,
     * and this is how a host decides whether that is necessary.
     *
     * @return The attached native handle, or nullptr (the target follows its own surface, or is not initialized).
     */
    [[nodiscard]] virtual void* nativeHandle() const = 0;

    /** @brief Announces a change of the surface size.
     *
     * The surface itself owns its size, so the authority order is surface > announcement > default: a target
     * whose surface belongs to a window system (an embedded host window, a swapchain) FOLLOWS THAT SURFACE and
     * uses this call to re-derive what hangs off the size, whereas one that owns its surface (headless, or it
     * created the window itself) applies the announcement. The engine records the announced numbers either way:
     * its frame context is what the passes that lay themselves out on the surface size read (see
     * RenderEngine::resize, which is the announcement's way in).
     *
     * @param width  Announced surface width in pixels.
     * @param height Announced surface height in pixels.
     */
    virtual void resize(int width, int height) = 0;

    /** @brief Finishes the frame the executor last ran: it becomes real work, and its image is presented.
     *
     * The frame's own body - acquiring the next presentable image, collecting, compiling - happens inside
     * RenderFrameExecutor::executeFrame; this is the half that records what was collected into the frame's graph,
     * submits it, and hands the acquired image back to the display. It is the ONE call that presents, it is
     * called once per frame the executor ran, and the executor itself must not present from executeFrame.
     */
    virtual void present() = 0;
};

VN_GRAPHICS_NS_END
