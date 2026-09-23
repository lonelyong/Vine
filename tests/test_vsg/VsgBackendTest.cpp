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
#include <vine/vsg/api/ContentStore.hpp>
#include <vine/vsg/api/DeviceProbe.hpp>
#include <vine/vsg/api/HostTargets.hpp>
#include <vine/vsg/api/VsgBackend.hpp>
#include <vine/vsg/api/WindowTarget.hpp>
#include <vine/vsg/core/FrameCompiler.hpp>

#include <vine/Buffer.hpp>
#include <vine/graphics/Camera.hpp>
#include <vine/graphics/DepthMode.hpp>
#include <vine/graphics/BuiltinShaders.hpp>
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
using vine::vsg::ContentStore;
using vine::vsg::core::TargetFacts;
using vine::vsg::core::depthPlan;
using vine::vsg::HostTargets;
using vine::vsg::PassRegistry;
using vine::vsg::VsgBackend;
using vine::vsg::WindowTarget;
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

/// @brief Whether a pixel is saturated red: linear (1, 0, 0), in either colour space and either byte order.
///
/// The byte order is the display server's (a B8G8R8A8 surface delivers red in the LAST byte), so the two
/// outer channels are read as a set - the same convention SessionContentTest's window probes use.
bool isRed(const std::array<std::uint8_t, 3>& pixel)
{
    return isColourByte(pixel[1], 0.0) &&
           ((isColourByte(pixel[0], 1.0) && isColourByte(pixel[2], 0.0)) ||
            (isColourByte(pixel[2], 1.0) && isColourByte(pixel[0], 0.0)));
}

/// @brief A red triangle under a view-block program: the content the off-screen cases draw.
struct TriangleFixture
{
    vine::intrusive_ptr<ShaderProgram> program;
    vine::intrusive_ptr<Geometry>      geometry;
    vine::intrusive_ptr<Material>      material;
    std::vector<RenderCommand>         commands;
};

