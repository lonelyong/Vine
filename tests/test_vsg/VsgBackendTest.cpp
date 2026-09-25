#include <gtest/gtest.h>

#include <array>
#include <chrono>
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

// TestHostWindow is an XCB window, so the cases that use it reach for the connection themselves; the guard
// matches the one around those cases.
#if !defined(_WIN32)
#    include <xcb/xcb.h>
#endif

#include "TestHostWindow.hpp"

using vn::graphics::Geometry;
using vn::graphics::Material;
using vn::graphics::RenderCommand;
using vn::graphics::RenderPass;
using vn::graphics::RenderTarget;
using vn::graphics::ShaderProgram;
using vn::graphics::ShaderStage;
using vn::graphics::ShaderStageType;
using vn::vsg::ContentAssembly;
using vn::vsg::ContentStore;
using vn::vsg::core::TargetFacts;
using vn::vsg::core::depthPlan;
using vn::vsg::BlockStorage;
using vn::vsg::HostTargets;
using vn::vsg::PassRegistry;
using vn::vsg::VsgBackend;
using vn::vsg::core::VariantPool;
using vn::vsg::WindowTarget;
using vn::vsg::api::probePhysicalDevices;
using vn::vsg::detail::BackendContentAccess;

namespace
{

/// @brief Prints a `vn::String` (UTF-8 bytes) as the bytes it holds.
std::string as_bytes(const vn::String& text)
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

/// @brief Whether a pixel is anything but the window's own background (what an UNTOUCHED window shows).
///
/// THE WAIT'S PREDICATE for a window something has just been presented to: "a frame ARRIVED" is the weakest
/// fact that means the display path caught up with the session, and asserting the picture itself stays the
/// caller's job (see TestHostWindow::waitForPixel for why waiting for the EXPECTED value would hide a
/// transiently wrong picture, and for the measured cost of polling too eagerly).
bool arrived(const std::array<std::uint8_t, 3>& pixel)
{
    return pixel[0] != 0U || pixel[1] != 0U || pixel[2] != 0U;
}

/// @brief A red triangle under a view-block program: the content the off-screen cases draw.
struct TriangleFixture
{
    vn::intrusive_ptr<ShaderProgram> program;
    vn::intrusive_ptr<Geometry>      geometry;
    vn::intrusive_ptr<Material>      material;
    std::vector<RenderCommand>         commands;
};

/// @brief Builds the triangle fixture: a view-block vertex stage and a constant red fragment stage.
TriangleFixture makeTriangle()
{
    TriangleFixture fixture;

    const vn::intrusive_ptr<ShaderProgram> program(new ShaderProgram());
    {
        ShaderStage vertex;
        vertex.type   = ShaderStageType::Vertex;
        vertex.source = vn::String(reinterpret_cast<const char8_t*>(
            "layout(location = 0) in vec3 position;\n"
            "layout(set = 0, binding = 0, std140) uniform VineViewBlock {\n"
            "    mat4 view; mat4 inv_view; mat4 proj; mat4 view_proj; vec4 cam_pos; vec4 frame; } vb;\n"
            "void main() { gl_Position = vb.view_proj * vec4(position, 1.0); }\n"));
        ShaderStage fragment;
        fragment.type   = ShaderStageType::Fragment;
        fragment.source = vn::String(reinterpret_cast<const char8_t*>(
            "layout(location = 0) out vec4 outColor;\n"
            "void main() { outColor = vec4(1.0, 0.0, 0.0, 1.0); }\n"));
        program->addStage(vertex);
        program->addStage(fragment);
    }
    fixture.program = program;

    const vn::intrusive_ptr<Geometry> geometry(new Geometry());
    const vn::intrusive_ptr<vn::Buffer<float>> positions = vn::intrusive_ptr<vn::Buffer<float>>(
        new vn::Buffer<float>(std::vector<float>{ -0.4F, -0.4F, 0.5F, 0.4F, -0.4F, 0.5F, 0.0F, 0.6F, 0.5F }));
    const vn::intrusive_ptr<vn::Buffer<std::uint32_t>> indices =
        vn::intrusive_ptr<vn::Buffer<std::uint32_t>>(
            new vn::Buffer<std::uint32_t>(std::vector<std::uint32_t>{ 0U, 1U, 2U }));
    geometry->setPositions(positions);
    geometry->setIndices(indices);
    geometry->setRevision(1U);
    fixture.geometry = geometry;

    const vn::intrusive_ptr<Material> material(new Material());
    material->setDiffuse(vn::Colorf(0.2F, 0.3F, 0.4F, 1.0F));
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
vn::intrusive_ptr<vn::graphics::Camera> cameraLookingAt(double x)
{
    vn::intrusive_ptr<vn::graphics::Camera> camera(new vn::graphics::Camera());
    camera->setViewMatrixAsLookAt(vn::math::Vec3d(x, 0.0, 1.5), vn::math::Vec3d(x, 0.0, 0.0),
                                  vn::math::Vec3d(0.0, 1.0, 0.0));
    camera->setProjectionMatrixAsOrtho(-1.0, 1.0, -1.0, 1.0, 0.5, 4.0);
    return camera;
}

/// @brief A packed grid of @p side x @p side clip-space positions covering [-0.9, 0.9] squared.
///
/// The per-frame-cost recipe's mesh, sized for the measurement rather than for beauty: 1024 vertices are
/// 12 KiB of vertex bytes, so "the host replaced them again" costs something a frame's fixed overhead
/// cannot hide. The positions are CLIP space (the recipe's programs pass them through unchanged) and the
/// grid covers the middle of the window, which is what lets one probe see it and another, near the left
/// edge, stop seeing it after the mesh has been shifted.
std::vector<float> gridPositions(int side)
{
    std::vector<float> positions;
    positions.reserve(static_cast<std::size_t>(side * side) * 3U);
    for (int y = 0; y < side; ++y) {
        for (int x = 0; x < side; ++x) {
            positions.push_back(-0.9F + 1.8F * static_cast<float>(x) / static_cast<float>(side - 1));
            positions.push_back(-0.9F + 1.8F * static_cast<float>(y) / static_cast<float>(side - 1));
            positions.push_back(0.5F);
        }
    }
    return positions;
}

/// @brief The two triangles of every grid cell, counter-clockwise in clip space.
///
/// The winding matters to the recipe's state regime: it turns Back-face culling ON and asserts the mesh
/// stays visible, and that assertion is also the proof that the state change really reached the
/// rasteriser (a cull mode that only changed a local would leave an unchanged picture either way). The
/// first spelling of this table was CLOCKWISE and the mesh vanished under Back-face culling - which is
/// how the assertion earned its keep before the recipe was ever recorded.
std::vector<std::uint32_t> gridIndices(int side)
{
    std::vector<std::uint32_t> indices;
    indices.reserve(static_cast<std::size_t>((side - 1) * (side - 1)) * 6U);
    for (int y = 0; y + 1 < side; ++y) {
        for (int x = 0; x + 1 < side; ++x) {
            const std::uint32_t a = static_cast<std::uint32_t>(y * side + x);
            const std::uint32_t b = a + 1U;
            const std::uint32_t c = a + static_cast<std::uint32_t>(side);
            const std::uint32_t d = c + 1U;
            indices.push_back(a);
            indices.push_back(b);
            indices.push_back(c);
            indices.push_back(b);
            indices.push_back(d);
            indices.push_back(c);
        }
    }
    return indices;
}

/// @brief The live counters one regime of the per-frame-cost recipe is judged by.
///
/// One counter per ANSWER, not per layer: a row (re)built says the edit was DATA the tables had to be told
/// about, an upload says bytes reached the device as a NEW stream, a compile says the edit was IDENTITY,
/// and a reuse says the frame switched between variants the pool already had.
struct EditCostCounters
{
    std::uint64_t builds{0};   ///< `ContentStore::builds()`: table rows (re)built.
    std::uint64_t uploads{0};  ///< `StreamUploads::uploads()`: streams uploaded as new objects.
    std::uint64_t created{0};  ///< `VariantPool::created()`: pipelines compiled.
    std::uint64_t reused{0};   ///< `VariantPool::reused()`: variant switches the pool served.
};

/// @brief Reads @ref EditCostCounters off a live backend.
///
/// @param store    The facade's content tables.
/// @param assembly The facade's content world (its stream sharing owns the upload count).
/// @param pool     The facade's compiled-pipeline pool.
/// @return The counters as they are now.
EditCostCounters readEditCosts(const ContentStore& store, ContentAssembly& assembly,
                               const vn::vsg::core::VariantPool& pool)
{
    EditCostCounters counters;
    counters.builds  = store.builds();
    counters.uploads = assembly.uploads().uploads();
    counters.created = pool.created();
    counters.reused  = pool.reused();
    return counters;
}

/// @brief What one regime moved, as signed deltas (a counter that went backwards is a bug worth seeing).
struct EditCostDelta
{
    std::int64_t builds{0};   ///< Rows rebuilt.
    std::int64_t uploads{0};  ///< Streams uploaded.
    std::int64_t created{0};  ///< Pipelines compiled.
    std::int64_t reused{0};   ///< Variant switches served by the pool.
};

/// @brief Subtracts two @ref EditCostCounters samples.
///
/// @param before Counters read before the regime.
/// @param after  Counters read after it.
/// @return The signed deltas.
EditCostDelta editCostDelta(const EditCostCounters& before, const EditCostCounters& after)
{
    const auto step = [](std::uint64_t from, std::uint64_t to) {
        return static_cast<std::int64_t>(to) - static_cast<std::int64_t>(from);
    };
    EditCostDelta delta;
    delta.builds  = step(before.builds, after.builds);
    delta.uploads = step(before.uploads, after.uploads);
    delta.created = step(before.created, after.created);
    delta.reused  = step(before.reused, after.reused);
    return delta;
}

}  // namespace

TEST(VsgBackendTest, TheSdkFacingBackendComesUpPresentsEmptyFramesAndSaysWhatItCannotServe)
{
    if (!deviceCaseAvailable())
    {
        GTEST_SKIP() << "no window system or no device satisfies the requirements";
    }

    vn::intrusive_ptr<VsgBackend> backend(new VsgBackend());
    std::size_t                     seen = 0;
    backend->setDiagnosticSink([&seen](const vn::graphics::RenderDiagnostic& diagnostic) {
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

    // 3. The content world was told what the device offers. Anisotropy is part of this backend's feature floor
    // (see api/DeviceFeatures) and pointless at the never-told default of 1x, so the number in the cache has to
    // have come from the device's own limit - which is what makes "the session hands it over" observable at
    // all (the entry point existed and nothing called it, and no test could tell). A device that reports no
    // anisotropy would read 1.0 here, which is why the assertion is "more than 1x" rather than a number.
    const vn::vsg::MaterialImages* images = BackendContentAccess::images(*backend);
    ASSERT_NE(images, nullptr);
    EXPECT_GT(images->maxAnisotropy(), 1.0F) << "the cache was never told the device's limit";

    // 4. The readbacks are served now, and their REFUSALS are classified rather than slammed shut: a target
    // this backend has not been told about is NotReady (a later announcement can make it readable), a call
    // with no target is Invalid, and each reason is said ONCE per episode - the request is synchronous, and a
    // caller polling a target that is not ready yet must not be flooded.
    const vn::intrusive_ptr<RenderTarget> target(new RenderTarget());
    std::vector<std::uint8_t>               pixels;
    std::vector<float>                      depths;
    const std::size_t                       before_unserved = backend->diagnosticCount();
    vn::graphics::ReadbackResult why = vn::graphics::ReadbackResult::Ok;
    EXPECT_FALSE(backend->readColorBuffer(target.get(), 0, pixels, &why));
    EXPECT_EQ(why, vn::graphics::ReadbackResult::NotReady) << "this backend was never told about it";
    EXPECT_TRUE(pixels.empty()) << "a refused read leaves the destination untouched";
    EXPECT_FALSE(backend->readDepthBuffer(target.get(), depths, &why));
    EXPECT_EQ(why, vn::graphics::ReadbackResult::NotReady);
    EXPECT_TRUE(depths.empty());
    EXPECT_EQ(backend->diagnosticCount(), before_unserved + 2U) << "one sentence per entry point, once";
    (void)backend->readColorBuffer(target.get(), 0, pixels, &why);
    (void)backend->readDepthBuffer(target.get(), depths, &why);
    EXPECT_EQ(backend->diagnosticCount(), before_unserved + 2U) << "a repeated refusal is the same episode";
    EXPECT_EQ(seen, backend->diagnosticCount());

    EXPECT_FALSE(backend->readColorBuffer(nullptr, 0, pixels, &why));
    EXPECT_EQ(why, vn::graphics::ReadbackResult::Invalid) << "no target was given, so the request is wrong";
    EXPECT_TRUE(backend->supportsRenderTargets()) << "the off-screen half is served now";

    // An off-screen target is HELD from the moment it is announced - even before it can be built, because a
    // host configures a target before it draws into it - and the release announcement drops it, which is what
    // lets the host destroy the object. Holding and releasing report nothing; a readback of a held target
    // with no attachments yet is NotReady, with the target's own episode.
    backend->setRenderTarget(target.get());
    EXPECT_EQ(BackendContentAccess::targets(*backend).live(), 1U);
    EXPECT_FALSE(backend->readColorBuffer(target.get(), 0, pixels, &why));
    EXPECT_EQ(why, vn::graphics::ReadbackResult::NotReady) << "held, but its attachments are not built yet";
    EXPECT_EQ(backend->diagnosticCount(), before_unserved + 3U) << "the held target's own episode";
    (void)backend->readColorBuffer(target.get(), 0, pixels, &why);
    EXPECT_EQ(backend->diagnosticCount(), before_unserved + 3U) << "and a repeat adds nothing";
    backend->releaseRenderTarget(target.get());
    EXPECT_EQ(BackendContentAccess::targets(*backend).live(), 0U);
    EXPECT_EQ(backend->diagnosticCount(), before_unserved + 3U) << "holding and releasing a target is served";

    // 5. The protocol still judges: a DRAWING call with no scope open has nothing to belong to, so the
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

    // 6. The announced size is the SURFACE's, and this backend FOLLOWS the surface it is on (the SDK's
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

    // 7. shutdown() is safe twice and leaves the backend initializable again (the SDK's contract for a
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

    vn::intrusive_ptr<VsgBackend> backend(new VsgBackend());
    backend->setDiagnosticSink([](const vn::graphics::RenderDiagnostic& diagnostic) {
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

    vn::intrusive_ptr<VsgBackend> backend(new VsgBackend());
    std::size_t                     seen = 0;
    backend->setDiagnosticSink([&seen](const vn::graphics::RenderDiagnostic& diagnostic) {
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
    const vn::intrusive_ptr<ShaderProgram> program(new ShaderProgram());
    {
        ShaderStage vertex;
        vertex.type   = ShaderStageType::Vertex;
        vertex.source = vn::String(reinterpret_cast<const char8_t*>(
            "layout(location = 0) in vec3 position;\n"
            "layout(set = 0, binding = 0, std140) uniform VineViewBlock {\n"
            "    mat4 view; mat4 inv_view; mat4 proj; mat4 view_proj; vec4 cam_pos; vec4 frame; } vb;\n"
            "void main() { gl_Position = vb.view_proj * vec4(position, 1.0); }\n"));
        ShaderStage fragment;
        fragment.type   = ShaderStageType::Fragment;
        fragment.source = vn::String(reinterpret_cast<const char8_t*>(
            "layout(location = 0) out vec4 outColor;\n"
            "layout(set = 0, binding = 0, std140) uniform VineViewBlock {\n"
            "    mat4 view; mat4 inv_view; mat4 proj; mat4 view_proj; vec4 cam_pos; vec4 frame; } vb;\n"
            "void main() { outColor = vec4(vb.frame.y / 128.0, abs(vb.cam_pos.x), vb.frame.z / 96.0, 1.0); }\n"));
        program->addStage(vertex);
        program->addStage(fragment);
    }

    const vn::intrusive_ptr<Geometry> geometry(new Geometry());
    const vn::intrusive_ptr<vn::Buffer<float>> positions = vn::intrusive_ptr<vn::Buffer<float>>(
        new vn::Buffer<float>(std::vector<float>{ -0.4F, -0.4F, 0.5F, 0.4F, -0.4F, 0.5F, 0.0F, 0.6F, 0.5F }));
    const vn::intrusive_ptr<vn::Buffer<std::uint32_t>> indices =
        vn::intrusive_ptr<vn::Buffer<std::uint32_t>>(
            new vn::Buffer<std::uint32_t>(std::vector<std::uint32_t>{ 0U, 1U, 2U }));
    geometry->setPositions(positions);
    geometry->setIndices(indices);
    geometry->setRevision(1U);

    const vn::intrusive_ptr<Material> material(new Material());
    material->setDiffuse(vn::Colorf(0.2F, 0.3F, 0.4F, 1.0F));

    RenderCommand command;
    command.geometry = geometry;
    command.material = material;
    command.program  = program;
    const std::vector<RenderCommand> commands{ command };

    const vn::intrusive_ptr<vn::graphics::Camera> camera_a = cameraLookingAt(0.5);
    const vn::intrusive_ptr<vn::graphics::Camera> camera_b = cameraLookingAt(-0.5);
    const vn::intrusive_ptr<RenderPass>             pass(new RenderPass());

    const vn::graphics::ClearPolicy clear{ vn::Color(0, 64, 0, 255), true };

    const auto drive = [&](const vn::graphics::Camera* frame_camera) {
        backend->beginFrame();
        backend->beginPass(pass.get());
        backend->setPassOrder(0);
        backend->setRenderTarget(nullptr);
        backend->setClearPolicy(clear);
        backend->setDepthMode(vn::graphics::DepthMode::TestAndWrite);
        backend->render(commands, frame_camera);
        backend->endPass();
        backend->endFrame();
        backend->swapBuffers();
    };
    // The display path lags the session's presents, so a picture is read after a couple more frames that
    // carry the SAME picture. A plan-free frame is not a settle: it repaints the window with the session's
    // own graph, and a read that catches one answers the window's background (measured: `read-error 0`,
    // the window's own clear - see TestHostWindow::waitForPixel).
    const auto settle = [&](const vn::graphics::Camera* frame_camera) {
        drive(frame_camera);
        drive(frame_camera);
    };

    // 1. The engine's PRE-FRAME WARM-UP: every enabled non-clearing pass executes once with NO frame open so
    // a backend can prepare its retained state. That is designed, not a protocol error - nothing is
    // collected and nothing is reported - and the pass' identity survives it into the frames that follow.
    backend->beginPass(pass.get());
    backend->setPassOrder(-1);
    backend->setRenderTarget(nullptr);
    backend->setClearPolicy(clear);
    backend->setDepthMode(vn::graphics::DepthMode::TestAndWrite);
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

    settle(camera_a.get());
    const auto left_a = host.waitForPixel(kWidth / 4, kHeight / 2, arrived,
                                          std::chrono::milliseconds{ 500 });  // inside the triangle
    const auto right_a = host.pixel(3 * kWidth / 4, kHeight / 2);            // never: the clear
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
    settle(camera_b.get());
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
    const vn::vsg::core::PassId first_id = registry.adopt(pass.get());
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

    vn::intrusive_ptr<VsgBackend> backend(new VsgBackend());
    std::size_t                     seen     = 0;
    std::vector<std::string>        messages;
    backend->setDiagnosticSink([&seen, &messages](const vn::graphics::RenderDiagnostic& diagnostic) {
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
    const vn::intrusive_ptr<RenderTarget> offscreen(new RenderTarget());
    offscreen->setName(u8"gbuffer");
    offscreen->attachColor(RenderTarget::ColorFormat::RGBA8);
    offscreen->setSize(64, 64);

    // The content of the off-screen pass: a red triangle under a view-block program.
    const TriangleFixture triangle = makeTriangle();

    const vn::intrusive_ptr<vn::graphics::Camera> camera = cameraLookingAt(0.5);

    // The full-screen program is the SDK's OWN copy program - the engine's vocabulary, not a text written for
    // this test: it declares its sampler as `layout(binding = 0)` and reads attachment 0 of its source.
    const vn::intrusive_ptr<ShaderProgram> copy_program = vn::graphics::screenCopyProgram(0);
    ASSERT_NE(copy_program, nullptr) << "the SDK ships the copy program the screen path draws with";

    const vn::intrusive_ptr<RenderPass> offscreen_pass(new RenderPass());
    const vn::intrusive_ptr<RenderPass> window_pass(new RenderPass());

    // The SCREEN's clear is blue and the TARGET's is green, so a window pixel says which one a frame ended
    // with: the screen draw covers the window, so a pixel the screen draw touched is the TARGET's picture -
    // the triangle, or the target's clear.
    const vn::graphics::ClearPolicy offscreen_clear{ vn::Color(0, 64, 0, 255), true };
    const vn::graphics::ClearPolicy window_clear{ vn::Color(0, 0, 64, 255), true };

    const auto drive = [&]() {
        backend->beginFrame();
        backend->beginPass(offscreen_pass.get());
        backend->setPassOrder(-1);
        backend->setRenderTarget(offscreen.get());
        backend->setClearPolicy(offscreen_clear);
        backend->setDepthMode(vn::graphics::DepthMode::TestAndWrite);
        backend->render(triangle.commands, camera.get());
        backend->endPass();

        backend->beginPass(window_pass.get());
        backend->setPassOrder(0);
        backend->setRenderTarget(nullptr);
        backend->setPassInputs({ offscreen.get() });
        backend->setClearPolicy(window_clear);
        backend->setDepthMode(vn::graphics::DepthMode::Disabled);
        backend->drawScreenProgram(offscreen.get(), copy_program.get(), camera.get());
        backend->endPass();

        backend->endFrame();
        backend->swapBuffers();
    };
    const auto settle = [&] {
        drive();
        drive();
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
    // The first picture after presenting is read through the bounded wait: the display path lags the session's
    // presents (see TestHostWindow::waitForPixel - measured: the same probe answers black, then the picture,
    // with `readError() == 0` both times).
    const auto left   = host.waitForPixel(kWidth / 4, kHeight / 2, arrived, std::chrono::milliseconds{ 500 });
    const auto right  = host.pixel(3 * kWidth / 4, kHeight / 2);
    const auto corner = host.pixel(2, 2);
    EXPECT_TRUE(isRed(left)) << "the triangle drawn into the TARGET reached the window through the screen draw: got ("
                             << static_cast<int>(left[0]) << ", " << static_cast<int>(left[1]) << ", "
                             << static_cast<int>(left[2]) << ") read-error " << static_cast<int>(host.readError())
                             << " (see HostWindowReadTest: a refusal is not a black pixel)";
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
    const vn::intrusive_ptr<RenderTarget> lenderless(new RenderTarget());
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

    vn::intrusive_ptr<VsgBackend> backend(new VsgBackend());
    std::size_t                     seen = 0;
    backend->setDiagnosticSink([&seen](const vn::graphics::RenderDiagnostic& diagnostic) {
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
    const vn::intrusive_ptr<RenderTarget> target(new RenderTarget());
    target->attachColor(RenderTarget::ColorFormat::RGBA8);
    target->attachDepth(RenderTarget::DepthFormat::D32F);
    target->setSize(64, 64);

    const TriangleFixture             triangle = makeTriangle();
    const vn::intrusive_ptr<vn::graphics::Camera> camera = cameraLookingAt(0.5);
    const vn::intrusive_ptr<RenderPass>             pass(new RenderPass());
    const vn::graphics::ClearPolicy                 clear{ vn::Color(0, 64, 0, 255), true };

    const auto drive = [&]() {
        backend->beginFrame();
        backend->beginPass(pass.get());
        backend->setPassOrder(0);
        backend->setRenderTarget(target.get());
        backend->setClearPolicy(clear);
        backend->setDepthMode(vn::graphics::DepthMode::TestAndWrite);
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
    vn::graphics::ReadbackResult why = vn::graphics::ReadbackResult::Failed;
    ASSERT_TRUE(backend->readColorBuffer(target.get(), 0, pixels, &why)) << "the pixels are read back";
    EXPECT_EQ(why, vn::graphics::ReadbackResult::Ok);
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
    EXPECT_EQ(why, vn::graphics::ReadbackResult::Ok);
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
    EXPECT_EQ(why, vn::graphics::ReadbackResult::Invalid);
    EXPECT_TRUE(scratch.empty()) << "a refused read leaves the destination untouched";
    const vn::intrusive_ptr<RenderTarget> unknown(new RenderTarget());
    unknown->attachColor(RenderTarget::ColorFormat::RGBA8);
    unknown->setSize(8, 8);
    EXPECT_FALSE(backend->readColorBuffer(unknown.get(), 0, scratch, &why));
    EXPECT_EQ(why, vn::graphics::ReadbackResult::NotReady) << "never announced to this backend";
    const vn::intrusive_ptr<RenderTarget> floats(new RenderTarget());
    floats->attachColor(RenderTarget::ColorFormat::RGBA16F);
    floats->setSize(8, 8);
    backend->setRenderTarget(floats.get());  // held and built (no frame is needed to build one)
    EXPECT_FALSE(backend->readColorBuffer(floats.get(), 0, scratch, &why));
    EXPECT_EQ(why, vn::graphics::ReadbackResult::Unsupported) << "this backend packs RGBA8 only";
    EXPECT_EQ(backend->deviceWaits(), waits_before + 2U) << "a refused readback never stops the device";

    // 4. A BORROWED depth is the lender's to read: the borrower says Unsupported without touching the device
    // (the SDK's own rule - the image belongs to the source target).
    const vn::intrusive_ptr<RenderTarget> lender(new RenderTarget());
    lender->attachColor(RenderTarget::ColorFormat::RGBA8);
    lender->attachDepth(RenderTarget::DepthFormat::D32F);
    lender->setSize(16, 16);
    backend->setRenderTarget(lender.get());
    const vn::intrusive_ptr<RenderTarget> borrower(new RenderTarget());
    borrower->attachColor(RenderTarget::ColorFormat::RGBA8);
    borrower->shareDepth(lender);
    borrower->setSize(16, 16);
    backend->setRenderTarget(borrower.get());

    HostTargets& targets = BackendContentAccess::targets(*backend);
    ASSERT_NE(targets.find(borrower.get()), nullptr);
    ASSERT_NE(targets.find(borrower.get())->target, nullptr) << "the borrower was built on the lender's depth";
    EXPECT_FALSE(backend->readDepthBuffer(borrower.get(), depths, &why));
    EXPECT_EQ(why, vn::graphics::ReadbackResult::Unsupported) << "read it through the source target";

    // The lender is held and BUILT, but no frame has ever drawn into it: there is nothing to read back yet
    // (and reading it costs nothing - not even a wait).
    EXPECT_FALSE(backend->readColorBuffer(lender.get(), 0, scratch, &why));
    EXPECT_EQ(why, vn::graphics::ReadbackResult::NotReady) << "nothing has been recorded into it";
    EXPECT_EQ(backend->deviceWaits(), waits_before + 2U) << "and none of those refusals stops the device";

    // 5. A target that was released is NotReady again - the SDK's own reading ("never rendered, or already
    // released"), not Unsupported: a re-announced handle can make it readable.
    backend->releaseRenderTarget(target.get());
    EXPECT_FALSE(backend->readColorBuffer(target.get(), 0, pixels, &why));
    EXPECT_EQ(why, vn::graphics::ReadbackResult::NotReady);

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

    vn::intrusive_ptr<VsgBackend> backend(new VsgBackend());
    std::size_t                     seen = 0;
    backend->setDiagnosticSink([&seen](const vn::graphics::RenderDiagnostic& diagnostic) {
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
    const vn::intrusive_ptr<ShaderProgram> program(new ShaderProgram());
    {
        ShaderStage vertex;
        vertex.type   = ShaderStageType::Vertex;
        vertex.source = vn::String(reinterpret_cast<const char8_t*>(
            "layout(location = 0) in vec3 position;\n"
            "layout(set = 0, binding = 0, std140) uniform VineViewBlock {\n"
            "    mat4 view; mat4 inv_view; mat4 proj; mat4 view_proj; vec4 cam_pos; vec4 frame; } vb;\n"
            "void main() { gl_Position = vb.view_proj * vec4(position, 1.0); }\n"));
        ShaderStage fragment;
        fragment.type   = ShaderStageType::Fragment;
        fragment.source = vn::String(reinterpret_cast<const char8_t*>(
            "layout(location = 0) out vec4 outColor;\n"
            "layout(set = 0, binding = 0, std140) uniform VineViewBlock {\n"
            "    mat4 view; mat4 inv_view; mat4 proj; mat4 view_proj; vec4 cam_pos; vec4 frame; } vb;\n"
            "void main() { outColor = vec4(vb.frame.y / 160.0, abs(vb.cam_pos.x), vb.frame.z / 192.0, 1.0); }\n"));
        program->addStage(vertex);
        program->addStage(fragment);
    }

    const vn::intrusive_ptr<Geometry> geometry(new Geometry());
    const vn::intrusive_ptr<vn::Buffer<float>> positions = vn::intrusive_ptr<vn::Buffer<float>>(
        new vn::Buffer<float>(std::vector<float>{ -0.4F, -0.4F, 0.5F, 0.4F, -0.4F, 0.5F, 0.0F, 0.6F, 0.5F }));
    const vn::intrusive_ptr<vn::Buffer<std::uint32_t>> indices =
        vn::intrusive_ptr<vn::Buffer<std::uint32_t>>(
            new vn::Buffer<std::uint32_t>(std::vector<std::uint32_t>{ 0U, 1U, 2U }));
    geometry->setPositions(positions);
    geometry->setIndices(indices);
    geometry->setRevision(1U);

    const vn::intrusive_ptr<Material> material(new Material());
    material->setDiffuse(vn::Colorf(0.2F, 0.3F, 0.4F, 1.0F));

    RenderCommand command;
    command.geometry = geometry;
    command.material = material;
    command.program  = program;
    const std::vector<RenderCommand> commands{ command };

    const vn::intrusive_ptr<vn::graphics::Camera> camera = cameraLookingAt(0.5);
    const vn::intrusive_ptr<RenderPass>             pass(new RenderPass());
    const vn::graphics::ClearPolicy                 clear{ vn::Color(0, 64, 0, 255), true };

    const auto drive = [&] {
        backend->beginFrame();
        backend->beginPass(pass.get());
        backend->setPassOrder(0);
        backend->setRenderTarget(nullptr);
        backend->setClearPolicy(clear);
        backend->setDepthMode(vn::graphics::DepthMode::TestAndWrite);
        backend->render(commands, camera.get());
        backend->endPass();
        backend->endFrame();
        backend->swapBuffers();
    };
    const auto settle = [&] {
        drive();
        drive();
    };

    drive();
    settle();

    // 1. The picture at the size the host's surface has: the triangle left of centre (the camera looks half a
    // unit to its right) with the extent encoded in it.
    const auto before_left  = host.waitForPixel(kWidth / 4, kHeight / 2, arrived, std::chrono::milliseconds{ 500 });
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

    ASSERT_TRUE(host.resize(96, 64))
        << "the platform must have the new size before it is announced: the backend follows the surface by "
           "reading it once (see TestHostWindow::resize)";
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
    const auto after_left  = host.waitForPixel(96 / 4, 64 / 2, arrived, std::chrono::milliseconds{ 500 });
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

    vn::intrusive_ptr<VsgBackend> backend(new VsgBackend());
    std::size_t                     seen = 0;
    backend->setDiagnosticSink([&seen](const vn::graphics::RenderDiagnostic& diagnostic) {
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
    const vn::intrusive_ptr<RenderTarget> shadow(new RenderTarget());
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
    vn::vsg::core::TargetFacts row;
    targets.facts(*shadow_entry, row);
    EXPECT_TRUE(row.depth.has_depth);
    EXPECT_FALSE(row.depth.borrowed);
    EXPECT_TRUE(row.depth.promotion) << "the built target's own policy answers";
    EXPECT_TRUE(vn::vsg::core::depthPlan(row.depth).sampleable) << "and the plan promises the depth to a shader";

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

    vn::intrusive_ptr<VsgBackend> backend(new VsgBackend());
    std::size_t                     seen = 0;
    backend->setDiagnosticSink([&seen](const vn::graphics::RenderDiagnostic& diagnostic) {
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
    const vn::intrusive_ptr<ShaderProgram> program(new ShaderProgram());
    {
        ShaderStage vertex;
        vertex.type   = ShaderStageType::Vertex;
        vertex.source = vn::String(reinterpret_cast<const char8_t*>(
            "layout(location = 0) in vec3 position;\n"
            "void main() { gl_Position = vec4(position, 1.0); }\n"));
        ShaderStage fragment;
        fragment.type   = ShaderStageType::Fragment;
        fragment.source = vn::String(reinterpret_cast<const char8_t*>(
            "layout(location = 0) out vec4 outColor;\n"
            "layout(set = 0, binding = 2, std140) uniform VineMaterialBlock\n"
            "{\n"
            "    vec4 ambient; vec4 diffuse; vec4 specular; float shininess;\n"
            "} material;\n"
            "void main() { outColor = material.diffuse; }\n"));
        program->addStage(vertex);
        program->addStage(fragment);
    }

    const vn::intrusive_ptr<Geometry> geometry(new Geometry());
    const vn::intrusive_ptr<vn::Buffer<float>> positions = vn::intrusive_ptr<vn::Buffer<float>>(
        new vn::Buffer<float>(std::vector<float>{ -0.4F, -0.4F, 0.5F, 0.4F, -0.4F, 0.5F, 0.0F, 0.6F, 0.5F }));
    const vn::intrusive_ptr<vn::Buffer<std::uint32_t>> indices =
        vn::intrusive_ptr<vn::Buffer<std::uint32_t>>(
            new vn::Buffer<std::uint32_t>(std::vector<std::uint32_t>{ 0U, 1U, 2U }));
    geometry->setPositions(positions);
    geometry->setIndices(indices);
    geometry->setRevision(1U);

    const vn::intrusive_ptr<Material> material(new Material());
    material->setDiffuse(vn::Colorf(0.25F, 0.5F, 0.75F, 1.0F));

    RenderCommand command;
    command.geometry = geometry;
    command.material = material;
    command.program  = program;
    const std::vector<RenderCommand> commands{ command };

    const vn::intrusive_ptr<vn::graphics::Camera> camera = cameraLookingAt(0.5);
    const vn::intrusive_ptr<RenderPass>             pass(new RenderPass());
    const vn::graphics::ClearPolicy                 clear{ vn::Color(0, 64, 0, 255), true };

    const auto drive = [&] {
        backend->beginFrame();
        backend->beginPass(pass.get());
        backend->setPassOrder(0);
        backend->setRenderTarget(nullptr);
        backend->setClearPolicy(clear);
        backend->setDepthMode(vn::graphics::DepthMode::TestAndWrite);
        backend->render(commands, camera.get());
        backend->endPass();
        backend->endFrame();
        backend->swapBuffers();
    };
    const auto settle = [&] {
        drive();
        drive();
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
    const auto first = host.waitForPixel(kWidth / 2, kHeight / 2, arrived, std::chrono::milliseconds{ 500 });
    EXPECT_TRUE(isColourByte(first[1], 0.5)) << "the material's green byte reached the picture, got ("
                                            << static_cast<int>(first[0]) << ", " << static_cast<int>(first[1])
                                            << ", " << static_cast<int>(first[2]) << ") read-error "
                                            << static_cast<int>(host.readError())
                                            << " (see HostWindowReadTest: a refusal is not a black pixel)";
    EXPECT_TRUE(outer(first, 0.25, 0.75)) << "and so did red and blue, in the server's order";

    // The host edits the material and DOES NOTHING ELSE. There is no announcement to make: the SDK's
    // RenderBackend has no "this material changed" entry point, so the touch the facade makes when the next
    // frame commands the material is the whole mechanism (see ContentStore::updateMaterial).
    material->setDiffuse(vn::Colorf(0.75F, 0.25F, 0.5F, 1.0F));
    const std::uint64_t builds_before = store->builds();
    drive();
    EXPECT_EQ(store->builds(), builds_before + 1U) << "the edit replaced exactly that material's row";
    settle();
    const auto edited = host.waitForPixel(kWidth / 2, kHeight / 2, arrived, std::chrono::milliseconds{ 500 });
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

TEST(VsgBackendTest, MeasureWhatEachKindOfEditCostsPerFrame)
{
    // THE QUESTION. A host keeps editing a live scene one thing at a time - a material's value, the
    // geometry's vertex bytes, a StateNode's state, the node's program - and the backend has to answer each
    // edit with the cheapest thing that is CORRECT. Which answer is correct is written down in
    // core/Keys.hpp: the program, its revision, the vertex layout, the target shape, the program variant
    // and the topology CLASS are identity (changing one is a compile); material values, stream bytes and
    // depth / cull / polygon / blend are DATA and must never recompile anything.
    //
    // This recipe drives the facade the way the material case above does - a live window, one pass, one
    // command - edits ONE thing per regime, times the frame's two halves (the record+compile half, then
    // the commit half) and reads the live counters around each regime. The numbers are PRINTED for the
    // design log; what is ASSERTED is machine-independent: a steady frame touches nothing, an edit that is
    // data creates no variant and no upload it should not, and the two edits that ARE identity (the
    // program, the topology class) create exactly one. Those two rows are also the recipe's positive
    // control: they are what shows the `created` counter CAN move in this very setup, which is what makes
    // the "no compile" rows falsifiable instead of vacuous.
    //
    // Run it where the numbers matter: ./build-release/bin/test_vsg --gtest_filter='*MeasureWhatEachKind*'
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

    constexpr int kWidth           = 128;
    constexpr int kHeight          = 96;
    constexpr int kGridSide        = 32;
    constexpr int kFramesPerRegime = 12;

    TestHostWindow host(connection, screen, kWidth, kHeight);

    vn::intrusive_ptr<VsgBackend> backend(new VsgBackend());
    std::size_t                   seen = 0;
    backend->setDiagnosticSink([&seen](const vn::graphics::RenderDiagnostic& diagnostic) {
        ++seen;
        std::printf("[cost] diagnostic: severity=%d category=%d message=%s\n",
                    static_cast<int>(diagnostic.severity), static_cast<int>(diagnostic.category),
                    as_bytes(diagnostic.message).c_str());
    });
    backend->setWindowHandle(host.handle());
    ASSERT_TRUE(backend->initialize());

    // The two programs: both shade with the material's own diffuse colour, so the picture cannot tell them
    // apart - what differs is the TEXT, and that is what the pipeline key carries (see PipelineKey::program).
    const auto material_program = [](const char* body) {
        const vn::intrusive_ptr<ShaderProgram> program(new ShaderProgram());
        ShaderStage                            vertex;
        vertex.type   = ShaderStageType::Vertex;
        vertex.source = vn::String(reinterpret_cast<const char8_t*>(
            "layout(location = 0) in vec3 position;\n"
            "void main() { gl_Position = vec4(position, 1.0); }\n"));
        std::string text = "layout(location = 0) out vec4 outColor;\n"
                           "layout(set = 0, binding = 2, std140) uniform VineMaterialBlock\n"
                           "{\n"
                           "    vec4 ambient; vec4 diffuse; vec4 specular; float shininess;\n"
                           "} material;\n"
                           "void main() { ";
        text += body;
        text += " }\n";
        ShaderStage fragment;
        fragment.type   = ShaderStageType::Fragment;
        fragment.source = vn::String(reinterpret_cast<const char8_t*>(text.data()), text.size());
        program->addStage(vertex);
        program->addStage(fragment);
        return program;
    };
    const vn::intrusive_ptr<ShaderProgram> program_a = material_program("outColor = material.diffuse;");
    const vn::intrusive_ptr<ShaderProgram> program_b =
        material_program("vec4 tinted = material.diffuse * 1.0; outColor = tinted;");

    vn::intrusive_ptr<Geometry>          geometry(new Geometry());
    vn::intrusive_ptr<vn::Buffer<float>> positions(new vn::Buffer<float>(gridPositions(kGridSide)));
    geometry->setPositions(positions);
    geometry->setIndices(vn::intrusive_ptr<const vn::Buffer<std::uint32_t>>(
        new vn::Buffer<std::uint32_t>(gridIndices(kGridSide))));
    geometry->bumpRevision();

    const vn::intrusive_ptr<Material> material(new Material());
    material->setDiffuse(vn::Colorf(0.3F, 0.4F, 0.3F, 1.0F));

    std::vector<RenderCommand> commands(1U);
    commands[0].geometry = geometry;
    commands[0].material = material;
    commands[0].program  = program_a;

    const vn::intrusive_ptr<vn::graphics::Camera> camera = cameraLookingAt(0.0);
    const vn::intrusive_ptr<RenderPass>           pass(new RenderPass());
    const vn::graphics::ClearPolicy               clear{ vn::Color(0, 64, 0, 255), true };

    ContentStore*    store    = BackendContentAccess::store(*backend);
    ContentAssembly* assembly = BackendContentAccess::assembly(*backend);
    ASSERT_NE(store, nullptr) << "a live session owns the content tables its frames are built from";
    ASSERT_NE(assembly, nullptr);
    VariantPool& pool = BackendContentAccess::pool(*backend);

    // One frame, split at the seam between the two halves of the cost: everything up to endFrame() is the
    // frame's own bookkeeping (facts, plan, blocks, streams), and swapBuffers() is the commit that records,
    // submits and presents. The number a host feels is the sum; the two halves are printed apart because
    // that is what tells a backend cost from a presentation cost.
    const auto drive = [&]() {
        const auto began = std::chrono::steady_clock::now();
        backend->beginFrame();
        backend->beginPass(pass.get());
        backend->setPassOrder(0);
        backend->setRenderTarget(nullptr);
        backend->setClearPolicy(clear);
        backend->setDepthMode(vn::graphics::DepthMode::TestAndWrite);
        backend->render(commands, camera.get());
        backend->endPass();
        backend->endFrame();
        const auto recorded  = std::chrono::steady_clock::now();
        backend->swapBuffers();
        const auto committed = std::chrono::steady_clock::now();
        const auto micros    = [](auto from, auto to) {
            return std::chrono::duration_cast<std::chrono::duration<double, std::micro>>(to - from).count();
        };
        return std::pair<double, double>{ micros(began, recorded), micros(recorded, committed) };
    };

    std::printf("\n");  // the measurements get their own lines
    for (int settle = 0; settle < 6; ++settle) {
        drive();  // the session's opening frames compile the initial pipelines and settle the window
    }

    const auto run = [&](const char* label, int frames, auto&& edit) {
        const EditCostCounters before = readEditCosts(*store, *assembly, pool);
        double                 record_us = 0.0;
        double                 commit_us = 0.0;
        for (int frame = 0; frame < frames; ++frame) {
            edit(frame);
            const auto [record, commit] = drive();
            record_us += record;
            commit_us += commit;
        }
        const EditCostCounters after  = readEditCosts(*store, *assembly, pool);
        const EditCostDelta    change = editCostDelta(before, after);
        std::printf("[cost] %-32s record %8.1f us/f  commit %8.1f us/f"
                    " | builds %+4lld uploads %+4lld created %+4lld reused %+4lld\n",
                    label, record_us / frames, commit_us / frames, static_cast<long long>(change.builds),
                    static_cast<long long>(change.uploads), static_cast<long long>(change.created),
                    static_cast<long long>(change.reused));
        return std::pair<EditCostCounters, EditCostCounters>{ before, after };
    };

    // 0. STEADY: the baseline every other row is compared against.
    {
        const auto [before, after] = run("steady (nothing edited)", kFramesPerRegime, [](int) {});
        const EditCostDelta change = editCostDelta(before, after);
        EXPECT_EQ(change.builds, 0) << "a steady frame builds no row";
        EXPECT_EQ(change.uploads, 0) << "and uploads nothing";
        EXPECT_EQ(change.created, 0) << "and compiles nothing";
    }

    // 1. MATERIAL: the value is edited in place every frame. The store's own compare-and-write replaces the
    //    row, and that is the WHOLE cost: a material value is not identity.
    {
        const auto [before, after] = run("material edited every frame", kFramesPerRegime, [&](int frame) {
            const float shade = 0.3F + 0.02F * static_cast<float>(frame + 1);
            material->setDiffuse(vn::Colorf(shade, 0.4F, shade, 1.0F));
        });
        const EditCostDelta change = editCostDelta(before, after);
        EXPECT_EQ(change.builds, kFramesPerRegime) << "each edit replaces exactly the material's row";
        EXPECT_EQ(change.created, 0) << "a material VALUE is data: the pipeline key cannot see it";
        EXPECT_EQ(change.uploads, 0) << "and it lands as a block write, not as an upload";
        drive();
        const auto pixel = host.waitForPixel(kWidth / 2, kHeight / 2, arrived, std::chrono::milliseconds{ 500 });
        EXPECT_TRUE(isColourByte(pixel[1], 0.4)) << "the mesh is still what the centre probe sees";
        EXPECT_TRUE(isColourByte(pixel[0], 0.54) && isColourByte(pixel[2], 0.54))
            << "and the LAST edited colour is in the picture (read-error " << static_cast<int>(host.readError())
            << ")";
    }

    // 2. GEOMETRY, A NEW BUFFER: the host re-created the vertex data every frame (a new Buffer object) and
    //    announced it - the engine's contract for "the data changed". The bytes must reach the device every
    //    frame, and the LAYOUT is unchanged, so the pipeline key cannot see any of it.
    //
    //    TWO uploads per frame, and the count is the layer's own shape rather than a waste: a stream key carries
    //    the GEOMETRY's announced revision (see GeometryFacts), so one announcement moves the identity of the
    //    channel AND of the index stream - the mesh's bytes and its index list both go up. What a revision
    //    announcement must never do is compile, and that is what the `created` row below gates on.
    {
        const auto [before, after] = run("geometry: a new buffer", kFramesPerRegime, [&](int) {
            geometry->setPositions(
                vn::intrusive_ptr<const vn::Buffer<float>>(new vn::Buffer<float>(gridPositions(kGridSide))));
            geometry->bumpRevision();
        });
        const EditCostDelta change = editCostDelta(before, after);
        EXPECT_GE(change.uploads, kFramesPerRegime)
            << "the bytes are re-sent every frame (one upload per stream the geometry names)";
        EXPECT_EQ(change.builds, kFramesPerRegime) << "each announcement is a new revision: one row";
        EXPECT_EQ(change.created, 0) << "the vertex layout did not change, so no pipeline is compiled";
    }

    // 3. GEOMETRY, THE SAME BUFFER: the host wrote through the buffer it had already handed over and
    //    announced it. There is NO in-place refresh path in production - the tested pure function that would
    //    have decided one had no caller and is gone (design log §11.16cr) - so a revision announcement
    //    rebuilds the node and re-sends every stream, and this row's point is that the spelling the host picks
    //    (a new buffer object, or writing through the old one) makes no difference at this layer. The shift is
    //    the delivery proof: 12 steps of +0.04 move the mesh's left edge from -0.9 to -0.42, so a probe that
    //    saw mesh has to see background.
    positions = vn::intrusive_ptr<vn::Buffer<float>>(new vn::Buffer<float>(gridPositions(kGridSide)));
    geometry->setPositions(positions);
    geometry->bumpRevision();
    drive();
    {
        const auto [before, after] = run("geometry: same buffer, new bytes", kFramesPerRegime, [&](int frame) {
            std::vector<float> shifted = gridPositions(kGridSide);
            const float        step    = 0.04F * static_cast<float>(frame + 1);
            for (std::size_t index = 0; index + 2 < shifted.size(); index += 3U) {
                shifted[index] += step;
            }
            const auto bytes = positions->data();
            for (std::size_t index = 0; index < shifted.size(); ++index) {
                bytes[index] = shifted[index];
            }
            positions->setRevision(positions->revision() + 1U);
            geometry->bumpRevision();
        });
        const EditCostDelta change = editCostDelta(before, after);
        EXPECT_GE(change.uploads, kFramesPerRegime) << "edited bytes still have to reach the device";
        EXPECT_EQ(change.created, 0) << "and the pipeline key cannot see bytes";
        drive();
        const auto left = host.waitForPixel(8, kHeight / 2, arrived, std::chrono::milliseconds{ 500 });
        EXPECT_TRUE(isGreenClear(left)) << "the shifted bytes reached the frame: the left edge is background"
                                       << " again (read-error " << static_cast<int>(host.readError()) << ")";
        const auto centre = host.waitForPixel(kWidth / 2, kHeight / 2, arrived, std::chrono::milliseconds{ 500 });
        EXPECT_TRUE(isColourByte(centre[0], 0.54)) << "and the middle is still the mesh";
    }

    // 4. STATE: a StateNode's cull mode flips every frame. State is DYNAMIC (core::DynamicState: "changing it
    //    must never recompile anything"), so the frame answers with a set command - and the picture must keep
    //    the mesh, which is what shows the flip reached the rasteriser instead of a local variable.
    {
        const auto [before, after] = run("state: cull mode flipped", kFramesPerRegime, [&](int frame) {
            commands[0].renderState.cullMode =
                (frame % 2 == 0) ? vn::graphics::CullMode::Back : vn::graphics::CullMode::None;
        });
        const EditCostDelta change = editCostDelta(before, after);
        EXPECT_EQ(change.builds, 0) << "a state change is not content: no row is built";
        EXPECT_EQ(change.uploads, 0) << "and nothing is uploaded";
        EXPECT_EQ(change.created, 0) << "cull is dynamic state: the pipeline key cannot see it (core/Keys.hpp)";
        commands[0].renderState.cullMode = vn::graphics::CullMode::Back;
        drive();
        const auto pixel = host.waitForPixel(kWidth / 2, kHeight / 2, arrived, std::chrono::milliseconds{ 500 });
        EXPECT_TRUE(isColourByte(pixel[0], 0.54)) << "Back-face culling kept the mesh visible";
    }

    // 5. THE NODE'S SHADER: the command's program alternates between two programs every frame. A PROGRAM is
    //    identity, so the FIRST frame with the second one compiles a pipeline - reported on its own line as
    //    the cold number - and every frame after that is a variant switch the pool already has.
    {
        const EditCostCounters cold_before    = readEditCosts(*store, *assembly, pool);
        commands[0].program                   = program_b;
        const auto [cold_record, cold_commit] = drive();
        const EditCostCounters cold_after     = readEditCosts(*store, *assembly, pool);
        const EditCostDelta    cold_change    = editCostDelta(cold_before, cold_after);
        std::printf("[cost] %-32s record %8.1f us    commit %8.1f us"
                    " | builds %+4lld uploads %+4lld created %+4lld reused %+4lld\n",
                    "program switched (first, cold)", cold_record, cold_commit,
                    static_cast<long long>(cold_change.builds), static_cast<long long>(cold_change.uploads),
                    static_cast<long long>(cold_change.created), static_cast<long long>(cold_change.reused));
        EXPECT_EQ(cold_change.created, 1) << "the first frame with a program compiles exactly one pipeline";

        const auto [before, after] = run("program switched every frame", kFramesPerRegime, [&](int frame) {
            commands[0].program = (frame % 2 == 0) ? program_a : program_b;
        });
        const EditCostDelta change = editCostDelta(before, after);
        EXPECT_EQ(change.created, 0) << "both variants are in the pool: no frame after the first compiles";
        EXPECT_GE(change.reused, kFramesPerRegime) << "every frame's switch is served by the variant pool";
        EXPECT_EQ(change.uploads, 0) << "switching a program moves no bytes";
    }

    // 6. THE BOUNDARY: a topology CLASS change IS identity - the key carries the topology, because dynamic
    //    primitive topology may only switch WITHIN a class (the reason is on PipelineKey::topology). This row
    //    is also the positive control for every "no compile" claim above: it shows the counter moving for an
    //    edit that really is identity, in this very setup.
    {
        const EditCostCounters before    = readEditCosts(*store, *assembly, pool);
        commands[0].renderState.topology = vn::graphics::Topology::Lines;
        drive();
        const EditCostCounters after = readEditCosts(*store, *assembly, pool);
        EXPECT_EQ(after.created - before.created, 1U)
            << "a different topology class is a different pipeline (see PipelineKey::topology)";
        EXPECT_EQ(after.uploads - before.uploads, 0U) << "and it moves no bytes either";
    }

    EXPECT_EQ(backend->deviceWaits(), 0U) << "none of these edits stops the device";
    EXPECT_EQ(seen, 0U) << "and every one of them is served silently";
    EXPECT_EQ(backend->diagnosticCount(), 0U);

    backend->shutdown();
    EXPECT_TRUE(host.alive());
}

TEST(VsgBackendTest, AFrameThatRanOutOfBudgetGrowsTheStorageBeforeTheNextFrame)
{
    // ONE BLOCK OF BUDGET, TWO DRAWS - AND WHAT THE FRAME DOES ABOUT IT. Frame 1's second draw block is
    // refused (the pass keeps drawing what fits, and the refusal is reported once); the storage REMEMBERS
    // what the frame tried, so the next beginFrame() replaces the whole storage: a NEW buffer (the old one
    // is parked, because a submitted frame may still name its bytes), the cached sets repointed at it, and
    // the growth reported as its own fact. The pixels are the evidence for both halves: frame 1 shows the
    // first command's colour only, frame 2 shows the second command's colour on top - which cannot happen
    // unless BOTH draw blocks landed in the replacement's bytes through sets that were repointed.
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

    vn::intrusive_ptr<VsgBackend> backend(new VsgBackend());
    std::vector<std::string>      messages;
    backend->setDiagnosticSink([&messages](const vn::graphics::RenderDiagnostic& diagnostic) {
        messages.push_back(as_bytes(diagnostic.message));
    });
    backend->setWindowHandle(host.handle());
    ASSERT_TRUE(backend->initialize());
    EXPECT_EQ(backend->diagnosticCount(), 0U);

    // The same pair the material case uses: clip-space positions passed through, and a fragment stage
    // shading with the material's own diffuse colour - so one probe at the window's centre sees whichever
    // drawing call drew LAST.
    const vn::intrusive_ptr<ShaderProgram> program(new ShaderProgram());
    {
        ShaderStage vertex;
        vertex.type   = ShaderStageType::Vertex;
        vertex.source = vn::String(reinterpret_cast<const char8_t*>(
            "layout(location = 0) in vec3 position;\n"
            "void main() { gl_Position = vec4(position, 1.0); }\n"));
        ShaderStage fragment;
        fragment.type   = ShaderStageType::Fragment;
        fragment.source = vn::String(reinterpret_cast<const char8_t*>(
            "layout(location = 0) out vec4 outColor;\n"
            "layout(set = 0, binding = 2, std140) uniform VineMaterialBlock\n"
            "{\n"
            "    vec4 ambient; vec4 diffuse; vec4 specular; float shininess;\n"
            "} material;\n"
            "void main() { outColor = material.diffuse; }\n"));
        program->addStage(vertex);
        program->addStage(fragment);
    }

    const vn::intrusive_ptr<Geometry> geometry(new Geometry());
    const vn::intrusive_ptr<vn::Buffer<float>> positions = vn::intrusive_ptr<vn::Buffer<float>>(
        new vn::Buffer<float>(std::vector<float>{ -0.4F, -0.4F, 0.5F, 0.4F, -0.4F, 0.5F, 0.0F, 0.6F, 0.5F }));
    const vn::intrusive_ptr<vn::Buffer<std::uint32_t>> indices =
        vn::intrusive_ptr<vn::Buffer<std::uint32_t>>(
            new vn::Buffer<std::uint32_t>(std::vector<std::uint32_t>{ 0U, 1U, 2U }));
    geometry->setPositions(positions);
    geometry->setIndices(indices);
    geometry->setRevision(1U);

    const vn::intrusive_ptr<Material> first_material(new Material());
    first_material->setDiffuse(vn::Colorf(0.25F, 0.6F, 0.25F, 1.0F));
    const vn::intrusive_ptr<Material> second_material(new Material());
    second_material->setDiffuse(vn::Colorf(0.75F, 0.2F, 0.75F, 1.0F));

    RenderCommand first;
    first.geometry = geometry;
    first.material = first_material;
    first.program  = program;
    RenderCommand second;
    second.geometry = geometry;
    second.material = second_material;
    second.program  = program;
    const std::vector<RenderCommand> commands{ first, second };

    const vn::intrusive_ptr<vn::graphics::Camera> camera = cameraLookingAt(0.5);
    const vn::intrusive_ptr<RenderPass>           pass(new RenderPass());
    const vn::graphics::ClearPolicy               clear{ vn::Color(0, 64, 0, 255), true };

    const auto drive = [&] {
        backend->beginFrame();
        backend->beginPass(pass.get());
        backend->setPassOrder(0);
        backend->setRenderTarget(nullptr);
        backend->setClearPolicy(clear);
        // Depth off, so the second drawing call is simply the one whose colour the probe sees.
        backend->setDepthMode(vn::graphics::DepthMode::Disabled);
        backend->render(commands, camera.get());
        backend->endPass();
        backend->endFrame();
        backend->swapBuffers();
    };

    // Put the drive on a storage that fits ONE draw block: the frame below will ask for two.
    BlockStorage::Layout tight;
    tight.draws.blocks_per_frame = 1U;
    ASSERT_TRUE(BackendContentAccess::useBlockStorage(*backend, tight));
    BlockStorage* const tight_storage = BackendContentAccess::storage(*backend);
    ASSERT_NE(tight_storage, nullptr);
    const std::uint64_t tight_capacity = tight_storage->capacityBytes();

    drive();
    EXPECT_EQ(tight_storage->overflows(), 1U) << "the frame's second draw block was refused";
    EXPECT_EQ(tight_storage->growthNeeded().draws, 2U) << "the frame TRIED two blocks: that is the request";
    EXPECT_EQ(tight_storage->capacityBytes(), tight_capacity) << "the storage itself never grows";
    ASSERT_EQ(backend->diagnosticCount(), 1U) << "the refused drawing call is reported once";
    EXPECT_NE(std::string::npos, messages.back().find("budget is full"));
    const auto dropped = host.waitForPixel(
        kWidth / 2, kHeight / 2,
        [](const std::array<std::uint8_t, 3>& pixel) { return isColourByte(pixel[1], 0.6); },
        std::chrono::milliseconds{ 500 });
    EXPECT_TRUE(isColourByte(dropped[1], 0.6))
        << "frame 1 shows the FIRST command's colour: the second was dropped (read-error "
        << static_cast<int>(host.readError()) << ")";

    drive();  // its beginFrame() is where the growth happens
    BlockStorage* const grown_storage = BackendContentAccess::storage(*backend);
    ASSERT_NE(grown_storage, tight_storage) << "the storage was replaced, not resized";
    EXPECT_GT(grown_storage->capacityBytes(), tight_capacity);
    EXPECT_EQ(grown_storage->layout().draws.blocks_per_frame, 2U) << "one over a budget of one doubles it";
    EXPECT_EQ(grown_storage->overflows(), 0U) << "both draw blocks fitted";
    EXPECT_FALSE(grown_storage->growthNeeded().needed());

    ASSERT_EQ(backend->diagnosticCount(), 2U) << "the growth is its own fact, said once";
    EXPECT_NE(std::string::npos, messages.back().find("grew")) << "and the host can see why the refusals stop";
    const auto grown_pixel = host.waitForPixel(
        kWidth / 2, kHeight / 2,
        [](const std::array<std::uint8_t, 3>& pixel) { return isColourByte(pixel[1], 0.2); },
        std::chrono::milliseconds{ 500 });
    EXPECT_TRUE(isColourByte(grown_pixel[1], 0.2))
        << "frame 2 shows the SECOND command on top: both blocks landed in the replacement's bytes";

    EXPECT_EQ(backend->deviceWaits(), 0U) << "growing between frames stops nothing";
    backend->shutdown();
    EXPECT_TRUE(host.alive());
}

#endif  // !defined(_WIN32)
