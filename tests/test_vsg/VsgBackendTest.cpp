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

#include <vine/Color.hpp>
#include <vine/graphics/RenderBackend.hpp>
#include <vine/graphics/RenderCommand.hpp>
#include <vine/graphics/RenderDiagnostic.hpp>
#include <vine/graphics/RenderPass.hpp>
#include <vine/graphics/RenderTarget.hpp>
#include <vine/vsg/internal/BackendContent.hpp>
#include <vine/vsg/internal/ContentAssembly.hpp>
#include <vine/vsg/internal/ContentHalves.hpp>
#include <vine/vsg/internal/ContentSets.hpp>
#include <vine/vsg/internal/ContentStore.hpp>
#include <vine/vsg/internal/Device.hpp>
#include <vine/vsg/internal/DeviceProbe.hpp>
#include <vine/vsg/internal/HostTargets.hpp>
#include <vine/vsg/internal/MaterialImages.hpp>
#include <vine/vsg/internal/VsgBackend.hpp>
#include <vine/vsg/internal/WindowTarget.hpp>
#include <vine/graphics/backend/FrameCompiler.hpp>

#include <vine/Buffer.hpp>
#include <vine/graphics/Camera.hpp>
#include <vine/graphics/DepthMode.hpp>
#include <vine/graphics/BuiltinShaders.hpp>
#include <vine/graphics/Geometry.hpp>
#include <vine/graphics/Material.hpp>
#include <vine/graphics/ShaderAbi.hpp>
#include <vine/graphics/ShaderProgram.hpp>
#include <vine/graphics/Texture.hpp>
#include <vine/imaging/Image.hpp>

// TestHostWindow is an XCB window, so the cases that use it reach for the connection themselves; the guard
// matches the one around those cases.
#if !defined(_WIN32)
#    include <xcb/xcb.h>
#endif

#include "SdkPixelFixture.hpp"
#include "TestFrame.hpp"
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
using vn::graphics::backend::TargetFacts;
using vn::graphics::backend::depthPlan;
using vn::vsg::BlockStorage;
using vn::vsg::HostTargets;
using vn::vsg::PassRegistry;
using vn::graphics::backend::VariantPool;
using vn::vsg::WindowTarget;
using vn::vsg::detail::BackendContentAccess;

/**
 * @brief The device these cases drive directly, with the ONE window a case presents through held explicitly.
 *
 * WHY IT IS NOT `VsgBackend`: the DEVICE IS NOT A WINDOW (see `.ai/design/device-is-not-a-window.md`) - it hands out
 * a target per window and has none of `present / resize / nativeHandle` of its own. A case here drives one window, so
 * this holds THAT window's target and names it in the frame, which is exactly what a host does; everything the cases
 * assert about the device (counters, clock, storage, targets) is inherited unchanged.
 */
class DirectDriveBackend : public vn::vsg::VsgBackend
{
  public:
    /// @brief Creates the device @p desc asks for (see VsgBackend's constructor); this drive adds no opinion of its own.
    ///
    /// @param desc How the device reaches the display (a case about a display-less device states None).
    explicit DirectDriveBackend(const vn::graphics::DeviceDesc& desc = {}) : vn::vsg::VsgBackend(desc) {}

    /// The window's handle is the target's CREATION input; a second announcement MOVES the target that exists.
    void setWindowHandle(void* native_handle)
    {
        if (target_ == nullptr)
        {
            target_ = createSurfaceTarget(native_handle);
        }
        else
        {
            target_->setWindowHandle(native_handle);
        }
    }

    [[nodiscard]] void* nativeHandle() const { return target_ != nullptr ? target_->nativeHandle() : nullptr; }

    void resize(int width, int height)
    {
        if (target_ != nullptr)
        {
            target_->resize(width, height);
        }
    }

    /// One argument: the frame is for the window this case holds (see RenderFrameExecutor::executeFrame).
    ///
    /// nullptr NOW MEANS "NO WINDOW" (the off-screen form, see `.ai/design/device-is-not-a-window.md` §6), so this
    /// form names the window's target EXPLICITLY: the target of the window this case holds, asked for from the
    /// device when the case never asked (initialize() brings the device's own window up, and a frame still has to be
    /// able to name it).
    void executeFrame(const vn::graphics::FrameDescription& frame)
    {
        vn::vsg::VsgBackend::executeFrame(frame, heldTarget());
    }

    /// The two-argument form, unchanged: a case that names a target explicitly (a foreign one, a bare presenter).
    void executeFrame(const vn::graphics::FrameDescription& frame, vn::graphics::PresentTarget* present_to) override
    {
        vn::vsg::VsgBackend::executeFrame(frame, present_to);
    }

    [[nodiscard]] vn::raw_ptr<vn::graphics::WindowPresentTarget> target() const { return target_; }

  private:
    /// The target of window 0, asked for on first use (see the one-argument executeFrame).
    [[nodiscard]] vn::raw_ptr<vn::graphics::WindowPresentTarget> heldTarget()
    {
        if (target_ == nullptr)
        {
            target_ = BackendContentAccess::surfaceTarget(*this, 0);
        }
        return target_;
    }

    /// Null until a case asks: stated here rather than left to `new X()` zero-initializing it (that help is what this
    /// class' constructor takes away - a user-provided constructor makes the value-initialization stop at the members).
    vn::raw_ptr<vn::graphics::WindowPresentTarget> target_{nullptr};
};

/// The name the cases use: the device they drive, one window each (see DirectDriveBackend).
using VsgBackend = DirectDriveBackend;

namespace
{

/// @brief Prints a `vn::String` (UTF-8 bytes) as the bytes it holds.
std::string as_bytes(const vn::String& text)
{
    return std::string(reinterpret_cast<const char*>(text.data()), text.size());
}

/// @brief Whether a device case can run at all (a device, a session on it, and off-screen pixels back).
///
/// The question is the SDK device probe's, not "is there a window system": a case that needs an X11 host window
/// asks for one itself (and skips when xcb refuses), so a case that only needs the device must not be held back
/// by a proxy that is false on Windows, where the device is right there.
bool deviceCaseAvailable()
{
    return SdkPixelFixture::available();
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
    camera->lookAt(vn::math::Vec3d(x, 0.0, 1.5), vn::math::Vec3d(x, 0.0, 0.0),
                                  vn::math::Vec3d(0.0, 1.0, 0.0));
    camera->setOrthographic(-1.0, 1.0, -1.0, 1.0, 0.5, 4.0);
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
                               const vn::graphics::backend::VariantPool& pool)
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
        GTEST_SKIP() << "no device session that serves off-screen pixels on this machine";
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
    backend->executeFrame(vn::graphics::FrameDescription{});
    EXPECT_EQ(backend->framesPresented(), 0U);
    EXPECT_EQ(backend->diagnosticCount(), 1U) << "one report for the frame that had no session";
    EXPECT_EQ(seen, backend->diagnosticCount()) << "the sink sees every counted diagnostic";

    // 2. The session comes up, and an EMPTY frame is still a frame that has to be presented: opening a frame
    // may acquire an image, and the present is the only call that hands it back (see api/Session).
    ASSERT_TRUE(backend->initialize());
    EXPECT_TRUE(backend->initialized());
    for (int index = 0; index < 3; ++index)
    {
        backend->executeFrame(vn::graphics::FrameDescription{});
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
    EXPECT_TRUE(backend->capabilities().render_targets) << "the off-screen half is served now";

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

    // 5. A pass that issues NOTHING is an entry with no description: the frame still runs and presents, and
    //    nothing is reported - there is no call sequence left to lose track of (see RenderFrameExecutor::executeFrame).
    //    A pass that was never announced to the engine is exactly this shape.
    const vn::intrusive_ptr<RenderPass> quiet(new RenderPass());
    backend->executeFrame(TestFrame().pass(quiet.get()).value());
    EXPECT_EQ(backend->diagnosticCount(), before_unserved + 3U)
        << "a pass that issues nothing is not a mistake, and nothing needs saying about it";
    EXPECT_EQ(backend->framesPresented(), 4U) << "and the frame it was part of still presents";

    // 6. The announced size is the SURFACE's, and this backend FOLLOWS the surface it is on (the SDK's
    // authority order: surface > announcement > default). Following re-reads the window and rebuilds its
    // swapchain, which stops the device once - the counter says so - and nothing is reported, because a live
    // size event is served now. The announcement is what the NEXT initialize() creates a window at, which is
    // the only way this layer can apply it to a window of its own (see the registered limit in the design
    // notes).
    backend->resize(320, 180);
    EXPECT_EQ(backend->diagnosticCount(), before_unserved + 3U) << "a live resize is served now";
    EXPECT_EQ(backend->deviceWaits(), 1U) << "following the surface rebuilds the swapchain, and that stops it";

    ASSERT_TRUE(backend->initialize()) << "re-initializing tears the old session down first (the SDK's contract)";
    EXPECT_EQ(backend->framesPresented(), 0U) << "a fresh session has presented nothing";
    WindowTarget* window = BackendContentAccess::windowTarget(*backend);
    ASSERT_NE(window, nullptr);
    EXPECT_EQ(window->width(), 320U) << "the announcement is what the new session's window came up at";
    EXPECT_EQ(window->height(), 180U);
    backend->executeFrame(vn::graphics::FrameDescription{});
    EXPECT_EQ(backend->framesPresented(), 1U);

    // 7. shutdown() is safe twice and leaves the backend initializable again (the SDK's contract for a
    // recreated surface).
    backend->shutdown();
    EXPECT_FALSE(backend->initialized());
    backend->shutdown();
    ASSERT_TRUE(backend->initialize());
    backend->executeFrame(vn::graphics::FrameDescription{});
    EXPECT_EQ(backend->framesPresented(), 1U);
    backend->shutdown();
}

#if !defined(_WIN32)

TEST(VsgBackendTest, TheHostReadsTheBackendsOwnFrameReport)
{
    if (!deviceCaseAvailable())
    {
        GTEST_SKIP() << "no device session that serves off-screen pixels on this machine";
    }

    vn::intrusive_ptr<VsgBackend> backend(new VsgBackend());
    ASSERT_TRUE(backend->initialize());

    // An EMPTY frame still answers: the counters are kept, and they describe a frame that recorded nothing.
    // (A backend that kept none would leave the part DISENGAGED - that distinction is what the report is for.)
    backend->executeFrame(vn::graphics::FrameDescription{});

    vn::graphics::FrameReport report = backend->frameReport();
    ASSERT_TRUE(report.counters.has_value()) << "this backend keeps its own frame counters";
    EXPECT_EQ(report.counters->passes, 0U);
    EXPECT_EQ(report.counters->draws, 0U);
    EXPECT_EQ(report.counters->invalid_schedules, 0U);

    // A DRAWN frame: the numbers a host reads here ARE the backend's own instrumentation (the same ones a
    // phase table gates on), not a second count taken from outside.
    const TriangleFixture                         fixture = makeTriangle();
    const vn::intrusive_ptr<vn::graphics::Camera> camera  = cameraLookingAt(0.0);
    const vn::intrusive_ptr<RenderPass>           pass(new RenderPass());

    backend->executeFrame(TestFrame()
        .pass(pass.get())
        .order(0)
        .target(nullptr)
        .clear(vn::graphics::ClearPolicy{ vn::Colorf(0.0F, 0.25F, 0.0F, 1.0F), true })
        .depth(vn::graphics::DepthMode::TestAndWrite)
        .content(fixture.commands, camera.get())
        .value());

    report = backend->frameReport();
    ASSERT_TRUE(report.counters.has_value());
    EXPECT_EQ(report.counters->passes, 1U) << "one pass scope ran";
    EXPECT_EQ(report.counters->draws, 1U) << "and it recorded one draw";
    EXPECT_EQ(report.counters->invalid_schedules, 0U) << "nothing was cyclic";

    // The retention numbers come out of the SAME answer, and they agree with the backend's own accessor.
    ASSERT_TRUE(report.retention.has_value());
    EXPECT_EQ(report.retention->device_waits, backend->deviceWaits())
        << "the counter the host reads is the one the backend counted";
    EXPECT_EQ(report.retention->device_waits, 0U) << "the frame path never idles the device";

    // The frame clock, as a host can see it: SUBMITTED is not COMPLETED, and the difference is what is
    // still in flight. The proof of completion is this backend's own (the command slot was recycled), so
    // the host reads the session's watermarks rather than assuming a lag.
    ASSERT_TRUE(report.progress.has_value()) << "this backend keeps its own frame clock";
    EXPECT_LE(report.progress->completed, report.progress->submitted) << "completion never runs ahead of submission";
    EXPECT_EQ(report.progress->inFlight(), report.progress->submitted - report.progress->completed);

    // One more frame, drawn this time: it reaches the queue, so the submitted watermark moves by one.
    backend->executeFrame(TestFrame()
        .pass(pass.get())
        .order(0)
        .target(nullptr)
        .content(fixture.commands, camera.get())
        .value());

    const std::uint64_t submitted_before = report.progress->submitted;
    report                               = backend->frameReport();
    ASSERT_TRUE(report.progress.has_value());
    EXPECT_EQ(report.progress->submitted, submitted_before + 1U) << "a drawn frame is handed to the queue";
    EXPECT_LE(report.progress->completed, report.progress->submitted);

    // And the capability the SDK asks before it plans an off-screen pass is answered by the same value.
    EXPECT_TRUE(backend->capabilities().render_targets) << "the off-screen half is served";
}

TEST(VsgBackendTest, AHostSurfaceIsAdoptedAndMovingToTheNextOneKeepsTheSession)
{
    if (!deviceCaseAvailable())
    {
        GTEST_SKIP() << "no device session that serves off-screen pixels on this machine";
    }

    int               screen_index = 0;
    xcb_connection_t* connection   = xcb_connect(nullptr, &screen_index);
    const TestXConnection connection_owner(connection);  // closed at scope end (see TestXConnection)
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
        backend->executeFrame(vn::graphics::FrameDescription{});
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
    backend->executeFrame(vn::graphics::FrameDescription{});
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
        GTEST_SKIP() << "no device session that serves off-screen pixels on this machine";
    }

    int               screen_index = 0;
    xcb_connection_t* connection   = xcb_connect(nullptr, &screen_index);
    const TestXConnection connection_owner(connection);  // closed at scope end (see TestXConnection)
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

    const vn::graphics::ClearPolicy clear{ vn::Colorf(0.0F, 0.25F, 0.0F, 1.0F), true };

    const auto drive = [&](const vn::graphics::Camera* frame_camera) {
        backend->executeFrame(TestFrame()
            .pass(pass.get())
            .order(0)
            .target(nullptr)
            .clear(clear)
            .depth(vn::graphics::DepthMode::TestAndWrite)
            .content(commands, frame_camera)
            .value());
    };
    // The display path lags the session's presents, so a picture is read after a couple more frames that
    // carry the SAME picture. A plan-free frame is not a settle: it repaints the window with the session's
    // own graph, and a read that catches one answers the window's background (measured: `read-error 0`,
    // the window's own clear - see TestHostWindow::waitForPixel).
    const auto settle = [&](const vn::graphics::Camera* frame_camera) {
        drive(frame_camera);
        drive(frame_camera);
    };

    PassRegistry&    registry = BackendContentAccess::passes(*backend);
    ContentAssembly* assembly = BackendContentAccess::assembly(*backend);
    ASSERT_NE(assembly, nullptr) << "a live session owns the content world its frames assemble";
    EXPECT_FALSE(registry.contains(pass.get())) << "nothing knows the pass before a frame names it";
    EXPECT_EQ(registry.live(), 0U);
    EXPECT_EQ(backend->framesPresented(), 0U);

    // 1. A frame, the SDK's order: the triangle through camera A lands LEFT of centre (the camera looks
    // half a unit to its right). Every object the command names is tracked when the frame is run, so the
    // tables answer for all of them when the plan records - and the pass' identity is the frame's, adopted
    // from the entry that named it (the engine's pre-frame warm-up, which used to register it before any
    // frame, is gone: the frame IS the moment a pass is known).
    drive(camera_a.get());
    EXPECT_EQ(backend->framesPresented(), 1U);
    EXPECT_EQ(backend->diagnosticCount(), 0U) << "the pass path and the content path are served now";
    EXPECT_TRUE(registry.contains(pass.get())) << "the frame's entry made the pass known";
    EXPECT_EQ(registry.live(), 1U);
    const auto& recorded = BackendContentAccess::executor(*backend).recorded();
    ASSERT_EQ(recorded.size(), 1U);
    EXPECT_EQ(recorded[0], registry.adopt(pass.get()))
        << "the frame records the pass its entry named, under that identity";

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
    const vn::graphics::backend::PassId first_id = registry.adopt(pass.get());
    backend->releasePass(pass.get());
    EXPECT_FALSE(registry.contains(pass.get()));
    EXPECT_EQ(registry.live(), 0U);
    EXPECT_NE(registry.adopt(pass.get()), first_id)
        << "a pass announced again (a frame naming it once more) gets a new identity";

    backend->shutdown();
    EXPECT_TRUE(host.alive());
}

