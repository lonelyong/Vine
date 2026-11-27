#include <vine/graphics/BuiltinShaders.hpp>
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
#include <vine/graphics/Texture.hpp>
#include <vine/graphics/WindowPresentTarget.hpp>

#include <vine/intrusive_ptr.hpp>
#include <vine/raw_ptr.hpp>

#include <gtest/gtest.h>

#include <cstddef>
#include <vector>

#include "PlanTestSupport.hpp"

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

    PresentOutcome executeFrame(const FrameDescription& frame, PresentTarget* present_to = nullptr) override
    {
        ++frames;
        last = &frame;
        return present_to != nullptr ? present_to->present() : PresentOutcome::Skipped;
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
    desc.stage         = PassStage::Shading;
    desc.order         = order;
    desc.kind          = DrawKind::Scene;
    desc.camera        = std::move(camera);
    desc.content       = vn::test::emptySceneForTest();
    desc.target        = vn::graphics::Target::explicitTarget(std::move(target));
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
TEST(PassScheduleTest, PassesWithoutDependenciesKeepTheAnnouncedOrder)
{
    auto engine  = intrusive_ptr<RenderEngine>(new RenderEngine());
    auto backend = intrusive_ptr<HeadlessBackend>(new HeadlessBackend());
    engine->setDevice(backend);
    ASSERT_TRUE(engine->initialize());

    auto camera = vn::make_intrusive<Camera>();

    vn::test::registerPassForTest(*engine, writingPass(u8"late", colourTarget(), 10, camera));
    vn::test::registerPassForTest(*engine, writingPass(u8"early", colourTarget(), -5, camera));
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
TEST(PassScheduleTest, AProducerRunsBeforeAConsumerAnnouncedEarlier)
{
    auto engine  = intrusive_ptr<RenderEngine>(new RenderEngine());
    auto backend = intrusive_ptr<HeadlessBackend>(new HeadlessBackend());
    engine->setDevice(backend);
    ASSERT_TRUE(engine->initialize());

    auto camera = vn::make_intrusive<Camera>();
    auto source = colourTarget();

    PassDesc consumer = writingPass(u8"consumer", colourTarget(), -100, camera);
    consumer.inputs   = { ImageInput::byTarget(source) };
    vn::test::registerPassForTest(*engine, consumer); // announced first, stacked first
    vn::test::registerPassForTest(*engine, writingPass(u8"producer", source, 100, camera));

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
TEST(PassScheduleTest, ACycleIsSkippedAndTheRestOfTheFrameStillRuns)
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
    a.inputs        = { ImageInput::byTarget(second) };
    b.inputs        = { ImageInput::byTarget(first) };
    vn::test::registerPassForTest(*engine, a);
    vn::test::registerPassForTest(*engine, b);
    vn::test::registerPassForTest(*engine, alone);

    engine->frame();

    const FrameDescription& frame = engine->frameDescription();
    ASSERT_EQ(frame.passes.size(), 1u) << "only the pass outside the cycle runs";
    EXPECT_EQ(frame.passes[0].pass->name(), u8"alone");
    EXPECT_EQ(engine->diagnosticCount(DiagnosticCategory::PassProtocolViolation), 1u)
        << "the skipped component is reported once per episode, as a PROTOCOL problem (a cycle is not missing content)";
}

/*
 * @brief The two reports say different things on purpose: the PLAN is the declaration order, the FRAME is the
 * execution order, and the difference is exactly the scheduler's own answer.
 *
 * A report that "the frame plan IS the execution order" would be the same mistake the retired backend graph
 * made from the other side - two tables, one picture. This case pins the split: same engine, same frame, two
 * orders, and each one named after the question it answers.
 */
TEST(PassScheduleTest, ThePlanKeepsTheDeclarationOrderAndTheFrameTheExecutionOrder)
{
    auto engine  = intrusive_ptr<RenderEngine>(new RenderEngine());
    auto backend = intrusive_ptr<HeadlessBackend>(new HeadlessBackend());
    engine->setDevice(backend);
    ASSERT_TRUE(engine->initialize());

    auto camera = vn::make_intrusive<Camera>();
    auto source = colourTarget();

    // Declared: the consumer at -10, the producer it reads at 50 - so the declaration order puts the reader
    // first, and only the edge can move the writer ahead of it.
    PassDesc consumer = writingPass(u8"consumer", colourTarget(), -10, camera);
    consumer.inputs   = { ImageInput::byTarget(source) };
    vn::test::registerPassForTest(*engine, consumer);
    vn::test::registerPassForTest(*engine, writingPass(u8"producer", source, 50, camera));
    engine->frame();

    const FrameTable plan = engine->frameTable();
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
TEST(PassScheduleTest, ATargetsSampleCountTravelsWithItAndDefaultsToOne)
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
    vn::test::registerPassForTest(*engine, writingPass(u8"msaa", target, 0, camera));
    engine->frame();

    // The count stays the TARGET's through a frame: nothing in the schedule copies it into a pass or a
    // pipeline, so there is exactly one place it can be wrong - and it is the place the host set.
    const FrameDescription& frame = engine->frameDescription();
    ASSERT_EQ(frame.passes.size(), 1u);
    EXPECT_EQ(target->samples(), 4u) << "a frame does not consume the count: the target keeps saying it";
}

/*
 * @brief A pass with no camera still CLEARS what it targets, so it is a producer: the consumer that samples that
 * target runs after it (review 19 Z11).
 *
 * The frame's schedule is decided before any pass is described, so "does this pass write its target" is asked of the
 * DECLARATION. The old answer here was "a clear is part of a draw, not a substitute for one", which let a composite
 * run before the clear it reads - and nothing was reported, because the other two answers (the produced table and the
 * wiring's filler check) already counted the camera-less pass' target as filled. The wrong picture was the only
 * symptom, which is why this case pins the ORDER and the resolved wire, not a sentence.
 */
TEST(PassScheduleTest, AClearOnlyPassRunsBeforeTheConsumerThatSamplesItsTarget)
{
    auto engine  = intrusive_ptr<RenderEngine>(new RenderEngine());
    auto backend = intrusive_ptr<HeadlessBackend>(new HeadlessBackend());
    engine->setDevice(backend);
    ASSERT_TRUE(engine->initialize());

    auto camera  = vn::make_intrusive<Camera>();
    auto cleared = colourTarget();

    // The reader is announced first AND stacks first: only the producer edge can put the clear ahead of it.
    PassDesc composite   = writingPass(u8"composite", colourTarget(), -100, camera);
    composite.kind       = DrawKind::Fullscreen;
    composite.program    = vn::graphics::fullscreenCopyProgram();
    composite.inputs     = { ImageInput::byTarget(cleared) };
    vn::test::registerPassForTest(*engine, composite);
    // The writer draws nothing at all: no camera, so its only effect is the clear it announces.
    vn::test::registerPassForTest(*engine, writingPass(u8"clear", cleared, 100, nullptr));

    engine->frame();

    const FrameDescription& frame = engine->frameDescription();
    ASSERT_EQ(frame.passes.size(), 2u);
    EXPECT_EQ(frame.passes[0].pass->name(), u8"clear") << "the clear is a write, so it is ordered first";
    EXPECT_EQ(frame.passes[1].pass->name(), u8"composite");
    ASSERT_TRUE(frame.passes[0].draw.has_value());
    EXPECT_TRUE(frame.passes[0].draw->packets.empty()) << "and the order came from the clear, not from a draw";
    EXPECT_TRUE(frame.passes[0].draw->attachment_policy.has_value());

    ASSERT_EQ(frame.passes[1].inputs.size(), 1u);
    EXPECT_EQ(frame.passes[1].inputs[0].target, cleared.get()) << "the composite samples what the clear wrote this frame";
}

/*
 * @brief ONE PREDICATE, checked against the announcement itself: `writesTarget()` equals "the entry this pass
 * announces changes its target", shape by shape (review 19 Z11).
 *
 * The question is asked twice by construction - of the declaration, because the order is decided before any
 * description exists, and of the value, which is what the backend records - so the two answers are compared here for
 * every shape a built-in kind can take. A drift between them is a wrong picture: the schedule orders (or does not
 * order) a reader by an answer the frame then contradicts.
 */
TEST(PassScheduleTest, TheWriterPredicateIsTheShapeOfTheAnnouncement)
{
    auto engine  = intrusive_ptr<RenderEngine>(new RenderEngine());
    auto backend = intrusive_ptr<HeadlessBackend>(new HeadlessBackend());
    engine->setDevice(backend);
    ASSERT_TRUE(engine->initialize());

    auto camera  = vn::make_intrusive<Camera>();
    auto content = vn::test::emptySceneForTest();
    auto sampled = colourTarget();
    auto texture = intrusive_ptr<Texture2D>(new Texture2D(4, 4, vn::imaging::PixelFormat::Rgba8Unorm));

    struct Shape
    {
        PassDesc                  desc;
        std::vector<ImageSource>  resolved; // what the engine would hand resolveInputTextures
        const char*               what;
        bool                      writes;
    };

    std::vector<Shape> shapes;

    // ① a scene pass that draws: a camera is a packet.
    PassDesc drawing = writingPass(u8"drawing", colourTarget(), 0, camera);
    drawing.content  = content;
    shapes.push_back({ std::move(drawing), {}, "scene pass with a camera draws", true });

    // ② the case this batch is about: no camera, so no packet - and the clear it announces is its whole effect.
    PassDesc clearing = writingPass(u8"clearing", colourTarget(), 0, nullptr);
    clearing.content  = content;
    shapes.push_back({ std::move(clearing), {}, "a camera-less pass clears what it targets", true });

    // ③ no camera and no clear: the pass announces where it is and changes nothing.
    PassDesc naming        = writingPass(u8"naming", colourTarget(), 0, nullptr);
    naming.content         = content;
    naming.clear_enabled   = false;
    naming.clear_depth     = false;
    shapes.push_back({ std::move(naming), {}, "a pass that states neither a draw nor a clear writes nothing", false });

    // ④ no scene: the pass announces nothing at all, so its declared target is not a written target.
    PassDesc contentless = writingPass(u8"contentless", colourTarget(), 0, camera);
    contentless.content  = nullptr;
    shapes.push_back({ std::move(contentless), {}, "a scene pass with no scene announces nothing", false });

    // ⑤ a full-screen pass drawing through its program over a resolved produced source.
    PassDesc drawn_screen = writingPass(u8"drawn_screen", colourTarget(), 0, camera);
    drawn_screen.kind     = DrawKind::Fullscreen;
    drawn_screen.program  = vn::graphics::fullscreenCopyProgram();
    drawn_screen.inputs   = { ImageInput::byTarget(sampled) };
    shapes.push_back({ std::move(drawn_screen), { ImageSource::producedTarget(sampled.get()) }, "a full-screen pass with a program draws", true });

    // ⑥ a full-screen pass whose only input is an imported texture: nothing can ever resolve to be its source.
    PassDesc textured  = writingPass(u8"textured", colourTarget(), 0, camera);
    textured.kind      = DrawKind::Fullscreen;
    textured.program   = vn::graphics::fullscreenCopyProgram(1);
    textured.inputs    = { ImageInput::byTexture(texture) };
    shapes.push_back({ std::move(textured), { ImageSource::imported(texture.get()) }, "a full-screen pass with no produced input announces nothing", false });

    // ⑦ no program, no packet - and the depth clear a HUD owns inside its own rectangle is still a write.
    PassDesc hud       = writingPass(u8"hud", colourTarget(), 0, camera);
    hud.kind           = DrawKind::Fullscreen;
    hud.inputs         = { ImageInput::byTarget(sampled) };
    hud.clear_depth    = true;
    shapes.push_back({ std::move(hud), { ImageSource::producedTarget(sampled.get()) }, "a full-screen pass that states a clear writes", true });

    for (std::size_t index = 0; index < shapes.size(); ++index) {
        // One pass at a time: an engine whose plan holds only this shape, so describe() sees exactly one declaration.
        vn::test::clearPassesForTest(*engine);
        std::vector<ImageSource> resolved = shapes[index].resolved;
        const auto               pass     = vn::test::applyPassForTest(*engine, shapes[index].desc);
        ASSERT_NE(pass, nullptr) << shapes[index].what;
        pass->resolveInputTextures(resolved);

        PassDraw   draw;
        const bool announced = pass->describe(pass->content(), draw);
        const bool effect    = announced && passHasEffect(draw);

        EXPECT_EQ(pass->writesTarget(), shapes[index].writes) << shapes[index].what;
        EXPECT_EQ(pass->writesTarget(), effect) << "the declaration's answer IS the announcement's: " << shapes[index].what;
        if (pass->writesTarget()) {
            EXPECT_TRUE(announced) << "a pass that writes always announces something: " << shapes[index].what;
        }
    }
}
