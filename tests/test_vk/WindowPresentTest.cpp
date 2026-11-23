/**
 * @brief M5's acceptance: a frame that names a HOST window is drawn into its swapchain and the platform accepts it.
 *
 * What this pins:
 *   - `DeviceDesc{Handle, window}` plus `createSurfaceTarget` is the SDK's window path: the target exists before
 *     `initialize()` (a Vulkan surface is a creation input), and `capabilities().presentation` says so;
 *   - `executeFrame(frame, target)` draws a pass whose `target` is null into the acquired swapchain image and returns
 *     `Presented`; a later frame after `resize()` presents again (the swapchain follows the surface);
 *   - the WINDOW's pixels are what the frame drew: after the present, the test reads the window back through the
 *     SAME instrument the vsg window cases use - `tests/test_vsg/TestHostWindow.hpp`: an `XGetImage` on X11 and a
 *     `PrintWindow` / `PW_RENDERFULLCONTENT` compositor read on Win32, with that file's platform traps (P87-P89)
 *     in one home - and compares the centre with the triangle's colour. The vk backend never sees the instrument.
 *
 * The cases skip when there is no Vulkan device, no desktop to create a window on, or no window instrument (a Linux
 * build without the xcb development package; the backend itself opens `libxcb.so.1` with dlopen and never links it).
 * Both platforms run the same assertions: the colours below are chosen so each one survives the window's own byte
 * order.
 */

#include <gtest/gtest.h>

#include <array>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <thread>
#include <vector>

#include <vine/Buffer.hpp>
#include <vine/String.hpp>
#include <vine/backend_vk/internal/WindowTarget.hpp>
#include <vine/graphics/CameraSnapshot.hpp>
#include <vine/graphics/FrameDescription.hpp>
#include <vine/graphics/FrameReport.hpp>
#include <vine/graphics/Geometry.hpp>
#include <vine/graphics/Primitive.hpp>
#include <vine/graphics/RenderBackendRegistry.hpp>
#include <vine/graphics/DrawItem.hpp>
#include <vine/graphics/RenderPass.hpp>
#include <vine/graphics/RenderTarget.hpp>
#include <vine/graphics/ShaderProgram.hpp>
#include <vine/math/Matrix4x4.hpp>

#include "PlanTestSupport.hpp"

// THE MEASURING INSTRUMENT, in one home. The host-window fixture is header-only and already carries the platform
// facts: an `XGetImage` through the xcb connection on X11, and on Win32 a `PrintWindow` `PW_RENDERFULLCONTENT`
// compositor read that pumps the thread's messages and waits for a composition pass (DwmFlush). It needs the xcb
// development package on Linux, so a build without it has no instrument and the window cases skip.
#if defined(_WIN32) || defined(VN_TEST_HAVE_XCB)
#    define VN_TEST_HAVE_WINDOW_INSTRUMENT 1
#    include "../test_vsg/TestHostWindow.hpp"
#endif

using vn::Colorf;
using vn::String;
using vn::graphics::CameraSnapshot;
using vn::graphics::ClearPolicy;
using vn::graphics::DeviceDesc;
using vn::graphics::DevicePresentation;
using vn::graphics::DrawPacket;
using vn::graphics::FrameDescription;
using vn::graphics::Geometry;
using vn::graphics::PassDraw;
using vn::graphics::PrepareReport;
using vn::graphics::PresentOutcome;
using vn::graphics::Primitive;
using vn::graphics::RenderBackendRegistry;
using vn::graphics::DrawItem;
using vn::graphics::RenderTarget;
using vn::graphics::ShaderProgram;
using vn::graphics::ShaderStage;
using vn::graphics::ShaderStageType;
using vn::graphics::SurfaceTargetResult;
using vn::graphics::Topology;
using vn::graphics::DrawKind;
using vn::intrusive_ptr;

