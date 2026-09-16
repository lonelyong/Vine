/**
 * @brief The host surface MOVES: a session follows the host's new window (C1).
 *
 * A host hands the backend a native window it owns and replaces it when the windowing system recreates
 * it (Qt does this on a reparent / screen change). The SDK contract for RenderBackend::setWindowHandle()
 * is that the backend MOVES to the new one, so the device, the render pass and every pipeline compiled
 * against it stay -- which is what this phase asserts, with the host's windows being the test's own:
 *
 *   1. two host windows, created by the test itself and never by the backend -- on its own X11 connection
 *      on X11, with its own window class on Win32;
 *   2. a session attached to the first, drawing and reading a pixel back from it;
 *   3. the host announces the second -> the session moves (VsgRenderer::windowBuildCount() stays flat,
 *      which is what "no new instance / physical device / device" looks like from outside);
 *   4. the picture after the move is the picture before it (the swapchain was rebuilt against the new
 *      surface and the render pass -- hence the pipelines -- survived);
 *   5. BOTH windows still exist on the server, and the one the session adopted is still there after
 *      shutdown(): the backend presents through the host's window, it never owns it.
 *
 * The last one is the assertion vsg's own platform window fails: it destroys the window it adopted (and on
 * Win32 then unregisters whatever class it finds on it), which is why this backend used to call
 * releaseWindow() before letting its window die.
 *
 * WHAT THE TWO WINDOW SYSTEMS SHARE is everything below the HostWindow class and the environment shim: the
 * frames, the counts, the refusals and the teardown are asserted once, so the two platforms cannot be held
 * to different standards. See VsgHostWindow.hpp for the same arrangement in the code under test.
 */

#include "selftest_support.hpp"

#include <cstdint>
#include <cstdio>
#include <cstdlib>

#if defined(_WIN32)
#    ifndef NOMINMAX
#        define NOMINMAX
#    endif
#    include <windows.h>
#else
#    include <xcb/xcb.h>
#endif

namespace selftest
{

namespace
{
#if defined(_WIN32)
/** @brief One host window this phase owns: created with the phase's own window class, never the backend's. */
class HostWindow
{
  public:
    HostWindow() = default;
    HostWindow(const HostWindow&) = delete;
    HostWindow& operator=(const HostWindow&) = delete;
    ~HostWindow() { close(); }

    /** @brief Creates and shows a window whose CLIENT area is @p width x @p height.
     *
     * The class is the phase's own, and it is deliberately NEVER unregistered: that makes it the second
     * thing a backend that hands the window back to vsg's destructor takes away (that destructor calls
     * DestroyWindow on the adopted handle and then UnregisterClass on its class, which for a host's window
     * class is the same kind of damage as destroying the window).
     *
     * @param width  Client-area width in pixels.
     * @param height Client-area height in pixels.
     * @return true when the window was created.
     */
    bool open(std::uint32_t width, std::uint32_t height)
    {
        if (!ensureClassRegistered()) {
            return false;
        }

        // Sized through the frame so that GetClientRect -- what vsg and this phase both read -- reports the
        // requested size, exactly as the X11 window below is created at that size.
        RECT rect{ 0, 0, static_cast<LONG>(width), static_cast<LONG>(height) };
        if (!::AdjustWindowRect(&rect, kWindowStyle, FALSE)) {
            return false;
        }

        window_ = ::CreateWindowExW(0, kClassName, L"Vine host surface (self-test)", kWindowStyle, CW_USEDEFAULT,
                                    CW_USEDEFAULT, rect.right - rect.left, rect.bottom - rect.top, nullptr, nullptr,
                                    ::GetModuleHandleW(nullptr), nullptr);
        if (window_ == nullptr) {
            return false;
        }
        // Shown but not activated: vsg's frame path SKIPS a window whose visible() is false, so a hidden
        // window would test a path no host has -- and taking the foreground away from whoever is running
        // the gate would be rude.
        ::ShowWindow(window_, SW_SHOWNOACTIVATE);
        return true;
    }