TEST(VsgBackendTest, TheSdkOffscreenTargetIsDrawnIntoSampledAndRebuilt)
{
    if (!deviceCaseAvailable())
    {
        GTEST_SKIP() << "no device session that serves off-screen pixels on this machine";
    }

    int               screen_index = 0;
    xcb_connection_t* connection   = xcb_connect(nullptr, &screen_index);
    const TestXConnection connection_owner(connection);  // closed at scope end (see TestXConnection)
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
    ASSERT_TRUE(backend->capabilities().render_targets) << "the off-screen half is served";

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
    const vn::graphics::ClearPolicy offscreen_clear{ vn::Colorf(0.0F, 0.25F, 0.0F, 1.0F), true };
    const vn::graphics::ClearPolicy window_clear{ vn::Colorf(0.0F, 0.0F, 0.25F, 1.0F), true };

    const auto drive = [&]() {
        backend->executeFrame(TestFrame()
                                  .pass(offscreen_pass.get(), -1)
                                  .target(offscreen.get())
                                  .clear(offscreen_clear)
                                  .depth(vn::graphics::DepthMode::TestAndWrite)
                                  .content(triangle.commands, camera.get())
                                  .pass(window_pass.get())
                                  .target(nullptr)
                                  .inputs({ offscreen.get() })
                                  .clear(window_clear)
                                  .depth(vn::graphics::DepthMode::Disabled)
                                  .screen(offscreen.get(), copy_program.get(), camera.get())
                                  .value());
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
    // A PASS that draws into it, on the other hand, needs it: the frame's entry is where a picture depends on
    // the target, so the description that cannot become objects is reported there - once, and only once.
    backend->executeFrame(TestFrame().pass(offscreen_pass.get()).target(lenderless.get()).value());
    EXPECT_EQ(backend->diagnosticCount(), 1U) << "inside a frame the missing lender is reported, once";
    // ... and the sentence NAMES both targets (the SDK's own label): "a render target could not be built" is
    // not something a host can act on, and the engine's builder names everything it creates.
    ASSERT_FALSE(messages.empty());
    EXPECT_NE(messages.back().find("'composite'"), std::string::npos) << messages.back();
    EXPECT_NE(messages.back().find("'gbuffer'"), std::string::npos) << messages.back();
    // Three entries naming it are still one episode: what is reported is the target's own condition, not the
    // number of passes that ran into it.
    backend->executeFrame(TestFrame()
                              .pass(offscreen_pass.get())
                              .target(lenderless.get())
                              .pass(window_pass.get())
                              .target(lenderless.get())
                              .pass(offscreen_pass.get())
                              .target(lenderless.get())
                              .value());
    EXPECT_EQ(backend->diagnosticCount(), 1U) << "three announcements are one episode";
    backend->releaseRenderTarget(lenderless.get());

    backend->shutdown();
    EXPECT_TRUE(host.alive());
}

/*
 * @brief An imported texture is bound where a content pass' program samples it.
 *
 * The pass declares ONE input, the imported texture, and its content program samples the pass' input set at
 * set 1 binding 0. A magenta center means the texture reached the shader; white means the stand-in was bound,
 * and the destination's blue clear means the draw never happened. This is the backend half of
 * `.ai/design/graphics-image-source.md` (the full-screen ABI has its own fixed source bindings; imported
 * textures reach content passes first).
 */
TEST(VsgBackendTest, AnImportedTextureInputReachesAContentProgram)
{
    if (!deviceCaseAvailable())
    {
        GTEST_SKIP() << "no device session that serves off-screen pixels on this machine";
    }

    int               screen_index = 0;
    xcb_connection_t* connection   = xcb_connect(nullptr, &screen_index);
    const TestXConnection connection_owner(connection);
    if (connection == nullptr || xcb_connection_has_error(connection) != 0)
    {
        GTEST_SKIP() << "no X display to create a host window on";
    }
    xcb_screen_t* screen = xcb_setup_roots_iterator(xcb_get_setup(connection)).data;
    ASSERT_NE(screen, nullptr);

    TestHostWindow host(connection, screen, 128, 96);
    vn::intrusive_ptr<VsgBackend> backend(new VsgBackend());
    backend->setWindowHandle(host.handle());
    ASSERT_TRUE(backend->initialize());
    ASSERT_TRUE(backend->capabilities().render_targets);

    const vn::intrusive_ptr<RenderTarget> destination(new RenderTarget());
    destination->attachColor(RenderTarget::ColorFormat::RGBA8);
    destination->setSize(64, 64);

    // A 2x2 texture filled with magenta: neither the destination's blue clear nor an absent texture's white
    // stand-in can produce it.
    const vn::intrusive_ptr<vn::graphics::Texture2D> texture(
        new vn::graphics::Texture2D(2, 2, vn::imaging::PixelFormat::Rgba8Unorm));
    {
        const vn::intrusive_ptr<vn::imaging::Image> image(
            new vn::imaging::Image(2, 2, vn::imaging::PixelFormat::Rgba8Unorm));
        const std::span<std::byte> pixels = image->mipData(0);
        for (std::size_t index = 0; index < pixels.size(); ++index)
        {
            const std::uint8_t channel = static_cast<std::uint8_t>(index % 4U);
            pixels[index] = static_cast<std::byte>(channel == 1U ? 0U : 255U);
        }
        texture->setImage(vn::intrusive_ptr<const vn::imaging::Image>(image));
    }

    // The content program: the triangle's view block, and the pass' input set as its only texture. The input
    // set is set 1 (see ContentPipeline::kInputSet), in declaration order.
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
            "layout(location = 0) out vec4 out_color;\n"
            "layout(set = 1, binding = 0) uniform sampler2D imported_tex;\n"
            "void main() { out_color = texture(imported_tex, vec2(0.5)); }\n"));
        program->addStage(vertex);
        program->addStage(fragment);
    }

    const TriangleFixture triangle = makeTriangle();
    std::vector<RenderCommand> commands = triangle.commands;
    commands.front().program            = program;
    const vn::intrusive_ptr<vn::graphics::Camera> camera = cameraLookingAt(0.5);
    const vn::intrusive_ptr<RenderPass> pass_object(new RenderPass());
    const vn::graphics::ClearPolicy destination_clear{ vn::Colorf(0.0F, 0.0F, 1.0F, 1.0F), true };

    backend->executeFrame(TestFrame()
                              .pass(pass_object.get(), 0)
                              .target(destination.get())
                              .inputs(std::vector<vn::graphics::ImageSource>{
                                  vn::graphics::ImageSource::imported(texture.get()) })
                              .clear(destination_clear)
                              .depth(vn::graphics::DepthMode::Disabled)
                              .content(commands, camera.get())
                              .value());

    EXPECT_EQ(BackendContentAccess::executor(*backend).recorded().size(), 1U)
        << "the content pass was recorded (no packet means no draw)";

    std::vector<std::uint8_t>    pixels;
    vn::graphics::ReadbackResult why = vn::graphics::ReadbackResult::Failed;
    ASSERT_TRUE(backend->readColorBuffer(destination.get(), 0, pixels, &why)) << "the destination is read back";
    const std::size_t at = (40U * 64U + 16U) * 4U;
    ASSERT_LT(at + 3U, pixels.size());
    EXPECT_TRUE(isColourByte(pixels[at], 1.0) && isColourByte(pixels[at + 1], 0.0) &&
                isColourByte(pixels[at + 2], 1.0))
        << "the imported texture is what the content program samples: got (" << static_cast<int>(pixels[at])
        << ", " << static_cast<int>(pixels[at + 1]) << ", " << static_cast<int>(pixels[at + 2]) << ")";
    EXPECT_EQ(pixels[at + 3], 255U);
}

/*
 * @brief A CUBE map declared as a pass input reaches the content program's `samplerCube`.
 *
 * The imported-input path was only ever exercised with 2D textures, but nothing in it is kind-aware: the upload
 * builds a six-layer cube view (`MaterialImages::acquire`), and the input set's layout comes from the program's
 * declarations rather than from the texture. This case is the proof: the +X face is green, every other face is
 * a different colour, and the program samples by a FIXED direction (1, 0, 0) - no texcoord channel and no
 * material is involved - so a green centre says a cube view reached a cube sampler.
 */
TEST(VsgBackendTest, ACubeMapInputReachesAContentProgramAsACubeSampler)
{
    if (!deviceCaseAvailable())
    {
        GTEST_SKIP() << "no device session that serves off-screen pixels on this machine";
    }

    int               screen_index = 0;
    xcb_connection_t* connection   = xcb_connect(nullptr, &screen_index);
    const TestXConnection connection_owner(connection);
    if (connection == nullptr || xcb_connection_has_error(connection) != 0)
    {
        GTEST_SKIP() << "no X display to create a host window on";
    }
    xcb_screen_t* screen = xcb_setup_roots_iterator(xcb_get_setup(connection)).data;
    ASSERT_NE(screen, nullptr);

    TestHostWindow host(connection, screen, 128, 96);
    vn::intrusive_ptr<VsgBackend> backend(new VsgBackend());
    backend->setWindowHandle(host.handle());
    ASSERT_TRUE(backend->initialize());
    ASSERT_TRUE(backend->capabilities().render_targets);

    const vn::intrusive_ptr<RenderTarget> destination(new RenderTarget());
    destination->attachColor(RenderTarget::ColorFormat::RGBA8);
    destination->setSize(64, 64);

    // Six 4x4 faces in Vulkan's layer order (CubeMap::Face), each a solid distinct colour. Four texels rather
    // than one: the cache's LINEAR filter bleeds across the cube's seams, and a face's colour is only pure
    // inside it (the same measurement ContentPassTest's cube case records).
    constexpr int kFaceSize = 4;
    const vn::intrusive_ptr<vn::graphics::CubeMap> cube(
        new vn::graphics::CubeMap(kFaceSize, vn::imaging::PixelFormat::Rgba8Unorm));
    const std::uint8_t face_colours[6][4]{ { 0U, 255U, 0U, 255U },      // PosX: green (the one this case reads)
                                           { 255U, 0U, 0U, 255U },      // NegX: red
                                           { 0U, 0U, 255U, 255U },      // PosY: blue
                                           { 255U, 255U, 0U, 255U },    // NegY: yellow
                                           { 255U, 0U, 255U, 255U },    // PosZ: magenta
                                           { 0U, 255U, 255U, 255U } };  // NegZ: cyan
    for (int face = 0; face < cube->layerCount(); ++face)
    {
        const vn::intrusive_ptr<vn::imaging::Image> image(
            new vn::imaging::Image(kFaceSize, kFaceSize, vn::imaging::PixelFormat::Rgba8Unorm));
        const std::span<std::byte> pixels = image->mipData(0);
        for (std::size_t texel = 0; texel < pixels.size() / 4U; ++texel)
        {
            for (std::size_t byte = 0; byte < 4U; ++byte)
            {
                pixels[texel * 4U + byte] = static_cast<std::byte>(face_colours[face][byte]);
            }
        }
        cube->setLayer(face, vn::intrusive_ptr<const vn::imaging::Image>(image));
    }

    // The same view block and input set as the 2D case, with the one difference this case is about: the input
    // set's sampler is declared `samplerCube`, and the direction is a constant.
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
            "layout(location = 0) out vec4 out_color;\n"
            "layout(set = 1, binding = 0) uniform samplerCube imported_cube;\n"
            "void main() { out_color = texture(imported_cube, vec3(1.0, 0.0, 0.0)); }\n"));
        program->addStage(vertex);
        program->addStage(fragment);
    }

    const TriangleFixture triangle = makeTriangle();
    std::vector<RenderCommand> commands = triangle.commands;
    commands.front().program            = program;
    const vn::intrusive_ptr<vn::graphics::Camera> camera = cameraLookingAt(0.5);
    const vn::intrusive_ptr<RenderPass> pass_object(new RenderPass());
    const vn::graphics::ClearPolicy destination_clear{ vn::Colorf(0.0F, 0.0F, 1.0F, 1.0F), true };

    backend->executeFrame(TestFrame()
                              .pass(pass_object.get(), 0)
                              .target(destination.get())
                              .inputs(std::vector<vn::graphics::ImageSource>{
                                  vn::graphics::ImageSource::imported(cube.get()) })
                              .clear(destination_clear)
                              .depth(vn::graphics::DepthMode::Disabled)
                              .content(commands, camera.get())
                              .value());

    EXPECT_EQ(BackendContentAccess::executor(*backend).recorded().size(), 1U)
        << "the content pass was recorded (no packet means no draw)";

    std::vector<std::uint8_t>    pixels;
    vn::graphics::ReadbackResult why = vn::graphics::ReadbackResult::Failed;
    ASSERT_TRUE(backend->readColorBuffer(destination.get(), 0, pixels, &why)) << "the destination is read back";
    const std::size_t at = (40U * 64U + 16U) * 4U;
    ASSERT_LT(at + 3U, pixels.size());
    EXPECT_TRUE(isColourByte(pixels[at], 0.0) && isColourByte(pixels[at + 1], 1.0) &&
                isColourByte(pixels[at + 2], 0.0))
        << "the cube's +X face is what the content program samples: got (" << static_cast<int>(pixels[at])
        << ", " << static_cast<int>(pixels[at + 1]) << ", " << static_cast<int>(pixels[at + 2]) << ")";
    EXPECT_EQ(pixels[at + 3], 255U);
    EXPECT_EQ(backend->diagnosticCount(), 0U) << "a complete cube is uploaded silently";
}

/*
 * @brief An INCOMPLETE cube declared as a pass input binds the WHITE CUBE, and says why once.
 *
 * The stand-in has to be a cube, not the 2D white texture: a `samplerCube` reading a 2D-array view is a
 * descriptor mismatch, so a white picture here is also the proof that the fallback followed the declaration's
 * kind. This is the failure half of the cube-input contract (the passing half is the case above).
 */
TEST(VsgBackendTest, AnIncompleteCubeInputBindsTheWhiteCubeAndSaysWhy)
{
    if (!deviceCaseAvailable())
    {
        GTEST_SKIP() << "no device session that serves off-screen pixels on this machine";
    }

    int               screen_index = 0;
    xcb_connection_t* connection   = xcb_connect(nullptr, &screen_index);
    const TestXConnection connection_owner(connection);
    if (connection == nullptr || xcb_connection_has_error(connection) != 0)
    {
        GTEST_SKIP() << "no X display to create a host window on";
    }
    xcb_screen_t* screen = xcb_setup_roots_iterator(xcb_get_setup(connection)).data;
    ASSERT_NE(screen, nullptr);

    TestHostWindow host(connection, screen, 128, 96);
    vn::intrusive_ptr<VsgBackend> backend(new VsgBackend());
    backend->setWindowHandle(host.handle());
    ASSERT_TRUE(backend->initialize());
    ASSERT_TRUE(backend->capabilities().render_targets);

    const vn::intrusive_ptr<RenderTarget> destination(new RenderTarget());
    destination->attachColor(RenderTarget::ColorFormat::RGBA8);
    destination->setSize(64, 64);

    // Not one face is filled: the upload is refused as Incomplete, which is the case the white stand-in
    // exists for.
    const vn::intrusive_ptr<vn::graphics::CubeMap> cube(
        new vn::graphics::CubeMap(4, vn::imaging::PixelFormat::Rgba8Unorm));

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
            "layout(location = 0) out vec4 out_color;\n"
            "layout(set = 1, binding = 0) uniform samplerCube imported_cube;\n"
            "void main() { out_color = texture(imported_cube, vec3(1.0, 0.0, 0.0)); }\n"));
        program->addStage(vertex);
        program->addStage(fragment);
    }

    const TriangleFixture triangle = makeTriangle();
    std::vector<RenderCommand> commands = triangle.commands;
    commands.front().program            = program;
    const vn::intrusive_ptr<vn::graphics::Camera> camera = cameraLookingAt(0.5);
    const vn::intrusive_ptr<RenderPass> pass_object(new RenderPass());
    const vn::graphics::ClearPolicy destination_clear{ vn::Colorf(0.0F, 0.0F, 1.0F, 1.0F), true };

    backend->executeFrame(TestFrame()
                              .pass(pass_object.get(), 0)
                              .target(destination.get())
                              .inputs(std::vector<vn::graphics::ImageSource>{
                                  vn::graphics::ImageSource::imported(cube.get()) })
                              .clear(destination_clear)
                              .depth(vn::graphics::DepthMode::Disabled)
                              .content(commands, camera.get())
                              .value());

    EXPECT_EQ(backend->diagnosticCount(), 1U) << "one report for the refused upload, and no other episode";

    std::vector<std::uint8_t>    pixels;
    vn::graphics::ReadbackResult why = vn::graphics::ReadbackResult::Failed;
    ASSERT_TRUE(backend->readColorBuffer(destination.get(), 0, pixels, &why)) << "the destination is read back";
    const std::size_t at = (40U * 64U + 16U) * 4U;
    ASSERT_LT(at + 3U, pixels.size());
    EXPECT_TRUE(isColourByte(pixels[at], 1.0) && isColourByte(pixels[at + 1], 1.0) &&
                isColourByte(pixels[at + 2], 1.0))
        << "the white CUBE stand-in is what the content program samples: got (" << static_cast<int>(pixels[at])
        << ", " << static_cast<int>(pixels[at + 1]) << ", " << static_cast<int>(pixels[at + 2]) << ")";
    EXPECT_EQ(pixels[at + 3], 255U);
}

/*
 * @brief The capabilities answer the FEATURES as device facts, in the same value as the rest.
 *
 * Two of the three are floors this backend refuses a device without (see DeviceRequirements): a live session
 * answering them is therefore a fact about the device it came up on, not a promise this file makes. The third,
 * GPU timings, is NOT a floor - a device that cannot write timestamps still runs whole frames - so it is only
 * asked to be the device's own stable answer.
 */
TEST(VsgBackendTest, TheCapabilitiesAnswerTheFeaturesTheDeviceActuallyHas)
{
    if (!deviceCaseAvailable())
    {
        GTEST_SKIP() << "no device session that serves off-screen pixels on this machine";
    }

    int               screen_index = 0;
    xcb_connection_t* connection   = xcb_connect(nullptr, &screen_index);
    const TestXConnection connection_owner(connection);
    if (connection == nullptr || xcb_connection_has_error(connection) != 0)
    {
        GTEST_SKIP() << "no X display to create a host window on";
    }
    xcb_screen_t* screen = xcb_setup_roots_iterator(xcb_get_setup(connection)).data;
    ASSERT_NE(screen, nullptr);

    TestHostWindow host(connection, screen, 128, 96);
    vn::intrusive_ptr<VsgBackend> backend(new VsgBackend());
    backend->setWindowHandle(host.handle());
    ASSERT_TRUE(backend->initialize());

    const vn::graphics::Capabilities caps = backend->capabilities();
    EXPECT_TRUE(caps.render_targets);
    EXPECT_TRUE(caps.presentation);
    EXPECT_TRUE(caps.supports(vn::graphics::Capabilities::Feature::Wireframe))
        << "the device floor requires fillModeNonSolid";
    EXPECT_TRUE(caps.supports(vn::graphics::Capabilities::Feature::AnisotropicFiltering))
        << "the device floor requires samplerAnisotropy";
    EXPECT_GE(caps.max_anisotropy, 1.0F);
    EXPECT_EQ(caps.supports(vn::graphics::Capabilities::Feature::AnisotropicFiltering),
              caps.max_anisotropy > 1.0F)
        << "the feature bit and the limit are the same device fact";
    EXPECT_GE(caps.max_color_attachments, 4U) << "the G-buffer carries four colour attachments";
    EXPECT_EQ(caps.supports(vn::graphics::Capabilities::Feature::GpuTimings),
              backend->capabilities().supports(vn::graphics::Capabilities::Feature::GpuTimings))
        << "the answer is the device's, and asking again does not change it";
}

/*
 * @brief Each colour attachment clears to ITS own stated colour, and an unstated one stays transparent black.
 *
 * An MRT pass had one clear colour and painted every attachment with it (or left the extras black); the policy now
 * carries a colour per attachment. The picture is the proof: two attachments, red and green clears, read back one
 * at a time - neither can stand in for the other.
 */
TEST(VsgBackendTest, EachAttachmentClearsToItsOwnColour)
{
    if (!deviceCaseAvailable())
    {
        GTEST_SKIP() << "no device session that serves off-screen pixels on this machine";
    }

    int               screen_index = 0;
    xcb_connection_t* connection   = xcb_connect(nullptr, &screen_index);
    const TestXConnection connection_owner(connection);
    if (connection == nullptr || xcb_connection_has_error(connection) != 0)
    {
        GTEST_SKIP() << "no X display to create a host window on";
    }
    xcb_screen_t* screen = xcb_setup_roots_iterator(xcb_get_setup(connection)).data;
    ASSERT_NE(screen, nullptr);

    TestHostWindow host(connection, screen, 128, 96);
    vn::intrusive_ptr<VsgBackend> backend(new VsgBackend());
    backend->setWindowHandle(host.handle());
    ASSERT_TRUE(backend->initialize());

    const vn::intrusive_ptr<RenderTarget> target(new RenderTarget());
    target->attachColor(RenderTarget::ColorFormat::RGBA8);
    target->attachColor(RenderTarget::ColorFormat::RGBA8);
    target->setSize(16, 16);

    const vn::intrusive_ptr<RenderPass> pass(new RenderPass());

    vn::graphics::ClearPolicy clear;
    clear.color  = vn::Colorf(1.0F, 0.0F, 0.0F, 1.0F);  // attachment 0: red
    clear.depth  = false;                      // there is no depth attachment
    clear.extra_colors.push_back(vn::Colorf(0.0F, 1.0F, 0.0F, 1.0F));  // attachment 1: green

    backend->executeFrame(TestFrame()
                              .pass(pass.get(), 0)
                              .target(target.get())
                              .clear(clear)
                              .depth(vn::graphics::DepthMode::Disabled)
                              .value());

    const auto readAttachment = [&backend, &target](int attachment, std::vector<std::uint8_t>& pixels) {
        vn::graphics::ReadbackResult why = vn::graphics::ReadbackResult::Failed;
        const bool                    ok  = backend->readColorBuffer(target.get(), attachment, pixels, &why);
        EXPECT_TRUE(ok) << "attachment " << attachment << " is read back";
        EXPECT_EQ(why, vn::graphics::ReadbackResult::Ok);
        return ok;
    };

    std::vector<std::uint8_t> first;
    ASSERT_TRUE(readAttachment(0, first));
    const std::size_t at = (4U * 16U + 4U) * 4U;
    ASSERT_LT(at + 3U, first.size());
    EXPECT_TRUE(isColourByte(first[at], 1.0) && isColourByte(first[at + 1], 0.0) &&
                isColourByte(first[at + 2], 0.0))
        << "attachment 0 is the colour stated for it: got (" << static_cast<int>(first[at]) << ", "
        << static_cast<int>(first[at + 1]) << ", " << static_cast<int>(first[at + 2]) << ")";

    std::vector<std::uint8_t> second;
    ASSERT_TRUE(readAttachment(1, second));
    ASSERT_LT(at + 3U, second.size());
    EXPECT_TRUE(isColourByte(second[at], 0.0) && isColourByte(second[at + 1], 1.0) &&
                isColourByte(second[at + 2], 0.0))
        << "attachment 1 is the colour stated for IT, not the pass' colour: got ("
        << static_cast<int>(second[at]) << ", " << static_cast<int>(second[at + 1]) << ", "
        << static_cast<int>(second[at + 2]) << ")";

    EXPECT_EQ(backend->diagnosticCount(), 0U) << "a per-attachment clear is not a report";
}

/*
 * @brief A clear colour is stored as the LINEAR value it is: the target's format, not the pipeline, does any encoding.
 *
 * The colour authored here is #808080 seen on a display, decoded once by the holder of the bytes (`toLinear()`, so
 * 0.2159). An 8-bit linear target stores that value as it stands (55), which is what makes this a device-level proof
 * that no transfer function runs inside the engine: encoding it first would store 128. The same 0.2159 written into
 * an sRGB surface is encoded by the GPU on write - that edge belongs to the hardware (the window's own format decides
 * it), which is the whole point of the separation.
 */