namespace
{

#if defined(VN_TEST_HAVE_WINDOW_INSTRUMENT)

constexpr int kWindowWidth  = 128;
constexpr int kWindowHeight = 96;

/// @brief Paints the whole window white WITHOUT the backend: the instrument check that runs before a frame.
///
/// @details
/// - X11 fills a rectangle through a GC on the test's own connection; Win32 fills through GDI, the same call the
///   fixture's own case makes.
/// - WHITE is one of the three colours this file uses, and it is deliberately one whose red and blue bytes are
///   equal: a Win32 read hands back the surface's own order (blue-green-red for a GDI window, the swapchain's for
///   a Vulkan one), so a symmetric colour reads the same either way.
/// - A desktop that cannot show the test this fill cannot show it a presented frame either, which is what makes
///   the result a SKIP condition rather than a failure.
void paintInstrument(TestDisplay& display, TestHostWindow& window)
{
#if defined(_WIN32)
    (void)display;
    const HWND painted = static_cast<HWND>(window.handle());
    const HDC  dc      = GetDC(painted);
    if (dc == nullptr) {
        return;
    }
    const HBRUSH  brush    = CreateSolidBrush(RGB(255, 255, 255));
    const HGDIOBJ previous = SelectObject(dc, brush);
    RECT          client{};
    GetClientRect(painted, &client);
    FillRect(dc, &client, brush);
    SelectObject(dc, previous);
    DeleteObject(brush);
    ReleaseDC(painted, dc);
    GdiFlush();
#else
    const auto           painted  = static_cast<xcb_window_t>(reinterpret_cast<std::uintptr_t>(window.handle()));
    const xcb_gcontext_t gc       = xcb_generate_id(display.connection());
    const std::uint32_t  mask     = XCB_GC_FOREGROUND;
    const std::uint32_t  values[] = { 0x00FFFFFFU };
    xcb_create_gc(display.connection(), gc, painted, mask, values);
    const xcb_rectangle_t rectangle{ 0, 0, static_cast<std::uint16_t>(kWindowWidth), static_cast<std::uint16_t>(kWindowHeight) };
    xcb_poly_fill_rectangle(display.connection(), painted, gc, 1, &rectangle);
    xcb_free_gc(display.connection(), gc);
    xcb_flush(display.connection());
#endif
}

/// @brief The M4/M5 content ABI: camera push, draw block, and a fragment writing one colour.
constexpr const char8_t* kVertexStage = u8R"(
#version 450
layout(location = 0) in vec3 in_position;
layout(push_constant) uniform PushConstants
{
    mat4 projection;
    mat4 modelView;
} vn_pc;
void main()
{
    gl_Position = vn_pc.projection * vn_pc.modelView * vec4(in_position, 1.0);
}
)";

constexpr const char8_t* kGreenFragment = u8R"(
#version 450
layout(location = 0) out vec4 out_color;
void main()
{
    out_color = vec4(0.0, 1.0, 0.0, 1.0);
}
)";

[[nodiscard]] intrusive_ptr<ShaderProgram> makeProgram()
{
    auto program = intrusive_ptr<ShaderProgram>(new ShaderProgram());
    program->setName(u8"vk_m5_window");
    program->addStage(ShaderStage{ ShaderStageType::Vertex, String(kVertexStage), String(u8"main") });
    program->addStage(ShaderStage{ ShaderStageType::Fragment, String(kGreenFragment), String(u8"main") });
    return program;
}

[[nodiscard]] intrusive_ptr<Geometry> makeTriangle()
{
    std::vector<float> positions{
        -0.75F, -0.75F, 0.5F,
        0.75F,  -0.75F, 0.5F,
        0.0F,   0.75F,  0.5F,
    };
    std::vector<std::uint32_t> indices{ 0U, 1U, 2U };

    auto geometry = intrusive_ptr<Geometry>(new Geometry());
    geometry->setPositions(intrusive_ptr<const vn::Buffer<float>>(new vn::Buffer<float>(std::move(positions))));

    Primitive primitive;
    primitive.topology = Topology::Triangles;
    primitive.indices  = Primitive::IndexSpan::whole(intrusive_ptr<const vn::Buffer<std::uint32_t>>(new vn::Buffer<std::uint32_t>(std::move(indices))));
    geometry->clearPrimitives();
    geometry->addPrimitive(primitive);
    geometry->bumpRevision();
    return geometry;
}

[[nodiscard]] CameraSnapshot identityCamera()
{
    CameraSnapshot camera;
    camera.present    = true;
    camera.view       = vn::math::Mat4d{};
    camera.projection = vn::math::Mat4d{};
    camera.eye        = vn::math::Vec3d( 0.0, 0.0, 0.0 );
    return camera;
}