/// @brief Builds the triangle fixture: a view-block vertex stage and a constant red fragment stage.
TriangleFixture makeTriangle()
{
    TriangleFixture fixture;

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
            "void main() { outColor = vec4(1.0, 0.0, 0.0, 1.0); }\n"));
        program->addStage(vertex);
        program->addStage(fragment);
    }
    fixture.program = program;

    const vine::intrusive_ptr<Geometry> geometry(new Geometry());
    const vine::intrusive_ptr<vine::Buffer<float>> positions = vine::intrusive_ptr<vine::Buffer<float>>(
        new vine::Buffer<float>(std::vector<float>{ -0.4F, -0.4F, 0.5F, 0.4F, -0.4F, 0.5F, 0.0F, 0.6F, 0.5F }));
    const vine::intrusive_ptr<vine::Buffer<std::uint32_t>> indices =
        vine::intrusive_ptr<vine::Buffer<std::uint32_t>>(
            new vine::Buffer<std::uint32_t>(std::vector<std::uint32_t>{ 0U, 1U, 2U }));
    geometry->setPositions(positions);
    geometry->setIndices(indices);
    geometry->setRevision(1U);
    fixture.geometry = geometry;

    const vine::intrusive_ptr<Material> material(new Material());
    material->setDiffuse(vine::Colorf(0.2F, 0.3F, 0.4F, 1.0F));
    fixture.material = material;

    RenderCommand command;
    command.geometry = geometry;
    command.material = material;
    command.program  = program;
    fixture.commands.push_back(command);
    return fixture;
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

    // 3. The readbacks are served now, and their REFUSALS are classified rather than slammed shut: a target
    // this backend has not been told about is NotReady (a later announcement can make it readable), a call
    // with no target is Invalid, and each reason is said ONCE per episode - the request is synchronous, and a
    // caller polling a target that is not ready yet must not be flooded.
    const vine::intrusive_ptr<RenderTarget> target(new RenderTarget());
    std::vector<std::uint8_t>               pixels;
    std::vector<float>                      depths;
    const std::size_t                       before_unserved = backend->diagnosticCount();
    vine::graphics::ReadbackResult why = vine::graphics::ReadbackResult::Ok;
    EXPECT_FALSE(backend->readColorBuffer(target.get(), 0, pixels, &why));
    EXPECT_EQ(why, vine::graphics::ReadbackResult::NotReady) << "this backend was never told about it";
    EXPECT_TRUE(pixels.empty()) << "a refused read leaves the destination untouched";
    EXPECT_FALSE(backend->readDepthBuffer(target.get(), depths, &why));
    EXPECT_EQ(why, vine::graphics::ReadbackResult::NotReady);
    EXPECT_TRUE(depths.empty());
    EXPECT_EQ(backend->diagnosticCount(), before_unserved + 2U) << "one sentence per entry point, once";
    (void)backend->readColorBuffer(target.get(), 0, pixels, &why);
    (void)backend->readDepthBuffer(target.get(), depths, &why);
    EXPECT_EQ(backend->diagnosticCount(), before_unserved + 2U) << "a repeated refusal is the same episode";
    EXPECT_EQ(seen, backend->diagnosticCount());

    EXPECT_FALSE(backend->readColorBuffer(nullptr, 0, pixels, &why));
    EXPECT_EQ(why, vine::graphics::ReadbackResult::Invalid) << "no target was given, so the request is wrong";
    EXPECT_TRUE(backend->supportsRenderTargets()) << "the off-screen half is served now";

    // An off-screen target is HELD from the moment it is announced - even before it can be built, because a
    // host configures a target before it draws into it - and the release announcement drops it, which is what
    // lets the host destroy the object. Holding and releasing report nothing; a readback of a held target
    // with no attachments yet is NotReady, with the target's own episode.
    backend->setRenderTarget(target.get());
    EXPECT_EQ(BackendContentAccess::targets(*backend).live(), 1U);
    EXPECT_FALSE(backend->readColorBuffer(target.get(), 0, pixels, &why));
    EXPECT_EQ(why, vine::graphics::ReadbackResult::NotReady) << "held, but its attachments are not built yet";
    EXPECT_EQ(backend->diagnosticCount(), before_unserved + 3U) << "the held target's own episode";
    (void)backend->readColorBuffer(target.get(), 0, pixels, &why);
    EXPECT_EQ(backend->diagnosticCount(), before_unserved + 3U) << "and a repeat adds nothing";
    backend->releaseRenderTarget(target.get());
    EXPECT_EQ(BackendContentAccess::targets(*backend).live(), 0U);
    EXPECT_EQ(backend->diagnosticCount(), before_unserved + 3U) << "holding and releasing a target is served";

    // 4. The protocol still judges: a DRAWING call with no scope open has nothing to belong to, so the
    // plan's recorder refuses it and says so - once per frame, because a host that lost its scopes hits this
    // on every pass. The frame is still a frame (it presents), it just has nothing in it.
    backend->beginFrame();
    backend->render({}, nullptr);
    backend->render({}, nullptr);
    backend->endFrame();
    backend->swapBuffers();
    EXPECT_EQ(backend->diagnosticCount(), before_unserved + 3U + 1U)
        << "the first scope-less draw is refused out loud, the second is the same episode";
    EXPECT_EQ(backend->framesPresented(), 4U) << "a frame whose draws were all refused still presents";

    // 5. The announced size is the SURFACE's, and this backend FOLLOWS the surface it is on (the SDK's
    // authority order: surface > announcement > default). Following re-reads the window and rebuilds its
    // swapchain, which stops the device once - the counter says so - and nothing is reported, because a live
    // size event is served now. The announcement is what the NEXT initialize() creates a window at, which is
    // the only way this layer can apply it to a window of its own (see the registered limit in the design
    // notes).
    backend->resize(320, 180);
    EXPECT_EQ(backend->diagnosticCount(), before_unserved + 3U + 1U) << "a live resize is served now";
    EXPECT_EQ(backend->deviceWaits(), 1U) << "following the surface rebuilds the swapchain, and that stops it";

    ASSERT_TRUE(backend->initialize()) << "re-initializing tears the old session down first (the SDK's contract)";
    EXPECT_EQ(backend->framesPresented(), 0U) << "a fresh session has presented nothing";
    WindowTarget* window = BackendContentAccess::windowTarget(*backend);
    ASSERT_NE(window, nullptr);
    EXPECT_EQ(window->width(), 320U) << "the announcement is what the new session's window came up at";
    EXPECT_EQ(window->height(), 180U);
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