TEST(VsgBackendTest, AClearColourIsAStoredLinearValueNotAnEncodedOne)
{
    if (!deviceCaseAvailable())
    {
        GTEST_SKIP() << "no device session that serves off-screen pixels on this machine";
    }

    int               screen_index = 0;
    xcb_connection_t* connection   = xcb_connect(nullptr, &screen_index);
    const TestXConnection connection_owner(connection);
    if (connection == nullptr || xcb_connection_has_error(connection) != 0)
    {
        GTEST_SKIP() << "no X display to create a host window on";
    }
    xcb_screen_t* screen = xcb_setup_roots_iterator(xcb_get_setup(connection)).data;
    ASSERT_NE(screen, nullptr);

    TestHostWindow host(connection, screen, 64, 64);
    vn::intrusive_ptr<VsgBackend> backend(new VsgBackend());
    backend->setWindowHandle(host.handle());
    ASSERT_TRUE(backend->initialize());

    const vn::intrusive_ptr<RenderTarget> target(new RenderTarget());
    target->attachColor(RenderTarget::ColorFormat::RGBA8);
    target->setSize(16, 16);

    const vn::intrusive_ptr<RenderPass> pass(new RenderPass());

    vn::graphics::ClearPolicy clear;
    clear.color = vn::Color(128, 128, 128, 255).toLinear();  // ≈ 0.2159: the light #808080 stands for
    clear.depth = false;                                    // there is no depth attachment

    backend->executeFrame(TestFrame()
                              .pass(pass.get(), 0)
                              .target(target.get())
                              .clear(clear)
                              .depth(vn::graphics::DepthMode::Disabled)
                              .value());

    std::vector<std::uint8_t>    pixels;
    vn::graphics::ReadbackResult why = vn::graphics::ReadbackResult::Failed;
    ASSERT_TRUE(backend->readColorBuffer(target.get(), 0, pixels, &why)) << "the target is read back";
    const std::size_t at = (8U * 16U + 8U) * 4U;
    ASSERT_LT(at + 3U, pixels.size());
    EXPECT_NEAR(static_cast<int>(pixels[at]), 55, 1)
        << "0.2159 stored in 8 bits is 55; 128 would mean the colour was ENCODED before it reached the target";
    EXPECT_NEAR(static_cast<int>(pixels[at + 1]), 55, 1);
    EXPECT_NEAR(static_cast<int>(pixels[at + 2]), 55, 1);
    EXPECT_EQ(pixels[at + 3], 255U) << "alpha is a fraction, and it crosses without a transfer function";
    EXPECT_EQ(backend->diagnosticCount(), 0U) << "a clear colour is not a report";
}

/*
 * @brief The frame report says how many frames were handed to PRESENT: the fact a window reader waits for.
 *
 * The count is the WINDOW half's own (`api::Session::framesPresented`): an off-screen frame names no window and
 * presents nothing, so it leaves the number alone; a committed window frame is handed to the platform's present call
 * even when it drew nothing, so the number moves by one per frame. It is a WATERMARK, not a sample of a moving clock:
 * asking twice without a frame between changes nothing.
 */
TEST(VsgBackendTest, TheFrameReportSaysHowManyFramesWereHandedToPresent)
{
    if (!deviceCaseAvailable())
    {
        GTEST_SKIP() << "no device session that serves off-screen pixels on this machine";
    }

    int               screen_index = 0;
    xcb_connection_t* connection   = xcb_connect(nullptr, &screen_index);
    const TestXConnection connection_owner(connection);
    if (connection == nullptr || xcb_connection_has_error(connection) != 0)
    {
        GTEST_SKIP() << "no X display to create a host window on";
    }
    xcb_screen_t* screen = xcb_setup_roots_iterator(xcb_get_setup(connection)).data;
    ASSERT_NE(screen, nullptr);

    TestHostWindow host(connection, screen, 128, 96);
    vn::intrusive_ptr<DirectDriveBackend> backend(new DirectDriveBackend());
    backend->setWindowHandle(host.handle());
    ASSERT_TRUE(backend->initialize());
    ASSERT_NE(backend->target(), nullptr);

    const auto progress = backend->frameReport().progress;
    ASSERT_TRUE(progress.has_value()) << "this backend keeps the frame clock";
    const std::uint64_t presented_before = progress->presented;

    // Two frames that draw nothing are still committed, and committing presents (see Session::commit).
    backend->executeFrame(vn::graphics::FrameDescription{});
    backend->executeFrame(vn::graphics::FrameDescription{});

    const auto after = backend->frameReport().progress;
    ASSERT_TRUE(after.has_value());
    EXPECT_EQ(after->presented, presented_before + 2U) << "one present per committed window frame";

    // A watermark, not a sample: asking again without a frame changes nothing.
    EXPECT_EQ(backend->frameReport().progress->presented, presented_before + 2U);
}

/*
 * @brief A re-filled imported texture reaches the NEXT frame, and the cache keeps one entry.
 *
 * The same Texture object is sampled by the same pass in two frames; between them `setImage` replaces its
 * pixels. The cache key is `(Texture*, revision)`, so the second frame must build new images (a new view)
 * rather than keep serving the first upload - the picture is the proof - and the entry must be REPLACED
 * rather than joined: `MaterialImages::count()` stays 1.
 */
TEST(VsgBackendTest, ARefilledImportedTextureIsReuploadedForTheNextFrame)
{
    if (!deviceCaseAvailable())
    {
        GTEST_SKIP() << "no device session that serves off-screen pixels on this machine";
    }

    int               screen_index = 0;
    xcb_connection_t* connection   = xcb_connect(nullptr, &screen_index);
    const TestXConnection connection_owner(connection);
    if (connection == nullptr || xcb_connection_has_error(connection) != 0)
    {
        GTEST_SKIP() << "no X display to create a host window on";
    }
    xcb_screen_t* screen = xcb_setup_roots_iterator(xcb_get_setup(connection)).data;
    ASSERT_NE(screen, nullptr);

    TestHostWindow host(connection, screen, 128, 96);
    vn::intrusive_ptr<VsgBackend> backend(new VsgBackend());
    backend->setWindowHandle(host.handle());
    ASSERT_TRUE(backend->initialize());
    ASSERT_TRUE(backend->capabilities().render_targets);

    const vn::intrusive_ptr<RenderTarget> destination(new RenderTarget());
    destination->attachColor(RenderTarget::ColorFormat::RGBA8);
    destination->setSize(64, 64);

    // One texture, two fills: magenta first, yellow after `setImage`. Neither colour can come from the
    // destination's blue clear or from an absent texture's white stand-in.
    const vn::intrusive_ptr<vn::graphics::Texture2D> texture(
        new vn::graphics::Texture2D(2, 2, vn::imaging::PixelFormat::Rgba8Unorm));
    const auto fill = [&texture](bool yellow) {
        const vn::intrusive_ptr<vn::imaging::Image> image(
            new vn::imaging::Image(2, 2, vn::imaging::PixelFormat::Rgba8Unorm));
        const std::span<std::byte> pixels = image->mipData(0);
        for (std::size_t index = 0; index < pixels.size(); ++index)
        {
            const std::uint8_t channel = static_cast<std::uint8_t>(index % 4U);
            // magenta (255, 0, 255): the green channel is zero; yellow (255, 255, 0): the blue one is.
            const std::uint8_t zeroed = yellow ? 2U : 1U;
            pixels[index]             = static_cast<std::byte>(channel == zeroed ? 0U : 255U);
        }
        texture->setImage(vn::intrusive_ptr<const vn::imaging::Image>(image));
    };
    fill(/*yellow=*/false);

    // The content program samples the pass' input set at set 1 binding 0 (see ContentPipeline::kInputSet).
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
            "layout(location = 0) out vec4 out_color;\n"
            "layout(set = 1, binding = 0) uniform sampler2D imported_tex;\n"
            "void main() { out_color = texture(imported_tex, vec2(0.5)); }\n"));
        program->addStage(vertex);
        program->addStage(fragment);
    }

    const TriangleFixture triangle = makeTriangle();
    std::vector<RenderCommand> commands = triangle.commands;
    commands.front().program            = program;
    const vn::intrusive_ptr<vn::graphics::Camera> camera = cameraLookingAt(0.0);
    const vn::intrusive_ptr<RenderPass> pass_object(new RenderPass());
    const vn::graphics::ClearPolicy destination_clear{ vn::Colorf(0.0F, 0.0F, 1.0F, 1.0F), true };

    const auto drive = [&]() {
        backend->executeFrame(TestFrame()
                                  .pass(pass_object.get(), 0)
                                  .target(destination.get())
                                  .inputs(std::vector<vn::graphics::ImageSource>{
                                      vn::graphics::ImageSource::imported(texture.get()) })
                                  .clear(destination_clear)
                                  .depth(vn::graphics::DepthMode::Disabled)
                                  .content(commands, camera.get())
                                  .value());
    };
    const auto centre = [&]() {
        std::vector<std::uint8_t>    pixels;
        vn::graphics::ReadbackResult why = vn::graphics::ReadbackResult::Failed;
        EXPECT_TRUE(backend->readColorBuffer(destination.get(), 0, pixels, &why)) << "the destination is read back";
        const std::size_t at = (32U * 64U + 32U) * 4U;
        if (at + 3U >= pixels.size())
        {
            return std::array<std::uint8_t, 4>{};
        }
        return std::array<std::uint8_t, 4>{ pixels[at], pixels[at + 1U], pixels[at + 2U], pixels[at + 3U] };
    };

    vn::vsg::MaterialImages* const images = BackendContentAccess::images(*backend);
    ASSERT_NE(images, nullptr);

    drive();
    const std::array<std::uint8_t, 4> before = centre();
    EXPECT_TRUE(isColourByte(before[0], 1.0) && isColourByte(before[1], 0.0) &&
                isColourByte(before[2], 1.0))
        << "the first frame samples the first fill (magenta): got (" << static_cast<int>(before[0]) << ", "
        << static_cast<int>(before[1]) << ", " << static_cast<int>(before[2]) << ")";
    EXPECT_EQ(images->count(), 1U) << "the first frame cached the imported texture";
    const std::uint64_t first_revision = texture->revision();

    fill(/*yellow=*/true);
    ASSERT_GT(texture->revision(), first_revision) << "setImage bumps the content revision";

    drive();
    const std::array<std::uint8_t, 4> after = centre();
    EXPECT_TRUE(isColourByte(after[0], 1.0) && isColourByte(after[1], 1.0) && isColourByte(after[2], 0.0))
        << "the next frame samples the re-filled texture (yellow); magenta would mean the old upload was "
           "kept: got ("
        << static_cast<int>(after[0]) << ", " << static_cast<int>(after[1]) << ", "
        << static_cast<int>(after[2]) << ")";
    EXPECT_EQ(images->count(), 1U)
        << "the new revision replaced the entry instead of adding a second one for the same texture";
}

/*
 * @brief One texture declared twice with different states is read two ways in the SAME pass.
 *
 * The 2x2 texture holds red in its left column and green in its right. The program samples both inputs at
 * u = 1.25: ClampToEdge reads the edge (green, r = 0), Repeat wraps to the left texel (red, r = 1). The output
 * is (0, 1, 0) only when each input kept its own state; (1, 1, 0) means both used the default Repeat and
 * (0, 0, 0) means both used ClampToEdge. `MaterialImages::count()` stays 1: the upload and view are shared,
 * only the samplers differ.
 */
TEST(VsgBackendTest, TwoInputStatesOverOneTextureSampleDifferently)
{
    if (!deviceCaseAvailable())
    {
        GTEST_SKIP() << "no device session that serves off-screen pixels on this machine";
    }

    int               screen_index = 0;
    xcb_connection_t* connection   = xcb_connect(nullptr, &screen_index);
    const TestXConnection connection_owner(connection);
    if (connection == nullptr || xcb_connection_has_error(connection) != 0)
    {
        GTEST_SKIP() << "no X display to create a host window on";
    }
    xcb_screen_t* screen = xcb_setup_roots_iterator(xcb_get_setup(connection)).data;
    ASSERT_NE(screen, nullptr);

    TestHostWindow host(connection, screen, 128, 96);
    vn::intrusive_ptr<VsgBackend> backend(new VsgBackend());
    backend->setWindowHandle(host.handle());
    ASSERT_TRUE(backend->initialize());
    ASSERT_TRUE(backend->capabilities().render_targets);

    const vn::intrusive_ptr<RenderTarget> destination(new RenderTarget());
    destination->attachColor(RenderTarget::ColorFormat::RGBA8);
    destination->setSize(64, 64);

    const vn::intrusive_ptr<vn::graphics::Texture2D> texture(
        new vn::graphics::Texture2D(2, 2, vn::imaging::PixelFormat::Rgba8Unorm));
    {
        const vn::intrusive_ptr<vn::imaging::Image> image(
            new vn::imaging::Image(2, 2, vn::imaging::PixelFormat::Rgba8Unorm));
        const std::span<std::byte> pixels = image->mipData(0);
        for (std::size_t texel = 0; texel < 4; ++texel)
        {
            const bool left = (texel % 2U) == 0U;
            pixels[texel * 4U + 0U] = static_cast<std::byte>(left ? 255 : 0);
            pixels[texel * 4U + 1U] = static_cast<std::byte>(left ? 0 : 255);
            pixels[texel * 4U + 2U] = std::byte{ 0 };
            pixels[texel * 4U + 3U] = std::byte{ 255 };
        }
        texture->setImage(vn::intrusive_ptr<const vn::imaging::Image>(image));
    }

    vn::graphics::Sampler clamp;
    clamp.min_filter  = vn::graphics::Sampler::Filter::Nearest;
    clamp.mag_filter  = vn::graphics::Sampler::Filter::Nearest;
    clamp.mipmap_mode = vn::graphics::Sampler::MipmapMode::Nearest;
    clamp.address_u   = vn::graphics::Sampler::AddressMode::ClampToEdge;
    clamp.address_v   = vn::graphics::Sampler::AddressMode::ClampToEdge;
    vn::graphics::Sampler repeat = clamp;
    repeat.address_u             = vn::graphics::Sampler::AddressMode::Repeat;
    repeat.address_v             = vn::graphics::Sampler::AddressMode::Repeat;

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
            "layout(location = 0) out vec4 out_color;\n"
            "layout(set = 1, binding = 0) uniform sampler2D clamp_tex;\n"
            "layout(set = 1, binding = 1) uniform sampler2D repeat_tex;\n"
            "void main() {\n"
            "    vec2 uv = vec2(1.25, 0.5);\n"
            "    out_color = vec4(texture(clamp_tex, uv).r, texture(repeat_tex, uv).r, 0.0, 1.0);\n"
            "}\n"));
        program->addStage(vertex);
        program->addStage(fragment);
    }

    const TriangleFixture triangle = makeTriangle();
    std::vector<RenderCommand> commands = triangle.commands;
    commands.front().program            = program;
    const vn::intrusive_ptr<vn::graphics::Camera> camera = cameraLookingAt(0.0);
    const vn::intrusive_ptr<RenderPass> pass_object(new RenderPass());
    const vn::graphics::ClearPolicy destination_clear{ vn::Colorf(0.0F, 0.0F, 1.0F, 1.0F), true };

    backend->executeFrame(TestFrame()
                              .pass(pass_object.get(), 0)
                              .target(destination.get())
                              .inputs(std::vector<vn::graphics::ImageSource>{
                                  vn::graphics::ImageSource::imported(texture.get(), clamp),
                                  vn::graphics::ImageSource::imported(texture.get(), repeat) })
                              .clear(destination_clear)
                              .depth(vn::graphics::DepthMode::Disabled)
                              .content(commands, camera.get())
                              .value());

    EXPECT_EQ(backend->diagnosticCount(), 0U) << "the two-state pass recorded";

    std::vector<std::uint8_t>    pixels;
    vn::graphics::ReadbackResult why = vn::graphics::ReadbackResult::Failed;
    ASSERT_TRUE(backend->readColorBuffer(destination.get(), 0, pixels, &why)) << "the destination is read back";
    const std::size_t at = (32U * 64U + 32U) * 4U;
    ASSERT_LT(at + 3U, pixels.size());
    EXPECT_TRUE(isColourByte(pixels[at], 0.0) && isColourByte(pixels[at + 1], 1.0) &&
                isColourByte(pixels[at + 2], 0.0))
        << "clamp reads the edge (r=0) and repeat wraps to the left texel (r=1): got ("
        << static_cast<int>(pixels[at]) << ", " << static_cast<int>(pixels[at + 1]) << ", "
        << static_cast<int>(pixels[at + 2]) << ")";
    EXPECT_EQ(pixels[at + 3], 255U);

    vn::vsg::MaterialImages* const images = BackendContentAccess::images(*backend);
    ASSERT_NE(images, nullptr);
    EXPECT_EQ(images->count(), 1U) << "one texture, one upload";
    EXPECT_EQ(images->samplerCount(), 2U) << "two declared states, two samplers";
}

/*
 * @brief A content pass that declares ONE produced attachment samples THAT image, not the target's first.
 *
 * The producer paints an MRT target: attachment 0 green, attachment 1 red. The consumer declares only
 * attachment 1 (`ImageSource::producedImage(source, 1, false)`) and its program samples the pass' input set at
 * set 1 binding 0. Red means the set carries the declared image; green means the whole target was offered and
 * the content ABI's declaration-order binding answered with attachment 0.
 */
TEST(VsgBackendTest, AProducedInputBindsOnlyTheAttachmentItNames)
{
    if (!deviceCaseAvailable())
    {
        GTEST_SKIP() << "no device session that serves off-screen pixels on this machine";
    }

    int               screen_index = 0;
    xcb_connection_t* connection   = xcb_connect(nullptr, &screen_index);
    const TestXConnection connection_owner(connection);
    if (connection == nullptr || xcb_connection_has_error(connection) != 0)
    {
        GTEST_SKIP() << "no X display to create a host window on";
    }
    xcb_screen_t* screen = xcb_setup_roots_iterator(xcb_get_setup(connection)).data;
    ASSERT_NE(screen, nullptr);

    TestHostWindow host(connection, screen, 128, 96);
    vn::intrusive_ptr<VsgBackend> backend(new VsgBackend());
    backend->setWindowHandle(host.handle());
    ASSERT_TRUE(backend->initialize());
    ASSERT_TRUE(backend->capabilities().render_targets);

    // The MRT source: attachment 0 green, attachment 1 red. The destination stays blue unless the image
    // reaches it, so the three outcomes are distinct.
    const vn::intrusive_ptr<RenderTarget> source(new RenderTarget());
    source->attachColor(RenderTarget::ColorFormat::RGBA8);
    source->attachColor(RenderTarget::ColorFormat::RGBA8);
    source->setSize(64, 64);
    const vn::intrusive_ptr<RenderTarget> destination(new RenderTarget());
    destination->attachColor(RenderTarget::ColorFormat::RGBA8);
    destination->setSize(64, 64);

    const vn::intrusive_ptr<ShaderProgram> producer_program(new ShaderProgram());
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
            "layout(location = 0) out vec4 first;\n"
            "layout(location = 1) out vec4 second;\n"
            "void main() { first = vec4(0.0, 1.0, 0.0, 1.0);\n"
            "              second = vec4(1.0, 0.0, 0.0, 1.0); }\n"));
        producer_program->addStage(vertex);
        producer_program->addStage(fragment);
    }

    // The content program samples the pass' input set at set 1 (see ContentPipeline::kInputSet): binding 0 is
    // the first declared input, which is the ONE image the consumer names.
    const vn::intrusive_ptr<ShaderProgram> consumer_program(new ShaderProgram());
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
            "layout(location = 0) out vec4 out_color;\n"
            "layout(set = 1, binding = 0) uniform sampler2D declared_tex;\n"
            "void main() { out_color = texture(declared_tex, vec2(0.5)); }\n"));
        consumer_program->addStage(vertex);
        consumer_program->addStage(fragment);
    }

    const TriangleFixture triangle = makeTriangle();
    std::vector<RenderCommand> producer_commands = triangle.commands;
    producer_commands.front().program            = producer_program;
    std::vector<RenderCommand> consumer_commands = triangle.commands;
    consumer_commands.front().program            = consumer_program;

    const vn::intrusive_ptr<vn::graphics::Camera> camera = cameraLookingAt(0.0);
    const vn::intrusive_ptr<RenderPass> producer_pass(new RenderPass());
    const vn::intrusive_ptr<RenderPass> consumer_pass(new RenderPass());
    const vn::graphics::ClearPolicy source_clear{ vn::Colorf(0.0F, 0.0F, 1.0F, 1.0F), true };
    const vn::graphics::ClearPolicy destination_clear{ vn::Colorf(0.0F, 0.0F, 1.0F, 1.0F), true };

    backend->executeFrame(TestFrame()
                              .pass(producer_pass.get(), -1)
                              .target(source.get())
                              .clear(source_clear)
                              .depth(vn::graphics::DepthMode::Disabled)
                              .content(producer_commands, camera.get())
                              .pass(consumer_pass.get(), 0)
                              .target(destination.get())
                              .inputs(std::vector<vn::graphics::ImageSource>{
                                  vn::graphics::ImageSource::producedImage(source.get(), 1, false) })
                              .clear(destination_clear)
                              .depth(vn::graphics::DepthMode::Disabled)
                              .content(consumer_commands, camera.get())
                              .value());

    EXPECT_EQ(backend->diagnosticCount(), 0U) << "the producer and the narrowed consumer both recorded";

    std::vector<std::uint8_t>    pixels;
    vn::graphics::ReadbackResult why = vn::graphics::ReadbackResult::Failed;
    ASSERT_TRUE(backend->readColorBuffer(destination.get(), 0, pixels, &why)) << "the destination is read back";
    const std::size_t at = (32U * 64U + 32U) * 4U;
    ASSERT_LT(at + 3U, pixels.size());
    EXPECT_TRUE(isColourByte(pixels[at], 1.0) && isColourByte(pixels[at + 1], 0.0) &&
                isColourByte(pixels[at + 2], 0.0))
        << "the declared attachment 1 is what the content program samples: got ("
        << static_cast<int>(pixels[at]) << ", " << static_cast<int>(pixels[at + 1]) << ", "
        << static_cast<int>(pixels[at + 2]) << ")";
    EXPECT_FALSE(isColourByte(pixels[at], 0.0) && isColourByte(pixels[at + 1], 1.0))
        << "green would be attachment 0: the whole-target offer reached the content input set";
    EXPECT_EQ(pixels[at + 3], 255U);
}