/// @brief A frame with ONE window pass (target null) that clears MAGENTA and draws the green triangle.
///
/// @details
/// - The clear is magenta rather than the old red for one reason: it has to be told apart from the triangle's green
///   on a Win32 read that hands the surface's own byte order back, and magenta (red and blue loud, green quiet) is
///   the same under either order while red is not.
/// - It is also not the colour a window shows on its own (black on X11, the class background on Win32), so a corner
///   that reads magenta is the PASS' clear and not an untouched window.
///
/// @param clear The pass' clear colour; magenta by default, and a two-window case passes its own so the windows
///              carry pictures that cannot be confused (see TwoWindowsOnOneDeviceKeepTheirOwnPictures).
[[nodiscard]] FrameDescription windowFrame(Colorf clear = Colorf(1.0F, 0.0F, 1.0F, 1.0F))
{
    const intrusive_ptr<ShaderProgram> program  = makeProgram();
    const intrusive_ptr<Geometry>      geometry = makeTriangle();

    DrawItem command(geometry, nullptr, vn::math::Mat4d{});
    command.program             = program;
    command.program_description = program->description();

    auto commands = std::make_shared<std::vector<DrawItem>>();
    commands->push_back(std::move(command));

    DrawPacket draw;
    draw.kind     = DrawKind::Scene;
    draw.view.camera   = identityCamera();
    draw.commands = commands;

    FrameDescription frame;
    FrameDescription::Pass entry;
    entry.pass = vn::test::framePassForTest();
    PassDraw pass;
    pass.target       = nullptr; // the window
    pass.clear        = ClearPolicy{};
    pass.clear->color = clear;
    pass.packets.push_back(std::move(draw));
    entry.draw = std::move(pass);
    frame.passes.push_back(std::move(entry));
    return frame;
}

/// @brief Runs @p frame until it presents (a freshly created swapchain's first acquires may answer Lost while the
///        surface settles), or answers the last outcome after @p attempts frames.
[[nodiscard]] PresentOutcome presentUntilReady(vn::graphics::RenderBackend& backend, const FrameDescription& frame, vn::raw_ptr<vn::graphics::WindowPresentTarget> target, int attempts)
{
    PresentOutcome outcome = PresentOutcome::Lost;
    for (int attempt = 0; attempt < attempts; ++attempt) {
        outcome = backend.executeFrame(frame, target);
        if (outcome == PresentOutcome::Presented) {
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(25));
    }
    return outcome;
}

#endif // VN_TEST_HAVE_WINDOW_INSTRUMENT

} // namespace

