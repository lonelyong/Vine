#include <vine/graphics/Camera.hpp>
#include <vine/graphics/Capabilities.hpp>
#include <vine/graphics/FrameDescription.hpp>
#include <vine/graphics/PassDesc.hpp>
#include <vine/graphics/PresentTarget.hpp>
#include <vine/graphics/RenderBackend.hpp>
#include <vine/graphics/RenderDevice.hpp>
#include <vine/graphics/RenderEngine.hpp>
#include <vine/graphics/RenderPass.hpp>
#include <vine/graphics/RenderTarget.hpp>
#include <vine/graphics/WindowPresentTarget.hpp>

#include <vine/intrusive_ptr.hpp>
#include <vine/raw_ptr.hpp>

#include <gtest/gtest.h>

#include <cstddef>
#include <vector>

using namespace vn::graphics;
using vn::intrusive_ptr;
using vn::raw_ptr;
using vn::String;

namespace
{

/**
 * @brief A device with no window half: enough to drive a frame, nothing to present.
 *
 * The scheduler is the ENGINE's, so these cases need a frame to compile and no pixels: this double counts frames and
 * honours every target it is handed.
 */
class HeadlessBackend : public RenderBackend {
  public:
    bool initialize() override
    {
        return up = true;
    }

    void shutdown() override
    {
        up = false;
    }

    [[nodiscard]] bool initialized() const noexcept override
    {
        return up;
    }

    [[nodiscard]] RenderFrameExecutor& executor() override
    {
        return *this;
    }

    [[nodiscard]] raw_ptr<WindowPresentTarget> createSurfaceTarget(void* native_handle, SurfaceTargetResult* why = nullptr) override
    {
        return RenderDevice::createSurfaceTarget(native_handle, why);
    }

    void executeFrame(const FrameDescription& frame, PresentTarget* present_to = nullptr) override
    {
        ++frames;
        last = &frame;
        if (present_to != nullptr) {
            present_to->present();
        }
    }

    void setRenderTarget(RenderTarget*) override
    {}

    [[nodiscard]] Capabilities capabilities() const noexcept override
    {
        return Capabilities{ .render_targets = true };
    }

    bool                            up     = true;
    int                             frames = 0;
    raw_ptr<const FrameDescription> last   = nullptr;
};

/** @brief A scene declaration that draws into @p target at an explicit order. */
PassDesc writingPass(const char8_t* name, intrusive_ptr<RenderTarget> target, int order, intrusive_ptr<Camera> camera)
{
    PassDesc desc;
    desc.name          = name;
    desc.tag           = u8"Test";
    desc.stage         = PassStage::Shading;
    desc.order         = order;
    desc.kind          = PassKind::Scene;
    desc.camera        = std::move(camera);
    desc.writes        = std::move(target);
    desc.clear_enabled = true;
    return desc;
}

/** @brief A fresh colour target of one attachment. */
intrusive_ptr<RenderTarget> colourTarget()
{
    auto target = vn::make_intrusive<RenderTarget>();
    target->setSize(64, 64);
    target->attachColor(RenderTarget::ColorFormat::RGBA8);
    return target;
}

} // namespace

/*
 * @brief Without a dependency, the frame keeps the announced order (the engine's own stacking rule).
 */
TEST(FrameScheduleTest, PassesWithoutDependenciesKeepTheAnnouncedOrder)
{
    auto engine  = intrusive_ptr<RenderEngine>(new RenderEngine());
    auto backend = intrusive_ptr<HeadlessBackend>(new HeadlessBackend());
    engine->setDevice(backend);
    ASSERT_TRUE(engine->initialize());

    auto camera = vn::make_intrusive<Camera>();

    engine->addPass(writingPass(u8"late", colourTarget(), 10, camera));
    engine->addPass(writingPass(u8"early", colourTarget(), -5, camera));
    engine->frame();

    const FrameDescription& frame = engine->frameDescription();
    ASSERT_EQ(frame.passes.size(), 2u);
    EXPECT_EQ(frame.passes[0].pass->name(), u8"early");
    EXPECT_EQ(frame.passes[1].pass->name(), u8"late");
}

/*
 * @brief A consumer announced BEFORE its producer still runs after it - and reads what the producer writes.
 *
 * This is the whole reason the order is not the announcement: the producer is announced later and stacked later,
 * and the edge is what puts it first.
 */
TEST(FrameScheduleTest, AProducerRunsBeforeAConsumerAnnouncedEarlier)
{
    auto engine  = intrusive_ptr<RenderEngine>(new RenderEngine());
    auto backend = intrusive_ptr<HeadlessBackend>(new HeadlessBackend());
    engine->setDevice(backend);
    ASSERT_TRUE(engine->initialize());

    auto camera = vn::make_intrusive<Camera>();
    auto source = colourTarget();

    PassDesc consumer      = writingPass(u8"consumer", colourTarget(), -100, camera);
    consumer.input_targets = { source };
    engine->addPass(consumer); // announced first, stacked first
    engine->addPass(writingPass(u8"producer", source, 100, camera));

    engine->frame();

    const FrameDescription& frame = engine->frameDescription();
    ASSERT_EQ(frame.passes.size(), 2u);
    EXPECT_EQ(frame.passes[0].pass->name(), u8"producer") << "the edge beats the announced order";
    EXPECT_EQ(frame.passes[1].pass->name(), u8"consumer");
    ASSERT_EQ(frame.passes[1].inputs.size(), 1u);
    EXPECT_EQ(frame.passes[1].inputs[0].target, source.get()) << "the consumer resolves the producer's target, not an unproduced input";
}

