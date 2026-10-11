#include <vine/graphics/Camera.hpp>
#include <vine/graphics/FrameDescription.hpp>
#include <vine/graphics/RenderBackend.hpp>
#include <vine/graphics/RenderDiagnostic.hpp>
#include <vine/graphics/RenderEngine.hpp>
#include <vine/graphics/RenderPass.hpp>
#include <vine/graphics/RenderSurface.hpp>
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

    /// Answers the capability the frame path is driven by: this double honours every target it is handed.
    [[nodiscard]] Capabilities capabilities() const noexcept override { return Capabilities{ .render_targets = true }; }

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

    /// Same capability as the windowless double: a target named by a pass is drawn into (see the frame case).
    [[nodiscard]] Capabilities capabilities() const noexcept override { return Capabilities{ .render_targets = true }; }

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
 * What this case can check about the negative is the ASK, so it counts the asks: the size the AREA announced
 * before the session came up went through the window half (there is none, so it stayed in the frame context),
 * and the frame asked once more - before presenting, which then had nothing to present with.
 */
TEST(HeadlessBackendTest, AFrameIsCompleteWithoutASurface)
{
    auto backend = intrusive_ptr<HeadlessBackend>(new HeadlessBackend());
    auto engine  = intrusive_ptr<RenderEngine>(new RenderEngine());
    engine->setBackend(backend);
    auto area = intrusive_ptr<RenderSurface>(new RenderSurface(engine.get()));

    area->resize(320, 240);  // a size the passes lay themselves out on - not a surface to follow
    const int asks_before_attach = backend->surface_asked;  // asked once when the backend was set (see capabilities)
    ASSERT_TRUE(engine->initialize());
    EXPECT_EQ(backend->surface_asked, asks_before_attach + 1)
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
    EXPECT_FALSE(engine->capabilities().presentation) << "no window half, nothing to present through: the value says so";
}

/**
 * @brief A window bound to a windowless backend is REFUSED, loudly, once.
 *
 * The one wiring this split can make unsatisfiable is a host binding a window to a backend that has no window
 * half - the handle has nowhere to go (see RenderSurface::bindTarget). Saying nothing would look exactly like
 * "bound" (the SDK's standing rule: report what could not be done instead of degrading silently), so the bind
 * reports it once per episode - and the frame that follows is still the frame, because a window is not what a
 * frame needs. The announcement stays on the AREA (the window is the host's statement, and a session that CAN
 * present takes it: see RenderEngine::initialize).
 */
TEST(HeadlessBackendTest, BindingATargetOnAWindowlessBackendIsRefusedOnce)
{
    auto backend = intrusive_ptr<HeadlessBackend>(new HeadlessBackend());
    auto engine  = intrusive_ptr<RenderEngine>(new RenderEngine());
    engine->setBackend(backend);

    std::vector<RenderDiagnostic> reported;
    engine->setDiagnosticSink([&reported](const RenderDiagnostic& diagnostic) { reported.push_back(diagnostic); });

    auto  area   = intrusive_ptr<RenderSurface>(new RenderSurface(engine.get()));
    int   marker = 0;
    void* handle = &marker;
    EXPECT_FALSE(area->bindTarget(handle)) << "no window half to take it: the bind is refused, not accepted";
    ASSERT_EQ(engine->diagnosticCount(DiagnosticCategory::PassProtocolViolation), 1u);
    ASSERT_EQ(reported.size(), 1u);
    EXPECT_EQ(reported.front().severity, DiagnosticSeverity::Warning);
    EXPECT_EQ(reported.front().category, DiagnosticCategory::PassProtocolViolation);

    // The same session, asked again: one report per episode, and the announcement stays on the area.
    EXPECT_FALSE(area->bindTarget(handle));
    EXPECT_EQ(engine->diagnosticCount(DiagnosticCategory::PassProtocolViolation), 1u);
    EXPECT_EQ(area->targetHandle(), handle) << "the AREA remembers the window the host bound";
    EXPECT_TRUE(area->hasTarget());

    // Attaching replays what the area holds - already said, so the episode does not repeat - and the frame runs.
    ASSERT_TRUE(engine->initialize());
    EXPECT_EQ(engine->diagnosticCount(DiagnosticCategory::PassProtocolViolation), 1u);
    engine->frame();
    EXPECT_EQ(backend->frames, 1) << "the frame is what the backend is for: the window was never part of it";
}

/**
 * @brief The same announcements on a backend WITH a window half reach it, silently, and a frame presents.
 *
 * The complement that makes the case above mean something: the engine is not reporting "a window was announced",
 * it is reporting "there is no window half to give it to". Where there IS one, the handle arrives before the
 * session comes up, the size follows live, every frame the engine runs is presented once - and the capability
 * that answers "can this session show frames at all" says so.
 */