/*
 * @brief A full-screen program samples an imported texture at its declared binding.
 *
 * The pass declares the imported texture first and its produced source second. The full-screen ABI binds the
 * source's colour attachment 0 at binding 0, and the imported texture at the program's next declared sampler
 * binding (1). The program samples binding 1, so magenta means the imported image reached the shader; red
 * means the source colour won the slot, and the destination's blue clear means the call never drew.
 */
TEST(VsgBackendTest, AnImportedTextureInputReachesAScreenProgram)
{
    if (!deviceCaseAvailable())
    {
        GTEST_SKIP() << "no device session that serves off-screen pixels on this machine";
    }

    int               screen_index = 0;
    xcb_connection_t* connection   = xcb_connect(nullptr, &screen_index);
    const TestXConnection connection_owner(connection);
    if (connection == nullptr || xcb_connection_has_error(connection) != 0)
    {
        GTEST_SKIP() << "no X display to create a host window on";
    }
    xcb_screen_t* screen = xcb_setup_roots_iterator(xcb_get_setup(connection)).data;
    ASSERT_NE(screen, nullptr);

    TestHostWindow host(connection, screen, 128, 96);
    vn::intrusive_ptr<VsgBackend> backend(new VsgBackend());
    backend->setWindowHandle(host.handle());
    ASSERT_TRUE(backend->initialize());
    ASSERT_TRUE(backend->capabilities().render_targets);

    const vn::intrusive_ptr<RenderTarget> source(new RenderTarget());
    source->attachColor(RenderTarget::ColorFormat::RGBA8);
    source->setSize(64, 64);
    const vn::intrusive_ptr<RenderTarget> destination(new RenderTarget());
    destination->attachColor(RenderTarget::ColorFormat::RGBA8);
    destination->setSize(64, 64);

    const vn::intrusive_ptr<vn::graphics::Texture2D> texture(
        new vn::graphics::Texture2D(2, 2, vn::imaging::PixelFormat::Rgba8Unorm));
    {
        const vn::intrusive_ptr<vn::imaging::Image> image(
            new vn::imaging::Image(2, 2, vn::imaging::PixelFormat::Rgba8Unorm));
        const std::span<std::byte> pixels = image->mipData(0);
        for (std::size_t index = 0; index < pixels.size(); ++index)
        {
            const std::uint8_t channel = static_cast<std::uint8_t>(index % 4U);
            pixels[index] = static_cast<std::byte>(channel == 1U ? 0U : 255U);
        }
        texture->setImage(vn::intrusive_ptr<const vn::imaging::Image>(image));
    }

    const vn::intrusive_ptr<ShaderProgram> program(new ShaderProgram());
    {
        ShaderStage fragment;
        fragment.type   = ShaderStageType::Fragment;
        fragment.source = vn::String(reinterpret_cast<const char8_t*>(
            "#version 450\n"
            "layout(location = 0) in vec2 vine_uv;\n"
            "layout(location = 0) out vec4 out_color;\n"
            "layout(binding = 1) uniform sampler2D imported_tex;\n"
            "void main() { out_color = texture(imported_tex, vec2(0.5)); }\n"));
        program->addStage(fragment);
    }

    const TriangleFixture triangle = makeTriangle();
    const vn::intrusive_ptr<vn::graphics::Camera> camera = cameraLookingAt(0.5);
    const vn::intrusive_ptr<RenderPass> source_pass(new RenderPass());
    const vn::intrusive_ptr<RenderPass> screen_pass(new RenderPass());
    const vn::graphics::ClearPolicy source_clear{ vn::Colorf(1.0F, 0.0F, 0.0F, 1.0F), true };
    const vn::graphics::ClearPolicy destination_clear{ vn::Colorf(0.0F, 0.0F, 1.0F, 1.0F), true };

    backend->executeFrame(TestFrame()
                              .pass(source_pass.get(), -1)
                              .target(source.get())
                              .clear(source_clear)
                              .depth(vn::graphics::DepthMode::Disabled)
                              .content(triangle.commands, camera.get())
                              .pass(screen_pass.get(), 0)
                              .target(destination.get())
                              .inputs(std::vector<vn::graphics::ImageSource>{
                                  vn::graphics::ImageSource::imported(texture.get()),
                                  vn::graphics::ImageSource::producedTarget(source.get()) })
                              .clear(destination_clear)
                              .depth(vn::graphics::DepthMode::Disabled)
                              .screen(source.get(), program.get(), camera.get())
                              .value());

    std::vector<std::uint8_t>    pixels;
    vn::graphics::ReadbackResult why = vn::graphics::ReadbackResult::Failed;
    ASSERT_TRUE(backend->readColorBuffer(destination.get(), 0, pixels, &why)) << "the destination is read back";
    const std::size_t at = (40U * 64U + 16U) * 4U;
    ASSERT_LT(at + 3U, pixels.size());
    EXPECT_TRUE(isColourByte(pixels[at], 1.0) && isColourByte(pixels[at + 1], 0.0) &&
                isColourByte(pixels[at + 2], 1.0))
        << "the imported texture is what the full-screen program samples: got ("
        << static_cast<int>(pixels[at]) << ", " << static_cast<int>(pixels[at + 1]) << ", "
        << static_cast<int>(pixels[at + 2]) << ")";
    EXPECT_EQ(pixels[at + 3], 255U);
}

TEST(VsgBackendTest, TheSdkReadsBackItsOwnTargetsPixelsAndDepths)
{
    if (!deviceCaseAvailable())
    {
        GTEST_SKIP() << "no device session that serves off-screen pixels on this machine";
    }

    int               screen_index = 0;
    xcb_connection_t* connection   = xcb_connect(nullptr, &screen_index);
    const TestXConnection connection_owner(connection);  // closed at scope end (see TestXConnection)
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
    const vn::graphics::ClearPolicy                 clear{ vn::Colorf(0.0F, 0.25F, 0.0F, 1.0F), true };

    const auto drive = [&]() {
        backend->executeFrame(TestFrame()
            .pass(pass.get())
            .order(0)
            .target(target.get())
            .clear(clear)
            .depth(vn::graphics::DepthMode::TestAndWrite)
            .content(triangle.commands, camera.get())
            .value());
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

// 跨帧差异相位（登记 B8）：现有像素相位全是静止相机，任何"某一帧读到别的帧的块"都看不见。
// 这条相位每帧只动一点相机，并把判据做成可读回的子类：帧 1..5 相机 0.95..0.75（红通道类 1）、
// 末帧 0.1（类 0），它的输出留在离屏 target 上；帧 7..14 相机 0.9、打进窗口。
//
// 判据刻意避开色彩空间：视图块的 x 经 step(0.75, x) 落成红通道 0 或 1（0 与 1 在 linear/sRGB
// 两种拼法下同字节）。末帧只可能读到自己的 0.1（红 0）；绑定冻结在早前帧、错 slab、串帧——
// 任何读到别帧数据的形状都会把它翻到 255。
//
// 诚实的边界（2026-09-25 探针实测）：§11.16dd 的受害帧是"最旧在飞帧（F−N）"，它的输出不是
// 一次 readback 能看到的东西（readback 只见最新帧），而本机框架的逐帧节流（submit/回拷阻塞
// ≈ 一个 GPU 帧）让 F−N 总在覆盖写之前收尾——把负载压到 1G 像素/帧也没造出重叠。那条缺陷
// 继续由结构性旋转用例（BlockStorageTest.TheSlabsRotate…）承担；本相位守的是"可读回"的
// 跨帧子类（末帧读到别帧的数据 / 绑定冻结）。
TEST(VsgBackendTest, TheLastFrameOfAMovingSequenceKeepsItsOwnViewBlockValue)
{
    if (!deviceCaseAvailable())
    {
        GTEST_SKIP() << "no device session that serves off-screen pixels on this machine";
    }

    int               screen_index = 0;
    xcb_connection_t* connection   = xcb_connect(nullptr, &screen_index);
    const TestXConnection connection_owner(connection);  // closed at scope end (see TestXConnection)
    if (connection == nullptr || xcb_connection_has_error(connection) != 0)
    {
        GTEST_SKIP() << "no X display to create a host window on";
    }
    xcb_screen_t* screen = xcb_setup_roots_iterator(xcb_get_setup(connection)).data;
    ASSERT_NE(screen, nullptr);

    constexpr int kTargetSide = 1024;

    TestHostWindow host(connection, screen, 128, 96);

    vn::intrusive_ptr<VsgBackend> backend(new VsgBackend());
    std::size_t                   seen = 0;
    backend->setDiagnosticSink([&seen](const vn::graphics::RenderDiagnostic& diagnostic) {
        ++seen;
        std::printf("[cross-frame] diagnostic: severity=%d category=%d message=%s\n",
                    static_cast<int>(diagnostic.severity), static_cast<int>(diagnostic.category),
                    as_bytes(diagnostic.message).c_str());
    });
    backend->setWindowHandle(host.handle());
    ASSERT_TRUE(backend->initialize());

    const vn::intrusive_ptr<RenderTarget> target(new RenderTarget());
    target->attachColor(RenderTarget::ColorFormat::RGBA8);
    target->setSize(kTargetSide, kTargetSide);

    // 全屏四边形 + 读视图块的片元：像素 = (step(0.75, |cam_pos.x|), 0.5, 0, 1)。
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
            "void main() { outColor = vec4(step(0.75, abs(vb.cam_pos.x)), 0.5, 0.0, 1.0); }\n"));
        program->addStage(vertex);
        program->addStage(fragment);
    }

    const vn::intrusive_ptr<Geometry> geometry(new Geometry());
    const vn::intrusive_ptr<vn::Buffer<float>> positions = vn::intrusive_ptr<vn::Buffer<float>>(
        new vn::Buffer<float>(std::vector<float>{ -2.0F, -2.0F, 0.5F, 2.0F, -2.0F, 0.5F, 2.0F, 2.0F, 0.5F, -2.0F, 2.0F, 0.5F }));
    const vn::intrusive_ptr<vn::Buffer<std::uint32_t>> indices =
        vn::intrusive_ptr<vn::Buffer<std::uint32_t>>(new vn::Buffer<std::uint32_t>(std::vector<std::uint32_t>{ 0U, 1U, 2U, 0U, 2U, 3U }));
    geometry->setPositions(positions);
    geometry->setIndices(indices);
    geometry->setRevision(1U);

    const vn::intrusive_ptr<Material> material(new Material());
    material->setDiffuse(vn::Colorf(0.2F, 0.3F, 0.4F, 1.0F));

    // 每帧把同一个全屏四边形画 8 遍：多一点填充量，让帧真正在 GPU 上跑起来（探针实测：Debug
    // 下 CPU 每帧与 GPU 帧同量级，纯粹加负载并不会制造出跨帧重叠窗口——本相位的职责见文件头）。
    constexpr int kDrawsPerFrame = 8;

    RenderCommand command;
    command.geometry = geometry;
    command.material = material;
    command.program  = program;
    const std::vector<RenderCommand> commands(kDrawsPerFrame, command);

    const vn::intrusive_ptr<RenderPass> pass(new RenderPass());
    const vn::graphics::ClearPolicy     clear{ vn::Colorf(0.0F, 0.25F, 0.0F, 1.0F), true };

    const auto drive_into = [&](RenderTarget* where, double camera_x) {
        const vn::intrusive_ptr<vn::graphics::Camera> camera = cameraLookingAt(camera_x);
        backend->executeFrame(TestFrame()
            .pass(pass.get())
            .order(0)
            .target(where)
            .clear(clear)
            .depth(vn::graphics::DepthMode::TestAndWrite)
            .content(commands, camera.get())
            .value());
    };

    // 帧 1..5：相机 0.95..0.75（红通道类 1）；末帧 0.1（类 0）——它的输出留在 target 上。
    const auto phase_start = std::chrono::steady_clock::now();
    for (int i = 0; i < 5; ++i)
    {
        drive_into(target.get(), 0.95 - 0.05 * i);
    }
    drive_into(target.get(), 0.1);
    // 帧 7..14：相机 0.9（类 1），打进窗口。任何"末帧读到别帧数据"都会把它的红通道翻到 255。
    for (int i = 7; i <= 14; ++i)
    {
        drive_into(nullptr, 0.9);
    }
    const auto phase_ms = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - phase_start).count();
    std::printf("[cross-frame] 14 frames (%d draws each) in %lld ms\n", kDrawsPerFrame, static_cast<long long>(phase_ms));

    std::vector<std::uint8_t>    pixels;
    vn::graphics::ReadbackResult why = vn::graphics::ReadbackResult::Failed;
    ASSERT_TRUE(backend->readColorBuffer(target.get(), 0, pixels, &why)) << "the target is read back";
    EXPECT_EQ(why, vn::graphics::ReadbackResult::Ok);
    ASSERT_EQ(pixels.size(), static_cast<std::size_t>(kTargetSide) * kTargetSide * 4U);

    const auto pixel = [&pixels](int x, int y) {
        const std::size_t at = (static_cast<std::size_t>(y) * kTargetSide + static_cast<std::size_t>(x)) * 4U;
        return std::array<std::uint8_t, 4>{ pixels[at], pixels[at + 1], pixels[at + 2], pixels[at + 3] };
    };

    // 五个采样点全在末帧的四边形内：绿通道必须是 0.5 那类（quad 画到了），红通道必须是 0 那类
    // （末帧读到的是自己的视图块）。红通道若成群出现 255，就是读到了别帧的数据。
    const int offsets[][2] = { { 0, 0 }, { -150, 0 }, { 150, 0 }, { 0, -150 }, { 0, 150 } };
    for (const auto& offset : offsets)
    {
        const auto sample = pixel(kTargetSide / 2 + offset[0], kTargetSide / 2 + offset[1]);
        std::printf("[cross-frame] pixel(%d,%d) = (%u, %u, %u, %u)\n",
                    kTargetSide / 2 + offset[0], kTargetSide / 2 + offset[1],
                    static_cast<unsigned>(sample[0]), static_cast<unsigned>(sample[1]),
                    static_cast<unsigned>(sample[2]), static_cast<unsigned>(sample[3]));
        EXPECT_GT(sample[1], 100U) << "the quad covered this sample (green 0.5)";
        EXPECT_LT(sample[0], 8U) << "this frame must be shaded from its OWN view block (step(0.75, 0.1) = 0); "
                                    "a read of any other frame's block (>=0.75) flips the red channel to 255";
    }
    EXPECT_EQ(backend->diagnosticCount(), 0U);

    backend->releaseRenderTarget(target.get());
    backend->shutdown();
    EXPECT_TRUE(host.alive());
}

TEST(VsgBackendTest, TheWindowFollowsItsHostsSurfaceThroughALiveResize)
{
    if (!deviceCaseAvailable())
    {
        GTEST_SKIP() << "no device session that serves off-screen pixels on this machine";
    }

    int               screen_index = 0;
    xcb_connection_t* connection   = xcb_connect(nullptr, &screen_index);
    const TestXConnection connection_owner(connection);  // closed at scope end (see TestXConnection)
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
    const vn::graphics::ClearPolicy                 clear{ vn::Colorf(0.0F, 0.25F, 0.0F, 1.0F), true };

    const auto drive = [&] {
        backend->executeFrame(TestFrame()
            .pass(pass.get())
            .order(0)
            .target(nullptr)
            .clear(clear)
            .depth(vn::graphics::DepthMode::TestAndWrite)
            .content(commands, camera.get())
            .value());
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
        GTEST_SKIP() << "no device session that serves off-screen pixels on this machine";
    }

    int               screen_index = 0;
    xcb_connection_t* connection   = xcb_connect(nullptr, &screen_index);
    const TestXConnection connection_owner(connection);  // closed at scope end (see TestXConnection)
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
    vn::graphics::backend::TargetFacts row;
    targets.facts(*shadow_entry, row);
    EXPECT_TRUE(row.depth.has_depth);
    EXPECT_FALSE(row.depth.borrowed);
    EXPECT_TRUE(row.depth.promotion) << "the built target's own policy answers";
    EXPECT_TRUE(vn::graphics::backend::depthPlan(row.depth).sampleable) << "and the plan promises the depth to a shader";

    // A frame that presents with it held is served silently - the target is part of the frame's world now,
    // and nothing about it needs saying.
    backend->executeFrame(vn::graphics::FrameDescription{});
    EXPECT_EQ(backend->framesPresented(), 1U);
    EXPECT_EQ(backend->diagnosticCount(), 0U);

    backend->shutdown();
    EXPECT_TRUE(host.alive());
}

