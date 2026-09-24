#include <vine/vsg/VsgHostWindow.hpp>

#include <cstdlib>

#include <vsg/vk/Instance.h>
#include <vsg/vk/Surface.h>

#include <vine/logging/Log.hpp>

VN_VSG_NS_BEGIN

namespace detail
{

void* VsgHostWindow::hostHandle() const noexcept
{
    return hostHandleToVoid(_window);
}

VsgHostWindow::VsgHostWindow(::vsg::ref_ptr<::vsg::WindowTraits> traits) :
    Inherit(traits)
{
    // The base class adopted the handle the traits carry; a window that arrives without one is not
    // something this class can serve.
    if (hostHandle() == nullptr) {
        throw ::vsg::Exception{"VsgHostWindow needs a host window handle in its traits.", VK_ERROR_INVALID_EXTERNAL_HANDLE};
    }

    // Whatever the base class registered ON the window it adopted is withdrawn before anything else runs: the
    // window is the host's, and this class MOVES off the handle it was constructed with (see
    // moveToHostSurface), so a registration left in place would sit on someone else's window and would no
    // longer be reachable from here. See withdrawHostWindowState().
    withdrawHostWindowState();

    // Which surface the backend is on is a fact the host (and anyone reading a log) needs: it is the
    // difference between "rendering into the window you handed us" and "rendering into one of our own".
    // The map state is part of that fact: vsg's frame path SKIPS a window whose visible() is false, and
    // then no pass -- off-screen ones included -- is recorded at all, with no validation error to show
    // for it. The handle is part of it too, and it is the only way to tell which window to look at: a
    // reader that wants the pixels (scripts/xwin2ppm.py) has to name THIS window, because a Qt container
    // keeps the render area as a child and the parent's name matches several windows.
    VN_LOGI("[VsgHostWindow] attached to the host window 0x{:x} ({}x{}, mapped={})", hostHandleValue(_window),
           _extent2D.width, _extent2D.height, visible());
}

bool VsgHostWindow::moveToHostSurface(void* native_handle)
{
    if (native_handle == nullptr) {
        return false;
    }
    const auto next = hostHandleFromVoid(native_handle);
    if (next == _window) {
        return true; // already attached to it: nothing to rebuild
    }
    if (_device == nullptr || _surface == nullptr) {
        // A window that never went as far as a device cannot be "moved": it simply follows the handle, and
        // the next initialize() builds everything on the new one.
        _window               = next;
        _traits->nativeWindow = next;
        return false;
    }

    const VkFormat previous_format = _imageFormat.format;

    _window               = next;
    _traits->nativeWindow = next;

    // The old surface named the old window; the device, the render pass and the pipelines stay (the caller
    // made the device idle, so nothing in flight still names the swapchain / depth image being replaced).
    // Dropping the swapchain here -- rather than at the first buildSwapchain() -- is deliberate: its
    // oldSwapchain argument is a hint for a swapchain of the SAME surface.
    //
    // WHICH members go, and which must NOT, is written down and checked by
    // scripts/check_vsg_window_surface_state.py against this very function: vsg's buildSwapchain()
    // APPENDS to `_frames`/`_indices` (uncleared, the old swapchain's framebuffers would stay in the frame
    // ring), ASSIGNS the depth and multisample images fresh (so resetting those releases them early rather
    // than saving anything), and DEREFERENCES its own `_renderPass` -- which is why that one is
    // deliberately absent below: nothing re-creates it before buildSwapchain(), so a null one segfaults in
    // Framebuffer's constructor (measured, not reasoned -- it is what the first version of this comment's
    // patch did).
    _swapchain.reset();
    _frames.clear();
    _indices.clear();
    _depthImageView.reset();
    _depthImage.reset();
    _multisampleImage.reset();
    _multisampleImageView.reset();
    _surface.reset();

    // The base class' own surface creation for this platform, on the SAME instance: that is what lets the
    // device and every pipeline compiled against it survive the move.
    _initSurface();
    _initFormats();
    // No window system hands out two windows whose visuals map to different swapchain formats on demand
    // (the format comes from the surface's supported list, and two visuals of the same display agree), so
    // the refusal below -- which costs the caller the instance, the device and every compiled pipeline --
    // could not be driven from a host's own calls. The hatch makes the COMPARISON fail instead, which is
    // the same code path the real condition takes: selftest_hostsurface.cpp sets it and asserts both the
    // report and the rebuild.
    const bool format_cannot_serve = std::getenv("VINE_HOST_MOVE_FORMAT_MISMATCH") != nullptr ||
                                     _imageFormat.format != previous_format;
    if (format_cannot_serve) {
        // A different presentation format means a different render pass, and every pipeline was compiled
        // against the one this session has: no move can serve that, so the caller starts a new session.
        return false;
    }

    // The base class' resize re-reads the host window's geometry and rebuilds the swapchain: exactly what
    // the new surface needs, with the size authority where it belongs -- the window.
    resize();
    _traits->width  = static_cast<int>(_extent2D.width);
    _traits->height = static_cast<int>(_extent2D.height);

    // The device, the render pass and every pipeline are still here: that is the point of the move, and the
    // log says so because it is otherwise invisible. The new handle goes in for the same reason the attach
    // line carries it: whoever reads the pixels has to follow the session to the window it moved to.
    VN_LOGI("[VsgHostWindow] moved to the host's new window 0x{:x} ({}x{}); the device and its pipelines were kept",
           hostHandleValue(_window), _extent2D.width, _extent2D.height);
    return true;
}

VsgHostWindow::~VsgHostWindow()
{
    clear();

    // Drop the handle BEFORE the base class lets go: the platform window's destructor destroys the window
    // it holds (X11: `xcb_destroy_window`; Win32: `DestroyWindow` and `UnregisterClass(GetClassName(...))`),
    // but this window is the HOST's and outlives us. The drop is exactly what the base class' own
    // releaseWindow() does; here it is this class' decision instead of a call every teardown path has to
    // remember.
    _window = {};
}

} // namespace detail

VN_VSG_NS_END
