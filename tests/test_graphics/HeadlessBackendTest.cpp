#include <vine/graphics/Camera.hpp>
#include <vine/graphics/FrameDescription.hpp>
#include <vine/graphics/RenderBackend.hpp>
#include <vine/graphics/RenderDevice.hpp>
#include <vine/graphics/RenderDiagnostic.hpp>
#include <vine/graphics/RenderEngine.hpp>
#include <vine/graphics/RenderPass.hpp>
#include <vine/graphics/RenderSurface.hpp>
#include <vine/graphics/RenderSurfaceTarget.hpp>
#include <vine/graphics/RenderTarget.hpp>
#include <vine/graphics/Scene.hpp>
#include <vine/raw_ptr.hpp>

#include <gtest/gtest.h>

#include <cstddef>
#include <memory>
#include <vector>

using namespace vn::graphics;
using vn::intrusive_ptr;
using vn::raw_ptr;

namespace
{

/**
 * @brief A device with NO window half at all: what an offline bake / a display-less CI run looks like.
 *
 * It implements the device contract plus the frame contract and nothing else - no target to hand out, no size to
 * follow, nothing to present - because createSurfaceTarget() answers "no presentation" (the answer the base class
 * gives, which this double only counts). That this double COMPILES is half of what the split claims: the window
 * half is the DEVICE's to hand out, so a device that has none overrides nothing instead of spelling out four
 * methods it has nothing to say about.
 */
class HeadlessBackend : public RenderBackend
{
  public:
    bool initialize() override { return ok = true; }
    void shutdown() override { ok = false; }

    [[nodiscard]] RenderFrameExecutor& executor() override { return *this; }

    /// The answer that makes this device windowless, counted: it presents nothing, through anything.
    [[nodiscard]] raw_ptr<RenderSurfaceTarget> createSurfaceTarget(void* native_handle,
                                                                 SurfaceTargetResult* why = nullptr) override
    {
        ++targets_asked;
        return RenderDevice::createSurfaceTarget(native_handle, why);
    }

    void executeFrame(const FrameDescription& frame, RenderSurfaceTarget* present_to = nullptr) override
    {
        ++frames;
        present_targets.push_back(present_to);
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
    /// How many times the device was asked for a window half (see createSurfaceTarget).
    int targets_asked = 0;
    std::vector<raw_ptr<RenderTarget>> targets;
    std::vector<RenderSurfaceTarget*>  present_targets;  ///< What each frame named (see executeFrame).
    std::size_t                        draws = 0;
};

/**
 * @brief A device WITH a window half, the plugin's shape in miniature: it hands ITSELF out as the target.
 *
 * One window is served at a time (a window carries one live swapchain), so the second ask is the "occupied"
 * episode - which is what the real backend answers too, and what a host has to hear rather than discover as a
 * picture that never appears. A refusal is reported ONCE per episode; the first ask is answered silently.
 */
class WindowedBackend : public RenderBackend, public RenderSurfaceTarget
{
  public:
    bool initialize() override
    {
        ++initializes;
        return true;
    }
    void shutdown() override {}

    [[nodiscard]] RenderFrameExecutor& executor() override { return *this; }

    [[nodiscard]] raw_ptr<RenderSurfaceTarget> createSurfaceTarget(void* native_handle,
                                                                 SurfaceTargetResult* why = nullptr) override
    {
        ++target_creations;
        if (target_taken) {
            if (why != nullptr) {
                *why = SurfaceTargetResult::WindowOccupied;
            }
            if (occupied_reports.shouldReport()) {
                reportDiagnostic(DiagnosticSeverity::Warning, DiagnosticCategory::UnsupportedRequest,
                                 u8"a second window was asked of a device that serves one");
            }
            return nullptr;
        }
        target_taken   = true;
        adopted_handle = native_handle;
        if (why != nullptr) {
            *why = SurfaceTargetResult::Ok;
        }
        return this;
    }

    void executeFrame(const FrameDescription&, RenderSurfaceTarget* present_to = nullptr) override
    {
        ++frames;
        present_targets.push_back(present_to);
    }

    void setRenderTarget(RenderTarget*) override {}

    /// This device presents (it has a window half) and honours the targets a pass names.
    [[nodiscard]] Capabilities capabilities() const noexcept override
    {
        return Capabilities{ .render_targets = true, .presentation = true };
    }