TEST(VsgBackendTest, AMaterialEditLandsOnTheNextFrameAndASteadyFrameRebuildsNothing)
{
    if (!deviceCaseAvailable())
    {
        GTEST_SKIP() << "no device session that serves off-screen pixels on this machine";
    }

    int               screen_index = 0;
    xcb_connection_t* connection   = xcb_connect(nullptr, &screen_index);
    const TestXConnection connection_owner(connection);  // closed at scope end (see TestXConnection)
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
    const vn::graphics::ClearPolicy                 clear{ vn::Colorf(0.0F, 0.25F, 0.0F, 1.0F), true };

    const auto drive = [&] {
        backend->executeFrame(TestFrame()
            .pass(pass.get())
            .order(0)
            .target(nullptr)
            .clear(clear)
            .depth(vn::graphics::DepthMode::TestAndWrite)
            .content(commands, camera.get())
            .value());
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
        GTEST_SKIP() << "no device session that serves off-screen pixels on this machine";
    }

    int               screen_index = 0;
    xcb_connection_t* connection   = xcb_connect(nullptr, &screen_index);
    const TestXConnection connection_owner(connection);  // closed at scope end (see TestXConnection)
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
    const vn::graphics::ClearPolicy               clear{ vn::Colorf(0.0F, 0.25F, 0.0F, 1.0F), true };

    ContentStore*    store    = BackendContentAccess::store(*backend);
    ContentAssembly* assembly = BackendContentAccess::assembly(*backend);
    ASSERT_NE(store, nullptr) << "a live session owns the content tables its frames are built from";
    ASSERT_NE(assembly, nullptr);
    VariantPool& pool = BackendContentAccess::pool(*backend);

    // One frame, ONE cost: the executor drives the frame to its tail (see RenderFrameExecutor::executeFrame), so the
    // bookkeeping and the commit/present are a single call - there is no seam left to split, and the number a host
    // feels is the whole frame's.
    const auto drive = [&]() {
        const auto began = std::chrono::steady_clock::now();
        backend->executeFrame(TestFrame()
            .pass(pass.get())
            .order(0)
            .target(nullptr)
            .clear(clear)
            .depth(vn::graphics::DepthMode::TestAndWrite)
            .content(commands, camera.get())
            .value());
        const auto ended = std::chrono::steady_clock::now();
        return std::chrono::duration_cast<std::chrono::duration<double, std::micro>>(ended - began).count();
    };

    std::printf("\n");  // the measurements get their own lines
    for (int settle = 0; settle < 6; ++settle) {
        drive();  // the session's opening frames compile the initial pipelines and settle the window
    }

    const auto run = [&](const char* label, int frames, auto&& edit) {
        const EditCostCounters before = readEditCosts(*store, *assembly, pool);
        double                 frame_us = 0.0;
        for (int frame = 0; frame < frames; ++frame) {
            edit(frame);
            frame_us += drive();
        }
        const EditCostCounters after  = readEditCosts(*store, *assembly, pool);
        const EditCostDelta    change = editCostDelta(before, after);
        std::printf("[cost] %-32s frame %8.1f us/f"
                    " | builds %+4lld uploads %+4lld created %+4lld reused %+4lld\n",
                    label, frame_us / frames, static_cast<long long>(change.builds),
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
            commands[0].authored_state.cullMode =
                (frame % 2 == 0) ? vn::graphics::CullMode::Back : vn::graphics::CullMode::None;
        });
        const EditCostDelta change = editCostDelta(before, after);
        EXPECT_EQ(change.builds, 0) << "a state change is not content: no row is built";
        EXPECT_EQ(change.uploads, 0) << "and nothing is uploaded";
        EXPECT_EQ(change.created, 0) << "cull is dynamic state: the pipeline key cannot see it (core/Keys.hpp)";
        commands[0].authored_state.cullMode = vn::graphics::CullMode::Back;
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
        const double           cold_us        = drive();
        const EditCostCounters cold_after     = readEditCosts(*store, *assembly, pool);
        const EditCostDelta    cold_change    = editCostDelta(cold_before, cold_after);
        std::printf("[cost] %-32s frame %8.1f us"
                    " | builds %+4lld uploads %+4lld created %+4lld reused %+4lld\n",
                    "program switched (first, cold)", cold_us,
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
        commands[0].authored_state.topology = vn::graphics::Topology::Lines;
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

TEST(VsgBackendTest, MeasureWhatAMultiChannelMeshCostsWhenOneChannelChanges)
{
    // B6'S SCALE, MADE A RECIPE. The registry (§6) keeps B6 gated on "a host with a MULTI-CHANNEL mesh
    // reports upload bandwidth", and the mechanism is right there in the code: the stream key of EVERY
    // channel - and of the index stream - carries the GEOMETRY's announced revision (see
    // src/internal/GeometryFacts.cpp), so one `bumpRevision()` moves the identity of all of them and the next
    // frame re-sends every byte of the mesh even when the host touched ONE channel. This recipe is the
    // cost curve that gate would be judged against: a 512x512 grid with the four canonical channels
    // (positions, normals, colour, texcoords) and indices, one channel edited per frame - and, for
    // contrast, ALL channels edited per frame, at the same announced-revision cost.
    //
    // The numbers are PRINTED for the design log; what is ASSERTED is the shape B6 would change: one
    // announcement re-uploads ALL FIVE streams (four channels + the index stream), whether the host
    // touched one channel or all of them, and it compiles nothing either way.
    //
    // Calibration while the recipe was made: 256x256 (4.24 MiB re-sent) cost +11..15 ms/frame and
    // 512x512 (16.98 MiB) cost +30..42 ms/frame on lavapipe - roughly LINEAR in bytes, so the cost is
    // the re-sent bytes (at 60 fps the 512 case alone asks for ~1 GB/s of upload bandwidth a host with
    // per-channel revisions simply would not send).
    if (!deviceCaseAvailable())
    {
        GTEST_SKIP() << "no device session that serves off-screen pixels on this machine";
    }

    int               screen_index = 0;
    xcb_connection_t* connection   = xcb_connect(nullptr, &screen_index);
    const TestXConnection connection_owner(connection);  // closed at scope end (see TestXConnection)
    if (connection == nullptr || xcb_connection_has_error(connection) != 0)
    {
        GTEST_SKIP() << "no X display to create a host window on";
    }
    xcb_screen_t* screen = xcb_setup_roots_iterator(xcb_get_setup(connection)).data;
    ASSERT_NE(screen, nullptr);

    constexpr int kWidth           = 128;
    constexpr int kHeight          = 96;
    constexpr int kSide            = 512;
    constexpr int kFramesPerRegime = 8;

    TestHostWindow host(connection, screen, kWidth, kHeight);

    vn::intrusive_ptr<VsgBackend> backend(new VsgBackend());
    backend->setWindowHandle(host.handle());
    ASSERT_TRUE(backend->initialize());
    EXPECT_EQ(backend->diagnosticCount(), 0U);

    // A positions-only program: the extra channels are the HOST's mesh, not this shader's business - the
    // uploads below happen because the geometry ENTRY names them, which is exactly B6's scope.
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

    // The mesh: 512x512 vertices with all four canonical channels and the index list every cell needs.
    const std::size_t vertex_count = static_cast<std::size_t>(kSide) * static_cast<std::size_t>(kSide);
    const std::size_t index_count =
        static_cast<std::size_t>(kSide - 1) * static_cast<std::size_t>(kSide - 1) * 6U;
    const std::size_t stream_bytes = vertex_count * 3U * 4U * 3U  // positions + normals + colour
                                     + vertex_count * 2U * 4U     // texcoords (two components)
                                     + (index_count + 6U) * 4U;   // the whole index buffer (see below)

    const auto repeating = [&](std::vector<float> one, std::size_t count) {
        std::vector<float> values;
        values.reserve(count);
        for (std::size_t index = 0; index < count; ++index) {
            values.insert(values.end(), one.begin(), one.end());
        }
        return values;
    };

    const vn::intrusive_ptr<vn::Buffer<float>> normals(
        new vn::Buffer<float>(repeating({ 0.0F, 0.0F, 1.0F }, vertex_count)));
    const vn::intrusive_ptr<vn::Buffer<float>> colour(
        new vn::Buffer<float>(repeating({ 0.8F, 0.8F, 0.8F }, vertex_count)));
    const vn::intrusive_ptr<vn::Buffer<float>> texcoords(
        new vn::Buffer<float>(repeating({ 0.0F, 0.0F }, vertex_count)));

    vn::intrusive_ptr<Geometry>          geometry(new Geometry());
    // The index buffer: the two screen-covering triangles FIRST - they are ALL the draw takes (the
    // six-index slice below) - then the full grid triangulation. The frame's raster cost therefore
    // vanishes, while the index STREAM stays the WHOLE buffer: the key normalises to it (see
    // src/internal/GeometryFacts.cpp), which is the part of B6's story this recipe must keep.
    const std::vector<float> grid_vertex_data = gridPositions(kSide);
    std::vector<std::uint32_t> index_data{ 0U, static_cast<std::uint32_t>(kSide - 1),
                                           static_cast<std::uint32_t>(kSide * kSide - 1), 0U,
                                           static_cast<std::uint32_t>(kSide * kSide - 1),
                                           static_cast<std::uint32_t>((kSide - 1) * kSide) };
    {
        const std::vector<std::uint32_t> grid = gridIndices(kSide);
        index_data.insert(index_data.end(), grid.begin(), grid.end());
    }

    vn::intrusive_ptr<vn::Buffer<float>> positions(new vn::Buffer<float>(grid_vertex_data));
    geometry->setPositions(positions);
    geometry->setNormals(normals);
    geometry->addBuffer(vn::graphics::attributeLocation(vn::graphics::VertexAttribute::Color),
                        vn::graphics::AttributeChannel::shared(colour, 3U));
    geometry->setTexcoords2(texcoords);
    geometry->setIndices(vn::intrusive_ptr<const vn::Buffer<std::uint32_t>>(
                             new vn::Buffer<std::uint32_t>(index_data)),
                         0U, 6U);
    geometry->bumpRevision();

    const vn::intrusive_ptr<Material> material(new Material());
    material->setDiffuse(vn::Colorf(0.3F, 0.4F, 0.3F, 1.0F));

    std::vector<RenderCommand> commands(1U);
    commands[0].geometry = geometry;
    commands[0].material = material;
    commands[0].program  = program;

    const vn::intrusive_ptr<vn::graphics::Camera> camera = cameraLookingAt(0.0);
    const vn::intrusive_ptr<RenderPass>           pass(new RenderPass());
    const vn::graphics::ClearPolicy               clear{ vn::Colorf(0.0F, 0.25F, 0.0F, 1.0F), true };

    ContentStore*    store    = BackendContentAccess::store(*backend);
    ContentAssembly* assembly = BackendContentAccess::assembly(*backend);
    ASSERT_NE(store, nullptr) << "a live session owns the content tables its frames are built from";
    ASSERT_NE(assembly, nullptr);
    VariantPool& pool = BackendContentAccess::pool(*backend);

    const auto drive = [&]() {
        const auto began = std::chrono::steady_clock::now();
        backend->executeFrame(TestFrame()
            .pass(pass.get())
            .order(0)
            .target(nullptr)
            .clear(clear)
            .depth(vn::graphics::DepthMode::TestAndWrite)
            .content(commands, camera.get())
            .value());
        const auto ended = std::chrono::steady_clock::now();
        return std::chrono::duration_cast<std::chrono::duration<double, std::micro>>(ended - began).count();
    };

    std::printf("\n");
    std::printf("[b6] mesh %dx%d (%zu verts), 4 channels + indices: one announcement re-sends 5 streams "
                "= %.2f MiB\n",
                kSide, kSide, vertex_count,
                static_cast<double>(stream_bytes) / (1024.0 * 1024.0));
    for (int settle = 0; settle < 6; ++settle) {
        drive();  // the session's opening frames compile the initial pipelines and settle the window
    }

    const auto run = [&](const char* label, int frames, auto&& edit) {
        const EditCostCounters before = readEditCosts(*store, *assembly, pool);
        double                 frame_us = 0.0;
        for (int frame = 0; frame < frames; ++frame) {
            edit(frame);
            frame_us += drive();
        }
        const EditCostCounters after  = readEditCosts(*store, *assembly, pool);
        const EditCostDelta    change = editCostDelta(before, after);
        std::printf("[b6] %-32s frame %8.1f us/f"
                    " | builds %+4lld uploads %+4lld created %+4lld reused %+4lld\n",
                    label, frame_us / frames, static_cast<long long>(change.builds),
                    static_cast<long long>(change.uploads), static_cast<long long>(change.created),
                    static_cast<long long>(change.reused));
        return std::pair<EditCostCounters, EditCostCounters>{ before, after };
    };

    {
        const auto [before, after] = run("steady (nothing edited)", kFramesPerRegime, [](int) {});
        const EditCostDelta change = editCostDelta(before, after);
        EXPECT_EQ(change.builds, 0) << "a steady frame builds no row";
        EXPECT_EQ(change.uploads, 0) << "and uploads nothing";
        EXPECT_EQ(change.created, 0) << "and compiles nothing";
    }

    // ONE channel re-authored (a new positions buffer, the spelling the SDK's contract names): the frame
    // pays for the WHOLE mesh - four channel streams and the index stream all carry the announced
    // revision. This is the row B6 exists to shrink.
    {
        const auto [before, after] = run("one channel re-authored", kFramesPerRegime, [&](int) {
            positions = vn::intrusive_ptr<vn::Buffer<float>>(new vn::Buffer<float>(grid_vertex_data));
            geometry->setPositions(positions);
            geometry->bumpRevision();
        });
        const EditCostDelta change = editCostDelta(before, after);
        EXPECT_EQ(change.builds, kFramesPerRegime) << "each announcement is a new revision: one row";
        EXPECT_EQ(change.created, 0) << "bytes are data: the pipeline key cannot see them";
        EXPECT_EQ(change.uploads, 5LL * kFramesPerRegime)
            << "one announcement moves EVERY channel and the index stream - the whole mesh is re-sent";
    }

    // ALL FOUR channels re-authored in place: the cost must be IDENTICAL to the one-channel row, because
    // today's granularity is the geometry, not the channel. The contrast is the point: what B6 would buy
    // is exactly the difference this row does NOT show.
    {
        const auto [before, after] = run("all channels re-authored", kFramesPerRegime, [&](int frame) {
            const auto touch = [&](const vn::intrusive_ptr<vn::Buffer<float>>& buffer) {
                const auto bytes = buffer->data();
                bytes[0]         = 0.01F * static_cast<float>(frame + 1);
                buffer->setRevision(buffer->revision() + 1U);
            };
            touch(positions);
            touch(normals);
            touch(colour);
            touch(texcoords);
            geometry->bumpRevision();
        });
        const EditCostDelta change = editCostDelta(before, after);
        EXPECT_EQ(change.uploads, 5LL * kFramesPerRegime)
            << "touching four channels costs exactly what touching one costs: no per-channel granularity";
        EXPECT_EQ(change.created, 0) << "and no variant either way";
    }

    drive();
    const auto centre = host.waitForPixel(kWidth / 2, kHeight / 2, arrived, std::chrono::milliseconds{ 500 });
    EXPECT_TRUE(isColourByte(centre[0], 0.3) && isColourByte(centre[1], 0.4) && isColourByte(centre[2], 0.3))
        << "the mesh is still what the centre probe sees, got (" << static_cast<int>(centre[0]) << ", "
        << static_cast<int>(centre[1]) << ", " << static_cast<int>(centre[2]) << ")";
    EXPECT_EQ(backend->deviceWaits(), 0U) << "re-sending the mesh stops nothing";
    backend->shutdown();
    EXPECT_TRUE(host.alive());
}

TEST(VsgBackendTest, MeasureWhatCommandsCostWhenTheirDrawingLandsNothing)
{
    // A4'S FLOOR, AND THE MATERIAL ARENA'S BOUNDARY. The registry (§6) keeps A4 gated on "material count
    // large enough that the per-frame O(commands) block compares enter the profile top". Before that
    // report exists, this recipe measures what a command costs when its DRAWING lands nothing: three
    // IDENTICAL vertices are a zero-area triangle, so no fragment ever runs and what is left is the
    // per-command floor - the draw block, the material note, the bind, the draw call. The material ARENA
    // holds 256 slots (BlockStorage's default layout), so the same recipe walks the bound: while the live
    // set fits, every note is a HIT and nothing is written; past it, the FIFO cascade means NO note
    // hits at all - every command allocates-and-evicts every frame, which is exactly the "per-frame
    // O(commands) material work" A4 worries about, quantised.
    //
    // The numbers are PRINTED for the design log; what is ASSERTED is machine-independent: nothing
    // compiles or uploads in any regime, the arena's counters say exactly what the bound implies, and
    // the window stays the clear colour - the drawings really did land nothing.
    if (!deviceCaseAvailable())
    {
        GTEST_SKIP() << "no device session that serves off-screen pixels on this machine";
    }

    int               screen_index = 0;
    xcb_connection_t* connection   = xcb_connect(nullptr, &screen_index);
    const TestXConnection connection_owner(connection);  // closed at scope end (see TestXConnection)
    if (connection == nullptr || xcb_connection_has_error(connection) != 0)
    {
        GTEST_SKIP() << "no X display to create a host window on";
    }
    xcb_screen_t* screen = xcb_setup_roots_iterator(xcb_get_setup(connection)).data;
    ASSERT_NE(screen, nullptr);

    constexpr int kWidth           = 128;
    constexpr int kHeight          = 96;
    constexpr int kFramesPerRegime = 12;

    TestHostWindow host(connection, screen, kWidth, kHeight);

    vn::intrusive_ptr<VsgBackend> backend(new VsgBackend());
    backend->setWindowHandle(host.handle());
    ASSERT_TRUE(backend->initialize());
    EXPECT_EQ(backend->diagnosticCount(), 0U);

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

    // Three IDENTICAL vertices: a zero-area triangle, which rasterises to no fragment at all.
    const vn::intrusive_ptr<Geometry> geometry(new Geometry());
    geometry->setPositions(vn::intrusive_ptr<const vn::Buffer<float>>(new vn::Buffer<float>(
        std::vector<float>{ -0.5F, -0.5F, 0.5F, -0.5F, -0.5F, 0.5F, -0.5F, -0.5F, 0.5F })));
    geometry->setIndices(vn::intrusive_ptr<const vn::Buffer<std::uint32_t>>(
        new vn::Buffer<std::uint32_t>(std::vector<std::uint32_t>{ 0U, 1U, 2U })));
    geometry->bumpRevision();

    const auto material_set = [](std::size_t count) {
        std::vector<vn::intrusive_ptr<Material>> materials;
        materials.reserve(count);
        for (std::size_t index = 0; index < count; ++index) {
            const vn::intrusive_ptr<Material> material(new Material());
            const float shade = 0.2F + 0.6F * static_cast<float>(index % 16U) / 15.0F;
            material->setDiffuse(vn::Colorf(shade, 0.4F, 1.0F - shade, 1.0F));
            materials.push_back(std::move(material));
        }
        return materials;
    };

    const auto commands_for = [&](const std::vector<vn::intrusive_ptr<Material>>& materials,
                                  std::size_t count) {
        std::vector<RenderCommand> built(count);
        for (std::size_t index = 0; index < count; ++index) {
            built[index].geometry = geometry;
            built[index].material = materials[index % materials.size()];
            built[index].program  = program;
        }
        return built;
    };

    std::vector<RenderCommand> commands = commands_for(material_set(1U), 1U);

    const vn::intrusive_ptr<vn::graphics::Camera> camera = cameraLookingAt(0.0);
    const vn::intrusive_ptr<RenderPass>           pass(new RenderPass());
    const vn::graphics::ClearPolicy               clear{ vn::Colorf(0.0F, 0.25F, 0.0F, 1.0F), true };

    ContentStore*    store    = BackendContentAccess::store(*backend);
    ContentAssembly* assembly = BackendContentAccess::assembly(*backend);
    ASSERT_NE(store, nullptr);
    ASSERT_NE(assembly, nullptr);
    VariantPool& pool = BackendContentAccess::pool(*backend);

    const auto drive = [&]() {
        const auto began = std::chrono::steady_clock::now();
        backend->executeFrame(TestFrame()
            .pass(pass.get())
            .order(0)
            .target(nullptr)
            .clear(clear)
            .depth(vn::graphics::DepthMode::TestAndWrite)
            .content(commands, camera.get())
            .value());
        const auto ended = std::chrono::steady_clock::now();
        return std::chrono::duration_cast<std::chrono::duration<double, std::micro>>(ended - began).count();
    };

    struct MaterialCounters
    {
        std::uint64_t writes{0};
        std::uint64_t hits{0};
        std::uint64_t evictions{0};
        std::size_t   live{0};
    };
    const auto read_material = [&]() {
        const BlockStorage* storage = BackendContentAccess::storage(*backend);
        return MaterialCounters{ storage->materialWrites(), storage->materialHits(),
                                 storage->materialEvictions(), storage->liveMaterials() };
    };

    struct RegimeCost
    {
        EditCostDelta content;
        std::int64_t  writes{0};
        std::int64_t  hits{0};
        std::int64_t  evictions{0};
        std::size_t   live{0};
        double        frame_us{0.0};
    };

    std::printf("\n");
    const auto measure = [&](const char* label) {
        for (int warm = 0; warm < 3; ++warm) {
            drive();
        }
        const EditCostCounters before  = readEditCosts(*store, *assembly, pool);
        const MaterialCounters material_before = read_material();
        double                 frame_us = 0.0;
        for (int frame = 0; frame < kFramesPerRegime; ++frame) {
            frame_us += drive();
        }
        const EditCostCounters after  = readEditCosts(*store, *assembly, pool);
        const MaterialCounters material_after = read_material();
        const EditCostDelta    change = editCostDelta(before, after);
        const auto             step   = [](std::uint64_t from, std::uint64_t to) {
            return static_cast<std::int64_t>(to) - static_cast<std::int64_t>(from);
        };
        const std::int64_t writes = step(material_before.writes, material_after.writes);
        const std::int64_t hits   = step(material_before.hits, material_after.hits);
        const std::int64_t evictions = step(material_before.evictions, material_after.evictions);
        std::printf("[a4] %-26s frame %7.1f us/f"
                    " | uploads %+3lld created %+3lld | write/f %7.1f hit/f %7.1f evict/f %7.1f live %zu\n",
                    label, frame_us / kFramesPerRegime,
                    static_cast<long long>(change.uploads), static_cast<long long>(change.created),
                    static_cast<double>(writes) / kFramesPerRegime,
                    static_cast<double>(hits) / kFramesPerRegime,
                    static_cast<double>(evictions) / kFramesPerRegime, material_after.live);
        return RegimeCost{ change,
                           writes,
                           hits,
                           evictions,
                           material_after.live,
                           frame_us / kFramesPerRegime };
    };

    for (int settle = 0; settle < 6; ++settle) {
        drive();
    }

    // 1. ONE command: the frame's own floor (baseline every other row subtracts).
    const RegimeCost one = measure("1 command, 1 material");
    EXPECT_EQ(one.content.builds, 0);
    EXPECT_EQ(one.content.uploads, 0);
    EXPECT_EQ(one.content.created, 0);
    EXPECT_EQ(one.writes, 0) << "one unchanged material is one hit per frame";
    EXPECT_EQ(one.hits, kFramesPerRegime);
    EXPECT_EQ(one.evictions, 0);
    EXPECT_EQ(one.live, 1U);

    // 2. MANY commands, ONE material: everything the delta over regime 1 shows is the per-command floor.
    commands = commands_for(material_set(1U), 2000U);
    const RegimeCost many = measure("2000 commands, 1 material");
    EXPECT_EQ(many.content.uploads, 0);
    EXPECT_EQ(many.content.created, 0) << "2000 commands of one program/layout/geometry compile nothing";
    EXPECT_EQ(many.writes, 0);
    EXPECT_EQ(many.hits, static_cast<std::int64_t>(kFramesPerRegime) * 2000);
    EXPECT_EQ(many.evictions, 0);
    EXPECT_EQ(many.live, 1U)
        << "the first 2000-command frame grew the block storage (the draw budget), arena included: "
           "this arena has noted one material - and nothing but eviction ever removes a slot";
    std::printf("[a4] per-command floor ~= %.2f us/frame (2000-command frame minus the one-command frame)\n",
                (many.frame_us - one.frame_us) / 1999.0);

    // 3. THE BOUND, FITTING SIDE: 200 distinct materials are still served from the 256 slots - no write,
    //    no eviction, every note a hit.
    commands = commands_for(material_set(200U), 200U);
    const RegimeCost fitting = measure("200 commands, 200 materials");
    EXPECT_EQ(fitting.writes, 0) << "the live set fits the arena: nothing is rewritten";
    EXPECT_EQ(fitting.hits, static_cast<std::int64_t>(kFramesPerRegime) * 200);
    EXPECT_EQ(fitting.evictions, 0);
    EXPECT_EQ(fitting.live, 201U)
        << "200 live materials read as 201 slots: the PREVIOUS regime's material is still resident, "
           "because the storage has no release path at all - a slot leaves by eviction alone";

    // 4. THE BOUND, MISSING SIDE BY 44: 300 distinct materials against 256 slots - and the FIFO cascade
    //    means NOT ONE note hits: every command of every frame allocates and evicts.
    commands = commands_for(material_set(300U), 300U);
    const RegimeCost tight = measure("300 commands, 300 materials");
    EXPECT_EQ(tight.writes, static_cast<std::int64_t>(kFramesPerRegime) * 300)
        << "past the bound the scene can NEVER have a steady frame";
    EXPECT_EQ(tight.hits, 0) << "the cascade evicts a material before its own note comes round";
    EXPECT_EQ(tight.evictions, static_cast<std::int64_t>(kFramesPerRegime) * 300)
        << "every allocation evicts: the arena churns its whole live set per frame";
    EXPECT_EQ(tight.live, 256U);

    // 5. THE EXTREME: 2000 distinct materials - the whole per-frame cost writes every block, and the
    //    counters are the same shape, 2000 times over.
    commands = commands_for(material_set(2000U), 2000U);
    const RegimeCost churn = measure("2000 commands, 2000 materials");
    EXPECT_EQ(churn.writes, static_cast<std::int64_t>(kFramesPerRegime) * 2000);
    EXPECT_EQ(churn.hits, 0);
    EXPECT_EQ(churn.evictions, static_cast<std::int64_t>(kFramesPerRegime) * 2000);
    EXPECT_EQ(churn.live, 256U);

    // The drawings landed nothing: whatever the regimes did to the arena, the window is still the clear.
    drive();
    const auto centre = host.waitForPixel(kWidth / 2, kHeight / 2, arrived, std::chrono::milliseconds{ 500 });
    EXPECT_TRUE(isGreenClear(centre)) << "a zero-area triangle lands no fragment, got ("
                                      << static_cast<int>(centre[0]) << ", " << static_cast<int>(centre[1])
                                      << ", " << static_cast<int>(centre[2]) << ")";
    EXPECT_EQ(backend->deviceWaits(), 0U) << "2000 notes a frame stop nothing";
    backend->shutdown();
    EXPECT_TRUE(host.alive());
}

TEST(VsgBackendTest, TheTextureCacheDropsTheOldestInsertionAndRebuildsWhatItDropped)
{
    // A6'S SEMANTICS, PINNED WITHOUT A COUNTER. The cache's own vocabulary (has/count) is enough to say
    // what the registry (§6) registers: eviction is by INSERTION order rather than by use, and past the
    // 256-entry bound the cache stops caching - in a cyclic pass over L > 256 textures EVERY acquire of
    // the NEXT pass misses (each texture is evicted before its own turn comes round again), so the cache
    // uploads all L textures per pass. A pass over 200 textures misses NOTHING.
    //
    // Nothing is drawn: this is the cache's contract, not a frame's. The numbers are printed; the
    // assertions are the misses counted through has() plus the one A/B pair that tells FIFO from LRU.
    if (!deviceCaseAvailable())
    {
        GTEST_SKIP() << "no device session that serves off-screen pixels on this machine";
    }

    int               screen_index = 0;
    xcb_connection_t* connection   = xcb_connect(nullptr, &screen_index);
    const TestXConnection connection_owner(connection);  // closed at scope end (see TestXConnection)
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
    backend->setWindowHandle(host.handle());
    ASSERT_TRUE(backend->initialize());

    vn::vsg::MaterialImages* images = BackendContentAccess::images(*backend);
    ASSERT_NE(images, nullptr);
    EXPECT_EQ(images->count(), 0U) << "a session that drew nothing caches no texture";

    const auto make_texture = [](int side, unsigned shade) {
        const vn::intrusive_ptr<vn::graphics::Texture2D> texture(
            new vn::graphics::Texture2D(side, side, vn::imaging::PixelFormat::Rgba8Unorm));
        const vn::intrusive_ptr<vn::imaging::Image> image(
            new vn::imaging::Image(side, side, vn::imaging::PixelFormat::Rgba8Unorm));
        const std::span<std::byte> pixels = image->mipData(0);
        for (std::size_t index = 0; index < pixels.size(); ++index) {
            pixels[index] = static_cast<std::byte>((index + shade) & 0xFFU);
        }
        texture->setImage(vn::intrusive_ptr<const vn::imaging::Image>(image));
        return texture;
    };
    const auto acquire = [&](const vn::intrusive_ptr<vn::graphics::Texture2D>& texture) {
        vn::vsg::detail::TextureReject reason = vn::vsg::detail::TextureReject::Ok;
        const vn::vsg::SamplerImage acquired = images->acquire(texture.get(), reason);
        EXPECT_EQ(reason, vn::vsg::detail::TextureReject::Ok) << "a complete 2D texture must upload";
        EXPECT_NE(acquired.view, nullptr);
        EXPECT_NE(acquired.sampler, nullptr);
    };

    std::printf("\n");
    // 1. THE POLICY: insertion order, NOT use. B is acquired right after A; A is then acquired AGAIN (a
    //    hit - same revision) before X pushes the cache one past its bound. A cache that refreshed
    //    recency on a hit would drop B; this one drops A.
    {
        const auto oldest = make_texture(2, 1U);
        const auto second = make_texture(2, 2U);
        acquire(oldest);
        acquire(second);
        std::vector<vn::intrusive_ptr<vn::graphics::Texture2D>> filler;
        filler.reserve(254U);
        for (unsigned index = 0; index < 254U; ++index) {
            filler.push_back(make_texture(2, 3U + index));
            acquire(filler.back());
        }
        ASSERT_EQ(images->count(), 256U) << "the bound is 256 entries";
        ASSERT_TRUE(images->has(oldest.get()));
        acquire(oldest);  // the hit an LRU cache would count as USE
        const auto one_past = make_texture(2, 9U);
        acquire(one_past);
        EXPECT_FALSE(images->has(oldest.get()))
            << "eviction is by insertion order: the re-acquired texture is still the OLDEST insertion";
        EXPECT_TRUE(images->has(second.get())) << "an LRU cache would have dropped this one instead";
        EXPECT_EQ(images->count(), 256U) << "one past the bound evicts exactly one entry";
    }

    // The pass runner: count the entries the NEXT acquire will have to REBUILD by asking has() BEFORE it.
    const auto pass = [&](const std::vector<vn::intrusive_ptr<vn::graphics::Texture2D>>& set) {
        std::size_t misses = 0;
        for (const auto& texture : set) {
            if (!images->has(texture.get())) {
                ++misses;
            }
            acquire(texture);
        }
        return misses;
    };
    const auto time_pass = [&](const std::vector<vn::intrusive_ptr<vn::graphics::Texture2D>>& set) {
        const auto        began = std::chrono::steady_clock::now();
        const std::size_t misses = pass(set);
        const auto        ended = std::chrono::steady_clock::now();
        const double      millis =
            std::chrono::duration_cast<std::chrono::duration<double, std::milli>>(ended - began).count();
        return std::pair<std::size_t, double>{ misses, millis };
    };

    // 2. A SET THAT FITS: 200 textures (64x64, 16 KiB each) are rebuilt once, then served every pass.
    std::vector<vn::intrusive_ptr<vn::graphics::Texture2D>> fitting;
    fitting.reserve(200U);
    for (unsigned index = 0; index < 200U; ++index) {
        fitting.push_back(make_texture(64, index));
    }
    const auto [first_fitting, first_fitting_ms]   = time_pass(fitting);
    const auto [after_fitting, after_fitting_ms]   = time_pass(fitting);
    EXPECT_EQ(first_fitting, 200U) << "first sight of every texture rebuilds it";
    EXPECT_EQ(after_fitting, 0U) << "a set that fits the bound is served every pass";
    std::printf("[a6] 200-texture pass: %zu misses then %zu; %.2f ms then %.2f ms\n", first_fitting,
                after_fitting, first_fitting_ms, after_fitting_ms);

    // 3. PAST THE BOUND: 300 textures - a pass's tail (the last 256 insertions) survives, and EVERY pass
    //    after it misses EVERY texture (each is evicted before its own turn comes round again).
    std::vector<vn::intrusive_ptr<vn::graphics::Texture2D>> exceeding;
    exceeding.reserve(300U);
    for (unsigned index = 0; index < 300U; ++index) {
        exceeding.push_back(make_texture(64, 64U + index));
    }
    const auto [first_exceeding, first_exceeding_ms] = time_pass(exceeding);
    EXPECT_EQ(first_exceeding, 300U) << "first sight of every texture rebuilds it";
    EXPECT_EQ(images->count(), 256U) << "the bound caps the entries";
    EXPECT_FALSE(images->has(exceeding[0].get()));
    EXPECT_TRUE(images->has(exceeding[44].get())) << "the last 256 insertions are what survives a pass";
    EXPECT_TRUE(images->has(exceeding[299].get()));
    const auto [after_exceeding, after_exceeding_ms] = time_pass(exceeding);
    EXPECT_EQ(after_exceeding, 300U) << "past the bound the cache uploads every texture of every pass";
    std::printf("[a6] 300-texture pass: %zu misses then %zu; %.2f ms then %.2f ms (~%.1f us per rebuilt "
                "64x64 texture)\n",
                first_exceeding, after_exceeding, first_exceeding_ms, after_exceeding_ms,
                (after_exceeding_ms - after_fitting_ms) * 1000.0 / 300.0);
    EXPECT_EQ(images->count(), 256U);

    backend->shutdown();
    EXPECT_TRUE(host.alive());
}

/**
 * @brief A working set larger than the texture cache is a COST, and a host can see it.
 *
 * The cache's entry bound (MaterialImages::kMaxEntries) rebuilds and re-uploads a texture that comes back after
 * being evicted. The picture stays right, so nothing on screen says a word about it - which is why the sweep turns
 * the counter into one `ResourcePressure` sentence per EPISODE (a stretch of frames that evict) instead of one per
 * frame: a host reading `diagnosticCount(ResourcePressure)` can tell "my scene holds more textures at once than the
 * cache keeps" from "a steady scene", and that number is the trigger the A6 register waits on.
 *
 * Nothing here draws: 257 live textures are enough to make the bound evict, and an EMPTY frame is what runs the
 * frame's tail (the sweep is the frame's, not "the frames that drew") - this case is about the sentence it writes.
 */
TEST(VsgBackendTest, AWorkingSetLargerThanTheTextureCacheIsReportedOncePerEpisode)
{
    vn::intrusive_ptr<VsgBackend> backend(new VsgBackend());
    ASSERT_TRUE(backend->initialize());

    vn::vsg::MaterialImages* images = BackendContentAccess::images(*backend);
    ASSERT_NE(images, nullptr);
    const auto reported = [&backend]() {
        return backend->diagnosticCount(vn::graphics::DiagnosticCategory::ResourcePressure);
    };
    const std::size_t before = reported();
    EXPECT_EQ(images->evictions(), 0U) << "nothing has been sampled yet";

    // One entry past the bound, every texture STILL HELD: this is "more textures live at once than the cache keeps",
    // not "nobody holds it" (the sweep releases those, and that is not a cost).
    const auto sample = [&images](const vn::intrusive_ptr<vn::graphics::Texture2D>& texture, unsigned shade) {
        const vn::intrusive_ptr<vn::imaging::Image> image(
            new vn::imaging::Image(2, 2, vn::imaging::PixelFormat::Rgba8Unorm));
        const std::span<std::byte> pixels = image->mipData(0);
        std::fill(pixels.begin(), pixels.end(), static_cast<std::byte>(shade & 0xFFU));
        texture->setImage(vn::intrusive_ptr<const vn::imaging::Image>(image));

        vn::vsg::detail::TextureReject reason = vn::vsg::detail::TextureReject::Ok;
        (void)images->acquire(texture.get(), reason);
        ASSERT_EQ(reason, vn::vsg::detail::TextureReject::Ok) << "a complete 2D texture must upload";
    };

    std::vector<vn::intrusive_ptr<vn::graphics::Texture2D>> textures;
    textures.reserve(vn::vsg::MaterialImages::kMaxEntries + 1U);
    for (std::size_t index = 0; index <= vn::vsg::MaterialImages::kMaxEntries; ++index)
    {
        textures.push_back(
            vn::intrusive_ptr<vn::graphics::Texture2D>(new vn::graphics::Texture2D(2, 2, vn::imaging::PixelFormat::Rgba8Unorm)));
        sample(textures.back(), static_cast<unsigned>(index));
    }
    ASSERT_EQ(images->evictions(), 1U) << "the bound had to drop exactly one entry to make room";

    // The frame's tail turns that into a sentence - ONCE, however many frames keep evicting. A frame that draws nothing
    // is still a frame (see VsgBackend's empty-frame path): it runs the sweep all the same.
    backend->executeFrame(vn::graphics::FrameDescription{});
    EXPECT_EQ(reported(), before + 1U) << "the sweep is where 'out of room' becomes something a host can see";
    backend->executeFrame(vn::graphics::FrameDescription{});
    EXPECT_EQ(reported(), before + 1U) << "the same episode is one sentence, not one per frame";

    // A SUSTAINED oversized working set: the counter moves on every frame that samples a texture which was just
    // evicted, and the episode rule is what keeps that from being one sentence per frame (the watermark alone would
    // let each moved counter through).
    textures.push_back(
        vn::intrusive_ptr<vn::graphics::Texture2D>(new vn::graphics::Texture2D(2, 2, vn::imaging::PixelFormat::Rgba8Unorm)));
    sample(textures.back(), 7U);
    ASSERT_EQ(images->evictions(), 2U);
    backend->executeFrame(vn::graphics::FrameDescription{});
    EXPECT_EQ(reported(), before + 2U) << "a new episode (the counter had stopped moving) gets its own sentence";

    textures.push_back(
        vn::intrusive_ptr<vn::graphics::Texture2D>(new vn::graphics::Texture2D(2, 2, vn::imaging::PixelFormat::Rgba8Unorm)));
    sample(textures.back(), 8U);
    ASSERT_EQ(images->evictions(), 3U);
    backend->executeFrame(vn::graphics::FrameDescription{});
    EXPECT_EQ(reported(), before + 2U) << "the same episode is still one sentence while the counter keeps moving";

    // Letting go of the textures is not the bound doing anything: the sweep releases them, and says nothing.
    textures.clear();
    backend->executeFrame(vn::graphics::FrameDescription{});
    EXPECT_EQ(reported(), before + 2U) << "releasing a texture is not evicting it";

    backend->shutdown();
}

TEST(VsgBackendTest, MeasureWhatRepeatedCreateAndDropLeavesBehind)
{
    // A3'S QUESTION, MADE A LEDGER. The host's only way to "release" content today is dropping its own
    // references (the SDK has no release entry - §6/A3), so this recipe runs exactly that over cycles:
    // every cycle creates a geometry, a material, a PROGRAM and a texture, draws the geometry once, then
    // lets all four go. At each cycle's end the tables, the stream registry and the image cache must be
    // back at their baseline - the sweep's counters (releasedContentObjects / releasedTextures /
    // releasedStreams) are the proof the letting-go happened - and what the ledger always shows as the
    // residue is the material arena's slot handling, which has no release path at all (eviction only).
    if (!deviceCaseAvailable())
    {
        GTEST_SKIP() << "no device session that serves off-screen pixels on this machine";
    }

    int               screen_index = 0;
    xcb_connection_t* connection   = xcb_connect(nullptr, &screen_index);
    const TestXConnection connection_owner(connection);  // closed at scope end (see TestXConnection)
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
    backend->setWindowHandle(host.handle());
    ASSERT_TRUE(backend->initialize());

    ContentStore*    store    = BackendContentAccess::store(*backend);
    ContentAssembly* assembly = BackendContentAccess::assembly(*backend);
    ASSERT_NE(store, nullptr);
    ASSERT_NE(assembly, nullptr);
    vn::vsg::MaterialImages* images = BackendContentAccess::images(*backend);
    ASSERT_NE(images, nullptr);
    BlockStorage* const storage = BackendContentAccess::storage(*backend);
    ASSERT_NE(storage, nullptr);

    const vn::intrusive_ptr<vn::graphics::Camera> camera = cameraLookingAt(0.0);
    const vn::intrusive_ptr<RenderPass>           pass(new RenderPass());
    const vn::graphics::ClearPolicy               clear{ vn::Colorf(0.0F, 0.25F, 0.0F, 1.0F), true };

    std::vector<RenderCommand> commands;

    const auto drive = [&]() {
        backend->executeFrame(TestFrame()
            .pass(pass.get())
            .order(0)
            .target(nullptr)
            .clear(clear)
            .depth(vn::graphics::DepthMode::Disabled)
            .content(commands, camera.get())
            .value());
    };

    constexpr int kCycles = 12;
    // The settle window has to outlive the frames whose PLANS may still name the dropped objects (the sweep
    // skips anything somebody else still holds), so it is generously above the executor's own window.
    constexpr int kSettle = 16;

    // One cycle, verbatim: the host makes a geometry, a material, a program and a texture, draws the
    // geometry once, and then lets all of them go.
    const auto run_cycle = [&](int cycle) {
        {
            // What the host makes THIS cycle: its own geometry, material, program and texture...
            const vn::intrusive_ptr<Geometry> geometry(new Geometry());
            geometry->setPositions(vn::intrusive_ptr<const vn::Buffer<float>>(new vn::Buffer<float>(
                std::vector<float>{ -0.5F, -0.5F, 0.5F, -0.5F, -0.5F, 0.5F, -0.5F, -0.5F, 0.5F })));
            geometry->setIndices(vn::intrusive_ptr<const vn::Buffer<std::uint32_t>>(
                new vn::Buffer<std::uint32_t>(std::vector<std::uint32_t>{ 0U, 1U, 2U })));
            geometry->bumpRevision();

            const vn::intrusive_ptr<Material> material(new Material());
            material->setDiffuse(vn::Colorf(0.3F, 0.4F, 0.5F, 1.0F));

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

            commands = std::vector<RenderCommand>(1U);
            commands[0].geometry = geometry;
            commands[0].material = material;
            commands[0].program  = program;

            // ... and a texture of its own, acquired through the cache the way a declared set would.
            const vn::intrusive_ptr<vn::graphics::Texture2D> texture(
                new vn::graphics::Texture2D(2, 2, vn::imaging::PixelFormat::Rgba8Unorm));
            {
                const vn::intrusive_ptr<vn::imaging::Image> image(
                    new vn::imaging::Image(2, 2, vn::imaging::PixelFormat::Rgba8Unorm));
                const std::span<std::byte> pixels = image->mipData(0);
                for (std::size_t index = 0; index < pixels.size(); ++index) {
                    pixels[index] = static_cast<std::byte>(
                        (index * 7U + static_cast<std::size_t>(cycle)) & 0xFFU);
                }
                texture->setImage(vn::intrusive_ptr<const vn::imaging::Image>(image));
            }
            vn::vsg::detail::TextureReject reason = vn::vsg::detail::TextureReject::Ok;
            (void)images->acquire(texture.get(), reason);
            EXPECT_EQ(reason, vn::vsg::detail::TextureReject::Ok);
            EXPECT_TRUE(images->has(texture.get())) << "the cache holds this cycle's texture, for now";

            drive();  // the one frame that names all of it
        }
        // The host lets EVERYTHING go here (the references above are gone).
        commands.clear();
    };
    const auto settle_frames = [&]() {
        for (int frame = 0; frame < kSettle; ++frame) {
            drive();
        }
    };

    // WARM-UP FIRST, and it stays OUT of the measured counters. Two parts: empty frames until the session has
    // LEARNED its in-flight slot count (a drop before that cannot be parked - RetirementQueue::retire
    // refuses to guess - and its rows are then retained for the session, by design; see
    // ContentStore::retained), then one whole cycle whose drop therefore parks like every later one.
    // Letting the warm-up absorb both makes every measured cycle's baseline the settled state.
    std::printf("\n");
    for (int warm = 0; warm < kSettle; ++warm) {
        drive();
    }
    run_cycle(0);
    settle_frames();
    const std::size_t retained_after_warmup = store->retained();
    const std::size_t base_geometry         = store->geometryEntries();
    const std::size_t base_material         = store->materialEntries();
    const std::size_t base_program          = store->programEntries();
    const std::size_t base_slots            = storage->liveMaterials();
    const std::size_t base_streams          = assembly->uploads().live();
    const std::size_t released_objects_before  = backend->releasedContentObjects();
    const std::size_t released_textures_before = backend->releasedTextures();
    std::printf("[a3] warm-up: rows g%zu m%zu p%zu retained %zu (pre-slot-knowledge drops stay for the "
                "session), slots %zu, streams %zu\n",
                base_geometry, base_material, base_program, retained_after_warmup, base_slots, base_streams);

    for (int cycle = 0; cycle < kCycles; ++cycle) {
        run_cycle(cycle + 1);
        settle_frames();

        EXPECT_EQ(store->geometryEntries(), base_geometry) << "cycle " << cycle << ": the row is let go";
        EXPECT_EQ(store->materialEntries(), base_material) << "cycle " << cycle << ": the row is let go";
        EXPECT_EQ(store->programEntries(), base_program) << "cycle " << cycle << ": the row is let go";
        EXPECT_EQ(images->count(), 0U) << "cycle " << cycle << ": the texture is let go";
        EXPECT_EQ(assembly->uploads().live(), base_streams) << "cycle " << cycle << ": the streams are let go";
        std::printf("[a3] cycle %2d: entries g%zu m%zu p%zu | images %zu streams %zu slots %zu | released "
                    "objects %zu textures %zu streams %zu\n",
                    cycle, store->geometryEntries(), store->materialEntries(), store->programEntries(),
                    images->count(), assembly->uploads().live(), storage->liveMaterials(),
                    backend->releasedContentObjects(), backend->releasedTextures(),
                    backend->releasedStreams());
    }

    // THE RELEASES REALLY HAPPENED, once per object and once per texture - the counters the sweep feeds.
    EXPECT_EQ(backend->releasedContentObjects() - released_objects_before, static_cast<std::size_t>(kCycles) * 3U)
        << "a geometry, a material and a program per cycle";
    EXPECT_EQ(backend->releasedTextures() - released_textures_before, static_cast<std::size_t>(kCycles))
        << "one texture per cycle";
    EXPECT_GE(backend->releasedStreams(), static_cast<std::size_t>(kCycles))
        << "the streams of the dropped geometries are let go once the naming window passes";
    EXPECT_EQ(store->retained(), retained_after_warmup)
        << "no drop during the measured cycles needed retention: every park came due";
    EXPECT_LE(store->retained(), 3U) << "only the first drops, before the slots were learned, are kept";

    // THE ONE RESIDUE the ledger shows: material SLOTS have no release path (eviction only, see §11.16cx),
    // and how many survive here is the allocator's business - a freed material's address may come back.
    const std::size_t slots = storage->liveMaterials();
    EXPECT_LE(slots, 256U) << "the arena's bound still holds";
    std::printf("[a3] residue: %zu/%zu material slots held, %zu above the session's baseline (no release "
                "path - eviction only)\n",
                slots, base_slots + static_cast<std::size_t>(kCycles), slots - base_slots);

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
        GTEST_SKIP() << "no device session that serves off-screen pixels on this machine";
    }

    int               screen_index = 0;
    xcb_connection_t* connection   = xcb_connect(nullptr, &screen_index);
    const TestXConnection connection_owner(connection);  // closed at scope end (see TestXConnection)
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
    const vn::graphics::ClearPolicy               clear{ vn::Colorf(0.0F, 0.25F, 0.0F, 1.0F), true };

    const auto drive = [&] {
        // Depth off, so the second drawing call is simply the one whose colour the probe sees - the pass states it
        // once, for its whole content (see PassRenderState).
        backend->executeFrame(TestFrame()
                                  .pass(pass.get())
                                  .target(nullptr)
                                  .clear(clear)
                                  .depth(vn::graphics::DepthMode::Disabled)
                                  .content(commands, camera.get())
                                  .value());
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

TEST(VsgBackendTest, ACountDeeperThanTheRingsServeReplacesTheStorageBeforeTheNextFrame)
{
    // THE IN-FLIGHT FLOOR'S OTHER HALF. BlockStorage::create raises a shallow layout (see its cases), and
    // this is what happens when the count the storage was built for turns out to be TOO SMALL: the session
    // LEARNS the framework's real count over the first frames, and a deeper one than the rings serve asks
    // for the same replacement growth takes - a new buffer, the old one parked, said out loud once. The
    // framework on this machine reports exactly the assumed count, so the deeper count is stated through
    // the seam that exists for it (see assumeInFlightSlots); the branch it reaches is the branch a real
    // deeper framework reaches.
    if (!deviceCaseAvailable())
    {
        GTEST_SKIP() << "no device session that serves off-screen pixels on this machine";
    }

    int               screen_index = 0;
    xcb_connection_t* connection   = xcb_connect(nullptr, &screen_index);
    const TestXConnection connection_owner(connection);  // closed at scope end (see TestXConnection)
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

    BlockStorage* const before = BackendContentAccess::storage(*backend);
    ASSERT_NE(before, nullptr);
    ASSERT_EQ(before->layout().views.slabs, vn::graphics::backend::perFrameCopies(vn::graphics::backend::kAssumedInFlightSlots))
        << "the session starts on the assumed count; the floor is what create() built";
    const std::uint64_t before_capacity = before->capacityBytes();

    BackendContentAccess::assumeInFlightSlots(*backend, vn::graphics::backend::kAssumedInFlightSlots + 1U);

    const vn::intrusive_ptr<vn::graphics::Camera> camera = cameraLookingAt(0.5);
    const vn::intrusive_ptr<RenderPass>           pass(new RenderPass());
    const vn::graphics::ClearPolicy               clear{ vn::Colorf(0.0F, 0.25F, 0.0F, 1.0F), true };
    const std::vector<RenderCommand>              no_commands;
    const auto                                    drive = [&] {
        backend->executeFrame(TestFrame()
            .pass(pass.get())
            .order(0)
            .target(nullptr)
            .clear(clear)
            .depth(vn::graphics::DepthMode::Disabled)
            .content(no_commands, camera.get())
            .value());
    };

    drive();  // its beginFrame() finds the rings too shallow for the stated count

    BlockStorage* const after = BackendContentAccess::storage(*backend);
    ASSERT_NE(after, before) << "replaced, not resized - the same swap growth makes";
    const std::uint32_t deeper = vn::graphics::backend::perFrameCopies(vn::graphics::backend::kAssumedInFlightSlots + 1U);
    EXPECT_EQ(after->layout().views.slabs, deeper);
    EXPECT_EQ(after->layout().draws.slabs, deeper) << "every per-frame shape is re-laid out together";
    EXPECT_EQ(after->layout().materials.copies, deeper);
    EXPECT_GT(after->capacityBytes(), before_capacity);
    ASSERT_EQ(backend->diagnosticCount(), 1U) << "the re-layout is its own fact, said once";
    EXPECT_NE(std::string::npos, messages.back().find("in-flight count (4)"))
        << "and the host can read WHY the bytes moved";
    EXPECT_EQ(after->overflows(), 0U);

    // AND IT SETTLES: the count it was re-laid out for is the count it now serves, so the next frame leaves
    // it alone - a storage replaced every frame would churn a buffer per frame and park without end.
    BlockStorage* const settled = after;
    drive();
    EXPECT_EQ(BackendContentAccess::storage(*backend), settled);
    EXPECT_EQ(backend->diagnosticCount(), 1U);

    backend->shutdown();
    EXPECT_TRUE(host.alive());
}

TEST(VsgBackendTest, AnOpaqueContentDrawDoesNotBlendAndATranslucentOneDoes)
{
    // THE ENABLE AND THE FACTORS, READ BACK FROM THE SURFACE. The resolved state is what decides blending
    // (StateCommands.cpp honours `state.blend.enabled`; FrameCompiler::resolveCommands is what turns it on for
    // opacity < 1). So a fragment writing (1, 0, 0, 0.5) into a ONE-attachment target over a blue clear:
    //   * opaque command -> writes its colour UNCHANGED: 1.0 red, the clear gone (blending off, as three.js /
    //     Filament / Unity treat an opaque material, even though the shader wrote an alpha below 1);
    //   * the same command at opacity 0.5 -> HALF red / HALF blue (the standard SRC_ALPHA pair).
    // The read-back accepts 0.5's linear AND sRGB spellings but not 1.0's, so the pair tells the two apart.
    if (!deviceCaseAvailable())
    {
        GTEST_SKIP() << "no device session that serves off-screen pixels on this machine";
    }

    int               screen_index = 0;
    xcb_connection_t* connection   = xcb_connect(nullptr, &screen_index);
    const TestXConnection connection_owner(connection);  // closed at scope end (see TestXConnection)
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
    backend->setWindowHandle(host.handle());
    ASSERT_TRUE(backend->initialize());
    EXPECT_EQ(backend->diagnosticCount(), 0U);

    // Clip-space positions passed through, and a fragment that writes a HALF-TRANSPARENT red: the alpha is
    // the shader's, so nothing here depends on how the engine feeds opacity into a program.
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
            "void main() { outColor = vec4(1.0, 0.0, 0.0, 0.5); }\n"));
        program->addStage(vertex);
        program->addStage(fragment);
    }

    const vn::intrusive_ptr<Geometry> geometry(new Geometry());
    const vn::intrusive_ptr<vn::Buffer<float>> positions = vn::intrusive_ptr<vn::Buffer<float>>(
        new vn::Buffer<float>(std::vector<float>{ -0.4F, -0.4F, 0.5F, 0.4F, -0.4F, 0.5F, 0.0F, 0.6F, 0.5F }));
    const vn::intrusive_ptr<vn::Buffer<std::uint32_t>> indices = vn::intrusive_ptr<vn::Buffer<std::uint32_t>>(
        new vn::Buffer<std::uint32_t>(std::vector<std::uint32_t>{ 0U, 1U, 2U }));
    geometry->setPositions(positions);
    geometry->setIndices(indices);
    geometry->setRevision(1U);

    const vn::intrusive_ptr<Material> material(new Material());
    material->setDiffuse(vn::Colorf(1.0F, 1.0F, 1.0F, 1.0F));

    RenderCommand command;
    command.geometry = geometry;
    command.material = material;
    command.program  = program;
    const std::vector<RenderCommand> opaque_commands{ command };
    RenderCommand                    translucent_command = command;
    translucent_command.opacity                          = 0.5F;
    const std::vector<RenderCommand> translucent_commands{ translucent_command };

    const vn::intrusive_ptr<vn::graphics::Camera> camera = cameraLookingAt(0.0);
    const vn::intrusive_ptr<RenderPass>           pass(new RenderPass());
    // A blue clear: the dst factor is what leaves it standing at half strength next to the fragment's red.
    const vn::graphics::ClearPolicy clear{ vn::Colorf(0.0F, 0.0F, 1.0F, 1.0F), true };

    const auto drive = [&](const std::vector<RenderCommand>& commands) {
        backend->executeFrame(TestFrame()
            .pass(pass.get())
            .order(0)
            .target(nullptr)
            .clear(clear)
            .depth(vn::graphics::DepthMode::Disabled)
            .content(commands, camera.get())
            .value());
    };

    // The display path lags the session's presents: present the SAME picture once more, then read (a
    // plan-free frame would repaint the window with the session's own graph).
    drive(opaque_commands);
    drive(opaque_commands);
    const auto opaque_centre = host.waitForPixel(
        kWidth / 2, kHeight / 2, [](const std::array<std::uint8_t, 3>& pixel) { return isRed(pixel); },
        std::chrono::milliseconds{ 500 });
    EXPECT_TRUE(isRed(opaque_centre))
        << "an opaque command writes its colour unchanged (blending off, shader alpha ignored), got ("
        << static_cast<int>(opaque_centre[0]) << ", " << static_cast<int>(opaque_centre[1]) << ", "
        << static_cast<int>(opaque_centre[2]) << ")";
    EXPECT_FALSE(isColourByte(opaque_centre[0], 0.5) || isColourByte(opaque_centre[2], 0.5))
        << "and the blue clear did not survive at half strength, got ("
        << static_cast<int>(opaque_centre[0]) << ", " << static_cast<int>(opaque_centre[1]) << ", "
        << static_cast<int>(opaque_centre[2]) << ")";

    drive(translucent_commands);
    drive(translucent_commands);
    const auto translucent_centre = host.waitForPixel(
        kWidth / 2, kHeight / 2,
        [](const std::array<std::uint8_t, 3>& pixel) { return isColourByte(pixel[2], 0.5); },
        std::chrono::milliseconds{ 500 });
    // Both outer channels are 0.5, so the surface's byte order does not matter for the centre.
    EXPECT_TRUE(isColourByte(translucent_centre[0], 0.5))
        << "src * SRC_ALPHA: the fragment's red at half alpha, got ("
        << static_cast<int>(translucent_centre[0]) << ", " << static_cast<int>(translucent_centre[1]) << ", "
        << static_cast<int>(translucent_centre[2]) << ")";
    EXPECT_TRUE(isColourByte(translucent_centre[2], 0.5))
        << "dst * ONE_MINUS_SRC_ALPHA: the clear's blue survives at half strength, got ("
        << static_cast<int>(translucent_centre[0]) << ", " << static_cast<int>(translucent_centre[1]) << ", "
        << static_cast<int>(translucent_centre[2]) << ")";

    // The corner never saw a fragment: the pass' own clear, in whichever byte order the surface uses.
    const auto corner = host.pixel(1, 1);
    const bool blue_clear = (isColourByte(corner[0], 0.0) && isColourByte(corner[2], 1.0)) ||
                            (isColourByte(corner[2], 0.0) && isColourByte(corner[0], 1.0));
    EXPECT_TRUE(blue_clear) << "the corner is the blue clear, got (" << static_cast<int>(corner[0]) << ", "
                            << static_cast<int>(corner[1]) << ", " << static_cast<int>(corner[2]) << ")";

    EXPECT_EQ(backend->deviceWaits(), 0U) << "reading the picture stops nothing";
    backend->shutdown();
    EXPECT_TRUE(host.alive());
}