TEST(VkWindowPresentTest, AWindowFrameIsDrawnAndPresented)
{
#if !defined(VN_TEST_HAVE_WINDOW_INSTRUMENT)
    GTEST_SKIP() << "this build has no window instrument (the xcb development package is missing)";
#else
    TestDisplay display;
    if (!display.open()) {
        GTEST_SKIP() << display.skipReason();
    }
    TestHostWindow window(display, kWindowWidth, kWindowHeight);
    if (window.handle() == nullptr) {
        GTEST_SKIP() << "a host window could not be created on this desktop";
    }
    ASSERT_TRUE(window.wasViewableAtCreation()) << "the fixture waits for a viewable window before a case presents anything";

    // INSTRUMENT CHECK: paint the window white with plain GDI / X and read it back. A desktop that cannot show the
    // test this fill cannot show it a presented frame either, so that answer makes the pixel half UNMEASURED rather
    // than failed - the present outcome, the device-loss flag and validation still judge the frame.
    paintInstrument(display, window);
    const std::array<std::uint8_t, 3> instrument = window.waitForPixel(
        kWindowWidth / 2,
        kWindowHeight / 2,
        [](const std::array<std::uint8_t, 3>& pixel) { return pixel[0] > 200 && pixel[1] > 200 && pixel[2] > 200; },
        std::chrono::milliseconds{ 1000 });
    const bool can_read_pixels =
        window.readError() == 0 && instrument[0] > 200 && instrument[1] > 200 && instrument[2] > 200;

    const DeviceDesc desc{ DevicePresentation::Handle, window.handle() };
    auto             backend = RenderBackendRegistry::instance().create(u8"vk", desc);
    ASSERT_NE(backend, nullptr) << "the factory did not answer the vk backend";

    // THE SDK'S FLOW: the window is a CREATION input, so the target is asked for before initialize().
    SurfaceTargetResult      result = SurfaceTargetResult::NoPresentation;
    vn::raw_ptr<vn::graphics::WindowPresentTarget> target = backend->createSurfaceTarget(window.handle(), &result);
    ASSERT_NE(target, nullptr) << "createSurfaceTarget answered " << static_cast<int>(result);
    EXPECT_EQ(result, SurfaceTargetResult::Ok);

    if (!backend->initialize()) {
        GTEST_SKIP() << "this machine has no Vulkan device the backend can come up on";
    }
    ASSERT_TRUE(backend->capabilities().presentation) << "a Handle device that came up must present";
    EXPECT_EQ(target->nativeHandle(), window.handle()) << "the target did not stay on the host window";

    // FRAME 1: one window pass, presented.
    EXPECT_EQ(backend->executeFrame(windowFrame(), target), PresentOutcome::Presented);

    // A HEALTHY PRESENT IS NOT A LOSS: the classification only fires on `VK_ERROR_DEVICE_LOST` (see
    // WindowTarget::deviceLost and Device::noteDeviceLost), so a presented frame leaves the flag clear - which is what
    // keeps `initialized()` true for the next frame.
    auto* window_target = dynamic_cast<vn::backend_vk::WindowTarget*>(target);
    ASSERT_NE(window_target, nullptr);
    EXPECT_FALSE(window_target->deviceLost());
    EXPECT_TRUE(backend->initialized()) << "a frame that presented must leave the session up";

    // THE WINDOW'S PIXELS, read by the TEST: the triangle covers the centre and the corner keeps the clear. Both are
    // asked for as PURE colours whose red and blue bytes are equal (see windowFrame), so the assertion holds under
    // either byte order a platform hands back. A window system that cannot show its own painted pixels (the
    // instrument check above) makes this half UNMEASURED, not failed.
    if (!can_read_pixels) {
        std::fprintf(stderr,
                     "[window test] the window instrument reads rgb=%u,%u,%u for a plain white fill; the pixel half is skipped\n",
                     static_cast<unsigned>(instrument[0]),
                     static_cast<unsigned>(instrument[1]),
                     static_cast<unsigned>(instrument[2]));
    }
    else {
        const auto magenta = [](const std::array<std::uint8_t, 3>& pixel) { return pixel[0] > 200 && pixel[1] < 40 && pixel[2] > 200; };
        const auto green   = [](const std::array<std::uint8_t, 3>& pixel) { return pixel[0] < 40 && pixel[1] > 200 && pixel[2] < 40; };
        const std::array<std::uint8_t, 3> centre =
            window.waitForPixel(kWindowWidth / 2, kWindowHeight / 2, green, std::chrono::milliseconds{ 1000 });
        const std::array<std::uint8_t, 3> corner = window.waitForPixel(2, 2, magenta, std::chrono::milliseconds{ 1000 });
        // The CLEAR is magenta everywhere the triangle is not, and the FRAGMENT green inside it: that separates "the
        // present is readable" from "the draw is missing" with one picture.
        EXPECT_TRUE(green(centre)) << "the triangle's green did not reach the window's centre pixel (rgb = " << static_cast<unsigned>(centre[0]) << ", " << static_cast<unsigned>(centre[1]) << ", " << static_cast<unsigned>(centre[2]) << ")";
        EXPECT_TRUE(magenta(corner)) << "the window's corner is not the pass' clear (rgb = " << static_cast<unsigned>(corner[0]) << ", " << static_cast<unsigned>(corner[1]) << ", " << static_cast<unsigned>(corner[2]) << ")";
    }

    // PREPARING A WINDOW FRAME touches neither the swapchain nor the queue: it is counted, it builds what the frame
    // would need, and nothing is acquired or presented (see RenderFrameExecutor::prepareFrame, P11). The pass builds
    // against the swapchain that already exists - preparing must not create one.
    const PrepareReport prepared = backend->prepareFrame(windowFrame(), target);
    EXPECT_TRUE(prepared.prepared()) << "outcome " << static_cast<int>(prepared.outcome);
    EXPECT_EQ(prepared.passes, 1U);
    EXPECT_EQ(prepared.draw_packets, 1U);

    // A SECOND ASK for the same window is refused: one live swapchain per window.
    const vn::raw_ptr<vn::graphics::WindowPresentTarget> again = backend->createSurfaceTarget(window.handle(), &result);
    EXPECT_EQ(result, SurfaceTargetResult::WindowOccupied);

    // RESIZE: another frame still presents (the swapchain follows the surface).
    target->resize(kWindowWidth, kWindowHeight);
    EXPECT_EQ(backend->executeFrame(windowFrame(), target), PresentOutcome::Presented);

    EXPECT_EQ(backend->diagnosticCount(vn::graphics::DiagnosticCategory::InitFailed), 0U)
        << "validation reported something on the window path";

    backend->shutdown();
#endif
}