    /** @brief Whether the window still exists.
     *
     * IsWindow asks the window manager for the window itself, which is the answer that matters: a destroyed
     * handle is reported as non-existent even though the value still reads like a handle.
     *
     * @return true when the window is still there.
     */
    [[nodiscard]] bool exists() const { return window_ != nullptr && ::IsWindow(window_) != 0; }

    /** @brief Destroys the window (this phase's own teardown, never the backend's doing). */
    void close()
    {
        if (window_ != nullptr) {
            ::DestroyWindow(window_);
            window_ = nullptr;
        }
    }

    /** @brief The handle as the host announces it.
     *
     * On Win32 the native handle IS a pointer, so it travels as-is; the X11 window below has to go through
     * an integer first. That difference -- and the `_putenv_s` vs `setenv` one -- is all this file's
     * platforms differ in.
     *
     * @return The value to hand to RenderBackend::setWindowHandle().
     */
    [[nodiscard]] void* handle() const noexcept { return reinterpret_cast<void*>(window_); }

  private:
    inline static constexpr const wchar_t* kClassName   = L"VineSelftestHostSurface";
    inline static constexpr DWORD          kWindowStyle = WS_OVERLAPPEDWINDOW;

    /** @brief Registers the phase's window class once per process.
     *
     * @return true when the class is in place.
     */
    static bool ensureClassRegistered()
    {
        static const bool registered = [] {
            WNDCLASSEXW description{};
            description.cbSize        = sizeof(WNDCLASSEXW);
            description.style         = CS_HREDRAW | CS_VREDRAW;
            description.lpfnWndProc   = ::DefWindowProcW;
            description.hInstance     = ::GetModuleHandleW(nullptr);
            description.hCursor       = ::LoadCursorW(nullptr, IDC_ARROW);
            description.lpszClassName = kClassName;
            return ::RegisterClassExW(&description) != 0 || ::GetLastError() == ERROR_CLASS_ALREADY_EXISTS;
        }();
        return registered;
    }

    HWND window_ = nullptr;
};
#else
/** @brief One host window this phase owns: created on the phase's own connection, never the backend's. */
class HostWindow
{
  public:
    HostWindow() = default;
    HostWindow(const HostWindow&) = delete;
    HostWindow& operator=(const HostWindow&) = delete;
    ~HostWindow() { close(); }

    /** @brief Creates and maps a window of @p width x @p height.
     *
     * @param width  Width in pixels.
     * @param height Height in pixels.
     * @return true when the server accepted the window.
     */
    bool open(std::uint32_t width, std::uint32_t height)
    {
        int screen_num = 0;
        connection_    = xcb_connect(nullptr, &screen_num);
        if (connection_ == nullptr || xcb_connection_has_error(connection_) != 0) {
            return false;
        }
        xcb_screen_iterator_t screens = xcb_setup_roots_iterator(xcb_get_setup(connection_));
        for (int i = 0; i < screen_num; ++i) {
            xcb_screen_next(&screens);
        }
        const xcb_screen_t* screen = screens.data;

        id_                   = xcb_generate_id(connection_);
        const std::uint32_t mask     = XCB_CW_BACK_PIXEL | XCB_CW_EVENT_MASK;
        const std::uint32_t values[] = { screen->black_pixel, XCB_EVENT_MASK_STRUCTURE_NOTIFY | XCB_EVENT_MASK_EXPOSURE };
        xcb_create_window(connection_, XCB_COPY_FROM_PARENT, id_, screen->root, 0, 0, static_cast<std::uint16_t>(width),
                          static_cast<std::uint16_t>(height), 0, XCB_WINDOW_CLASS_INPUT_OUTPUT, screen->root_visual, mask, values);
        xcb_map_window(connection_, id_);
        xcb_flush(connection_);
        return true;
    }

    /** @brief Whether the window still exists, asked of the server.
     *
     * A destroyed window has no geometry to report, so the reply -- not an error code -- is the answer.
     *
     * @return true when the server still knows the window.
     */
    [[nodiscard]] bool exists() const
    {
        if (connection_ == nullptr || id_ == 0) {
            return false;
        }
        xcb_get_geometry_reply_t* reply = xcb_get_geometry_reply(connection_, xcb_get_geometry(connection_, id_), nullptr);
        if (reply == nullptr) {
            return false;
        }
        free(reply);
        return true;
    }

