/**
 * @brief M5's acceptance: a frame that names a HOST window is drawn into its swapchain and the platform accepts it.
 *
 * What this pins:
 *   - `DeviceDesc{Handle, window}` plus `createSurfaceTarget` is the SDK's window path: the target exists before
 *     `initialize()` (a Vulkan surface is a creation input), and `capabilities().presentation` says so;
 *   - `executeFrame(frame, target)` draws a pass whose `target` is null into the acquired swapchain image and returns
 *     `Presented`; a later frame after `resize()` presents again (the swapchain follows the surface);
 *   - the WINDOW's pixels are what the frame drew: after the present, the test reads the window with XCB (its own
 *     measuring instrument, never the backend's) and compares the centre with the triangle's colour.
 *
 * The cases skip when there is no Vulkan device, no X display, or the test was built without xcb. The plugin itself
 * never links xcb: it opens `libxcb.so.1` with dlopen for the surface connection.
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
#include <vine/graphics/CameraSnapshot.hpp>
#include <vine/graphics/FrameDescription.hpp>
#include <vine/graphics/Geometry.hpp>
#include <vine/graphics/Primitive.hpp>
#include <vine/graphics/RenderBackendRegistry.hpp>
#include <vine/graphics/RenderCommand.hpp>
#include <vine/graphics/RenderTarget.hpp>
#include <vine/graphics/ShaderProgram.hpp>
#include <vine/math/Matrix4x4.hpp>

#if defined(VN_TEST_HAVE_XCB)
#    include <xcb/xcb.h>
#endif

using vn::Colorf;
using vn::String;
using vn::graphics::CameraSnapshot;
using vn::graphics::ClearPolicy;
using vn::graphics::DeviceDesc;
using vn::graphics::DevicePresentation;
using vn::graphics::DrawDescription;
using vn::graphics::FrameDescription;
using vn::graphics::Geometry;
using vn::graphics::PassDraw;
using vn::graphics::PresentOutcome;
using vn::graphics::Primitive;
using vn::graphics::RenderBackendRegistry;
using vn::graphics::RenderCommand;
using vn::graphics::RenderTarget;
using vn::graphics::ShaderProgram;
using vn::graphics::ShaderStage;
using vn::graphics::ShaderStageType;
using vn::graphics::SurfaceTargetResult;
using vn::graphics::Topology;
using vn::intrusive_ptr;

namespace
{

#if defined(VN_TEST_HAVE_XCB)

constexpr int kWindowWidth  = 128;
constexpr int kWindowHeight = 96;

/// @brief A test-owned X11 window: the measuring instrument the backend must not know about.
struct TestWindow {
    xcb_connection_t* connection{ nullptr };
    xcb_window_t      window{ 0 };
    xcb_screen_t*     screen{ nullptr };

    TestWindow() = default;
    TestWindow(const TestWindow&) = delete;
    TestWindow& operator=(const TestWindow&) = delete;

    ~TestWindow()
    {
        if (connection != nullptr) {
            if (window != 0) {
                xcb_destroy_window(connection, window);
            }
            xcb_disconnect(connection);
        }
    }

    [[nodiscard]] bool create()
    {
        connection = xcb_connect(nullptr, nullptr);
        if (connection == nullptr || xcb_connection_has_error(connection) != 0) {
            return false;
        }
        xcb_screen_iterator_t iterator = xcb_setup_roots_iterator(xcb_get_setup(connection));
        screen                         = iterator.data;
        if (screen == nullptr) {
            return false;
        }
        window = xcb_generate_id(connection);
        const std::uint32_t mask   = XCB_CW_BACK_PIXEL | XCB_CW_EVENT_MASK;
        const std::uint32_t values[]{ screen->black_pixel, XCB_EVENT_MASK_EXPOSURE };
        xcb_create_window(connection,
                          XCB_COPY_FROM_PARENT,
                          window,
                          screen->root,
                          0,
                          0,
                          kWindowWidth,
                          kWindowHeight,
                          0,
                          XCB_WINDOW_CLASS_INPUT_OUTPUT,
                          screen->root_visual,
                          mask,
                          values);
        xcb_map_window(connection, window);
        xcb_flush(connection);
        return true;
    }

    /** @brief The window handle as the SDK carries it (`xcb_window_t` widened to a `void*`). */
    [[nodiscard]] void* handle() const noexcept
    {
        return reinterpret_cast<void*>(static_cast<std::uintptr_t>(window));
    }

    /** @brief Reads one pixel as (r, g, b) from the window, or false when the read is refused. */
    [[nodiscard]] bool pixel(int x, int y, std::array<int, 3>& out)
    {
        const xcb_get_image_cookie_t cookie = xcb_get_image(connection, XCB_IMAGE_FORMAT_Z_PIXMAP, window, x, y, 1U, 1U, ~0U);
        xcb_get_image_reply_t*      reply  = xcb_get_image_reply(connection, cookie, nullptr);
        if (reply == nullptr) {
            return false;
        }
        const std::uint8_t* data = xcb_get_image_data(reply);
        const int           bpp  = xcb_get_image_data_length(reply) / 1; // one pixel
        if (bpp >= 4) {
            // 24/32-bit TrueColor: B, G, R, X
            out = { data[2], data[1], data[0] };
        }
        else if (bpp >= 3) {
            out = { data[0], data[1], data[2] };
        }
        else {
            free(reply);
            return false;
        }
        free(reply);
        return true;
    }
};

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

