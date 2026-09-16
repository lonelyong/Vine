#include <vine/vsg/VsgHostWindow.hpp>

#include <vsg/vk/Instance.h>
#include <vsg/vk/Surface.h>

#if defined(_WIN32)
#include <vulkan/vulkan_win32.h>
#else
#include <vulkan/vulkan_xcb.h>
#include <xcb/xcb.h>
#endif

#include <vine/logging/Log.hpp>

V_VSG_NS_BEGIN

namespace detail
{

#if defined(_WIN32)

void* VsgHostWindow::hostHandle() const noexcept
{
    return static_cast<void*>(_window);
}

VsgHostWindow::VsgHostWindow(::vsg::ref_ptr<::vsg::WindowTraits> traits) :
    Inherit(traits)
{
    // The base class adopted the handle the traits carry; a window that arrives without one is not
    // something this class can serve.
    if (_window == nullptr) {
        throw ::vsg::Exception{"VsgHostWindow needs a host window handle in its traits.", VK_ERROR_INVALID_EXTERNAL_HANDLE};
    }
    // Which surface the backend is on is a fact the host (and anyone reading a log) needs: it is the
    // difference between "rendering into the window you handed us" and "rendering into one of our own".
    // The map state is part of that fact: vsg's frame path SKIPS a window whose visible() is false, and
    // then no pass -- off-screen ones included -- is recorded at all, with no validation error to show
    // for it. The handle is part of it too, and it is the only way to tell which window to look at: a
    // reader that wants the pixels (scripts/xwin2ppm.py) has to name THIS window, because a Qt container
    // keeps the render area as a child and the parent's name matches several windows.
    V_LOGI("[VsgHostWindow] attached to the host window 0x{:x} ({}x{}, mapped={})", static_cast<std::uintptr_t>(_window),
           _extent2D.width, _extent2D.height, visible());
}

bool VsgHostWindow::moveToHostSurface(void* native_handle)
{
    if (native_handle == nullptr) {
        return false;
    }
    const HWND next = static_cast<HWND>(native_handle);
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
    if (_imageFormat.format != previous_format) {
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
    V_LOGI("[VsgHostWindow] moved to the host's new window 0x{:x} ({}x{}); the device and its pipelines were kept",
           static_cast<std::uintptr_t>(_window), _extent2D.width, _extent2D.height);
    return true;
}

VsgHostWindow::~VsgHostWindow()
{
    clear();

    // Drop the handle BEFORE the base class lets go: its destructor destroys the window it holds and
    // unregisters whatever class it finds on it, but this window is the HOST's and outlives us. The
    // drop is exactly what the base class' own releaseWindow() does.
    _window = {};
}

#else // X11

void* VsgHostWindow::hostHandle() const noexcept
{
    return reinterpret_cast<void*>(static_cast<std::uintptr_t>(_window));
}

VsgHostWindow::VsgHostWindow(::vsg::ref_ptr<::vsg::WindowTraits> traits) :
    Inherit(traits)
{
    // The base class adopted the handle the traits carry; a window that arrives without one is not
    // something this class can serve.
    if (_window == 0) {
        throw ::vsg::Exception{"VsgHostWindow needs a host window handle in its traits.", VK_ERROR_INVALID_EXTERNAL_HANDLE};
    }
    // Which surface the backend is on is a fact the host (and anyone reading a log) needs: it is the
    // difference between "rendering into the window you handed us" and "rendering into one of our own".
    // The map state is part of that fact: vsg's frame path SKIPS a window whose visible() is false, and
    // then no pass -- off-screen ones included -- is recorded at all, with no validation error to show
    // for it. The handle is part of it too, and it is the only way to tell which window to look at: a
    // reader that wants the pixels (scripts/xwin2ppm.py) has to name THIS window, because a Qt container
    // keeps the render area as a child and the parent's name matches several windows.
    V_LOGI("[VsgHostWindow] attached to the host window 0x{:x} ({}x{}, mapped={})", static_cast<std::uintptr_t>(_window),
           _extent2D.width, _extent2D.height, visible());
}

bool VsgHostWindow::moveToHostSurface(void* native_handle)
{
    if (native_handle == nullptr) {
        return false;
    }
    const auto next = static_cast<xcb_window_t>(reinterpret_cast<std::uintptr_t>(native_handle));
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
    if (_imageFormat.format != previous_format) {
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
    V_LOGI("[VsgHostWindow] moved to the host's new window 0x{:x} ({}x{}); the device and its pipelines were kept",
           static_cast<std::uintptr_t>(_window), _extent2D.width, _extent2D.height);
    return true;
}

VsgHostWindow::~VsgHostWindow()
{
    clear();

    // Drop the handle BEFORE the base class lets go: its destructor calls `xcb_destroy_window` on the
    // window it holds, but this window is the HOST's and outlives us. The drop is exactly what the base
    // class' own releaseWindow() does.
    _window = 0;
}

#endif

} // namespace detail

V_VSG_NS_END