TEST(VkWindowPresentTest, AWindowHandleMovesWithoutRebuildingTheDevice)
{
#if !defined(VN_TEST_HAVE_WINDOW_INSTRUMENT)
    GTEST_SKIP() << "this build has no window instrument (the xcb development package is missing)";
#else
    // CAP-45: a platform may destroy and recreate a host window under the application, and the SDK's answer is one
    // announcement ON THE TARGET (WindowPresentTarget::setWindowHandle) - not a new target, not a new device. This is
    // the fact the vk side never had a named case for (the reference backend has SessionMoveTest).
    TestDisplay display;
    if (!display.open()) {
        GTEST_SKIP() << display.skipReason();
    }
    TestHostWindow first(display, kWindowWidth, kWindowHeight);
    TestHostWindow second(display, kWindowWidth, kWindowHeight);
    if (first.handle() == nullptr || second.handle() == nullptr) {
        GTEST_SKIP() << "host windows could not be created on this desktop";
    }

    const DeviceDesc desc{ DevicePresentation::Handle, first.handle() };
    auto             backend = RenderBackendRegistry::instance().create(u8"vk", desc);
    ASSERT_NE(backend, nullptr) << "the factory did not answer the vk backend";

    SurfaceTargetResult result = SurfaceTargetResult::NoPresentation;
    vn::raw_ptr<vn::graphics::WindowPresentTarget> target = backend->createSurfaceTarget(first.handle(), &result);
    ASSERT_NE(target, nullptr) << "createSurfaceTarget answered " << static_cast<int>(result);
    if (!backend->initialize()) {
        GTEST_SKIP() << "this machine has no Vulkan device the backend can come up on";
    }

    // ONE FRAME VALUE, RUN TWICE: the content it names is what residency is about, so a rebuilt device would show up
    // as an upload on the second frame (see the report half below).
    const FrameDescription frame = windowFrame();
    EXPECT_EQ(presentUntilReady(*backend, frame, target, 40), PresentOutcome::Presented);
    EXPECT_EQ(presentUntilReady(*backend, frame, target, 40), PresentOutcome::Presented)
        << "the steady frame through the original handle did not present";

    // THE WINDOW IS RECREATED UNDER THE APPLICATION: the host announces the new handle on the target it already holds.
    target->setWindowHandle(second.handle());
    EXPECT_EQ(target->nativeHandle(), second.handle()) << "the target did not follow the handle";

    // THE DEVICE IS STILL THE SAME DEVICE: no shutdown()/initialize() happened, so it is up, it still presents, and
    // what it built for this content is still resident.
    EXPECT_TRUE(backend->initialized()) << "moving a window handle is not a session rebuild";
    EXPECT_TRUE(backend->capabilities().presentation);

    EXPECT_EQ(presentUntilReady(*backend, frame, target, 40), PresentOutcome::Presented)
        << "a frame through the moved handle did not reach the present call";

    const vn::graphics::FrameReport after = backend->frameReport();
    ASSERT_TRUE(after.uploads.has_value()) << "this backend keeps upload counters";
    EXPECT_EQ(after.uploads->stream_uploads, 0U) << "the geometry stayed resident: a rebuilt device would re-upload it";
    EXPECT_EQ(after.uploads->texture_uploads, 0U);

    EXPECT_EQ(backend->diagnosticCount(vn::graphics::DiagnosticCategory::InitFailed), 0U)
        << "validation reported something on the move path";

    backend->shutdown();
#endif
}