    void setWindowHandle(void* handle) override { adopted_handle = handle; }
    [[nodiscard]] void* nativeHandle() const override { return adopted_handle; }
    void resize(int width, int height) override
    {
        ++resize_calls;
        last_width  = width;
        last_height = height;
    }
    void present() override { ++presents; }

    int   frames           = 0;
    int   presents         = 0;
    int   target_creations = 0;
    int   initializes      = 0;
    int   resize_calls     = 0;
    int   last_width       = 0;
    int   last_height      = 0;
    void* adopted_handle   = nullptr;
    bool  target_taken     = false;
    std::vector<RenderSurfaceTarget*> present_targets;  ///< What each frame named (see executeFrame).

  private:
    ReportOnce occupied_reports;
};

/**
 * @brief A device that serves SEVERAL windows: one target per window, made on demand, owned by the device.
 *
 * The target shape the contracts describe (Device : Target = 1 : N) in miniature: the device keeps what it made
 * (the areas BORROW their targets), each target remembers the window it was made for and the size it followed,
 * and the presents are counted per target - which is what makes "two areas, one device, one frame each, each
 * presented through ITS own window" checkable without a GPU.
 */
class FleetBackend : public RenderBackend
{
  public:
    class FleetTarget : public RenderSurfaceTarget
    {
      public:
        explicit FleetTarget(void* handle) : handle_(handle) {}

        void setWindowHandle(void* handle) override { handle_ = handle; }
        [[nodiscard]] void* nativeHandle() const override { return handle_; }
        void resize(int width, int height) override
        {
            width_  = width;
            height_ = height;
        }
        void present() override { ++presents; }

        int presents = 0;
        int width_   = 0;
        int height_  = 0;

      private:
        void* handle_ = nullptr;
    };

    bool initialize() override
    {
        ++initializes;
        return true;
    }
    void shutdown() override {}
    [[nodiscard]] RenderFrameExecutor& executor() override { return *this; }

    [[nodiscard]] raw_ptr<RenderSurfaceTarget> createSurfaceTarget(void* native_handle,
                                                                 SurfaceTargetResult* why = nullptr) override
    {
        targets.push_back(std::make_unique<FleetTarget>(native_handle));
        if (why != nullptr) {
            *why = SurfaceTargetResult::Ok;
        }
        return targets.back().get();
    }

    void executeFrame(const FrameDescription&, RenderSurfaceTarget* present_to = nullptr) override
    {
        ++frames;
        present_targets.push_back(present_to);
    }

    void setRenderTarget(RenderTarget*) override {}
    [[nodiscard]] Capabilities capabilities() const noexcept override
    {
        return Capabilities{ .render_targets = true, .presentation = true };
    }

