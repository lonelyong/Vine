#pragma once
#include "graphics_global.hpp"

#include "PresentTarget.hpp"

VN_GRAPHICS_NS_BEGIN

/**
 * @brief A presenter that IS a platform window: PresentTarget plus the window's own two questions.
 *
 * @details
 * - WHY THE WINDOW FACE IS SEPARATE. Everything a FRAME needs is on PresentTarget (a size, a present); a native
 *   handle is not one of them. Who asks for it is a host that owns the window: it compares the handle it has with the
 *   one the target is attached to (so it knows whether a re-announcement is needed) and it re-announces when the
 *   platform RECREATES the window underneath it - which MOVES the binding instead of rebuilding it, so the device,
 *   its caches and every compiled pipeline survive a window recreation. A presenter that is not a window (an encoder,
 *   a network sink, a file) has none of that, and the frame never asks.
 * - WHO HANDS ONE OUT. The DEVICE (RenderDevice::createSurfaceTarget, from the host's native handle): one presenter
 *   per window, because the platform allows one live swapchain per window, so a device that serves several windows
 *   hands out one per window and a device that presents nothing hands out none (it answers "no presentation"
 *   instead). The AREA holds it (RenderSurface::bindTarget) and is what announces the host's window and size
 *   (RenderSurface::resize, WindowPresentTarget::setWindowHandle), and the executor presents the frame it just ran
 *   through the presenter that frame named (see RenderFrameExecutor::executeFrame).
 * - THREADING and lifetime follow the device's own rules (see RenderDevice): one thread, serialised calls, and a
 *   target outlives neither the window it is on nor the device that made it.
 */
class VN_GRAPHICS_API WindowPresentTarget : public PresentTarget {
  public:
    /** @brief Binds a host native window this target presents through.
     *
     * @details
     * - Called when the platform recreates the window a target is already on: the binding is MOVED to the new handle
     *   rather than rebuilt, so the device, its caches and every compiled pipeline survive a window recreation
     *   instead of being taken down and built again (see RenderDevice::createSurfaceTarget for the first window,
     *   which is a creation input rather than a move).
     *
     * @param native_handle Native window handle (HWND on Windows), or nullptr when the target's device should not be
     *                      attached to a host window - it then follows the window it created for itself, which is
     *                      also what it does when no host window is ever announced.
     */
    virtual void setWindowHandle(void* native_handle) = 0;

    /** @brief Gets the native handle the target is currently attached to.
     *
     * @details
     * - Answers "which surface am I on" for a host that has to notice the windowing system recreating that surface
     *   underneath the target: re-announcing through setWindowHandle() is how the target is moved to the new one, and
     *   this is how a host decides whether that is necessary.
     *
     * @return The attached native handle, or nullptr (the target follows its own surface, or is not initialized).
     */
    [[nodiscard]] virtual void* nativeHandle() const = 0;
};

VN_GRAPHICS_NS_END