TEST(VsgBackendTest, TheSdkOffscreenTargetIsDrawnIntoSampledAndRebuilt)
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
    std::size_t                     seen     = 0;
    std::vector<std::string>        messages;
    backend->setDiagnosticSink([&seen, &messages](const vine::graphics::RenderDiagnostic& diagnostic) {
        ++seen;
        messages.push_back(as_bytes(diagnostic.message));
        std::printf("[facade] diagnostic: severity=%d category=%d message=%s\n",
                    static_cast<int>(diagnostic.severity), static_cast<int>(diagnostic.category),
                    as_bytes(diagnostic.message).c_str());
    });
    backend->setWindowHandle(host.handle());
    ASSERT_TRUE(backend->initialize());
    ASSERT_TRUE(backend->supportsRenderTargets()) << "the off-screen half is served";

    // The host's own target, described the way a pipeline builder describes one: one RGBA8 attachment, 64x64.
    // The objects behind it are the backend's (the SDK's own contract: "the RenderTarget stays a logical
    // description"), built the first time a pass draws into it. Named the way the builder names its targets:
    // a refusal has to be a sentence a host can act on.
    const vine::intrusive_ptr<RenderTarget> offscreen(new RenderTarget());
    offscreen->setName(u8"gbuffer");
    offscreen->attachColor(RenderTarget::ColorFormat::RGBA8);
    offscreen->setSize(64, 64);

    // The content of the off-screen pass: a red triangle under a view-block program.
    const TriangleFixture triangle = makeTriangle();

    const vine::intrusive_ptr<vine::graphics::Camera> camera = cameraLookingAt(0.5);

    // The full-screen program is the SDK's OWN copy program - the engine's vocabulary, not a text written for
    // this test: it declares its sampler as `layout(binding = 0)` and reads attachment 0 of its source.
    const vine::intrusive_ptr<ShaderProgram> copy_program = vine::graphics::screenCopyProgram(0);
    ASSERT_NE(copy_program, nullptr) << "the SDK ships the copy program the screen path draws with";

    const vine::intrusive_ptr<RenderPass> offscreen_pass(new RenderPass());
    const vine::intrusive_ptr<RenderPass> window_pass(new RenderPass());

    // The SCREEN's clear is blue and the TARGET's is green, so a window pixel says which one a frame ended
    // with: the screen draw covers the window, so a pixel the screen draw touched is the TARGET's picture -
    // the triangle, or the target's clear.
    const vine::graphics::ClearPolicy offscreen_clear{ vine::Color(0, 64, 0, 255), true };
    const vine::graphics::ClearPolicy window_clear{ vine::Color(0, 0, 64, 255), true };

    const auto drive = [&]() {
        backend->beginFrame();
        backend->beginPass(offscreen_pass.get());
        backend->setPassOrder(-1);
        backend->setRenderTarget(offscreen.get());
        backend->setClearPolicy(offscreen_clear);
        backend->setDepthMode(vine::graphics::DepthMode::TestAndWrite);
        backend->render(triangle.commands, camera.get());
        backend->endPass();

        backend->beginPass(window_pass.get());
        backend->setPassOrder(0);
        backend->setRenderTarget(nullptr);
        backend->setPassInputs({ offscreen.get() });
        backend->setClearPolicy(window_clear);
        backend->setDepthMode(vine::graphics::DepthMode::Disabled);
        backend->drawScreenProgram(offscreen.get(), copy_program.get(), camera.get());
        backend->endPass();

        backend->endFrame();
        backend->swapBuffers();
    };
    const auto settle = [&] {
        for (int index = 0; index < 2; ++index)
        {
            backend->beginFrame();
            backend->endFrame();
            backend->swapBuffers();
        }
    };

    // 1. One frame, two passes: the content draws into the TARGET (its own clear and camera), and a full-screen
    // copy takes what it wrote to the WINDOW - through the declared input, which is the only thing a screen
    // draw reads.
    drive();
    EXPECT_EQ(backend->diagnosticCount(), 0U) << "two passes, one reading the other's colour attachment";
    const auto& recorded = BackendContentAccess::executor(*backend).recorded();
    ASSERT_EQ(recorded.size(), 2U) << "both passes are recorded, in the plan's execution order";
    EXPECT_EQ(recorded[0], BackendContentAccess::passes(*backend).adopt(offscreen_pass.get()));
    EXPECT_EQ(recorded[1], BackendContentAccess::passes(*backend).adopt(window_pass.get()));

    // The facts of a BUILT target, which are what the plan was compiled from.
    HostTargets&       targets = BackendContentAccess::targets(*backend);
    HostTargets::Entry* entry  = targets.find(offscreen.get());
    ASSERT_NE(entry, nullptr);
    ASSERT_NE(entry->target, nullptr) << "the target was built when its first pass was announced";
    TargetFacts row;
    targets.facts(*entry, row);
    EXPECT_EQ(row.wanted.width, 64);
    EXPECT_EQ(row.wanted.height, 64);
    EXPECT_FALSE(row.wanted.shape.device_color_formats.empty())
        << "a built target states the device's own spelling of its shape";
    EXPECT_TRUE(row.current.built) << "its first pass has been recorded (see OffscreenTarget::written)";
    EXPECT_EQ(row.depth.has_depth, false) << "the host attached no depth";

    settle();
    const auto left   = host.pixel(kWidth / 4, kHeight / 2);
    const auto right  = host.pixel(3 * kWidth / 4, kHeight / 2);
    const auto corner = host.pixel(2, 2);
    EXPECT_TRUE(isRed(left)) << "the triangle drawn into the TARGET reached the window through the screen draw: got ("
                             << static_cast<int>(left[0]) << ", " << static_cast<int>(left[1]) << ", "
                             << static_cast<int>(left[2]) << ")";
    EXPECT_TRUE(isGreenClear(right)) << "the rest of the window is the TARGET's clear, not the window's own";
    EXPECT_TRUE(isGreenClear(corner));

    // 2. The host RESIZES its target: the facts say the extent moved, the plan answers for it, and the executor
    // applies the answer before the frame records - so the pass draws into images of the new size.
    offscreen->setSize(32, 48);
    drive();
    EXPECT_EQ(entry->target->width(), 32U) << "the plan's answer was applied to the target";
    EXPECT_EQ(entry->target->height(), 48U);
    EXPECT_EQ(backend->diagnosticCount(), 0U);
    settle();
    EXPECT_TRUE(isRed(host.pixel(kWidth / 4, kHeight / 2))) << "the picture survives the resize";

    // 3. The host attaches a SECOND colour attachment: the target's SHAPE changed, which the plan answers with
    // a rebuild - and the frame that follows draws into the rebuilt target (a plan that said Rebuild is not
    // recorded against the old shape's pipelines, see VsgExecutor).
    offscreen->attachColor(RenderTarget::ColorFormat::RGBA8);
    drive();
    EXPECT_EQ(entry->target->colorAttachmentCount(), 2U) << "the rebuild gave the target the shape the host asked for";
    EXPECT_EQ(backend->diagnosticCount(), 0U);
    settle();
    EXPECT_TRUE(isRed(host.pixel(kWidth / 4, kHeight / 2)))
        << "the screen still samples attachment 0 of the rebuilt target";

    // 4. The release announcement: everything this backend held for the target goes, and the host may destroy
    // the object (nothing may be dereferenced after this call - see the SDK's note).
    backend->releaseRenderTarget(offscreen.get());
    EXPECT_EQ(targets.live(), 0U);
    EXPECT_EQ(targets.find(offscreen.get()), nullptr);
    EXPECT_EQ(backend->diagnosticCount(), 0U) << "the release is served silently";

    // 5. A target that borrows a depth from one this backend does not hold cannot be built, and that is SAID
    // (once) - INSIDE a frame, where it blocks the picture. Outside one the announcement is CONFIGURATION:
    // the host sets its pipeline up before it draws (the same rule NotBuilt follows), and a borrow whose
    // lender is announced later in that setup is order rather than failure - the engine's own pipeline does
    // exactly that (measured on the demo: the composite arrives before its G-buffer, and builds on the next
    // in-frame announcement). A pass staged into an unbuilt target is reported by the executor either way.
    const vine::intrusive_ptr<RenderTarget> lenderless(new RenderTarget());
    lenderless->setName(u8"composite");
    lenderless->attachColor(RenderTarget::ColorFormat::RGBA8);
    lenderless->shareDepth(offscreen);  // the lender was released a moment ago
    lenderless->setSize(32, 32);
    backend->setRenderTarget(lenderless.get());
    EXPECT_EQ(backend->diagnosticCount(), 0U) << "outside a frame nothing is said: this is configuration";
    EXPECT_EQ(targets.find(lenderless.get())->target, nullptr) << "and nothing was built for it";
    backend->beginFrame();
    backend->setRenderTarget(lenderless.get());
    EXPECT_EQ(backend->diagnosticCount(), 1U) << "inside a frame the missing lender is reported, once";
    // ... and the sentence NAMES both targets (the SDK's own label): "a render target could not be built" is
    // not something a host can act on, and the engine's builder names everything it creates.
    ASSERT_FALSE(messages.empty());
    EXPECT_NE(messages.back().find("'composite'"), std::string::npos) << messages.back();
    EXPECT_NE(messages.back().find("'gbuffer'"), std::string::npos) << messages.back();
    backend->setRenderTarget(lenderless.get());
    backend->setRenderTarget(lenderless.get());
    EXPECT_EQ(backend->diagnosticCount(), 1U) << "three announcements are one episode";
    backend->endFrame();
    backend->swapBuffers();
    backend->releaseRenderTarget(lenderless.get());

    backend->shutdown();
    EXPECT_TRUE(host.alive());
}