TEST(HeadlessBackendTest, ATargetBoundToAWindowedBackendReachesItUnreported)
{
    auto backend = intrusive_ptr<WindowedBackend>(new WindowedBackend());
    auto engine  = intrusive_ptr<RenderEngine>(new RenderEngine());
    engine->setBackend(backend);

    auto  area   = intrusive_ptr<RenderSurface>(new RenderSurface(engine.get()));
    int   marker = 0;
    void* handle = &marker;
    ASSERT_TRUE(area->bindTarget(handle));
    area->resize(640, 480);
    ASSERT_TRUE(engine->initialize());

    EXPECT_EQ(backend->adopted_handle, handle) << "the handle reached the window half before the session came up";
    EXPECT_EQ(backend->nativeHandle(), handle);
    EXPECT_EQ(backend->last_width, 640);
    EXPECT_EQ(backend->last_height, 480) << "the size announced before initialize is handed over once it can be honoured";
    EXPECT_EQ(engine->diagnosticCount(), 0u);
    EXPECT_TRUE(engine->capabilities().presentation) << "this session CAN present: the value says so";

    engine->frame();
    engine->frame();
    EXPECT_EQ(backend->frames, 2);
    EXPECT_EQ(backend->presents, 2) << "one present per frame, driven by the engine";
}

/**
 * @brief A re-announced handle MOVES the window; the device (and what was compiled on it) stays.
 *
 * The platform recreating the native window (a reparent, a screen change) is the same area on a new handle: the
 * announcement reaches the window half, which MOVES onto it rather than rebuilding (see
 * RenderSurfaceBinding::setWindowHandle), and the session the host re-attaches to is the one it had.
 */
TEST(HeadlessBackendTest, AReAnnouncedHandleMovesTheWindowAndTheDeviceStays)
{
    auto backend = intrusive_ptr<WindowedBackend>(new WindowedBackend());
    auto engine  = intrusive_ptr<RenderEngine>(new RenderEngine());
    engine->setBackend(backend);
    auto area = intrusive_ptr<RenderSurface>(new RenderSurface(engine.get()));
    area->resize(320, 240);

    auto pass = intrusive_ptr<RenderPass>(new RenderPass());
    engine->addPass(pass, 42);

    int first  = 0;
    int second = 0;
    ASSERT_TRUE(area->bindTarget(&first));
    ASSERT_TRUE(engine->initialize());
    engine->frame();
    EXPECT_EQ(backend->presents, 1);
    EXPECT_EQ(area->targetHandle(), static_cast<void*>(&first));

    // The window the platform shows now is another one: the same area re-announces, and the binding is moved.
    ASSERT_TRUE(area->bindTarget(&second));
    EXPECT_EQ(backend->adopted_handle, static_cast<void*>(&second)) << "the new handle reached the window half";
    EXPECT_EQ(area->targetHandle(), static_cast<void*>(&second));
    ASSERT_TRUE(engine->initialize());  // the host re-attaches - the rule for a recreated window
    engine->frame();

    EXPECT_EQ(backend->nativeHandle(), static_cast<void*>(&second))
        << "the session was moved onto the NEW window: what is handed over is what the AREA says now";
    EXPECT_EQ(backend->presents, 2) << "the same session keeps presenting";
    EXPECT_EQ(engine->passCount(), 1u) << "what was registered on the device is still registered";
    EXPECT_EQ(engine->backend(), backend.get()) << "the device identity: the same backend, moved onto the new window";
    EXPECT_EQ(engine->diagnosticCount(), 0u);
}

/**
 * @brief The session reads the AREA for the window to move onto - never a copy of the handle of its own.
 *
 * A window is announced before a backend exists (the window comes first: an application opens its window and then
 * picks what renders into it), and the handle may be replaced before that backend is attached. What the window
 * half is handed at an attach is therefore the area's CURRENT answer, not what was announced first: this is what
 * "the area owns the window" (RenderSurface::bindTarget) means for the engine, which holds no handle.
 */
TEST(HeadlessBackendTest, TheAttachHandsOverTheAreasCurrentWindow)
{
    auto engine = intrusive_ptr<RenderEngine>(new RenderEngine());
    auto area   = intrusive_ptr<RenderSurface>(new RenderSurface(engine.get()));
    area->resize(800, 600);

    int first  = 0;
    int second = 0;
    ASSERT_TRUE(area->bindTarget(&first)) << "no backend yet: the announcement is kept on the area";

    auto backend = intrusive_ptr<WindowedBackend>(new WindowedBackend());
    engine->setBackend(backend);
    ASSERT_TRUE(area->bindTarget(&second)) << "still no session: the area's answer is the new window";

    ASSERT_TRUE(engine->initialize());
    EXPECT_EQ(backend->adopted_handle, static_cast<void*>(&second))
        << "the attach handed over the area's CURRENT handle, not the one announced first";
    EXPECT_EQ(backend->last_width, 800);
    EXPECT_EQ(backend->last_height, 600);
}