/// @brief A frame with ONE window pass (target null) that clears black and draws the triangle.
[[nodiscard]] FrameDescription windowFrame()
{
    const intrusive_ptr<ShaderProgram> program  = makeProgram();
    const intrusive_ptr<Geometry>      geometry = makeTriangle();

    RenderCommand command(geometry, nullptr, vn::math::Mat4d{});
    command.program             = program;
    command.program_description = program->description();

    auto commands = std::make_shared<std::vector<RenderCommand>>();
    commands->push_back(std::move(command));

    DrawDescription draw;
    draw.kind     = DrawDescription::Kind::Scene;
    draw.camera   = identityCamera();
    draw.commands = commands;

    FrameDescription frame;
    FrameDescription::Pass entry;
    PassDraw pass;
    pass.target       = nullptr; // the window
    pass.clear        = ClearPolicy{};
    pass.clear->color = Colorf(1.0F, 0.0F, 0.0F, 1.0F);
    pass.draws.push_back(std::move(draw));
    entry.draw = std::move(pass);
    frame.passes.push_back(std::move(entry));
    return frame;
}

#endif // VN_TEST_HAVE_XCB

} // namespace

TEST(VkWindowPresentTest, AWindowFrameIsDrawnAndPresented)
{
#if !defined(VN_TEST_HAVE_XCB)
    GTEST_SKIP() << "this build has no xcb development package, so the window test has no measuring instrument";
#else
    TestWindow window;
    if (!window.create()) {
        GTEST_SKIP() << "no X display is reachable through xcb";
    }

    // INSTRUMENT CHECK: paint the window red with plain XCB and read it back. A black answer here means the window
    // system (not the backend) is what the test cannot see, so the pixel half is skipped rather than mislabelled.
    const xcb_gcontext_t gc       = xcb_generate_id(window.connection);
    const std::uint32_t  gc_mask  = XCB_GC_FOREGROUND;
    const std::uint32_t  gc_value = 0x00FF0000U;
    xcb_create_gc(window.connection, gc, window.window, gc_mask, &gc_value);
    const xcb_rectangle_t rectangle{ 0, 0, kWindowWidth, kWindowHeight };
    xcb_poly_fill_rectangle(window.connection, window.window, gc, 1U, &rectangle);
    xcb_flush(window.connection);
    std::array<int, 3> instrument{};
    const bool         instrument_ok = window.pixel(kWindowWidth / 2, kWindowHeight / 2, instrument);
    const bool         can_read_pixels = instrument_ok && instrument[0] > 200;

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

    // THE WINDOW'S PIXELS, read by the TEST: the triangle covers the centre and the corner keeps the clear. A window
    // system that cannot hand back its own painted pixels (the instrument check above) makes this half UNMEASURED,
    // not failed: the present outcome and validation still judge the frame.
    if (!can_read_pixels) {
        std::fprintf(stderr,
                     "[window test] the X instrument reads black for a plain XCB fill (rgb=%d,%d,%d); the pixel half is skipped\n",
                     instrument[0],
                     instrument[1],
                     instrument[2]);
    }
    else {
        std::array<int, 3> centre{};
        std::array<int, 3> corner{};
        bool               have_centre = false;
        bool               have_corner = false;
        for (int attempt = 0; attempt < 40 && !(have_centre && have_corner); ++attempt) {
            have_centre = window.pixel(kWindowWidth / 2, kWindowHeight / 2, centre);
            have_corner = window.pixel(2, 2, corner);
            if (have_centre && have_corner && centre[1] > 200 && corner[0] > 200) {
                break;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(25));
        }
        ASSERT_TRUE(have_centre && have_corner) << "the window could not be read back";
        // The CLEAR is red everywhere the triangle is not, and the FRAGMENT green inside it: that separates "the
        // present is readable" from "the draw is missing" with one picture.
        EXPECT_GT(centre[1], 200) << "the triangle's green did not reach the window's centre pixel (rgb = " << centre[0] << ", " << centre[1] << ", " << centre[2] << ")";
        EXPECT_LT(centre[0], 40);
        EXPECT_GT(corner[0], 200) << "the window's corner is not the pass' clear (rgb = " << corner[0] << ", " << corner[1] << ", " << corner[2] << ")";
    }

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