/*
 * @brief A cycle is a declaration no order can satisfy: its whole component is skipped, the rest still runs.
 */
TEST(FrameScheduleTest, ACycleIsSkippedAndTheRestOfTheFrameStillRuns)
{
    auto engine  = intrusive_ptr<RenderEngine>(new RenderEngine());
    auto backend = intrusive_ptr<HeadlessBackend>(new HeadlessBackend());
    engine->setDevice(backend);
    ASSERT_TRUE(engine->initialize());

    auto camera = vn::make_intrusive<Camera>();
    auto first  = colourTarget();
    auto second = colourTarget();

    PassDesc a      = writingPass(u8"a", first, -10, camera);
    PassDesc b      = writingPass(u8"b", second, 0, camera);
    PassDesc alone  = writingPass(u8"alone", colourTarget(), 20, camera);
    a.input_targets = { second };
    b.input_targets = { first };
    engine->addPass(a);
    engine->addPass(b);
    engine->addPass(alone);

    engine->frame();

    const FrameDescription& frame = engine->frameDescription();
    ASSERT_EQ(frame.passes.size(), 1u) << "only the pass outside the cycle runs";
    EXPECT_EQ(frame.passes[0].pass->name(), u8"alone");
    EXPECT_EQ(engine->diagnosticCount(DiagnosticCategory::ContentSkipped), 1u) << "the skipped component is reported once per episode";
}

/*
 * @brief The two reports say different things on purpose: the PLAN is the declaration order, the FRAME is the
 * execution order, and the difference is exactly the scheduler's own answer.
 *
 * A report that "the frame plan IS the execution order" would be the same mistake the retired backend graph
 * made from the other side - two tables, one picture. This case pins the split: same engine, same frame, two
 * orders, and each one named after the question it answers.
 */
TEST(FrameScheduleTest, ThePlanKeepsTheDeclarationOrderAndTheFrameTheExecutionOrder)
{
    auto engine  = intrusive_ptr<RenderEngine>(new RenderEngine());
    auto backend = intrusive_ptr<HeadlessBackend>(new HeadlessBackend());
    engine->setDevice(backend);
    ASSERT_TRUE(engine->initialize());

    auto camera = vn::make_intrusive<Camera>();
    auto source = colourTarget();

    // Declared: the consumer at -10, the producer it reads at 50 - so the declaration order puts the reader
    // first, and only the edge can move the writer ahead of it.
    PassDesc consumer      = writingPass(u8"consumer", colourTarget(), -10, camera);
    consumer.input_targets = { source };
    engine->addPass(consumer);
    engine->addPass(writingPass(u8"producer", source, 50, camera));
    engine->frame();

    const FramePlan plan = engine->framePlan();
    ASSERT_EQ(plan.passes.size(), 2u);
    EXPECT_EQ(plan.passes[0].name, u8"consumer") << "the plan answers 'what did the host declare', in order";
    EXPECT_EQ(plan.passes[0].order, -10);
    EXPECT_EQ(plan.passes[1].name, u8"producer");
    EXPECT_EQ(plan.passes[1].order, 50);

    const FrameDescription& frame = engine->frameDescription();
    ASSERT_EQ(frame.passes.size(), 2u);
    EXPECT_EQ(frame.passes[0].pass->name(), u8"producer") << "the frame answers 'what runs, in what order'";
    EXPECT_EQ(frame.passes[1].pass->name(), u8"consumer");
}

/*
 * @brief The sample count is the TARGET's property, and the frame's description carries it to the backend.
 *
 * The count decides how many samples the target's images hold and which pipeline the API accepts for it, so
 * it has to travel with the target rather than with a pass or a pipeline: a pass draws INTO a target, and two
 * passes on targets of different counts are two pipelines of one pass kind. This case pins the SDK half -
 * the round trip, the "0 means one" rule, and the value reaching the target's shape.
 */
TEST(FrameScheduleTest, ATargetsSampleCountTravelsWithItAndDefaultsToOne)
{
    auto engine  = intrusive_ptr<RenderEngine>(new RenderEngine());
    auto backend = intrusive_ptr<HeadlessBackend>(new HeadlessBackend());
    engine->setDevice(backend);
    ASSERT_TRUE(engine->initialize());

    auto target = colourTarget();
    EXPECT_EQ(target->samples(), 1u) << "a target nobody configured is single-sample: the default cannot be 0";

    target->setSamples(4);
    EXPECT_EQ(target->samples(), 4u);

    target->setSamples(0);
    EXPECT_EQ(target->samples(), 1u) << "0 means one sample - there is no such thing as a sample-less target";

    target->setSamples(4);
    auto camera = vn::make_intrusive<Camera>();
    engine->addPass(writingPass(u8"msaa", target, 0, camera));
    engine->frame();

    // The count stays the TARGET's through a frame: nothing in the schedule copies it into a pass or a
    // pipeline, so there is exactly one place it can be wrong - and it is the place the host set.
    const FrameDescription& frame = engine->frameDescription();
    ASSERT_EQ(frame.passes.size(), 1u);
    EXPECT_EQ(target->samples(), 4u) << "a frame does not consume the count: the target keeps saying it";
}