TEST(VsgBackendTest, TheSdkReadsBackItsOwnTargetsPixelsAndDepths)
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
    std::size_t                     seen = 0;
    backend->setDiagnosticSink([&seen](const vine::graphics::RenderDiagnostic& diagnostic) {
        ++seen;
        std::printf("[facade] diagnostic: severity=%d category=%d message=%s\n",
                    static_cast<int>(diagnostic.severity), static_cast<int>(diagnostic.category),
                    as_bytes(diagnostic.message).c_str());
    });
    backend->setWindowHandle(host.handle());
    ASSERT_TRUE(backend->initialize());

    // The target the frame draws into: one RGBA8 colour attachment and a D32F depth - the format that has a
    // plain depth copy (a combined depth/stencil image does not, and this backend says so instead of
    // decoding it wrongly).
    const vine::intrusive_ptr<RenderTarget> target(new RenderTarget());
    target->attachColor(RenderTarget::ColorFormat::RGBA8);
    target->attachDepth(RenderTarget::DepthFormat::D32F);
    target->setSize(64, 64);

    const TriangleFixture             triangle = makeTriangle();
    const vine::intrusive_ptr<vine::graphics::Camera> camera = cameraLookingAt(0.5);
    const vine::intrusive_ptr<RenderPass>             pass(new RenderPass());
    const vine::graphics::ClearPolicy                 clear{ vine::Color(0, 64, 0, 255), true };

    const auto drive = [&]() {
        backend->beginFrame();
        backend->beginPass(pass.get());
        backend->setPassOrder(0);
        backend->setRenderTarget(target.get());
        backend->setClearPolicy(clear);
        backend->setDepthMode(vine::graphics::DepthMode::TestAndWrite);
        backend->render(triangle.commands, camera.get());
        backend->endPass();
        backend->endFrame();
        backend->swapBuffers();
    };

    drive();
    EXPECT_EQ(backend->diagnosticCount(), 0U) << "the frame recorded";

    const std::size_t waits_before = backend->deviceWaits();

    // 1. The colour attachment, as the SDK promises it: tightly packed RGBA8 rows, the picture the frame left.
    std::vector<std::uint8_t>      pixels;
    std::vector<float>             depths;
    vine::graphics::ReadbackResult why = vine::graphics::ReadbackResult::Failed;
    ASSERT_TRUE(backend->readColorBuffer(target.get(), 0, pixels, &why)) << "the pixels are read back";
    EXPECT_EQ(why, vine::graphics::ReadbackResult::Ok);
    ASSERT_EQ(pixels.size(), 64U * 64U * 4U) << "width * height * 4 bytes, tightly packed";
    const auto pixel = [&pixels](int x, int y) {
        const std::size_t at = (static_cast<std::size_t>(y) * 64U + static_cast<std::size_t>(x)) * 4U;
        return std::array<std::uint8_t, 4>{ pixels[at], pixels[at + 1], pixels[at + 2], pixels[at + 3] };
    };
    // The target's own image is RGBA8 - not the display server's BGRA surface - so the channels are the ones
    // the format defines (which is exactly why the WINDOW probes read the outer two as a set).
    const auto inside = pixel(16, 40);
    EXPECT_TRUE(isColourByte(inside[0], 1.0) && isColourByte(inside[1], 0.0) && isColourByte(inside[2], 0.0))
        << "the triangle is where the camera put it: got (" << static_cast<int>(inside[0]) << ", "
        << static_cast<int>(inside[1]) << ", " << static_cast<int>(inside[2]) << ")";
    EXPECT_EQ(inside[3], 255U) << "and it is opaque";
    const auto outside = pixel(48, 8);
    EXPECT_TRUE(isGreenClear({ outside[0], outside[1], outside[2] }))
        << "outside the triangle is the pass' clear, read back in the image's own order";
    EXPECT_EQ(backend->deviceWaits(), waits_before + 1U) << "the readback stopped the device exactly once";
    EXPECT_EQ(backend->diagnosticCount(), 0U) << "a readback that works says nothing";

    // 2. The depth attachment, as normalised values: the clear is the reverse-Z far plane (0) and the
    // triangle's fragments wrote a depth between it and the near plane.
    ASSERT_TRUE(backend->readDepthBuffer(target.get(), depths, &why));
    EXPECT_EQ(why, vine::graphics::ReadbackResult::Ok);
    ASSERT_EQ(depths.size(), 64U * 64U) << "one value per texel";
    EXPECT_GT(depths[40U * 64U + 16U], 0.05F) << "the triangle's depth is not the far plane";
    EXPECT_LT(depths[40U * 64U + 16U], 0.95F);
    EXPECT_FLOAT_EQ(depths[8U * 64U + 48U], 0.0F) << "the clear is the reverse-Z far plane";
    for (const float value : depths)
    {
        EXPECT_GE(value, 0.0F);
        EXPECT_LE(value, 1.0F);
    }
    EXPECT_EQ(backend->deviceWaits(), waits_before + 2U);

    // 3. The refusals are classified, and none of them stops the device: a wrong attachment index is Invalid,
    // a target this backend does not hold is NotReady, a format it cannot pack is Unsupported - and no
    // submission is made for any of them.
    std::vector<std::uint8_t> scratch;
    EXPECT_FALSE(backend->readColorBuffer(target.get(), 5, scratch, &why));
    EXPECT_EQ(why, vine::graphics::ReadbackResult::Invalid);
    EXPECT_TRUE(scratch.empty()) << "a refused read leaves the destination untouched";
    const vine::intrusive_ptr<RenderTarget> unknown(new RenderTarget());
    unknown->attachColor(RenderTarget::ColorFormat::RGBA8);
    unknown->setSize(8, 8);
    EXPECT_FALSE(backend->readColorBuffer(unknown.get(), 0, scratch, &why));
    EXPECT_EQ(why, vine::graphics::ReadbackResult::NotReady) << "never announced to this backend";
    const vine::intrusive_ptr<RenderTarget> floats(new RenderTarget());
    floats->attachColor(RenderTarget::ColorFormat::RGBA16F);
    floats->setSize(8, 8);
    backend->setRenderTarget(floats.get());  // held and built (no frame is needed to build one)
    EXPECT_FALSE(backend->readColorBuffer(floats.get(), 0, scratch, &why));
    EXPECT_EQ(why, vine::graphics::ReadbackResult::Unsupported) << "this backend packs RGBA8 only";
    EXPECT_EQ(backend->deviceWaits(), waits_before + 2U) << "a refused readback never stops the device";

    // 4. A BORROWED depth is the lender's to read: the borrower says Unsupported without touching the device
    // (the SDK's own rule - the image belongs to the source target).
    const vine::intrusive_ptr<RenderTarget> lender(new RenderTarget());
    lender->attachColor(RenderTarget::ColorFormat::RGBA8);
    lender->attachDepth(RenderTarget::DepthFormat::D32F);
    lender->setSize(16, 16);
    backend->setRenderTarget(lender.get());
    const vine::intrusive_ptr<RenderTarget> borrower(new RenderTarget());
    borrower->attachColor(RenderTarget::ColorFormat::RGBA8);
    borrower->shareDepth(lender);
    borrower->setSize(16, 16);
    backend->setRenderTarget(borrower.get());

    HostTargets& targets = BackendContentAccess::targets(*backend);
    ASSERT_NE(targets.find(borrower.get()), nullptr);
    ASSERT_NE(targets.find(borrower.get())->target, nullptr) << "the borrower was built on the lender's depth";
    EXPECT_FALSE(backend->readDepthBuffer(borrower.get(), depths, &why));
    EXPECT_EQ(why, vine::graphics::ReadbackResult::Unsupported) << "read it through the source target";

    // The lender is held and BUILT, but no frame has ever drawn into it: there is nothing to read back yet
    // (and reading it costs nothing - not even a wait).
    EXPECT_FALSE(backend->readColorBuffer(lender.get(), 0, scratch, &why));
    EXPECT_EQ(why, vine::graphics::ReadbackResult::NotReady) << "nothing has been recorded into it";
    EXPECT_EQ(backend->deviceWaits(), waits_before + 2U) << "and none of those refusals stops the device";

    // 5. A target that was released is NotReady again - the SDK's own reading ("never rendered, or already
    // released"), not Unsupported: a re-announced handle can make it readable.
    backend->releaseRenderTarget(target.get());
    EXPECT_FALSE(backend->readColorBuffer(target.get(), 0, pixels, &why));
    EXPECT_EQ(why, vine::graphics::ReadbackResult::NotReady);

    backend->releaseRenderTarget(borrower.get());
    backend->releaseRenderTarget(lender.get());
    backend->releaseRenderTarget(floats.get());
    backend->shutdown();
    EXPECT_TRUE(host.alive());
}

