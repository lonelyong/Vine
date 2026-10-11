#include <vine/graphics/Camera.hpp>
#include <vine/graphics/FrameDescription.hpp>
#include <vine/graphics/RenderBackend.hpp>
#include <vine/graphics/RenderDiagnostic.hpp>
#include <vine/graphics/RenderEngine.hpp>
#include <vine/graphics/RenderPass.hpp>
#include <vine/graphics/RenderTarget.hpp>
#include <vine/graphics/Scene.hpp>
#include <vine/raw_ptr.hpp>

#include <gtest/gtest.h>

#include <cstddef>
#include <vector>

using namespace vn::graphics;
using vn::intrusive_ptr;
using vn::raw_ptr;

namespace
{

/**
 * @brief A backend with NO window half at all: what an offline bake / a display-less CI run looks like.
 *
 * It implements the frame contract and nothing else - no window handle, no size to follow, nothing to present -
 * because `surface()` answers null. That this double COMPILES is half of what the split claims: before it, the
 * four window entries were pure virtual on RenderBackend, so a backend like this had to spell out four methods it
 * had nothing to say about (the "empty shells" the design note calls out).
 */
class HeadlessBackend : public RenderBackend
{
  public:
    bool initialize() override { return ok = true; }
    void shutdown() override { ok = false; }

    /// The answer that makes this backend windowless: it presents nothing, through anything.
    [[nodiscard]] RenderSurfaceBinding* surface() override
    {
        ++surface_asked;
        return nullptr;
    }

    void executeFrame(const FrameDescription& frame) override
    {
        ++frames;
        for (const FrameDescription::Pass& entry : frame.passes) {
            if (!entry.description.has_value()) {
                continue;  // a pass that ran and issued nothing
            }
            targets.push_back(entry.description->target);
            draws += entry.description->draws.size();
        }
    }

    void setRenderTarget(RenderTarget*) override {}

    bool supportsRenderTargets() override { return true; }

    bool ok = false;
    int  frames = 0;
    /// How many times the engine asked for the window half (see RenderEngine::frame).
    int surface_asked = 0;
    std::vector<raw_ptr<RenderTarget>> targets;
    std::size_t                        draws = 0;
};

/// A backend WITH a window half, the plugin's shape in miniature: the frame contract plus the surface binding,
/// the backend itself being the binding.
class WindowedBackend : public RenderBackend, public RenderSurfaceBinding
{
  public:
    bool initialize() override { return true; }
    void shutdown() override {}

    [[nodiscard]] RenderSurfaceBinding* surface() override { return this; }

    void executeFrame(const FrameDescription&) override { ++frames; }

    void setRenderTarget(RenderTarget*) override {}
    bool supportsRenderTargets() override { return true; }

    void setWindowHandle(void* handle) override { adopted_handle = handle; }
    [[nodiscard]] void* nativeHandle() const override { return adopted_handle; }
    void resize(int width, int height) override
    {
        last_width  = width;
        last_height = height;
    }
    void present() override { ++presents; }

    int   frames         = 0;
    int   presents       = 0;
    int   last_width     = 0;
    int   last_height    = 0;
    void* adopted_handle = nullptr;
};

}  // namespace

/**
 * @brief A frame is complete without a surface: a windowless backend is driven for whole frames.
 *
 * The window half of a backend is optional (see RenderBackend::surface), and "optional" has to mean "absent":
 * a backend that only draws into off-screen targets runs a frame whose pass reaches the target it names, with
 * nothing presented - and nothing reported, because a host with no window is not a fault.
 *
 * What this case can check about the negative is the ASK, so it counts the asks: the size announced before the
 * session came up went through the window half (there is none, so it stayed in the engine's frame context), and
 * the frame asked once more - before presenting, which then had nothing to present with.
 */
