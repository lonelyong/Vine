#include <gtest/gtest.h>

#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

#include <vine/graphics/RenderBackend.hpp>
#include <vine/graphics/RenderCommand.hpp>
#include <vine/graphics/RenderDiagnostic.hpp>
#include <vine/graphics/RenderPass.hpp>
#include <vine/graphics/RenderTarget.hpp>
#include <vine/vsg/api/BackendContent.hpp>
#include <vine/vsg/api/ContentAssembly.hpp>
#include <vine/vsg/api/ContentHalves.hpp>
#include <vine/vsg/api/ContentSets.hpp>
#include <vine/vsg/api/DeviceProbe.hpp>
#include <vine/vsg/api/VsgBackend.hpp>

#include <vine/Buffer.hpp>
#include <vine/graphics/Camera.hpp>
#include <vine/graphics/DepthMode.hpp>
#include <vine/graphics/Geometry.hpp>
#include <vine/graphics/Material.hpp>
#include <vine/graphics/ShaderProgram.hpp>

#include "TestHostWindow.hpp"

using vine::graphics::Geometry;
using vine::graphics::Material;
using vine::graphics::RenderCommand;
using vine::graphics::RenderPass;
using vine::graphics::RenderTarget;
using vine::graphics::ShaderProgram;
using vine::graphics::ShaderStage;
using vine::graphics::ShaderStageType;
using vine::vsg::ContentAssembly;
using vine::vsg::PassRegistry;
using vine::vsg::VsgBackend;
using vine::vsg::api::probePhysicalDevices;
using vine::vsg::detail::BackendContentAccess;

namespace
{

/// @brief Prints a `vine::String` (UTF-8 bytes) as the bytes it holds.
std::string as_bytes(const vine::String& text)
{
    return std::string(reinterpret_cast<const char*>(text.data()), text.size());
}

/// @brief Whether a device case can run at all (a window system and one usable physical device).
bool deviceCaseAvailable()
{
    if (std::getenv("DISPLAY") == nullptr)
    {
        return false;
    }
    const auto probed = probePhysicalDevices();
    return probed.ok && probed.usableCount() != 0;
}

/// @brief Whether @p byte is the display server's spelling of @p value.
///
/// Both the linear value and its sRGB encoding count: the surface's colour space is the platform's, and the
/// two outer bytes are read as a set rather than as named channels (the same helper SessionContentTest
/// reads its window with).
bool isColourByte(std::uint8_t byte, double value)
{
    const double encoded = value <= 0.0031308 ? 12.92 * value : 1.055 * std::pow(value, 1.0 / 2.4) - 0.055;
    const double actual  = static_cast<double>(byte);
    return std::abs(actual - 255.0 * value) <= 6.0 || std::abs(actual - 255.0 * encoded) <= 6.0;
}

/// @brief Whether a pixel is the pass' clear colour: linear (0, 0.25, 0), in either colour space.
bool isGreenClear(const std::array<std::uint8_t, 3>& pixel)
{
    return isColourByte(pixel[1], 0.25) && isColourByte(pixel[0], 0.0) && isColourByte(pixel[2], 0.0);
}

/// @brief A camera looking at (x, 0, 0) from (x, 0, 1.5), through an orthographic window of [-1, 1] squared.
///
/// The world x in [-0.4, 0.4] then lands HALF A UNIT to one side of the camera's centre, which is how the
/// content case moves its triangle between the window's halves (see SessionContentTest for the same pair).
vine::intrusive_ptr<vine::graphics::Camera> cameraLookingAt(double x)
{
    vine::intrusive_ptr<vine::graphics::Camera> camera(new vine::graphics::Camera());
    camera->setViewMatrixAsLookAt(vine::math::Vec3d(x, 0.0, 1.5), vine::math::Vec3d(x, 0.0, 0.0),
                                  vine::math::Vec3d(0.0, 1.0, 0.0));
    camera->setProjectionMatrixAsOrtho(-1.0, 1.0, -1.0, 1.0, 0.5, 4.0);
    return camera;
}

}  // namespace

