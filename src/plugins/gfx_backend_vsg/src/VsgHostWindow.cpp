#include <vine/vsg/VsgHostWindow.hpp>

#include <any>
#include <cstdint>
#include <utility>

#include <vsg/vk/Instance.h>
#include <vsg/vk/Surface.h>

#if !defined(_WIN32)
#include <vsg/platform/xcb/Xcb_Window.h> // vsgXcb::Xcb_Surface: the surface creation vsg itself uses
#include <vulkan/vulkan_xcb.h>
#include <xcb/xcb.h>
#else
#include <vulkan/vulkan_win32.h>
#endif

#include <vine/logging/Log.hpp>

V_VSG_NS_BEGIN

namespace detail
{

#if !defined(_WIN32)

namespace
{
/** @brief The host window handle the traits carry, as the X server names it.
 *
 * The announcing side stores it as the platform's id type (see makeWindowTraits): an xcb_window_t is a
 * uint32_t, so that is what the traits hold, and a handle of any other type in there is not one.
 *
 * @param traits Traits the window was created from.
 * @return The window id, or 0 when the traits name no host window.
 */
xcb_window_t hostWindowFromTraits(const ::vsg::WindowTraits& traits)
{
    if (!traits.nativeWindow.has_value()) {
        return 0;
    }
    return static_cast<xcb_window_t>(std::any_cast<unsigned int>(traits.nativeWindow));
}

/** @brief The host window's current geometry, or false when the server could not answer.
 *
 * @param connection Connection to ask on.
 * @param window     Window to ask about.
 * @param width      Receives the width in pixels.
 * @param height     Receives the height in pixels.
 * @return true when the server answered.
 */
bool hostWindowExtent(xcb_connection_t* connection, xcb_window_t window, std::uint32_t& width, std::uint32_t& height)
{
    xcb_get_geometry_reply_t* geometry = xcb_get_geometry_reply(connection, xcb_get_geometry(connection, window), nullptr);
    if (geometry == nullptr) {
        return false;
    }
    width  = geometry->width;
    height = geometry->height;
    free(geometry);
    return true;
}
} // namespace

const char* VsgHostWindow::instanceExtensionSurfaceName() const
{
    return VK_KHR_XCB_SURFACE_EXTENSION_NAME;
}

bool VsgHostWindow::valid() const
{
    return host_window_ != 0 && connection_ != nullptr;
}

bool VsgHostWindow::visible() const
{
    if (!valid()) {
        return false;
    }
    // An adopted window shows us none of the host's map/unmap events, so a window that is not known to be
    // mapped is asked about again; the mapped case is the steady one and costs nothing. Answering false
    // here is not cosmetic: the frame path asks this before recording, so a false would make the WHOLE
    // frame -- every pass, off-screen ones included -- disappear without one validation error to show for
    // it (the base class answers false for a window no platform class claimed).
    if (!window_mapped_) {
        refreshHostWindowState();
    }
    return window_mapped_;
}

void VsgHostWindow::refreshHostWindowState() const
{
    auto* connection = static_cast<xcb_connection_t*>(connection_);
    if (connection == nullptr || host_window_ == 0) {
        window_mapped_ = false;
        return;
    }
    xcb_get_window_attributes_reply_t* attributes = xcb_get_window_attributes_reply(
        connection, xcb_get_window_attributes(connection, static_cast<xcb_window_t>(host_window_)), nullptr);
    if (attributes == nullptr) {
        window_mapped_ = false;
        return;
    }
    window_mapped_ = attributes->map_state == XCB_MAP_STATE_VIEWABLE;
    free(attributes);
}

VsgHostWindow::VsgHostWindow(::vsg::ref_ptr<::vsg::WindowTraits> traits) :
    Inherit(traits)
{
    // Our OWN connection: the host's belongs to the host, and a window id is global to the server, so the
    // surface is created from ours (the same choice vsg's platform window makes for its own windows).
    int       screen_num = 0;
    xcb_connection_t* connection =
        xcb_connect(traits->display.empty() ? nullptr : traits->display.c_str(), &screen_num);
    if (connection == nullptr || xcb_connection_has_error(connection) != 0) {
        if (connection != nullptr) {
            xcb_disconnect(connection);
        }
        throw ::vsg::Exception{"VsgHostWindow failed to open a window system connection.", VK_ERROR_INVALID_EXTERNAL_HANDLE};
    }
    connection_  = connection;
    host_window_ = hostWindowFromTraits(*traits);
    if (host_window_ == 0) {
        throw ::vsg::Exception{"VsgHostWindow needs a host window handle in its traits.", VK_ERROR_INVALID_EXTERNAL_HANDLE};
    }
    // The host window owns the size (see resize()), but the swapchain built on the first frame need one.
    std::uint32_t width  = 0;
    std::uint32_t height = 0;
    if (hostWindowExtent(connection, static_cast<xcb_window_t>(host_window_), width, height)) {
        _extent2D.width  = width;
        _extent2D.height = height;
        traits->width    = static_cast<int>(width);
        traits->height   = static_cast<int>(height);
    }
    // Which surface the backend is on is a fact the host (and anyone reading a log) needs: it is the
    // difference between "rendering into the window you handed us" and "rendering into one of our own".
    // The map state is part of that fact: a host window that is not on screen yet answers visible() false,
    // and vsg's frame path then records nothing at all (see valid() / visible()).
    refreshHostWindowState();
    V_LOGI("[VsgHostWindow] attached to the host window ({}x{}, mapped={})", _extent2D.width, _extent2D.height,
           window_mapped_);
}

void VsgHostWindow::_initSurface()
{
    if (!_instance) {
        _initInstance();
    }
    // vsg's own surface class for this platform, so the surface carries exactly what vsg's window would
    // have created -- on OUR instance, which is what lets a move keep the device.
    _surface = new vsgXcb::Xcb_Surface(_instance, static_cast<xcb_connection_t*>(connection_),
                                       static_cast<xcb_window_t>(host_window_));
}

bool VsgHostWindow::moveToHostSurface(void* native_handle)
{
    if (native_handle == nullptr) {
        return false;
    }
    const auto next = static_cast<xcb_window_t>(reinterpret_cast<std::uintptr_t>(native_handle));
    if (next == host_window_) {
        return true; // already attached to it: nothing to rebuild
    }
    if (_device == nullptr || _surface == nullptr) {
        // A window that never went as far as a device cannot be "moved": the next initialize() picks the
        // handle up from the traits instead (the caller stores it there).
        host_window_    = next;
        _traits->nativeWindow = static_cast<unsigned int>(next);
        return false;
    }

    const VkFormat previous_format = _imageFormat.format;

    host_window_ = next;

    // The old surface named the old window; the device, the render pass and the pipelines stay (the
    // caller made the device idle, so nothing in flight still names the swapchain / depth image being
    // replaced). Dropping the swapchain here -- rather than at the first buildSwapchain() -- is
    // deliberate: its oldSwapchain argument is a hint for a swapchain of the SAME surface.
    _swapchain.reset();
    _frames.clear();
    _indices.clear();
    _depthImageView.reset();
    _depthImage.reset();
    _multisampleImage.reset();
    _multisampleImageView.reset();
    _surface.reset();

    _initSurface();
    _initFormats();
    if (_imageFormat.format != previous_format) {
        // A different presentation format means a different render pass, and every pipeline was compiled
        // against the one this session has: no move can serve that, so the caller starts a new session.
        return false;
    }

    buildSwapchain();

    std::uint32_t width  = 0;
    std::uint32_t height = 0;
    if (hostWindowExtent(static_cast<xcb_connection_t*>(connection_), static_cast<xcb_window_t>(host_window_), width, height)) {
        _traits->width  = static_cast<int>(width);
        _traits->height = static_cast<int>(height);
    }
    _traits->nativeWindow = static_cast<unsigned int>(next);
    refreshHostWindowState();
    // The device, the render pass and every pipeline are still here: that is the point of the move, and the
    // log says so because it is otherwise invisible.
    V_LOGI("[VsgHostWindow] moved to the host's new window ({}x{}); the device and its pipelines were kept",
           _extent2D.width, _extent2D.height);
    return true;
}

void VsgHostWindow::resize()
{
    auto* connection = static_cast<xcb_connection_t*>(connection_);
    if (connection == nullptr) {
        return;
    }
    std::uint32_t width  = 0;
    std::uint32_t height = 0;
    if (!hostWindowExtent(connection, static_cast<xcb_window_t>(host_window_), width, height)) {
        return;
    }
    _extent2D.width  = width;
    _extent2D.height = height;
    // The same round trip the geometry cost us: keep the map state honest while we are talking to the server.
    refreshHostWindowState();
    buildSwapchain();
}

VsgHostWindow::~VsgHostWindow()
{
    // The Vulkan objects go with the window; the HOST's window and connection do NOT. That is the whole
    // point of this class: vsg's platform window destroys the window it adopted, so a backend built on it
    // has to hand the window back (releaseWindow()) before letting it die.
    clear();

    if (connection_ != nullptr) {
        xcb_disconnect(static_cast<xcb_connection_t*>(connection_));
        connection_ = nullptr;
    }
}

#else // _WIN32

namespace
{
/** @brief The host window handle the traits carry, as Win32 names it.
 *
 * @param traits Traits the window was created from.
 * @return The HWND, or null when the traits name no host window.
 */
HWND hostWindowFromTraits(const ::vsg::WindowTraits& traits)
{
    if (!traits.nativeWindow.has_value()) {
        return nullptr;
    }
    return static_cast<HWND>(std::any_cast<HWND>(traits.nativeWindow));
}

/** @brief The host window's client area, or false when Win32 could not answer.
 *
 * @param window Window to ask about.
 * @param width  Receives the width in pixels.
 * @param height Receives the height in pixels.
 * @return true when Win32 answered.
 */
bool hostWindowExtent(HWND window, std::uint32_t& width, std::uint32_t& height)
{
    RECT rect{};
    if (!::GetClientRect(window, &rect)) {
        return false;
    }
    width  = static_cast<std::uint32_t>(rect.right - rect.left);
    height = static_cast<std::uint32_t>(rect.bottom - rect.top);
    return true;
}
} // namespace

const char* VsgHostWindow::instanceExtensionSurfaceName() const
{
    return VK_KHR_WIN32_SURFACE_EXTENSION_NAME;
}

bool VsgHostWindow::valid() const
{
    return host_window_ != 0;
}

bool VsgHostWindow::visible() const
{
    if (!valid()) {
        return false;
    }
    // See the X11 branch: the frame path skips a window whose visible() is false, so an adopted window has
    // to answer for itself. A window that is not known to be visible is asked about again.
    if (!window_mapped_) {
        refreshHostWindowState();
    }
    return window_mapped_;
}

void VsgHostWindow::refreshHostWindowState() const
{
    const HWND window = static_cast<HWND>(reinterpret_cast<void*>(host_window_));
    window_mapped_    = ::IsWindow(window) != FALSE && ::IsWindowVisible(window) != FALSE;
}

VsgHostWindow::VsgHostWindow(::vsg::ref_ptr<::vsg::WindowTraits> traits) :
    Inherit(traits)
{
    host_window_ = reinterpret_cast<std::uintptr_t>(hostWindowFromTraits(*traits));
    if (host_window_ == 0) {
        throw ::vsg::Exception{"VsgHostWindow needs a host window handle in its traits.", VK_ERROR_INVALID_EXTERNAL_HANDLE};
    }
    std::uint32_t width  = 0;
    std::uint32_t height = 0;
    if (hostWindowExtent(static_cast<HWND>(reinterpret_cast<void*>(host_window_)), width, height)) {
        _extent2D.width  = width;
        _extent2D.height = height;
        traits->width    = static_cast<int>(width);
        traits->height   = static_cast<int>(height);
    }
    // The map state decides whether vsg's frame path records anything for this window at all (see
    // visible()), so it is read here and refreshed wherever we talk to Win32 again.
    refreshHostWindowState();
}

void VsgHostWindow::_initSurface()
{
    if (!_instance) {
        _initInstance();
    }
    VkWin32SurfaceCreateInfoKHR surfaceCreateInfo{};
    surfaceCreateInfo.sType     = VK_STRUCTURE_TYPE_WIN32_SURFACE_CREATE_INFO_KHR;
    surfaceCreateInfo.hinstance = ::GetModuleHandle(nullptr);
    surfaceCreateInfo.hwnd      = static_cast<HWND>(reinterpret_cast<void*>(host_window_));

    VkSurfaceKHR vk_surface = VK_NULL_HANDLE;
    if (vkCreateWin32SurfaceKHR(*_instance, &surfaceCreateInfo, _instance->getAllocationCallbacks(), &vk_surface) != VK_SUCCESS) {
        throw ::vsg::Exception{"VsgHostWindow failed to create a Win32 surface.", VK_ERROR_INVALID_EXTERNAL_HANDLE};
    }
    _surface = ::vsg::Surface::create(vk_surface, _instance);
}

bool VsgHostWindow::moveToHostSurface(void* native_handle)
{
    if (native_handle == nullptr) {
        return false;
    }
    const auto next = reinterpret_cast<std::uintptr_t>(native_handle);
    if (next == host_window_) {
        return true;
    }
    if (_device == nullptr || _surface == nullptr) {
        host_window_          = next;
        _traits->nativeWindow = static_cast<HWND>(native_handle);
        return false;
    }

    const VkFormat previous_format = _imageFormat.format;
    host_window_                   = next;

    _swapchain.reset();
    _frames.clear();
    _indices.clear();
    _depthImageView.reset();
    _depthImage.reset();
    _multisampleImage.reset();
    _multisampleImageView.reset();
    _surface.reset();

    _initSurface();
    _initFormats();
    if (_imageFormat.format != previous_format) {
        return false;
    }

    buildSwapchain();

    std::uint32_t width  = 0;
    std::uint32_t height = 0;
    if (hostWindowExtent(static_cast<HWND>(reinterpret_cast<void*>(host_window_)), width, height)) {
        _traits->width  = static_cast<int>(width);
        _traits->height = static_cast<int>(height);
    }
    _traits->nativeWindow = static_cast<HWND>(native_handle);
    refreshHostWindowState();
    return true;
}

void VsgHostWindow::resize()
{
    std::uint32_t width  = 0;
    std::uint32_t height = 0;
    if (!hostWindowExtent(static_cast<HWND>(reinterpret_cast<void*>(host_window_)), width, height)) {
        return;
    }
    _extent2D.width  = width;
    _extent2D.height = height;
    refreshHostWindowState();
    buildSwapchain();
}

VsgHostWindow::~VsgHostWindow()
{
    // The Vulkan objects go with the window; the HOST's window does NOT (see the X11 branch: this is why
    // the backend used to call releaseWindow() before letting vsg's platform window die).
    clear();
}

#endif // _WIN32

} // namespace detail

V_VSG_NS_END