TEST(VsgBackendTest, TheWindowFollowsItsHostsSurfaceThroughALiveResize)
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
    EXPECT_EQ(backend->diagnosticCount(), 0U);

    // The content says what the plan carried: the surface's extent in the two outer bytes (a resize therefore
    // shows up in the PICTURE, not only in the shape the target reports) and the camera's x in the green byte.
    // The divisors keep every value in [0, 1] for both sizes (128x96 before, 96x64 after).
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
            "void main() { outColor = vec4(vb.frame.y / 160.0, abs(vb.cam_pos.x), vb.frame.z / 192.0, 1.0); }\n"));
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

    const vine::intrusive_ptr<vine::graphics::Camera> camera = cameraLookingAt(0.5);
    const vine::intrusive_ptr<RenderPass>             pass(new RenderPass());
    const vine::graphics::ClearPolicy                 clear{ vine::Color(0, 64, 0, 255), true };

    const auto drive = [&] {
        backend->beginFrame();
        backend->beginPass(pass.get());
        backend->setPassOrder(0);
        backend->setRenderTarget(nullptr);
        backend->setClearPolicy(clear);
        backend->setDepthMode(vine::graphics::DepthMode::TestAndWrite);
        backend->render(commands, camera.get());
        backend->endPass();
        backend->endFrame();
        backend->swapBuffers();
    };
    const auto settle = [&] {
        for (int index = 0; index < 2; ++index)
        {
            backend->beginFrame();
            backend->endFrame();
            backend->swapBuffers();
        }
    };

    drive();
    settle();

    // 1. The picture at the size the host's surface has: the triangle left of centre (the camera looks half a
    // unit to its right) with the extent encoded in it.
    const auto before_left  = host.pixel(kWidth / 4, kHeight / 2);
    const auto before_right = host.pixel(3 * kWidth / 4, kHeight / 2);
    EXPECT_TRUE(isColourByte(before_left[1], 0.5))
        << "the camera's x is in the picture, got (" << static_cast<int>(before_left[0]) << ", "
        << static_cast<int>(before_left[1]) << ", " << static_cast<int>(before_left[2]) << ")";
    EXPECT_TRUE((isColourByte(before_left[0], 0.8) && isColourByte(before_left[2], 0.5)) ||
                (isColourByte(before_left[2], 0.8) && isColourByte(before_left[0], 0.5)))
        << "and the surface's 128x96 came with it (the outer bytes are read as a set: the server picks the order)";
    EXPECT_TRUE(isGreenClear(before_right)) << "the right quarter is the clear colour";

    // 2. The host resizes its surface and announces it. THE SURFACE OWNS ITS SIZE (RenderBackend::resize's
    // authority order), so the backend FOLLOWS it: the window's geometry is re-read and its swapchain is
    // rebuilt - which stops the device exactly once, the price the counter makes visible - and nothing is
    // reported, because this is served. A 0x0 announcement is not a size a surface can have: it costs
    // nothing, not even that stop.
    const std::size_t waits_before = backend->deviceWaits();
    backend->resize(0, 0);
    EXPECT_EQ(backend->deviceWaits(), waits_before) << "a size no surface can have is not followed";

    host.resize(96, 64);
    backend->resize(96, 64);
    EXPECT_EQ(backend->deviceWaits(), waits_before + 1U)
        << "following the surface rebuilds the swapchain, and that stops the device";
    EXPECT_EQ(backend->diagnosticCount(), 0U) << "a live size event is served now";
    WindowTarget* window = BackendContentAccess::windowTarget(*backend);
    ASSERT_NE(window, nullptr);
    EXPECT_EQ(window->width(), 96U) << "the window is on the surface's new size";
    EXPECT_EQ(window->height(), 64U);

    // 3. The next frame is recorded at the NEW size: the render area, the view block every pass is given and
    // the swapchain it presents all follow the surface - nobody re-tells them.
    drive();
    settle();
    const auto after_left  = host.pixel(96 / 4, 64 / 2);
    const auto after_right = host.pixel(3 * 96 / 4, 64 / 2);
    EXPECT_TRUE(isColourByte(after_left[1], 0.5))
        << "the picture is still there, got (" << static_cast<int>(after_left[0]) << ", "
        << static_cast<int>(after_left[1]) << ", " << static_cast<int>(after_left[2]) << ")";
    EXPECT_TRUE((isColourByte(after_left[0], 0.6) && isColourByte(after_left[2], 1.0 / 3.0)) ||
                (isColourByte(after_left[2], 0.6) && isColourByte(after_left[0], 1.0 / 3.0)))
        << "and the view block carries 96x64 now, not the size the window came up at: got ("
        << static_cast<int>(after_left[0]) << ", " << static_cast<int>(after_left[2]) << ")";
    EXPECT_TRUE(isGreenClear(after_right)) << "the right quarter is the clear colour at the new size too";
    EXPECT_EQ(seen, backend->diagnosticCount());

    backend->shutdown();
    EXPECT_TRUE(host.alive());
}