TEST(VsgBackendTest, TheSdkFacingBackendComesUpPresentsEmptyFramesAndSaysWhatItCannotServe)
{
    if (!deviceCaseAvailable())
    {
        GTEST_SKIP() << "no window system or no device satisfies the requirements";
    }

    vine::intrusive_ptr<VsgBackend> backend(new VsgBackend());
    std::size_t                     seen = 0;
    backend->setDiagnosticSink([&seen](const vine::graphics::RenderDiagnostic& diagnostic) {
        ++seen;
        std::printf("[facade] diagnostic: severity=%d category=%d message=%s\n",
                    static_cast<int>(diagnostic.severity), static_cast<int>(diagnostic.category),
                    as_bytes(diagnostic.message).c_str());
    });

    // 1. Nothing is up before initialize(), and a frame asked for anyway SAYS so instead of vanishing.
    EXPECT_FALSE(backend->initialized());
    EXPECT_EQ(backend->nativeHandle(), nullptr) << "this backend owns its window: there is no host surface";
    backend->beginFrame();
    backend->endFrame();
    backend->swapBuffers();
    EXPECT_EQ(backend->framesPresented(), 0U);
    EXPECT_EQ(backend->diagnosticCount(), 1U) << "one report for the frame that had no session";
    EXPECT_EQ(seen, backend->diagnosticCount()) << "the sink sees every counted diagnostic";

    // 2. The session comes up, and an EMPTY frame is still a frame that has to be presented: opening a frame
    // may acquire an image, and the present is the only call that hands it back (see api/Session).
    ASSERT_TRUE(backend->initialize());
    EXPECT_TRUE(backend->initialized());
    for (int index = 0; index < 3; ++index)
    {
        backend->beginFrame();
        backend->endFrame();
        backend->swapBuffers();
    }
    EXPECT_EQ(backend->framesPresented(), 3U);
    EXPECT_EQ(backend->deviceWaits(), 0U) << "the frame path never stops the device";
    EXPECT_EQ(backend->diagnosticCount(), 1U) << "a healthy session reports nothing";

    // 3. What is still NOT served says so, once per entry point: a backend that dropped it in silence would
    // be a working-looking black screen, which is the one answer the SDK forbids. (The pass protocol and
    // the content drawing ARE served now - see the content case below.)
    const vine::intrusive_ptr<RenderTarget> target(new RenderTarget());
    std::vector<std::uint8_t>               pixels;
    std::vector<float>                      depths;
    const std::size_t                       before_unserved = backend->diagnosticCount();
    backend->setRenderTarget(target.get());    // an off-screen target (and its attachments) is not served
    backend->setPassInputs({ target.get() });  // nor are the pass inputs that read one
    backend->drawScreenProgram(target.get(), nullptr, nullptr);
    vine::graphics::ReadbackResult why = vine::graphics::ReadbackResult::Ok;
    EXPECT_FALSE(backend->readColorBuffer(target.get(), 0, pixels, &why));
    EXPECT_EQ(why, vine::graphics::ReadbackResult::Unsupported);
    EXPECT_FALSE(backend->readDepthBuffer(target.get(), depths, &why));
    EXPECT_EQ(backend->diagnosticCount(), before_unserved + 5U)
        << "an off-screen target, its declared inputs, a screen draw and the two readbacks report once each";
    backend->setRenderTarget(target.get());
    backend->setPassInputs({ target.get() });
    backend->drawScreenProgram(target.get(), nullptr, nullptr);
    (void)backend->readColorBuffer(target.get(), 0, pixels);
    (void)backend->readDepthBuffer(target.get(), depths);
    EXPECT_EQ(backend->diagnosticCount(), before_unserved + 5U) << "a repeated call is the same episode";
    EXPECT_EQ(seen, backend->diagnosticCount());
    EXPECT_FALSE(backend->supportsRenderTargets())
        << "declining is how the engine knows before it stages off-screen work";

    // 4. The protocol still judges: a DRAWING call with no scope open has nothing to belong to, so the
    // plan's recorder refuses it and says so - once per frame, because a host that lost its scopes hits this
    // on every pass. The frame is still a frame (it presents), it just has nothing in it.
    backend->beginFrame();
    backend->render({}, nullptr);
    backend->render({}, nullptr);
    backend->endFrame();
    backend->swapBuffers();
    EXPECT_EQ(backend->diagnosticCount(), before_unserved + 5U + 1U)
        << "the first scope-less draw is refused out loud, the second is the same episode";
    EXPECT_EQ(backend->framesPresented(), 4U) << "a frame whose draws were all refused still presents";

    // 5. The announced size is the SURFACE's, and this backend owns the surface it created: the announcement
    // is applied by the next initialize() (the window comes up at it) and reported while the session is live.
    backend->resize(320, 180);
    EXPECT_EQ(backend->diagnosticCount(), before_unserved + 5U + 1U + 1U)
        << "a live surface keeps its size for now, and says so";

    ASSERT_TRUE(backend->initialize()) << "re-initializing tears the old session down first (the SDK's contract)";
    EXPECT_EQ(backend->framesPresented(), 0U) << "a fresh session has presented nothing";
    backend->beginFrame();
    backend->endFrame();
    backend->swapBuffers();
    EXPECT_EQ(backend->framesPresented(), 1U);

    // 6. shutdown() is safe twice and leaves the backend initializable again (the SDK's contract for a
    // recreated surface).
    backend->shutdown();
    EXPECT_FALSE(backend->initialized());
    backend->shutdown();
    ASSERT_TRUE(backend->initialize());
    backend->beginFrame();
    backend->endFrame();
    backend->swapBuffers();
    EXPECT_EQ(backend->framesPresented(), 1U);
    backend->shutdown();
}