    /** @brief Destroys the window (and its connection, which is this phase's own). */
    void close()
    {
        if (connection_ == nullptr) {
            return;
        }
        if (id_ != 0) {
            xcb_destroy_window(connection_, id_);
            id_ = 0;
        }
        xcb_flush(connection_);
        xcb_disconnect(connection_);
        connection_ = nullptr;
    }

    /** @brief The handle as the host announces it.
     *
     * The X window id is an integer, so it reaches the `void*` RenderBackend::setWindowHandle() takes
     * through one; the Win32 window above is a pointer and travels as-is.
     *
     * @return The value to hand to RenderBackend::setWindowHandle().
     */
    [[nodiscard]] void* handle() const noexcept
    {
        return reinterpret_cast<void*>(static_cast<std::uintptr_t>(id_));
    }

  private:
    xcb_connection_t* connection_ = nullptr;
    xcb_window_t      id_         = 0;
};
#endif

/** @brief Sets or clears the environment variable a hatch is read from.
 *
 * The hatches themselves are read with std::getenv() by the code under test, so this has to write the
 * environment the running process sees: setenv()/unsetenv() on POSIX, _putenv_s() on Windows (which updates
 * what getenv() reads in the same CRT).
 *
 * @param name Variable name.
 * @param on   true to set it to "1", false to clear it.
 */
void setEnvironmentFlag(const char* name, bool on)
{
#if defined(_WIN32)
    (void)::_putenv_s(name, on ? "1" : "");
#else
    if (on) {
        setenv(name, "1", 1);
    }
    else {
        unsetenv(name);
    }
#endif
}
} // namespace

bool runHostSurfaceMovePhase(vine::vsg::VsgRenderer& renderer, const CameraPtr& camera, int frames)
{
    HostWindow host_a;
    HostWindow host_b;
    if (!host_a.open(320, 180) || !host_b.open(320, 180)) {
        std::fprintf(stderr, "[selftest] FAIL: the host-surface phase could not create its own host windows\n");
        return false;
    }

    // A session on a HOST window is what a host gives this backend; the session the test drove so far ran on
    // vsg's own window, so it is replaced here. vsg's device cap is back at its default, so a leaked device
    // would throw right here rather than pass quietly.
    renderer.shutdown();
    renderer.setWindowHandle(host_a.handle());
    if (!renderer.initialize()) {
        std::fprintf(stderr, "[selftest] FAIL: a session could not attach to the host surface the phase announced\n");
        return false;
    }

    auto              pass     = RenderPassPtr(new RenderPass());
    auto              quad     = makeVisibleQuad();
    auto              material = MaterialPtr(new Material());
    material->setDiffuse(vine::Colorf(0.9f, 0.15f, 0.05f, 1.0f));
    const RenderCommand command(quad, material, Mat4d());
    const vine::Color   clear_color(10, 20, 30, 255);

    // Every frame goes to the HOST SURFACE itself. That is the target a host presents, and it is the one the
    // move has to keep working: a surface or swapchain rebuilt against the wrong window fails as a validation
    // error rather than silently, so presenting is the observable this phase drives. The PIXEL this phase
    // compares across the move is read from an off-screen target drawn in the same frame, because the window
    // target has no readback path of its own.
    // The off-screen target this phase reads back. Its size is named ONCE: the pixel coordinates below are
    // derived from it (they were spelled 256 / 144 / 128 / 72 in three places until 2026-09-16, so changing
    // the target would have left the samples pointing at the old layout).
    constexpr std::uint32_t kPixelsWidth  = 256u;
    constexpr std::uint32_t kPixelsHeight = 144u;
    auto                    pixels_target = RenderTargetPtr(new RenderTarget());
    pixels_target->setSize(static_cast<int>(kPixelsWidth), static_cast<int>(kPixelsHeight));
    pixels_target->attachColor(RenderTarget::ColorFormat::RGBA8);
    pixels_target->attachDepth(RenderTarget::DepthFormat::D24);
    // Its own pass: the window pass and this one run in the same frame, and a pass names ONE target.
    auto pixels_pass = RenderPassPtr(new RenderPass());

    const auto draw = [&]() {
        for (int i = 0; i < frames; ++i) {
            FrameScope frame(renderer);
            PassScope  window_pass(renderer, pass.get(), 0, nullptr, clear_color, true,
                                  vine::graphics::DepthMode::TestAndWrite);
            renderer.render(std::vector<RenderCommand>{ command }, camera.get());
            PassScope offscreen_pass(renderer, pixels_pass.get(), 0, pixels_target.get(), clear_color, true,
                                     vine::graphics::DepthMode::TestAndWrite);
            renderer.render(std::vector<RenderCommand>{ command }, camera.get());
        }
    };
    const auto readCentre = [&](const char* when, int centre[3], int corner[3], bool& ok) {
        std::vector<std::uint8_t> pixels;
        if (!renderer.readColorBuffer(pixels_target.get(), 0, pixels)) {
            std::fprintf(stderr, "[selftest] FAIL: the phase's own target could not be read back (%s the move)\n", when);
            ok = false;
            return;
        }
        const auto at = [&pixels](std::uint32_t x, std::uint32_t y, int channel) {
            return static_cast<int>(
                pixels[(static_cast<std::size_t>(y) * kPixelsWidth + x) * 4u + static_cast<std::size_t>(channel)]);
        };
        for (int channel = 0; channel < 3; ++channel) {
            centre[channel] = at(kPixelsWidth / 2u, kPixelsHeight / 2u, channel);
            corner[channel] = at(0u, 0u, channel);
        }
        std::fprintf(stderr, "[host-surface] %s the move: centre %d,%d,%d, corner %d,%d,%d\n", when, centre[0],
                     centre[1], centre[2], corner[0], corner[1], corner[2]);
    };

    bool ok        = true;
    int  before[3] = { 0, 0, 0 };
    int  corner[3] = { 0, 0, 0 };

    draw();
    readCentre("before", before, corner, ok);

    const std::size_t windows_before = renderer.windowBuildCount();
    const std::size_t waits_before   = renderer.deviceWaitCount();

    // Re-announcing the window the session is ALREADY on must keep it, not rebuild it: a host that repeats
    // setWindowHandle() + initialize() (a show/resize event, say) would otherwise pay a full session rebuild
    // -- the instance, the device and every compiled pipeline -- for nothing.
    renderer.setWindowHandle(host_a.handle());
    if (!renderer.initialize()) {
        std::fprintf(stderr, "[selftest] FAIL: re-announcing the window the session is already on was refused\n");
        return false;
    }
    const bool same_handle_kept = renderer.windowBuildCount() == windows_before && renderer.deviceWaitCount() == waits_before;
    if (!same_handle_kept) {
        std::fprintf(stderr,
                     "[selftest] FAIL: re-announcing the SAME host window rebuilt the session (windows built"
                     " %zu before, %zu after; %zu counted device stop(s))\n",
                     windows_before, renderer.windowBuildCount(), renderer.deviceWaitCount() - waits_before);
        ok = false;
    }

    // The host replaces its window and announces the new one: the session must FOLLOW it.
    renderer.setWindowHandle(host_b.handle());
    if (!renderer.initialize()) {
        std::fprintf(stderr, "[selftest] FAIL: the session did not come up on the host's new window\n");
        return false;
    }
    if (renderer.windowBuildCount() != windows_before) {
        std::fprintf(stderr,
                     "[selftest] FAIL: the session was REBUILT for the host's new window (%zu window build(s) before,"
                     " %zu after): the device, its instance and every compiled pipeline went with it\n",
                     windows_before, renderer.windowBuildCount());
        ok = false;
    }
    if (renderer.deviceWaitCount() != waits_before + 1u) {
        // The move stops the device exactly once, through the counted path: the surface / swapchain / depth
        // image being replaced may still be named by work in flight.
        std::fprintf(stderr, "[selftest] FAIL: the move took %zu counted device stop(s), expected exactly 1\n",
                     renderer.deviceWaitCount() - waits_before);
        ok = false;
    }
    // Read the two counts here: they are session-scoped, so the shutdown at the end of this phase clears them.
    const std::size_t builds_after = renderer.windowBuildCount();
    const std::size_t stops        = renderer.deviceWaitCount() - waits_before;
    if (!host_a.exists() || !host_b.exists()) {
        std::fprintf(stderr,
                     "[selftest] FAIL: the backend destroyed a host window (announced=%d, adopted=%d, both must"
                     " still exist)\n",
                     static_cast<int>(host_a.exists()), static_cast<int>(host_b.exists()));
        ok = false;
    }

    // Drive the session again on the NEW surface: the swapchain was rebuilt in place, so this is the frame that
    // proves the move left a session that still renders and presents.
    draw();
    int after[3] = { 0, 0, 0 };
    readCentre("after", after, corner, ok);

    // The pixel is the part of this phase a validation-clean run cannot fake. The corner holds the pass' own
    // clear colour and the centre the quad, so "the centre differs from the corner" is what says the frame
    // really drew -- and it is exactly what a frame the engine SKIPPED cannot produce (a skipped frame leaves
    // the whole off-screen target untouched, i.e. transparent black at both points, reported with no error).
    const auto drew_the_quad = [](const int centre[3], const int corner_rgb[3]) {
        return centre[0] != corner_rgb[0] || centre[1] != corner_rgb[1] || centre[2] != corner_rgb[2];
    };
    if (!drew_the_quad(before, corner) || !drew_the_quad(after, corner)) {
        std::fprintf(stderr,
                     "[selftest] FAIL: the off-screen frame this phase rides on did not draw (centre before the"
                     " move %d,%d,%d, after %d,%d,%d, corner %d,%d,%d: a frame the engine skipped reads back\n"
                     "untouched at both points)\n",
                     before[0], before[1], before[2], after[0], after[1], after[2], corner[0], corner[1], corner[2]);
        ok = false;
    }
    if (before[0] != after[0] || before[1] != after[1] || before[2] != after[2]) {
        std::fprintf(stderr,
                     "[selftest] FAIL: the picture changed across the move (before %d,%d,%d, after %d,%d,%d)\n",
                     before[0], before[1], before[2], after[0], after[1], after[2]);
        ok = false;
    }

    // A refusal is a REBUILD -- the instance, the device and every compiled pipeline -- so a host that could
    // have avoided it (by not taking its window away while the session runs) has to be able to HEAR about it:
    // every refusal of a move reports a Warning / UnsupportedRequest. All three paths are driven here: the
    // first two from the host's own calls, the third through the format hatch (see below), because no window
    // system hands out two visuals whose swapchain formats differ on demand.
    std::size_t refusals = 0;
    renderer.setDiagnosticSink([&refusals](const vine::graphics::RenderDiagnostic& diagnostic) {
        if (diagnostic.severity == vine::graphics::DiagnosticSeverity::Warning &&
            diagnostic.category == vine::graphics::DiagnosticCategory::UnsupportedRequest) {
            ++refusals;
        }
    });
    const std::size_t builds_before_refusals = renderer.windowBuildCount();

    // Path 1: the host announces NO window (it has none to give). There is no surface to move onto, so the
    // session is rebuilt on a window of this backend's own.
    renderer.setWindowHandle(nullptr);
    if (!renderer.initialize()) {
        std::fprintf(stderr, "[selftest] FAIL: the session did not come up after the host announced no window\n");
        return false;
    }
    if (refusals != 1u || renderer.windowBuildCount() != builds_before_refusals + 1u) {
        std::fprintf(stderr,
                     "[selftest] FAIL: announcing no window neither reported a refusal nor rebuilt (%zu refusal(s),"
                     " %zu window build(s) before, %zu after)\n",
                     refusals, builds_before_refusals, renderer.windowBuildCount());
        ok = false;
    }

    // Path 2: the session is on this backend's OWN window now (the rebuild above), so the host's window can
    // only be served by starting a fresh session -- reported rather than silent, for the same reason.
    renderer.setWindowHandle(host_a.handle());
    if (!renderer.initialize()) {
        std::fprintf(stderr, "[selftest] FAIL: the session did not come up on the announced window again\n");
        return false;
    }
    if (refusals != 2u || renderer.windowBuildCount() != builds_before_refusals + 2u) {
        std::fprintf(stderr,
                     "[selftest] FAIL: a session that is not on a host window neither reported a refusal nor"
                     " rebuilt (%zu refusal(s), %zu window build(s) before, %zu after)\n",
                     refusals, builds_before_refusals, renderer.windowBuildCount());
        ok = false;
    }

    // Back onto the host's presenting window, so the teardown below still runs against the window the host
    // last handed over: that assertion is about the ADOPTED window outliving us.
    renderer.setWindowHandle(host_b.handle());
    if (!renderer.initialize()) {
        std::fprintf(stderr, "[selftest] FAIL: the session did not move back onto the host's window\n");
        return false;
    }

    // Path 3: a host window whose swapchain format cannot serve this session's render pass. That needs two
    // windows whose visuals map to different swapchain formats -- a property of the driver, not of this code --
    // so the phase sets the hatch that makes the window's comparison fail instead. The code path is the one the
    // real condition takes: refuse, report, rebuild. Measured from here, so the steps above do not have to be
    // counted again.
    const std::size_t refusals_before_third = refusals;
    const std::size_t builds_before_third   = renderer.windowBuildCount();
    setEnvironmentFlag("VINE_HOST_MOVE_FORMAT_MISMATCH", true);
    renderer.setWindowHandle(host_a.handle());
    const bool came_up_after_mismatch = renderer.initialize();
    setEnvironmentFlag("VINE_HOST_MOVE_FORMAT_MISMATCH", false);
    if (!came_up_after_mismatch) {
        std::fprintf(stderr, "[selftest] FAIL: the session did not come up after the format mismatch was forced\n");
        return false;
    }
    if (refusals != refusals_before_third + 1u || renderer.windowBuildCount() != builds_before_third + 1u) {
        std::fprintf(stderr,
                     "[selftest] FAIL: a window whose swapchain format cannot serve this session neither reported"
                     " a refusal nor rebuilt (%zu refusal(s) for that step, %zu window build(s) before, %zu after)\n",
                     refusals - refusals_before_third, builds_before_third, renderer.windowBuildCount());
        ok = false;
    }
    // The sink captures a local, so it goes before the local does (the convention the other phases follow).
    renderer.setDiagnosticSink({});

    std::fprintf(stderr,
                 "[host-surface] refusals: %zu reported (announcing no window, a window while the session was on"
                 " one of the backend's own, and a window whose swapchain format cannot serve this session), %zu"
                 " window build(s) for them, and the session is back on the host's window\n",
                 refusals, renderer.windowBuildCount() - builds_before_refusals);

    // Let go of the session: the window the host handed over is the HOST's, so it must outlive us -- the
    // assertion vsg's own platform window fails (it destroys the window it adopted). This is the phase's
    // SECOND look, after the refusals above rather than only after the move: a refusal is a rebuild, and a
    // rebuild destroys the window object the session was on, which is where a window the class failed to hand
    // back is taken down. Checking only after the move would leave that to be discovered as a later step
    // failing to attach to a handle that no longer names a window.
    if (!host_a.exists() || !host_b.exists()) {
        std::fprintf(stderr,
                     "[selftest] FAIL: the backend destroyed a host window while it moved or refused (announced=%d,"
                     " adopted=%d, both must still exist)\n",
                     static_cast<int>(host_a.exists()), static_cast<int>(host_b.exists()));
        ok = false;
    }
    renderer.shutdown();
    if (!host_b.exists()) {
        std::fprintf(stderr, "[selftest] FAIL: shutting the session down destroyed the host's window\n");
        ok = false;
    }

    std::fprintf(stderr,
                 "[host-surface] move: the session followed the host's new window (windows built %zu before, %zu"
                 " after; %zu counted device stop(s); host windows intact; the session still presented it; centre"
                 " %d,%d,%d before and after; a repeated handle kept the session: %s)\n",
                 windows_before, builds_after, stops, after[0], after[1], after[2], same_handle_kept ? "yes" : "no");
    return ok;
}

} // namespace selftest