    int                                       frames      = 0;
    int                                       initializes = 0;
    std::vector<std::unique_ptr<FleetTarget>> targets;  ///< What this device made (it owns them).
    std::vector<RenderSurfaceTarget*>         present_targets;
};

}  // namespace

/**
 * @brief A frame is complete without a target: a device that presents nothing is driven for whole frames.
 *
 * The window half is the device's to hand out (see RenderDevice::createSurfaceTarget), and "optional" has to mean
 * "absent": the engine asks the device for a target when its area holds none, the device answers "no presentation"
 * (silently - a display-less device is a first-class state, not a fault), and the frame that follows runs a pass
 * that draws into the off-screen target it names with nothing acquired and nothing presented.
 */
TEST(HeadlessBackendTest, AFrameIsCompleteWithoutATarget)
{
    auto backend = intrusive_ptr<HeadlessBackend>(new HeadlessBackend());
    auto engine  = intrusive_ptr<RenderEngine>(new RenderEngine());
    engine->setDevice(backend);
    auto area = intrusive_ptr<RenderSurface>(new RenderSurface(engine.get()));

    area->resize(320, 240);  // a size the passes lay themselves out on - not a surface to follow
    const int asks_before_attach = backend->targets_asked;
    ASSERT_TRUE(engine->initialize());
    EXPECT_EQ(backend->targets_asked, asks_before_attach + 1)
        << "the engine asked the device for a target of its own (the area held none)";
    EXPECT_FALSE(area->hasTarget()) << "this device makes none, so the area has nothing to present through";
    EXPECT_EQ(engine->frameContext().surface_width, 320)
        << "the size stays in the frame context: there is no window half to take it";

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

    engine->frame();

    EXPECT_EQ(backend->frames, 1);
    ASSERT_EQ(backend->targets.size(), 1u);
    EXPECT_EQ(backend->targets.front(), target.get()) << "the frame reached the target the pass names";
    EXPECT_EQ(backend->draws, 1u);
    ASSERT_EQ(backend->present_targets.size(), 1u);
    EXPECT_EQ(backend->present_targets.front(), nullptr)
        << "the frame named no target: there was nothing to resolve, acquire or present";
    EXPECT_EQ(engine->diagnosticCount(), 0u) << "a windowless device is not a problem";
    EXPECT_FALSE(engine->capabilities().presentation) << "the DEVICE answers it: nothing to present through";
}

/**
 * @brief A device that presents nothing answers "no presentation", and says nothing else.
 *
 * The machine-readable answer is what a host plans around (RenderDevice::createSurfaceTarget): it decides to draw
 * off-screen, or to bring up a device that can present - which is a HOST's decision, so the device does not spend
 * anybody's diagnostic budget on it. A window is not what a frame needs either: the same device is driven for
 * whole frames with the window it never got.
 */
TEST(HeadlessBackendTest, ADeviceThatPresentsNothingAnswersNoTargetSilently)
{
    auto backend = intrusive_ptr<HeadlessBackend>(new HeadlessBackend());
    auto engine  = intrusive_ptr<RenderEngine>(new RenderEngine());
    engine->setDevice(backend);

    std::vector<RenderDiagnostic> reported;
    engine->setDiagnosticSink([&reported](const RenderDiagnostic& diagnostic) { reported.push_back(diagnostic); });

    int   marker = 0;
    void* handle = &marker;

    SurfaceTargetResult why = SurfaceTargetResult::Ok;
    EXPECT_EQ(backend->createSurfaceTarget(handle, &why), nullptr) << "no window half to hand one out";
    EXPECT_EQ(why, SurfaceTargetResult::NoPresentation);
    EXPECT_EQ(engine->diagnosticCount(), 0u) << "a first-class state is answered, not reported";
    EXPECT_TRUE(reported.empty());

    ASSERT_TRUE(engine->initialize());
    engine->frame();
    EXPECT_EQ(backend->frames, 1) << "the frame is what the device is for: the window was never part of it";
    EXPECT_EQ(engine->diagnosticCount(), 0u);
}

/**
 * @brief A window that is already served is refused, with the reason, once.
 *
 * One window carries one live swapchain, so a device that already serves one answers the second ask with
 * WindowOccupied - and reports it, because a host that asked has to hear it rather than discover a picture that
 * never appears. It is an EPISODE: asking twice says it once, and the first ask says nothing at all.
 */
TEST(HeadlessBackendTest, AWindowAlreadyServedIsRefusedWithItsReasonOnce)
{
    auto backend = intrusive_ptr<WindowedBackend>(new WindowedBackend());
    auto engine  = intrusive_ptr<RenderEngine>(new RenderEngine());
    engine->setDevice(backend);

    std::vector<RenderDiagnostic> reported;
    engine->setDiagnosticSink([&reported](const RenderDiagnostic& diagnostic) { reported.push_back(diagnostic); });

    int                 first  = 0;
    int                 second = 0;
    SurfaceTargetResult why    = SurfaceTargetResult::Ok;
    EXPECT_NE(backend->createSurfaceTarget(&first, &why), nullptr);
    EXPECT_EQ(why, SurfaceTargetResult::Ok);
    EXPECT_EQ(backend->target_creations, 1);
    EXPECT_EQ(engine->diagnosticCount(), 0u) << "the first window is not an episode";

    EXPECT_EQ(backend->createSurfaceTarget(&second, &why), nullptr);
    EXPECT_EQ(why, SurfaceTargetResult::WindowOccupied);
    ASSERT_EQ(engine->diagnosticCount(DiagnosticCategory::UnsupportedRequest), 1u);
    ASSERT_EQ(reported.size(), 1u);
    EXPECT_EQ(reported.front().severity, DiagnosticSeverity::Warning);
    EXPECT_EQ(backend->adopted_handle, static_cast<void*>(&first)) << "the window it already serves was left alone";

    EXPECT_EQ(backend->createSurfaceTarget(&second, &why), nullptr);
    EXPECT_EQ(engine->diagnosticCount(DiagnosticCategory::UnsupportedRequest), 1u)
        << "one report per episode, however often the same ask comes";
}

/**
 * @brief A target the host made on its device reaches it, unreported, and every frame presents through it.
 *
 * The complement that makes the cases above mean something: the device is not refusing "a window", it is answering
 * "no presentation" or "already occupied". Where it does present, the host asks it for the target of the window it
 * has, the AREA holds it, the size follows it, and every frame the engine runs names it and is presented through
 * it once.
 */
TEST(HeadlessBackendTest, ATargetTheHostMadeReachesTheFrameUnreported)
{
    auto backend = intrusive_ptr<WindowedBackend>(new WindowedBackend());
    auto engine  = intrusive_ptr<RenderEngine>(new RenderEngine());
    engine->setDevice(backend);

    auto  area   = intrusive_ptr<RenderSurface>(new RenderSurface(engine.get()));
    int   marker = 0;
    void* handle = &marker;

    SurfaceTargetResult          why    = SurfaceTargetResult::Ok;
    raw_ptr<RenderSurfaceTarget> target = backend->createSurfaceTarget(handle, &why);
    ASSERT_NE(target, nullptr);
    EXPECT_EQ(why, SurfaceTargetResult::Ok);
    area->bindTarget(target);
    area->resize(640, 480);

    const int creations_before_attach = backend->target_creations;
    ASSERT_TRUE(engine->initialize());

    EXPECT_EQ(backend->target_creations, creations_before_attach)
        << "the area held a target: the device was not asked for a second one";
    EXPECT_EQ(backend->adopted_handle, handle) << "the window was a creation input, not a later announcement";
    EXPECT_EQ(target->nativeHandle(), handle);
    EXPECT_EQ(backend->last_width, 640);
    EXPECT_EQ(backend->last_height, 480) << "the size announced before initialize reaches the target that can take it";
    EXPECT_EQ(engine->diagnosticCount(), 0u);
    EXPECT_TRUE(engine->capabilities().presentation) << "this device CAN present: the value says so";

    engine->frame();
    engine->frame();
    EXPECT_EQ(backend->frames, 2);
    EXPECT_EQ(backend->presents, 2) << "one present per frame, through the area's target";
    ASSERT_EQ(backend->present_targets.size(), 2u);
    EXPECT_EQ(backend->present_targets.back(), target) << "the frame names the target the AREA holds";
}

/**
 * @brief A re-announced window MOVES the target; the device (and what was compiled on it) stays.
 *
 * The platform recreating the native window (a reparent, a screen change) is the same area on a new handle: the
 * host announces it ON THE TARGET (RenderSurfaceTarget::setWindowHandle), which is what moves an established
 * session onto the new window - not a second target, and not a rebuilt device.
 */
TEST(HeadlessBackendTest, AReAnnouncedWindowMovesTheTargetAndTheDeviceStays)
{
    auto backend = intrusive_ptr<WindowedBackend>(new WindowedBackend());
    auto engine  = intrusive_ptr<RenderEngine>(new RenderEngine());
    engine->setDevice(backend);
    auto area = intrusive_ptr<RenderSurface>(new RenderSurface(engine.get()));
    area->resize(320, 240);

    auto pass = intrusive_ptr<RenderPass>(new RenderPass());
    engine->addPass(pass, 42);

    int first  = 0;
    int second = 0;
    area->bindTarget(backend->createSurfaceTarget(&first));
    ASSERT_TRUE(engine->initialize());
    engine->frame();
    EXPECT_EQ(backend->presents, 1);

    // The window the platform shows now is another one: the same target is re-announced (rule 5 of the axis).
    area->target()->setWindowHandle(&second);
    ASSERT_TRUE(engine->initialize());  // the host re-attaches - the rule for a recreated window
    engine->frame();

    EXPECT_EQ(backend->nativeHandle(), static_cast<void*>(&second)) << "the target moved onto the new window";
    EXPECT_EQ(backend->target_creations, 1) << "a re-announced window is not a second target";
    EXPECT_EQ(backend->presents, 2) << "the same device keeps presenting";
    EXPECT_EQ(backend->initializes, 2) << "the device was re-initialized, not replaced";
    EXPECT_EQ(engine->passCount(), 1u) << "what was registered on the device is still registered";
    EXPECT_EQ(engine->device(), backend.get()) << "the device identity: the same object, moved onto the new window";
    EXPECT_EQ(engine->diagnosticCount(), 0u);
}

/**
 * @brief ONE device serves several areas, one window each: the frames do not cross.
 *
 * Device : Target is 1 : N and Device : Engine is 1 : N (the cardinalities this axis settles), so what has to hold
 * is that nothing crosses: each area holds ITS target, each frame names the target of ITS area, and the device that
 * made both is one object. What a shared device then compiles once and uploads once is the real-device battery's
 * question (the plugin's content world moves onto the device later); this is the wiring beneath it.
 */
TEST(HeadlessBackendTest, OneDeviceServesSeveralAreasWithoutCrossingThem)
{
    auto device = intrusive_ptr<FleetBackend>(new FleetBackend());

    auto engine_a = intrusive_ptr<RenderEngine>(new RenderEngine());
    auto engine_b = intrusive_ptr<RenderEngine>(new RenderEngine());
    engine_a->setDevice(device);
    engine_b->setDevice(device);

    auto area_a = intrusive_ptr<RenderSurface>(new RenderSurface(engine_a.get()));
    auto area_b = intrusive_ptr<RenderSurface>(new RenderSurface(engine_b.get()));

    int handle_a = 0;
    int handle_b = 0;
    area_a->bindTarget(device->createSurfaceTarget(&handle_a));
    area_b->bindTarget(device->createSurfaceTarget(&handle_b));
    ASSERT_NE(area_a->target(), nullptr);
    ASSERT_NE(area_b->target(), nullptr);
    EXPECT_NE(area_a->target(), area_b->target()) << "one target per window, both made by the same device";

    ASSERT_TRUE(engine_a->initialize());
    ASSERT_TRUE(engine_b->initialize());
    area_a->resize(320, 200);
    area_b->resize(640, 400);

    engine_a->frame();
    engine_b->frame();

    EXPECT_EQ(device->targets.size(), 2u) << "the device made both targets and owns them (Device : Target = 1 : N)";
    ASSERT_EQ(device->present_targets.size(), 2u);
    EXPECT_EQ(device->present_targets[0], area_a->target()) << "each frame resolves to ITS area's target";
    EXPECT_EQ(device->present_targets[1], area_b->target());
    EXPECT_EQ(device->targets[0]->width_, 320);
    EXPECT_EQ(device->targets[1]->width_, 640) << "and each target followed the size of its own area";
    EXPECT_EQ(device->targets[0]->presents, 1);
    EXPECT_EQ(device->targets[1]->presents, 1);
    EXPECT_EQ(engine_a->diagnosticCount() + engine_b->diagnosticCount(), 0u);
}

/**
 * @brief The engine asks the device for a target only when the area holds none - never over a host's.
 *
 * A host that bound the target of its window is not overridden by the frame path, and the window it bound is what
 * the session comes up on, however late that announcement was made (the window comes first: an application opens
 * its window and then picks what renders into it).
 */
TEST(HeadlessBackendTest, TheDeviceIsAskedForItsOwnTargetOnlyWhenTheAreaHoldsNone)
{
    auto engine = intrusive_ptr<RenderEngine>(new RenderEngine());
    auto area   = intrusive_ptr<RenderSurface>(new RenderSurface(engine.get()));
    area->resize(800, 600);

    auto backend = intrusive_ptr<WindowedBackend>(new WindowedBackend());
    engine->setDevice(backend);

    int first  = 0;
    int second = 0;
    area->bindTarget(backend->createSurfaceTarget(&first));
    ASSERT_NE(area->target(), nullptr);
    area->target()->setWindowHandle(&second);  // the host follows a window the platform recreated before the attach

    ASSERT_TRUE(engine->initialize());
    EXPECT_EQ(backend->target_creations, 1) << "the area held a target: the device was not asked for another";
    EXPECT_EQ(backend->nativeHandle(), static_cast<void*>(&second))
        << "the attach hands over what the TARGET says now, never a copy of the engine's";
    EXPECT_EQ(backend->last_width, 800);
    EXPECT_EQ(backend->last_height, 600);
}