#if !defined(_WIN32)

TEST(VsgBackendTest, AHostSurfaceIsAdoptedAndMovingToTheNextOneKeepsTheSession)
{
    if (!deviceCaseAvailable())
    {
        GTEST_SKIP() << "no window system or no device satisfies the requirements";
    }

    int               screen_index = 0;
    xcb_connection_t* connection   = xcb_connect(nullptr, &screen_index);
    if (connection == nullptr || xcb_connection_has_error(connection) != 0)
    {
        GTEST_SKIP() << "no X display to create a host window on";
    }
    xcb_screen_t* screen = xcb_setup_roots_iterator(xcb_get_setup(connection)).data;
    ASSERT_NE(screen, nullptr);

    TestHostWindow host(connection, screen, 128, 96);

    vine::intrusive_ptr<VsgBackend> backend(new VsgBackend());
    backend->setDiagnosticSink([](const vine::graphics::RenderDiagnostic& diagnostic) {
        std::printf("[facade] diagnostic: severity=%d category=%d message=%s\n",
                    static_cast<int>(diagnostic.severity), static_cast<int>(diagnostic.category),
                    as_bytes(diagnostic.message).c_str());
    });

    // The handle is announced BEFORE initialize() (the SDK's order) and adopted there: the SDK's question
    // "which surface am I on" then has an answer, and the host's window survives the session (a backend that
    // destroyed it would be a backend drawing into somebody else's memory).
    backend->setWindowHandle(host.handle());
    ASSERT_TRUE(backend->initialize());
    EXPECT_EQ(backend->nativeHandle(), host.handle());
    for (int index = 0; index < 2; ++index)
    {
        backend->beginFrame();
        backend->endFrame();
        backend->swapBuffers();
    }
    EXPECT_EQ(backend->framesPresented(), 2U);
    EXPECT_TRUE(host.alive());

    // Re-announcing a DIFFERENT handle follows the host onto its recreated window: the session keeps the
    // device and every compiled pipeline (core::planSessionMove), so the presented count goes ON instead of
    // starting over - which is exactly what a rebuilt session would do.
    TestHostWindow second(connection, screen, 128, 96);
    backend->setWindowHandle(second.handle());
    ASSERT_TRUE(backend->initialize());
    EXPECT_EQ(backend->nativeHandle(), second.handle());
    backend->beginFrame();
    backend->endFrame();
    backend->swapBuffers();
    EXPECT_EQ(backend->framesPresented(), 3U) << "a move keeps the session: the frames it presented are still there";
    EXPECT_TRUE(host.alive()) << "the window the session moved OFF is the host's, and stays the host's";
    EXPECT_TRUE(second.alive());

    backend->shutdown();
    EXPECT_TRUE(host.alive());
    EXPECT_TRUE(second.alive()) << "shutdown releases the session, never the host's window";
}