TEST(VsgBackendTest, ADepthOnlyTargetIsHeldBuiltAndOfferedAsASampledInput)
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

    // THE ENGINE'S SHADOW MAP, as a target: attachDepth with NO colour attachment, and its depth promoted so
    // the shading can sample it. A depth-only target is a target - refusing it ("at least one colour
    // attachment") took the app's whole deferred pipeline down with it: the shadow pass had nowhere to draw,
    // the lighting pass that samples the map lost an input, and it was refused with it (the app smoke found
    // this: 'shadow_map' never built, 'input 1 offers no depth texture').
    const vine::intrusive_ptr<RenderTarget> shadow(new RenderTarget());
    shadow->setName(u8"shadow_map");
    shadow->setSize(64, 64);
    shadow->attachDepth(RenderTarget::DepthFormat::D24);
    shadow->setDepthPromotion(true);

    backend->setRenderTarget(shadow.get());
    HostTargets& targets              = BackendContentAccess::targets(*backend);
    HostTargets::Entry* shadow_entry  = targets.find(shadow.get());
    ASSERT_NE(shadow_entry, nullptr);
    ASSERT_NE(shadow_entry->target, nullptr) << "a depth-only target is built like any other";
    EXPECT_EQ(shadow_entry->target->colorAttachmentCount(), 0U);
    EXPECT_TRUE(shadow_entry->target->depthView() != nullptr) << "and its depth is a real image view";
    EXPECT_TRUE(shadow_entry->target->layout().depth_format.has_value());
    EXPECT_EQ(backend->diagnosticCount(), 0U) << "nothing about it needed saying";

    // THE PLAN CAN PROMISE ITS DEPTH (what the compiler reads): the fact row states the promotion the target
    // was built with, and core::depthPlan answers "sampleable" - so a pass that declares the map as an input
    // is offered its depth (see the facade's offer loop). Before this slice the row could not exist at all:
    // the description was refused before anything was built, and the app's lighting pass lost its input.
    vine::vsg::core::TargetFacts row;
    targets.facts(*shadow_entry, row);
    EXPECT_TRUE(row.depth.has_depth);
    EXPECT_FALSE(row.depth.borrowed);
    EXPECT_TRUE(row.depth.promotion) << "the built target's own policy answers";
    EXPECT_TRUE(vine::vsg::core::depthPlan(row.depth).sampleable) << "and the plan promises the depth to a shader";

    // A frame that presents with it held is served silently - the target is part of the frame's world now,
    // and nothing about it needs saying.
    backend->beginFrame();
    backend->endFrame();
    backend->swapBuffers();
    EXPECT_EQ(backend->framesPresented(), 1U);
    EXPECT_EQ(backend->diagnosticCount(), 0U);

    backend->shutdown();
    EXPECT_TRUE(host.alive());
}