TEST(VsgBackendTest, TheDocumentedScaleGrowsTheDrawBudgetOnceAndThenServesEveryCommand)
{
    // THE TRIGGER SCALE ITSELF, MADE A RECIPE. The design log names ~2000 drawables as the size at which the
    // 1024-block budget stops being generous; this case drives that shape - ONE drawing call carrying 2000
    // commands - against the DEFAULT budgets, and pins what settling in costs:
    //
    //   * frame 1 asks for 2000 draw blocks (one per command) - and 976 are refused, with the call's own
    //     light and shadow blocks (one each per CALL) fitting comfortably;
    //   * frame 2's beginFrame() has replaced the storage (draws 1024 -> 2048, the doubling policy already
    //     covering the 2000 the frame tried), and every command of the same content is served;
    //   * frame 3 is steady: nothing replaced, nothing said.
    //
    // The per-frame record / commit times are PRINTED for the design log; nothing machine-dependent is
    // asserted.
    if (!deviceCaseAvailable())
    {
        GTEST_SKIP() << "no device session that serves off-screen pixels on this machine";
    }

    int               screen_index = 0;
    xcb_connection_t* connection   = xcb_connect(nullptr, &screen_index);
    const TestXConnection connection_owner(connection);  // closed at scope end (see TestXConnection)
    if (connection == nullptr || xcb_connection_has_error(connection) != 0)
    {
        GTEST_SKIP() << "no X display to create a host window on";
    }
    xcb_screen_t* screen = xcb_setup_roots_iterator(xcb_get_setup(connection)).data;
    ASSERT_NE(screen, nullptr);

    constexpr int kWidth  = 128;
    constexpr int kHeight = 96;
    constexpr int kDraws  = 2000;  // the design log's trigger scale
    TestHostWindow host(connection, screen, kWidth, kHeight);

    vn::intrusive_ptr<VsgBackend> backend(new VsgBackend());
    std::vector<std::string>      messages;
    backend->setDiagnosticSink([&messages](const vn::graphics::RenderDiagnostic& diagnostic) {
        messages.push_back(as_bytes(diagnostic.message));
    });
    backend->setWindowHandle(host.handle());
    ASSERT_TRUE(backend->initialize());
    EXPECT_EQ(backend->diagnosticCount(), 0U);

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
    material->setDiffuse(vn::Colorf(0.25F, 0.6F, 0.25F, 1.0F));

    RenderCommand command;
    command.geometry = geometry;
    command.material = material;
    command.program  = program;
    const std::vector<RenderCommand> commands(static_cast<std::size_t>(kDraws), command);

    const vn::intrusive_ptr<vn::graphics::Camera> camera = cameraLookingAt(0.5);
    const vn::intrusive_ptr<RenderPass>           pass(new RenderPass());
    const vn::graphics::ClearPolicy               clear{ vn::Colorf(0.0F, 0.25F, 0.0F, 1.0F), true };

    const auto drive = [&]() {
        const auto began = std::chrono::steady_clock::now();
        backend->executeFrame(TestFrame()
            .pass(pass.get())
            .order(0)
            .target(nullptr)
            .clear(clear)
            .depth(vn::graphics::DepthMode::Disabled)
            .content(commands, camera.get())
            .value());
        const auto ended = std::chrono::steady_clock::now();
        return std::chrono::duration_cast<std::chrono::duration<double, std::milli>>(ended - began).count();
    };

    // Frame 1: the default budgets; the light blocks run out first (the call checks them first).
    const double frame1_ms = drive();
    std::printf("[cost] %d draws, frame 1 (default budgets): %.1f ms\n", kDraws, frame1_ms);
    BlockStorage* const default_storage = BackendContentAccess::storage(*backend);
    ASSERT_NE(default_storage, nullptr);
    EXPECT_EQ(default_storage->overflows(), kDraws - 1024) << "976 draw blocks were refused";
    EXPECT_EQ(default_storage->growthNeeded().draws, kDraws) << "the request is what the frame TRIED";
    EXPECT_EQ(default_storage->growthNeeded().lights, 0U) << "the call's ONE light block fitted";
    EXPECT_EQ(default_storage->growthNeeded().shadows, 0U) << "and so did its ONE shadow block";
    ASSERT_FALSE(messages.empty());
    EXPECT_NE(std::string::npos, messages.front().find("draw block")) << "the first refusal names the block";
    EXPECT_NE(std::string::npos, messages.back().find("976")) << "the summary counts how many commands were lost";

    // Frame 2: the draw region grew, and the SAME content is served whole - the storage itself is not
    // resized: it was replaced by one whose draw budget covers what the frame tried.
    const double frame2_ms = drive();
    std::printf("[cost] %d draws, frame 2 (draws grew): %.1f ms\n", kDraws, frame2_ms);
    BlockStorage* const grown = BackendContentAccess::storage(*backend);
    ASSERT_NE(grown, default_storage) << "frame 2's beginFrame() replaced the storage";
    EXPECT_EQ(grown->layout().draws.blocks_per_frame, 2048U) << "doubling covers the 2000 the frame tried";
    EXPECT_EQ(grown->layout().lights.blocks_per_frame, 1024U) << "only the exhausted region grows";
    EXPECT_EQ(grown->layout().shadows.blocks_per_frame, 1024U);
    EXPECT_EQ(grown->overflows(), 0U) << "all 2000 commands are served";
    EXPECT_FALSE(grown->growthNeeded().needed()) << "and the storage asks for nothing more";
    EXPECT_EQ(backend->diagnosticCount(), 3U) << "two lines for the lost frame, one for the growth";

    // Frame 3: steady - the settling is ONE growth event, and a settled frame says nothing at all.
    const double frame3_ms = drive();
    std::printf("[cost] %d draws, frame 3 (steady): %.1f ms\n", kDraws, frame3_ms);
    EXPECT_EQ(BackendContentAccess::storage(*backend), grown) << "a settled frame replaces nothing";
    EXPECT_EQ(grown->overflows(), 0U);
    EXPECT_EQ(backend->diagnosticCount(), 3U) << "and says nothing new";

    // The books: exactly one growth, and the frame that asked for it was the last one to lose content.
    std::size_t growths = 0U;
    for (const std::string& message : messages) {
        if (message.find("grew") != std::string::npos) {
            ++growths;
        }
    }
    EXPECT_EQ(growths, 1U) << "one growth event settled the documented scale";

    // The picture after all of it: the triangle the commands draw, and a frame path that never stopped the
    // device while the storage was replaced under it.
    const auto pixel = host.waitForPixel(
        kWidth / 2, kHeight / 2,
        [](const std::array<std::uint8_t, 3>& value) { return isColourByte(value[1], 0.6); },
        std::chrono::milliseconds{ 500 });
    EXPECT_TRUE(isColourByte(pixel[1], 0.6)) << "the 2000-call pass still lands in the window";
    EXPECT_EQ(backend->deviceWaits(), 0U);

    backend->shutdown();
    EXPECT_TRUE(host.alive());
}