TEST(VsgBackendTest, TheSdkPassProtocolDrawsContentIntoTheWindow)
{
    if (!deviceCaseAvailable())
    {
        GTEST_SKIP() << "no window system or no device satisfies the requirements";
    }

    int               screen_index = 0;
    xcb_connection_t* connection   = xcb_connect(nullptr, &screen_index);
    if (connection == nullptr || xcb_connection_has_error(connection) != 0)
    {
        GTEST_SKIP() << "no X display to create a host window on";
    }
    xcb_screen_t* screen = xcb_setup_roots_iterator(xcb_get_setup(connection)).data;
    ASSERT_NE(screen, nullptr);

    constexpr int kWidth  = 128;
    constexpr int kHeight = 96;
    TestHostWindow host(connection, screen, kWidth, kHeight);

    vine::intrusive_ptr<VsgBackend> backend(new VsgBackend());
    std::size_t                     seen = 0;
    backend->setDiagnosticSink([&seen](const vine::graphics::RenderDiagnostic& diagnostic) {
        ++seen;
        std::printf("[facade] diagnostic: severity=%d category=%d message=%s\n",
                    static_cast<int>(diagnostic.severity), static_cast<int>(diagnostic.category),
                    as_bytes(diagnostic.message).c_str());
    });
    backend->setWindowHandle(host.handle());
    ASSERT_TRUE(backend->initialize());
    EXPECT_EQ(backend->diagnosticCount(), 0U) << "a healthy session brings the content world up silently";

    // The content the frames draw: a triangle under a view-block program (the same pair the session's own
    // content case drives), shaded so the picture itself says what the plan carried - the camera's x in the
    // green byte, the target's extent in the outer two.
    const vine::intrusive_ptr<ShaderProgram> program(new ShaderProgram());
    {
        ShaderStage vertex;
        vertex.type   = ShaderStageType::Vertex;
        vertex.source = vine::String(reinterpret_cast<const char8_t*>(
            "layout(location = 0) in vec3 position;\n"
            "layout(set = 0, binding = 0, std140) uniform VineViewBlock {\n"
            "    mat4 view; mat4 inv_view; mat4 proj; mat4 view_proj; vec4 cam_pos; vec4 frame; } vb;\n"
            "void main() { gl_Position = vb.view_proj * vec4(position, 1.0); }\n"));
        ShaderStage fragment;
        fragment.type   = ShaderStageType::Fragment;
        fragment.source = vine::String(reinterpret_cast<const char8_t*>(
            "layout(location = 0) out vec4 outColor;\n"
            "layout(set = 0, binding = 0, std140) uniform VineViewBlock {\n"
            "    mat4 view; mat4 inv_view; mat4 proj; mat4 view_proj; vec4 cam_pos; vec4 frame; } vb;\n"
            "void main() { outColor = vec4(vb.frame.y / 128.0, abs(vb.cam_pos.x), vb.frame.z / 96.0, 1.0); }\n"));
        program->addStage(vertex);
        program->addStage(fragment);
    }

    const vine::intrusive_ptr<Geometry> geometry(new Geometry());
    const vine::intrusive_ptr<vine::Buffer<float>> positions = vine::intrusive_ptr<vine::Buffer<float>>(
        new vine::Buffer<float>(std::vector<float>{ -0.4F, -0.4F, 0.5F, 0.4F, -0.4F, 0.5F, 0.0F, 0.6F, 0.5F }));
    const vine::intrusive_ptr<vine::Buffer<std::uint32_t>> indices =
        vine::intrusive_ptr<vine::Buffer<std::uint32_t>>(
            new vine::Buffer<std::uint32_t>(std::vector<std::uint32_t>{ 0U, 1U, 2U }));
    geometry->setPositions(positions);
    geometry->setIndices(indices);
    geometry->setRevision(1U);

    const vine::intrusive_ptr<Material> material(new Material());
    material->setDiffuse(vine::Colorf(0.2F, 0.3F, 0.4F, 1.0F));

    RenderCommand command;
    command.geometry = geometry;
    command.material = material;
    command.program  = program;
    const std::vector<RenderCommand> commands{ command };

    const vine::intrusive_ptr<vine::graphics::Camera> camera_a = cameraLookingAt(0.5);
    const vine::intrusive_ptr<vine::graphics::Camera> camera_b = cameraLookingAt(-0.5);
    const vine::intrusive_ptr<RenderPass>             pass(new RenderPass());

    const vine::graphics::ClearPolicy clear{ vine::Color(0, 64, 0, 255), true };

    const auto drive = [&](const vine::graphics::Camera* frame_camera) {
        backend->beginFrame();
        backend->beginPass(pass.get());
        backend->setPassOrder(0);
        backend->setRenderTarget(nullptr);
        backend->setClearPolicy(clear);
        backend->setDepthMode(vine::graphics::DepthMode::TestAndWrite);
        backend->render(commands, frame_camera);
        backend->endPass();
        backend->endFrame();
        backend->swapBuffers();
    };
    // A presentation reaches the display server asynchronously, so a picture is read after a couple of
    // plan-free frames re-present it (the same settle SessionContentTest uses).
    const auto settle = [&] {
        for (int index = 0; index < 2; ++index)
        {
            backend->beginFrame();
            backend->endFrame();
            backend->swapBuffers();
        }
    };

    // 1. The engine's PRE-FRAME WARM-UP: every enabled non-clearing pass executes once with NO frame open so
    // a backend can prepare its retained state. That is designed, not a protocol error - nothing is
    // collected and nothing is reported - and the pass' identity survives it into the frames that follow.
    backend->beginPass(pass.get());
    backend->setPassOrder(-1);
    backend->setRenderTarget(nullptr);
    backend->setClearPolicy(clear);
    backend->setDepthMode(vine::graphics::DepthMode::TestAndWrite);
    backend->render(commands, camera_a.get());
    backend->endPass();
    EXPECT_EQ(backend->diagnosticCount(), 0U) << "a warm-up is designed, not a protocol error";
    EXPECT_EQ(backend->framesPresented(), 0U) << "it draws nothing: no frame was open";
    EXPECT_EQ(seen, 0U);

    PassRegistry&    registry = BackendContentAccess::passes(*backend);
    ContentAssembly* assembly = BackendContentAccess::assembly(*backend);
    ASSERT_NE(assembly, nullptr) << "a live session owns the content world its frames assemble";
    EXPECT_TRUE(registry.contains(pass.get())) << "the warm-up's pass is known before any frame";
    EXPECT_EQ(registry.live(), 1U);

    // 2. A real frame, the SDK's order: the triangle through camera A lands LEFT of centre (the camera looks
    // half a unit to its right). Every object the command names is tracked when the call arrives, so the
    // tables answer for all of them when the plan records.
    drive(camera_a.get());
    EXPECT_EQ(backend->framesPresented(), 1U);
    EXPECT_EQ(backend->diagnosticCount(), 0U) << "the pass protocol and the content path are served now";
    const auto& recorded = BackendContentAccess::executor(*backend).recorded();
    ASSERT_EQ(recorded.size(), 1U);
    EXPECT_EQ(recorded[0], registry.adopt(pass.get()))
        << "the frame records the pass the warm-up announced, under the same identity";

    settle();
    const auto left_a  = host.pixel(kWidth / 4, kHeight / 2);      // inside the triangle
    const auto right_a = host.pixel(3 * kWidth / 4, kHeight / 2);  // never: the clear
    EXPECT_TRUE(isColourByte(left_a[1], 0.5))
        << "the camera's x reached the fragment stage through the view block the facade built: got ("
        << static_cast<int>(left_a[0]) << ", " << static_cast<int>(left_a[1]) << ", "
        << static_cast<int>(left_a[2]) << ")";
    EXPECT_TRUE(isColourByte(left_a[0], 1.0)) << "and the window's width came with it";
    EXPECT_TRUE(isColourByte(left_a[2], 1.0)) << "and so did its height";
    EXPECT_TRUE(isGreenClear(right_a)) << "the right quarter is the pass' clear, so the view really moved it";

    // 3. A second frame draws the same content: the content world reuses what it built (the halves and the
    // declared sets are keyed, so a steady frame builds nothing) and the frame still records.
    const std::uint64_t half_builds = assembly->halves().builds();
    const std::size_t   set_count   = assembly->sets().sets();
    drive(camera_a.get());
    EXPECT_EQ(assembly->halves().builds(), half_builds) << "a steady frame builds no half";
    EXPECT_EQ(assembly->sets().sets(), set_count) << "and its declared set is reused";

    // 4. A third frame MOVES the picture (camera B puts the triangle in the right half). What the last frame
    // recorded is REPLACED, not accumulated - if it stacked, this frame would draw both pictures and the left
    // quarter would still show the old triangle (see WindowTarget::beginFrame).
    drive(camera_b.get());
    settle();
    const auto left_b  = host.pixel(kWidth / 4, kHeight / 2);
    const auto right_b = host.pixel(3 * kWidth / 4, kHeight / 2);
    EXPECT_TRUE(isGreenClear(left_b)) << "the previous frame's picture is gone, got ("
                                      << static_cast<int>(left_b[0]) << ", " << static_cast<int>(left_b[1])
                                      << ", " << static_cast<int>(left_b[2]) << ")";
    EXPECT_TRUE(isColourByte(right_b[1], 0.5))
        << "and this frame's landed where its camera put it: got (" << static_cast<int>(right_b[0])
        << ", " << static_cast<int>(right_b[1]) << ", " << static_cast<int>(right_b[2]) << ")";
    EXPECT_EQ(backend->deviceWaits(), 0U) << "the content path never stops the device";

    // 5. The pass identity is this layer's bookkeeping: the SDK's releasePass() forgets it (nothing retained
    // is keyed by a pass yet), and a re-announced object is a NEW pass - the plan's numbers are never
    // re-issued, because what they keyed may still be remembered elsewhere (see api/PassRegistry).
    const vine::vsg::core::PassId first_id = registry.adopt(pass.get());
    backend->releasePass(pass.get());
    EXPECT_FALSE(registry.contains(pass.get()));
    EXPECT_EQ(registry.live(), 0U);
    backend->beginPass(pass.get());
    EXPECT_NE(registry.adopt(pass.get()), first_id) << "a re-announced pass gets a new identity";

    backend->shutdown();
    EXPECT_TRUE(host.alive());
}

#endif  // !defined(_WIN32)