TEST(VsgBackendTest, AMaterialEditLandsOnTheNextFrameAndASteadyFrameRebuildsNothing)
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
    EXPECT_EQ(backend->diagnosticCount(), 0U);

    // The content shades with the MATERIAL's own diffuse colour. The engine edits materials in place and
    // expects the picture to follow - the implementation this backend replaces refreshed every commanded
    // material every frame for exactly that reason - so the pixels are the evidence for both halves: the
    // edit landing, and the steady frame that must not rebuild anything.
    const vine::intrusive_ptr<ShaderProgram> program(new ShaderProgram());
    {
        ShaderStage vertex;
        vertex.type   = ShaderStageType::Vertex;
        vertex.source = vine::String(reinterpret_cast<const char8_t*>(
            "layout(location = 0) in vec3 position;\n"
            "void main() { gl_Position = vec4(position, 1.0); }\n"));
        ShaderStage fragment;
        fragment.type   = ShaderStageType::Fragment;
        fragment.source = vine::String(reinterpret_cast<const char8_t*>(
            "layout(location = 0) out vec4 outColor;\n"
            "layout(set = 0, binding = 2, std140) uniform VineMaterialBlock\n"
            "{\n"
            "    vec4 ambient; vec4 diffuse; vec4 specular; float shininess;\n"
            "} material;\n"
            "void main() { outColor = material.diffuse; }\n"));
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
    material->setDiffuse(vine::Colorf(0.25F, 0.5F, 0.75F, 1.0F));

    RenderCommand command;
    command.geometry = geometry;
    command.material = material;
    command.program  = program;
    const std::vector<RenderCommand> commands{ command };

    const vine::intrusive_ptr<vine::graphics::Camera> camera = cameraLookingAt(0.5);
    const vine::intrusive_ptr<RenderPass>             pass(new RenderPass());
    const vine::graphics::ClearPolicy                 clear{ vine::Color(0, 64, 0, 255), true };

    const auto drive = [&] {
        backend->beginFrame();
        backend->beginPass(pass.get());
        backend->setPassOrder(0);
        backend->setRenderTarget(nullptr);
        backend->setClearPolicy(clear);
        backend->setDepthMode(vine::graphics::DepthMode::TestAndWrite);
        backend->render(commands, camera.get());
        backend->endPass();
        backend->endFrame();
        backend->swapBuffers();
    };
    const auto settle = [&] {
        for (int index = 0; index < 2; ++index)
        {
            backend->beginFrame();
            backend->endFrame();
            backend->swapBuffers();
        }
    };
    // The two outer bytes are the red and blue of the material's diffuse, in the server's order.
    const auto outer = [](const std::array<std::uint8_t, 3>& pixel, double red, double blue) {
        return (isColourByte(pixel[0], red) && isColourByte(pixel[2], blue)) ||
               (isColourByte(pixel[2], red) && isColourByte(pixel[0], blue));
    };

    ContentStore* store = BackendContentAccess::store(*backend);
    ASSERT_NE(store, nullptr) << "a live session owns the content tables its frames are built from";

    drive();
    settle();
    // The vertex stage passes the geometry's own coordinates through, so the triangle covers the middle of
    // the window: probe inside it, and off to the left for the clear.
    const auto first = host.pixel(kWidth / 2, kHeight / 2);
    EXPECT_TRUE(isColourByte(first[1], 0.5)) << "the material's green byte reached the picture, got ("
                                            << static_cast<int>(first[0]) << ", " << static_cast<int>(first[1])
                                            << ", " << static_cast<int>(first[2]) << ")";
    EXPECT_TRUE(outer(first, 0.25, 0.75)) << "and so did red and blue, in the server's order";

    // The host edits the material and DOES NOTHING ELSE. There is no announcement to make: the SDK's
    // RenderBackend has no "this material changed" entry point, so the touch the facade makes when the next
    // frame commands the material is the whole mechanism (see ContentStore::updateMaterial).
    material->setDiffuse(vine::Colorf(0.75F, 0.25F, 0.5F, 1.0F));
    const std::uint64_t builds_before = store->builds();
    drive();
    EXPECT_EQ(store->builds(), builds_before + 1U) << "the edit replaced exactly that material's row";
    settle();
    const auto edited = host.pixel(kWidth / 2, kHeight / 2);
    EXPECT_TRUE(isColourByte(edited[1], 0.25)) << "the edit lands on the very next frame, got ("
                                               << static_cast<int>(edited[0]) << ", "
                                               << static_cast<int>(edited[1]) << ", "
                                               << static_cast<int>(edited[2]) << ")";
    EXPECT_TRUE(outer(edited, 0.75, 0.5)) << "and the outer channels followed";

    // A frame that changes nothing compares and writes nothing: the touch must not rebuild the row it just
    // found unchanged (the steady-state property the old manager owned).
    const std::uint64_t steady_before = store->builds();
    drive();
    settle();
    EXPECT_EQ(store->builds(), steady_before) << "a steady frame builds no row";
    EXPECT_TRUE(isColourByte(host.pixel(kWidth / 2, kHeight / 2)[1], 0.25)) << "and shows the same picture";
    EXPECT_EQ(backend->deviceWaits(), 0U) << "none of this stops the device";
    EXPECT_EQ(seen, backend->diagnosticCount());

    backend->shutdown();
    EXPECT_TRUE(host.alive());
}

#endif  // !defined(_WIN32)