#endif  // !defined(_WIN32)

TEST(VsgBackendTest, TheSameWindowIsNeverServedTwiceAndADifferentOneGetsItsOwnTarget)
{
    // ONE DEVICE, SEVERAL WINDOWS (S10b). What a device hands out is one target per WINDOW, because a window is a
    // surface plus a swapchain and the platform allows one live swapchain per window; the DEVICE is the thing that
    // is shared. So: the same window asked for twice is the "occupied" episode, said once; a different window gets
    // its own target object - and the first window is answered by the aggregate itself (RenderBackend).
    vn::intrusive_ptr<VsgBackend> backend(new VsgBackend());
    std::vector<vn::graphics::RenderDiagnostic> reported;
    backend->setDiagnosticSink([&reported](const vn::graphics::RenderDiagnostic& diagnostic) {
        reported.push_back(diagnostic);
    });

    // The device's own window (no handle): the device answers for it itself, so a host that drives one window
    // directly has one object to hold.
    vn::graphics::SurfaceTargetResult why = vn::graphics::SurfaceTargetResult::Ok;
    vn::raw_ptr<vn::graphics::WindowPresentTarget> own = backend->createSurfaceTarget(nullptr, &why);
    ASSERT_NE(own, nullptr);
    EXPECT_EQ(why, vn::graphics::SurfaceTargetResult::Ok);
    EXPECT_EQ(own->nativeHandle(), nullptr) << "nothing is attached before a session comes up";
    EXPECT_EQ(backend->diagnosticCount(), 0U) << "the first window is not an episode";
    EXPECT_EQ(backend->windowCount(), 1U);

    // The same window again: occupied, said once.
    EXPECT_EQ(backend->createSurfaceTarget(nullptr, &why), nullptr);
    EXPECT_EQ(why, vn::graphics::SurfaceTargetResult::WindowOccupied);
    ASSERT_EQ(backend->diagnosticCount(vn::graphics::DiagnosticCategory::UnsupportedRequest), 1U);
    ASSERT_EQ(reported.size(), 1U);
    EXPECT_EQ(reported.front().severity, vn::graphics::DiagnosticSeverity::Warning);

    EXPECT_EQ(backend->createSurfaceTarget(nullptr, &why), nullptr);
    EXPECT_EQ(backend->diagnosticCount(vn::graphics::DiagnosticCategory::UnsupportedRequest), 1U)
        << "one report per episode, however often the same ask comes";

    // The device comes up on the one window it serves. (Done HERE, before the second window is asked for: every
    // window a device knows is brought up by initialize(), and `second` below is a value the test can name but the
    // platform cannot adopt as a window - while a target asked for on a LIVE device never needs to be adopted at
    // all, which is the fact this case tests.)
    if (deviceCaseAvailable())
    {
        ASSERT_TRUE(backend->initialize());
        EXPECT_EQ(backend->deviceWaits(), 0U) << "serving a window is not a device stop";
    }

    // A DIFFERENT window: served, through its own target object (the same device, one more surface and swapchain).
    int  second = 0;
    vn::raw_ptr<vn::graphics::WindowPresentTarget> other = backend->createSurfaceTarget(&second, &why);
    ASSERT_NE(other, nullptr);
    EXPECT_EQ(why, vn::graphics::SurfaceTargetResult::Ok);
    EXPECT_NE(other, own) << "and it is not the first window's target";
    EXPECT_NE(static_cast<const void*>(own), static_cast<const void*>(backend.get()))
        << "neither target IS the device: the type does not even allow it (see device-is-not-a-window.md)";
    EXPECT_NE(static_cast<const void*>(other), static_cast<const void*>(backend.get()));
    EXPECT_EQ(other->nativeHandle(), nullptr) << "nothing is attached before ITS session comes up";
    EXPECT_EQ(backend->windowCount(), 2U);
    EXPECT_EQ(backend->diagnosticCount(vn::graphics::DiagnosticCategory::UnsupportedRequest), 1U)
        << "a second window is not an episode";
    EXPECT_EQ(backend->deviceWaits(), 0U) << "handing out a target is not a device stop either";
}

