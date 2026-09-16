/**
 * @brief The host surface MOVES: a session follows the host's new window (C1).
 *
 * A host hands the backend a native window it owns and replaces it when the windowing system recreates
 * it (Qt does this on a reparent / screen change). The SDK contract for RenderBackend::setWindowHandle()
 * is that the backend MOVES to the new one, so the device, the render pass and every pipeline compiled
 * against it stay -- which is what this phase asserts, with the host's windows being the test's own:
 *
 *   1. two host windows, created by the test on its own connection (never the backend's);
 *   2. a session attached to the first, drawing and reading a pixel back from it;
 *   3. the host announces the second -> the session moves (VsgRenderer::windowBuildCount() stays flat,
 *      which is what "no new instance / physical device / device" looks like from outside);
 *   4. the picture after the move is the picture before it (the swapchain was rebuilt against the new
 *      surface and the render pass -- hence the pipelines -- survived);
 *   5. BOTH windows still exist on the server, and the one the session adopted is still there after
 *      shutdown(): the backend presents through the host's window, it never owns it.
 *
 * The last one is the assertion vsg's own platform window fails: it destroys the window it adopted, which
 * is why this backend used to call releaseWindow() before letting its window die.
 */

#include "selftest_support.hpp"

#include <cstdio>

#if !defined(_WIN32)
#include <cstdint>
#include <cstdlib>

#include <xcb/xcb.h>
#endif

namespace selftest
{

#if !defined(_WIN32)

namespace
{
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

    /** @brief The window's id, as the host announces it. */
    [[nodiscard]] xcb_window_t id() const noexcept { return id_; }

  private:
    xcb_connection_t* connection_ = nullptr;
    xcb_window_t      id_         = 0;
};
} // namespace

#endif // !_WIN32

bool runHostSurfaceMovePhase(vine::vsg::VsgRenderer& renderer, const CameraPtr& camera, int frames)
{
#if defined(_WIN32)
    // This phase builds its host windows with X11, which is the window system this gate runs on; the Win32
    // branch of VsgHostWindow is exercised by the app gate on Windows instead.
    (void)renderer;
    (void)camera;
    (void)frames;
    return true;
#else
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
    renderer.setWindowHandle(reinterpret_cast<void*>(static_cast<std::uintptr_t>(host_a.id())));
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
    auto pixels_target = RenderTargetPtr(new RenderTarget());
    pixels_target->setSize(256, 144);
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
            return static_cast<int>(pixels[(static_cast<std::size_t>(y) * 256u + x) * 4u + static_cast<std::size_t>(channel)]);
        };
        for (int channel = 0; channel < 3; ++channel) {
            centre[channel] = at(128u, 72u, channel);
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

    // The host replaces its window and announces the new one: the session must FOLLOW it.
    renderer.setWindowHandle(reinterpret_cast<void*>(static_cast<std::uintptr_t>(host_b.id())));
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

    // Let go of the session: the window the host handed over is the HOST's, so it must outlive us -- the
    // assertion vsg's own platform window fails (it destroys the window it adopted).
    renderer.shutdown();
    if (!host_b.exists()) {
        std::fprintf(stderr, "[selftest] FAIL: shutting the session down destroyed the host's window\n");
        ok = false;
    }

    std::fprintf(stderr,
                 "[host-surface] move: the session followed the host's new window (windows built %zu before, %zu"
                 " after; %zu counted device stop(s); host windows intact; the session still presented it; centre"
                 " %d,%d,%d before and after)\n",
                 windows_before, builds_after, stops, after[0], after[1], after[2]);
    return ok;
#endif // !_WIN32
}

} // namespace selftest