TEST(HeadlessBackendTest, AFrameIsCompleteWithoutASurface)
{
    auto backend = intrusive_ptr<HeadlessBackend>(new HeadlessBackend());
    auto engine  = intrusive_ptr<RenderEngine>(new RenderEngine());
    engine->setBackend(backend);

    engine->resize(320, 240);  // a size the passes lay themselves out on - not a surface to follow
    ASSERT_TRUE(engine->initialize());
    EXPECT_EQ(backend->surface_asked, 1)
        << "the announced size was handed to the window half (there is none: it stays in the frame context)";

    auto target = intrusive_ptr<RenderTarget>(new RenderTarget());
    target->attachColor(RenderTarget::ColorFormat::RGBA8);
    target->setSize(320, 240);

    auto scene  = intrusive_ptr<Scene>(new Scene());
    auto camera = intrusive_ptr<Camera>(new Camera());
    camera->setAspect(320.0 / 240.0);  // shaped for the rectangle it draws into: nothing to report about the picture
    auto pass = intrusive_ptr<RenderPass>(new RenderPass());
    pass->setCamera(camera);
    pass->setRenderTarget(target);
    engine->addPass(pass, scene, 0);

    const int asks_before_frame = backend->surface_asked;
    engine->frame();

    EXPECT_EQ(backend->frames, 1);
    ASSERT_EQ(backend->targets.size(), 1u);
    EXPECT_EQ(backend->targets.front(), target.get()) << "the frame reached the target the pass names";
    EXPECT_EQ(backend->draws, 1u);
    EXPECT_EQ(backend->surface_asked, asks_before_frame + 1)
        << "the window half was asked for before presenting - and answered null, so nothing was presented";
    EXPECT_EQ(engine->diagnosticCount(), 0u) << "a windowless host is not a problem";
}

/**
 * @brief A window announced to a windowless backend is refused LOUDLY, not dropped.
 *
 * The one wiring this split can make unsatisfiable is a host handing a window to a backend that has no window
 * half - the handle has nowhere to go. Saying nothing would look exactly like "attached" (the SDK's standing rule:
 * report what could not be done instead of degrading silently), so the engine reports it and the frame that
 * follows is still the frame - a window is not what a frame needs.
 */
TEST(HeadlessBackendTest, AWindowAnnouncedToAWindowlessBackendIsReported)
{
    auto backend = intrusive_ptr<HeadlessBackend>(new HeadlessBackend());
    auto engine  = intrusive_ptr<RenderEngine>(new RenderEngine());
    engine->setBackend(backend);

    std::vector<RenderDiagnostic> reported;
    engine->setDiagnosticSink([&reported](const RenderDiagnostic& diagnostic) { reported.push_back(diagnostic); });

    int   marker = 0;
    void* handle = &marker;
    engine->setWindowHandle(handle);
    ASSERT_TRUE(engine->initialize());

    ASSERT_EQ(engine->diagnosticCount(DiagnosticCategory::PassProtocolViolation), 1u);
    ASSERT_EQ(reported.size(), 1u);
    EXPECT_EQ(reported.front().severity, DiagnosticSeverity::Warning);
    EXPECT_EQ(reported.front().category, DiagnosticCategory::PassProtocolViolation);

    engine->frame();
    EXPECT_EQ(backend->frames, 1) << "the frame is what the backend is for: the window was never part of it";
}

/**
 * @brief The same announcements on a backend WITH a window half reach it, silently, and a frame presents.
 *
 * The complement that makes the case above mean something: the engine is not reporting "a window was announced",
 * it is reporting "there is no window half to give it to". Where there IS one, the handle arrives before the
 * session comes up, the size follows live, and every frame the engine runs is presented once.
 */
TEST(HeadlessBackendTest, AWindowAnnouncedToAWindowedBackendReachesItUnreported)
{
    auto backend = intrusive_ptr<WindowedBackend>(new WindowedBackend());
    auto engine  = intrusive_ptr<RenderEngine>(new RenderEngine());
    engine->setBackend(backend);

    int   marker = 0;
    void* handle = &marker;
    engine->setWindowHandle(handle);
    engine->resize(640, 480);
    ASSERT_TRUE(engine->initialize());

    EXPECT_EQ(backend->adopted_handle, handle) << "the handle reached the window half before the session came up";
    EXPECT_EQ(backend->nativeHandle(), handle);
    EXPECT_EQ(backend->last_width, 640);
    EXPECT_EQ(backend->last_height, 480) << "the size announced before initialize is handed over once it can be honoured";
    EXPECT_EQ(engine->diagnosticCount(), 0u);

    engine->frame();
    engine->frame();
    EXPECT_EQ(backend->frames, 2);
    EXPECT_EQ(backend->presents, 2) << "one present per frame, driven by the engine";
}