TEST(VkWindowPresentTest, TwoWindowsOnOneDeviceKeepTheirOwnPictures)
{
#if !defined(VN_TEST_HAVE_WINDOW_INSTRUMENT)
    GTEST_SKIP() << "this build has no window instrument (the xcb development package is missing)";
#else
    // ONE DEVICE, SEVERAL WINDOWS (review 8, K3). The reference backend had the tree's only case for this
    // (VsgBackendTest.ADifferentWindowGetsItsOwnTarget), and it asserted target identity plus a counter; the
    // capability the engine promises - a device is not a window, many windows are many engines on ONE device
    // (graphics-architecture.md 2.4, ADR-2026-11-13) - needs a stronger fact: two live swapchains, and each
    // window keeps the picture ITS OWN frame drew.
    TestDisplay display;
    if (!display.open()) {
        GTEST_SKIP() << display.skipReason();
    }
    TestHostWindow first(display, kWindowWidth, kWindowHeight);
    TestHostWindow second(display, kWindowWidth, kWindowHeight);
    if (first.handle() == nullptr || second.handle() == nullptr) {
        GTEST_SKIP() << "two host windows could not be created on this desktop";
    }
    ASSERT_TRUE(first.wasViewableAtCreation() && second.wasViewableAtCreation())
        << "the fixture waits for a viewable window before a case presents anything";

    // The instrument check on BOTH windows: a desktop that cannot read one of them leaves the pixel half
    // unmeasured rather than failed (see AWindowFrameIsDrawnAndPresented).
    paintInstrument(display, first);
    paintInstrument(display, second);
    const auto white = [](const std::array<std::uint8_t, 3>& pixel) {
        return pixel[0] > 200 && pixel[1] > 200 && pixel[2] > 200;
    };
    const std::array<std::uint8_t, 3> first_instrument =
        first.waitForPixel(kWindowWidth / 2, kWindowHeight / 2, white, std::chrono::milliseconds{ 1000 });
    const std::array<std::uint8_t, 3> second_instrument =
        second.waitForPixel(kWindowWidth / 2, kWindowHeight / 2, white, std::chrono::milliseconds{ 1000 });
    const bool can_read_pixels =
        first.readError() == 0 && second.readError() == 0 && white(first_instrument) && white(second_instrument);

    const DeviceDesc desc{ DevicePresentation::Handle, first.handle() };
    auto             backend = RenderBackendRegistry::instance().create(u8"vk", desc);
    ASSERT_NE(backend, nullptr) << "the factory did not answer the vk backend";

    // TWO TARGETS, each on its own handle, both asked for BEFORE initialize() (a surface is a creation input).
    SurfaceTargetResult result = SurfaceTargetResult::NoPresentation;
    const vn::raw_ptr<vn::graphics::WindowPresentTarget> first_target =
        backend->createSurfaceTarget(first.handle(), &result);
    ASSERT_NE(first_target, nullptr) << "createSurfaceTarget answered " << static_cast<int>(result);
    EXPECT_EQ(result, SurfaceTargetResult::Ok);
    const vn::raw_ptr<vn::graphics::WindowPresentTarget> second_target =
        backend->createSurfaceTarget(second.handle(), &result);
    ASSERT_NE(second_target, nullptr) << "the second window was refused: " << static_cast<int>(result);
    EXPECT_EQ(result, SurfaceTargetResult::Ok);
    EXPECT_NE(first_target, second_target) << "two windows are two targets, not one target twice";
    EXPECT_EQ(first_target->nativeHandle(), first.handle());
    EXPECT_EQ(second_target->nativeHandle(), second.handle());

    if (!backend->initialize()) {
        GTEST_SKIP() << "this machine has no Vulkan device the backend can come up on";
    }
    ASSERT_TRUE(backend->capabilities().presentation) << "a Handle device that came up must present";

    // A CARRIES MAGENTA, B CARRIES GREEN (the triangle is green on both, so only the clear tells them apart).
    EXPECT_EQ(presentUntilReady(*backend, windowFrame(Colorf(1.0F, 0.0F, 1.0F, 1.0F)), first_target, 40),
              PresentOutcome::Presented);
    EXPECT_EQ(presentUntilReady(*backend, windowFrame(Colorf(0.0F, 1.0F, 0.0F, 1.0F)), second_target, 40),
              PresentOutcome::Presented);

    // THE ORDER IS THE POINT: the first window is read AFTER the second presented. One swapchain shared by both
    // windows would show the second window's picture on the first - green in the corner, where the first
    // window's own pass cleared magenta.
    if (!can_read_pixels) {
        std::fprintf(stderr,
                     "[window test] the instruments read rgb=%u,%u,%u / %u,%u,%u for a plain white fill; the pixel "
                     "half is skipped\n",
                     static_cast<unsigned>(first_instrument[0]),
                     static_cast<unsigned>(first_instrument[1]),
                     static_cast<unsigned>(first_instrument[2]),
                     static_cast<unsigned>(second_instrument[0]),
                     static_cast<unsigned>(second_instrument[1]),
                     static_cast<unsigned>(second_instrument[2]));
    }
    else {
        const auto magenta = [](const std::array<std::uint8_t, 3>& pixel) {
            return pixel[0] > 200 && pixel[1] < 40 && pixel[2] > 200;
        };
        const auto green = [](const std::array<std::uint8_t, 3>& pixel) {
            return pixel[0] < 40 && pixel[1] > 200 && pixel[2] < 40;
        };
        const std::array<std::uint8_t, 3> first_centre =
            first.waitForPixel(kWindowWidth / 2, kWindowHeight / 2, green, std::chrono::milliseconds{ 1000 });
        const std::array<std::uint8_t, 3> first_corner =
            first.waitForPixel(2, 2, magenta, std::chrono::milliseconds{ 1000 });
        const std::array<std::uint8_t, 3> second_corner =
            second.waitForPixel(2, 2, green, std::chrono::milliseconds{ 1000 });
        EXPECT_TRUE(green(first_centre)) << "the first window's triangle did not reach its centre";
        EXPECT_TRUE(magenta(first_corner))
            << "the first window no longer shows its OWN pass' clear after the second presented (rgb = "
            << static_cast<unsigned>(first_corner[0]) << ", " << static_cast<unsigned>(first_corner[1]) << ", "
            << static_cast<unsigned>(first_corner[2]) << ")";
        EXPECT_TRUE(green(second_corner)) << "the second window is not its own pass' clear (rgb = "
                                          << static_cast<unsigned>(second_corner[0]) << ", "
                                          << static_cast<unsigned>(second_corner[1]) << ", "
                                          << static_cast<unsigned>(second_corner[2]) << ")";
    }

    // ONE LIVE SWAPCHAIN PER WINDOW: both handles stay occupied, and the second ask is refused for each.
    EXPECT_EQ(backend->createSurfaceTarget(first.handle(), &result), nullptr);
    EXPECT_EQ(result, SurfaceTargetResult::WindowOccupied);
    EXPECT_EQ(backend->createSurfaceTarget(second.handle(), &result), nullptr);
    EXPECT_EQ(result, SurfaceTargetResult::WindowOccupied);
    EXPECT_TRUE(backend->initialized()) << "serving a second window is not a device stop";

    EXPECT_EQ(backend->diagnosticCount(vn::graphics::DiagnosticCategory::InitFailed), 0U)
        << "validation reported something on the two-window path";

    backend->shutdown();
#endif
}

TEST(VkWindowPresentTest, ADeviceWithNoPresentationRefusesAWindow)
{
    auto backend = RenderBackendRegistry::instance().create(u8"vk", DeviceDesc{ DevicePresentation::None, nullptr });
    ASSERT_NE(backend, nullptr);
    if (!backend->initialize()) {
        GTEST_SKIP() << "this machine has no Vulkan device the backend can come up on";
    }
    EXPECT_FALSE(backend->capabilities().presentation);

    SurfaceTargetResult why = SurfaceTargetResult::Ok;
    EXPECT_EQ(backend->createSurfaceTarget(reinterpret_cast<void*>(static_cast<std::uintptr_t>(1U)), &why), nullptr);
    EXPECT_EQ(why, SurfaceTargetResult::NoPresentation) << "a display-less device refuses silently, not with a report";

    backend->shutdown();
}