TEST(VsgBackendTest, ANullTargetRunsTheFrameWithoutAnyWindow)
{
    if (!deviceCaseAvailable())
    {
        GTEST_SKIP() << "no device session that serves off-screen pixels on this machine";
    }

    // A FRAME THAT NAMES NO TARGET IS NOT A FRAME FOR WINDOW 0 (see RenderFrameExecutor::executeFrame and
    // `.ai/design/device-is-not-a-window.md` §6): it is the off-screen form. It acquires nothing, presents nothing,
    // brings no window into existence, and reports nothing - and the next frame through a window's target still
    // presents exactly once.
    vn::intrusive_ptr<vn::vsg::VsgBackend> backend(new vn::vsg::VsgBackend());
    std::vector<vn::graphics::RenderDiagnostic> reported;
    backend->setDiagnosticSink([&reported](const vn::graphics::RenderDiagnostic& diagnostic) {
        reported.push_back(diagnostic);
    });

    // The window is asked for BEFORE the device comes up (the SDK's order), so this case can drive a windowed frame
    // afterwards without depending on the device's own window.
    vn::graphics::SurfaceTargetResult why = vn::graphics::SurfaceTargetResult::Ok;
    vn::raw_ptr<vn::graphics::WindowPresentTarget> window = backend->createSurfaceTarget(nullptr, &why);
    ASSERT_NE(window, nullptr);
    ASSERT_EQ(why, vn::graphics::SurfaceTargetResult::Ok);
    ASSERT_TRUE(backend->initialize());
    EXPECT_EQ(backend->diagnosticCount(), 0U) << "a healthy session reports nothing";

    // 1. The off-screen frame: submitted (its passes, if any, draw into the targets they name), presented nowhere.
    const std::size_t windows_before = backend->windowCount();
    backend->executeFrame(vn::graphics::FrameDescription{});
    EXPECT_EQ(backend->framesPresented(), 0U) << "an off-screen frame presents nothing";
    EXPECT_EQ(backend->windowCount(), windows_before) << "and it does not become a window";
    EXPECT_EQ(backend->diagnosticCount(), 0U) << "the off-screen form is a first-class frame, not a problem";
    const vn::graphics::FrameReport offscreen_report = backend->frameReport();
    ASSERT_TRUE(offscreen_report.progress.has_value()) << "the device's frame clock answers it";
    EXPECT_EQ(offscreen_report.progress->submitted, 1U) << "and the frame really reached the queue";

    // 2. The same value through the window's target: recorded, submitted and presented - once.
    backend->executeFrame(vn::graphics::FrameDescription{}, window);
    EXPECT_EQ(backend->framesPresented(), 1U);
    EXPECT_EQ(backend->diagnosticCount(), 0U);
    EXPECT_EQ(reported.size(), backend->diagnosticCount()) << "the sink sees every counted diagnostic";

    // 3. EVIDENCE, NOT A GAP: an off-screen frame's GPU work counts towards the DEVICE's completion watermark like a
    //    window's does (see core::DeviceClock). Each session learns its in-flight count over its own first frames, so
    //    drive enough of both and read the completed watermark - a session that never reports would keep the device's
    //    minimum pinned where it joined, and this number would stay 0.
    for (int index = 0; index < 3; ++index)
    {
        backend->executeFrame(vn::graphics::FrameDescription{});
    }
    for (int index = 0; index < 4; ++index)
    {
        backend->executeFrame(vn::graphics::FrameDescription{}, window);
    }
    const vn::graphics::FrameReport evidence = backend->frameReport();
    ASSERT_TRUE(evidence.progress.has_value());
    EXPECT_EQ(backend->framesPresented(), 5U) << "the window's frames present; the off-screen ones never do";
    EXPECT_GT(evidence.progress->completed, 0U)
        << "the off-screen session's evidence reached the device clock (the watermark is the minimum over sources)";
    EXPECT_LE(evidence.progress->completed, evidence.progress->submitted);

    backend->shutdown();
}

TEST(VsgBackendTest, ADeviceCreatedWithNoPresentationRunsWholeFramesWithoutAWindow)
{
    if (!deviceCaseAvailable())
    {
        GTEST_SKIP() << "no device session that serves off-screen pixels on this machine";
    }

    // A DEVICE CREATED TO PRESENT NOTHING (see DeviceDesc::None) has no window half for its whole life: initialize()
    // creates the device through the windowless seam, so no window is ever created, no target is handed out, and a
    // frame that names none is submitted like any other. This is the shape a headless phase asks for - and the reason
    // asking for a target is answered, not reported.
    vn::graphics::DeviceDesc desc;
    desc.presentation = vn::graphics::DevicePresentation::None;
    // The plain device, not this file's drive: that one holds a window's target so a case can name it, and this device
    // has no window to hold (its one-argument executeFrame would ask for window 0's target and be answered NoPresentation).
    vn::intrusive_ptr<vn::vsg::VsgBackend> backend(new vn::vsg::VsgBackend(desc));
    std::vector<vn::graphics::RenderDiagnostic> reported;
    backend->setDiagnosticSink([&reported](const vn::graphics::RenderDiagnostic& diagnostic) {
        reported.push_back(diagnostic);
    });

    // 1. It says so before anything is created, and asking for a window is the silent NoPresentation.
    EXPECT_FALSE(backend->capabilities().presentation) << "a device created to present nothing says so";
    EXPECT_TRUE(backend->capabilities().render_targets) << "targets are still the half it does serve";
    EXPECT_EQ(backend->windowCount(), 0U);
    vn::graphics::SurfaceTargetResult why = vn::graphics::SurfaceTargetResult::Ok;
    EXPECT_EQ(backend->createSurfaceTarget(nullptr, &why), nullptr);
    EXPECT_EQ(why, vn::graphics::SurfaceTargetResult::NoPresentation);
    EXPECT_EQ(backend->diagnosticCount(), 0U) << "a display-less device is a state, not a failure";
    EXPECT_EQ(backend->windowCount(), 0U) << "and asking did not become a window";

    // 2. It comes up without one, and stays without one.
    ASSERT_TRUE(backend->initialize());
    EXPECT_TRUE(backend->initialized());
    EXPECT_EQ(backend->windowCount(), 0U) << "the windowless seam created the device, not a window's session";
    EXPECT_EQ(backend->diagnosticCount(), 0U);

    // 3. A frame that names no target is the only kind it has, and it is a whole frame: recorded, submitted, and
    //    never presented.
    backend->executeFrame(vn::graphics::FrameDescription{});
    EXPECT_EQ(backend->framesPresented(), 0U);
    EXPECT_EQ(backend->windowCount(), 0U);
    EXPECT_EQ(backend->diagnosticCount(), 0U);
    const vn::graphics::FrameReport report = backend->frameReport();
    ASSERT_TRUE(report.progress.has_value()) << "the device's frame clock answers it";
    EXPECT_EQ(report.progress->submitted, 1U) << "the frame really reached the queue";
    for (int index = 0; index < 2; ++index)
    {
        backend->executeFrame(vn::graphics::FrameDescription{});
    }
    EXPECT_EQ(backend->framesPresented(), 0U);

    backend->shutdown();
    EXPECT_FALSE(backend->initialized());
    backend->shutdown();  // idempotent, as for any other device
    EXPECT_EQ(reported.size(), backend->diagnosticCount());
}

TEST(VsgBackendTest, ADeviceCreatedOnAHandleWithoutOneIsRefusedWithItsReason)
{
    // A HANDLE IS A CREATION INPUT (see DeviceDesc): a device asked for the host's window cannot invent one, so an
    // empty handle is refused where it is stated - before a device is created, before a window of our own appears,
    // and with a sentence the host can act on.
    vn::graphics::DeviceDesc desc;
    desc.presentation = vn::graphics::DevicePresentation::Handle;
    vn::intrusive_ptr<VsgBackend> backend(new VsgBackend(desc));
    std::vector<vn::graphics::RenderDiagnostic> reported;
    backend->setDiagnosticSink([&reported](const vn::graphics::RenderDiagnostic& diagnostic) {
        reported.push_back(diagnostic);
    });

    EXPECT_TRUE(backend->capabilities().presentation) << "it was created TO present";
    EXPECT_FALSE(backend->initialize());
    EXPECT_FALSE(backend->initialized());
    ASSERT_EQ(backend->diagnosticCount(vn::graphics::DiagnosticCategory::InitFailed), 1U);
    ASSERT_EQ(reported.size(), 1U);
    EXPECT_EQ(reported.front().severity, vn::graphics::DiagnosticSeverity::Error);
    EXPECT_FALSE(reported.front().message.empty()) << "false without a reason is not the SDK's contract";
    EXPECT_EQ(backend->windowCount(), 0U) << "no window of our own was made instead";
}

TEST(VsgBackendTest, AFrameThatNamesATargetTheDeviceDidNotHandOutIsRefusedOnce)
{
    if (!deviceCaseAvailable())
    {
        GTEST_SKIP() << "no device session that serves off-screen pixels on this machine";
    }

    // A frame's default framebuffer resolves to the target that frame names (RenderFrameExecutor::executeFrame),
    // and this device can only serve the window it handed out: a frame naming another device's target is wiring
    // this backend could not have produced, so nothing is recorded and the reason is said once per episode
    // instead of being drawn into someone else's window.
    class ForeignTarget : public vn::graphics::WindowPresentTarget
    {
      public:
        void setWindowHandle(void*) override {}
        [[nodiscard]] void* nativeHandle() const override { return nullptr; }
        void resize(int, int) override {}
        void present() override { ++presents; }

        int presents = 0;
    };

    vn::intrusive_ptr<VsgBackend> backend(new VsgBackend());
    std::vector<vn::graphics::RenderDiagnostic> reported;
    backend->setDiagnosticSink([&reported](const vn::graphics::RenderDiagnostic& diagnostic) {
        reported.push_back(diagnostic);
    });

    vn::graphics::SurfaceTargetResult why = vn::graphics::SurfaceTargetResult::Ok;
    vn::raw_ptr<vn::graphics::WindowPresentTarget> own = backend->createSurfaceTarget(nullptr, &why);
    ASSERT_NE(own, nullptr);
    ASSERT_TRUE(backend->initialize());
    EXPECT_EQ(backend->diagnosticCount(), 0U) << "a healthy session reports nothing";

    ForeignTarget foreign;
    backend->executeFrame(vn::graphics::FrameDescription{}, &foreign);
    ASSERT_EQ(backend->diagnosticCount(vn::graphics::DiagnosticCategory::UnsupportedRequest), 1U);
    ASSERT_EQ(reported.size(), 1U);
    EXPECT_EQ(reported.front().severity, vn::graphics::DiagnosticSeverity::Warning);
    EXPECT_EQ(foreign.presents, 0) << "the foreign surface was not presented through";

    backend->executeFrame(vn::graphics::FrameDescription{}, &foreign);
    EXPECT_EQ(backend->diagnosticCount(vn::graphics::DiagnosticCategory::UnsupportedRequest), 1U)
        << "one report per episode";

    // Its OWN target is served, and it is the one the frame is presented through.
    const std::uint64_t before = backend->framesPresented();
    backend->executeFrame(vn::graphics::FrameDescription{}, own);
    EXPECT_EQ(backend->framesPresented(), before + 1U);
    EXPECT_EQ(backend->windowCount(), 1U) << "the frame went through the one window this device serves";

    backend->shutdown();
}

// 设备槽位用尽是**应用真会撞上的一道边界**：一块渲染面 = 一次 attach = 一个设备（`RenderControl` 每开一个 3D 文档
// 就是一个），而本 build 的同时设备数是 vsg 的编译期常量（`VSG_MAX_DEVICES`）。vsg 那边用**抛异常**拒绝（`vsg::Exception`
// 不是 `std::exception`，所以只写 `catch (std::exception)` 是接不住的），于是这件事有两个门要看清：无窗的 seam
// （`createDevice`，无头相位与测试走它）和会话（应用走它）。这条用例把"两扇门都答成理由、而且理由说得出上限"钉住。
TEST(VsgBackendTest, ADeviceCapThatIsReachedIsAReasonAndNotAnException)
{
    if (!deviceCaseAvailable())
    {
        GTEST_SKIP() << "no device session that serves off-screen pixels on this machine";
    }

    const std::uint32_t cap = ::vsg::Device::maxNumDevices();
    ASSERT_GT(cap, 0U) << "a build that allows no device at all cannot run this suite";

    // 1. The window-less seam: fill this build's slots, then ask for one more.
    std::vector<vn::vsg::DeviceResult> held;
    held.reserve(cap);
    for (std::uint32_t index = 0; index < cap; ++index)
    {
        held.push_back(vn::vsg::createDevice());
        ASSERT_TRUE(held.back().ok) << "slot " << index << ": " << as_bytes(held.back().error);
    }

    const vn::vsg::DeviceResult refused = vn::vsg::createDevice();
    EXPECT_FALSE(refused.ok) << "the cap is reached: the seam answers, it does not throw";
    EXPECT_EQ(refused.device, nullptr) << "a refused creation hands out no device";
    const std::string seam_reason = as_bytes(refused.error);
    EXPECT_NE(seam_reason.find("at most"), std::string::npos) << seam_reason;
    EXPECT_NE(seam_reason.find(std::to_string(cap)), std::string::npos)
        << "the reason names the cap by vsg's own number (it stays true if the build changes it): " << seam_reason;

    // 2. The session door, which is the one a host meets: with every slot taken it fails like every other
    //    attach failure - false plus a reason (the control's Failed state and its failureReason), not an
    //    exception out of a Qt event loop.
    {
        vn::intrusive_ptr<VsgBackend>               backend(new VsgBackend());
        std::vector<vn::graphics::RenderDiagnostic> reported;
        backend->setDiagnosticSink([&reported](const vn::graphics::RenderDiagnostic& diagnostic) {
            reported.push_back(diagnostic);
        });

        EXPECT_FALSE(backend->initialize()) << "a session cannot come up without a device";
        EXPECT_FALSE(backend->initialized());
        ASSERT_FALSE(reported.empty()) << "the failure is reported, not swallowed";
        EXPECT_EQ(reported.back().severity, vn::graphics::DiagnosticSeverity::Error);
        EXPECT_EQ(reported.back().category, vn::graphics::DiagnosticCategory::InitFailed);
        const std::string session_reason = as_bytes(reported.back().message);
        EXPECT_NE(session_reason.find("at most"), std::string::npos) << session_reason;
        EXPECT_NE(session_reason.find(std::to_string(cap)), std::string::npos) << session_reason;
    }

    // 3. A slot comes back when a device goes: the refusal is about devices ALIVE, not about a counter that
    //    can only go up. (vsg reuses a released id, and the seam must not answer "cap reached" afterwards.)
    held.pop_back();
    const vn::vsg::DeviceResult reused = vn::vsg::createDevice();
    EXPECT_TRUE(reused.ok) << as_bytes(reused.error);
}