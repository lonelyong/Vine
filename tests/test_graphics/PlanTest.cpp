#include <vine/graphics/Camera.hpp>
#include <vine/graphics/Capabilities.hpp>
#include <vine/graphics/FrameDescription.hpp>
#include <vine/graphics/PassDesc.hpp>
#include <vine/graphics/Plan.hpp>
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
#include <span>
#include <string>
#include <vector>

using namespace vn::graphics;
using vn::intrusive_ptr;
using vn::raw_ptr;
using vn::String;

namespace
{

/**
 * @brief A device with no window half: enough to compile a frame, nothing to present.
 *
 * The plan is the ENGINE's, so these cases need a frame to compile and no pixels.
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

/** @brief A scene declaration with a name, a target and an order (the fields a plan diffs on). */
PassDesc namedPass(const char8_t* name, intrusive_ptr<RenderTarget> target, int order, intrusive_ptr<Camera> camera)
{
    PassDesc desc;
    desc.name   = name;
    desc.stage  = PassStage::Shading;
    desc.order  = order;
    desc.kind   = PassKind::Scene;
    desc.camera = std::move(camera);
    desc.writes = std::move(target);
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

/** @brief Answers whether @p problems carry @p code. */
bool carries(const std::vector<PlanProblem>& problems, PlanProblemCode code)
{
    for (const PlanProblem& problem : problems) {
        if (problem.code == code) {
            return true;
        }
    }
    return false;
}

/** @brief The names of a plan, in order (what the diff is judged by). */
std::vector<String> namesOf(const Plan& plan)
{
    std::vector<String> names;
    for (const PassDesc& pass : plan.passes) {
        names.push_back(pass.name);
    }
    return names;
}

/** @brief A pass name with a number, so "too many passes" needs no hand-written list. */
String numbered(const char8_t* prefix, std::size_t index)
{
    const std::string digits = std::to_string(index);
    return String(prefix) + String(std::u8string(digits.begin(), digits.end()));
}

} // namespace

/*
 * @brief T7: a pass is added WITHOUT any add call - the shape is one value, applied in one call.
 */
TEST(PlanTest, APassIsAddedWithoutAnyAddCall)
{
    auto engine  = intrusive_ptr<RenderEngine>(new RenderEngine());
    auto backend = intrusive_ptr<HeadlessBackend>(new HeadlessBackend());
    engine->setDevice(backend);
    ASSERT_TRUE(engine->initialize());

    auto camera = vn::make_intrusive<Camera>();

    Plan shape;
    shape.passes.push_back(namedPass(u8"main", colourTarget(), 0, camera));
    shape.passes.push_back(namedPass(u8"hud", colourTarget(), 60, camera));

    EXPECT_TRUE(engine->applyPlan(shape).empty()) << "a valid shape is applied and answers no problem";
    EXPECT_EQ(engine->passCount(), 2u);

    engine->frame();

    // The passes RUN: the frame the engine hands the backend is what the declared shape compiled to.
    ASSERT_NE(backend->last, nullptr);
    ASSERT_EQ(backend->last->passes.size(), 2u);
    EXPECT_EQ(backend->last->passes[0].pass->name(), u8"main") << "declaration order decides equal priorities, and 0 runs before 60";
    EXPECT_EQ(backend->last->passes[1].pass->name(), u8"hud");
}

/*
 * @brief The shape read back is the shape that reproduces it: applyPlan(plan()) changes nothing.
 *
 * This is what makes the value usable as a diff input at all - if reading the shape back and re-applying it
 * were not a no-op, every host that round-trips a shape would re-order its frame.
 */
TEST(PlanTest, TheShapeReadBackIsTheShapeThatReproducesIt)
{
    auto engine  = intrusive_ptr<RenderEngine>(new RenderEngine());
    auto backend = intrusive_ptr<HeadlessBackend>(new HeadlessBackend());
    engine->setDevice(backend);
    ASSERT_TRUE(engine->initialize());

    auto camera = vn::make_intrusive<Camera>();

    Plan shape;
    shape.passes.push_back(namedPass(u8"late", colourTarget(), 20, camera));
    // Declared by STAGE only: the read-back has to state the effective order, or re-applying it would move it.
    PassDesc by_stage = namedPass(u8"early", colourTarget(), 0, camera);
    by_stage.order.reset();
    by_stage.stage = PassStage::Depth;
    shape.passes.push_back(by_stage);
    ASSERT_TRUE(engine->applyPlan(shape).empty());

    const Plan read_back = engine->plan();
    ASSERT_EQ(read_back.passes.size(), 2u);
    EXPECT_EQ(read_back.passes[0].name, u8"early") << "the table is ordered, not declaration-ordered";
    EXPECT_EQ(read_back.passes[0].order.value_or(0), passStageOrder(PassStage::Depth));

    EXPECT_TRUE(engine->applyPlan(read_back).empty());
    EXPECT_EQ(engine->passCount(), 2u);
    EXPECT_EQ(namesOf(engine->plan())[0], u8"early");
}

/*
 * @brief A name that disappears is removed, a name that appears is added: one call, no removePass/addPass.
 */
TEST(PlanTest, ARenamedPassLeavesExactlyTheDeclaredNames)
{
    auto engine  = intrusive_ptr<RenderEngine>(new RenderEngine());
    auto backend = intrusive_ptr<HeadlessBackend>(new HeadlessBackend());
    engine->setDevice(backend);
    ASSERT_TRUE(engine->initialize());

    auto camera = vn::make_intrusive<Camera>();

    Plan first;
    first.passes.push_back(namedPass(u8"a", colourTarget(), 0, camera));
    first.passes.push_back(namedPass(u8"b", colourTarget(), 10, camera));
    ASSERT_TRUE(engine->applyPlan(first).empty());

    Plan second;
    second.passes.push_back(namedPass(u8"b", colourTarget(), 10, camera));
    second.passes.push_back(namedPass(u8"c", colourTarget(), 20, camera));
    EXPECT_TRUE(engine->applyPlan(second).empty());

    const std::vector<String> names = namesOf(engine->plan());
    ASSERT_EQ(names.size(), 2u);
    EXPECT_EQ(names[0], u8"b");
    EXPECT_EQ(names[1], u8"c");
}

/*
 * @brief A changed target under an UNCHANGED name is an update in place - same node, same backend state.
 *
 * P7's failure mode is the opposite: "same name, different content" read as remove + add, which throws away the
 * pipeline variant and the retained descriptor set every time a host re-states its shape.
 */
TEST(PlanTest, AChangedTargetUpdatesTheSameNodeInPlace)
{
    auto engine  = intrusive_ptr<RenderEngine>(new RenderEngine());
    auto backend = intrusive_ptr<HeadlessBackend>(new HeadlessBackend());
    engine->setDevice(backend);
    ASSERT_TRUE(engine->initialize());

    auto camera = vn::make_intrusive<Camera>();
    auto first  = colourTarget();
    auto second = colourTarget();

    Plan initial;
    initial.passes.push_back(namedPass(u8"main", first, 0, camera));
    ASSERT_TRUE(engine->applyPlan(initial).empty());

    const raw_ptr<const RenderPass> before = engine->framePlan().passes.at(0).pass;
    ASSERT_NE(before, nullptr);

    Plan restated;
    restated.passes.push_back(namedPass(u8"main", second, 0, camera));
    EXPECT_TRUE(engine->applyPlan(restated).empty());

    const FramePlan after = engine->framePlan();
    ASSERT_EQ(after.passes.size(), 1u);
    EXPECT_EQ(after.passes[0].pass, before) << "the node survives a re-stated declaration";
    EXPECT_EQ(after.passes[0].target, second.get()) << "and it draws into the target the new shape states";
}

/*
 * @brief T30 (1/4): a pass with no name cannot be diffed, so the plan is refused with its own code.
 */
TEST(PlanTest, AnEmptyPassNameIsRefusedWithItsCode)
{
    Plan     shape;
    PassDesc unnamed;
    unnamed.stage = PassStage::Shading;
    shape.passes.push_back(unnamed);

    const std::vector<PlanProblem> problems = validatePlan(shape);
    EXPECT_TRUE(carries(problems, PlanProblemCode::EmptyPassName));
}

/*
 * @brief T30 (2/4): two passes answering one name are refused rather than one silently winning.
 */
TEST(PlanTest, ADuplicateNameIsRefusedWithItsCode)
{
    Plan     shape;
    PassDesc first;
    first.name = u8"main";
    PassDesc second;
    second.name = u8"main";
    shape.passes.push_back(first);
    shape.passes.push_back(second);

    const std::vector<PlanProblem> problems = validatePlan(shape);
    EXPECT_TRUE(carries(problems, PlanProblemCode::DuplicatePassName));
}

/*
 * @brief T30 (3/4): a named input nobody publishes is refused - unless a HOST binding serves it.
 */
TEST(PlanTest, AnUnknownInputIsRefusedWithItsCodeUnlessTheHostServesIt)
{
    PassDesc main;
    main.name        = u8"main";
    main.input_names = { String(u8"composite") };
    Plan shape;
    shape.passes.push_back(main);

    EXPECT_TRUE(carries(validatePlan(shape), PlanProblemCode::UnknownInput));

    const std::vector<String> host_serves{ String(u8"composite") };
    EXPECT_TRUE(validatePlan(shape, std::span<const String>(host_serves)).empty()) << "a host binding is a served input";
}

/*
 * @brief T30 (4/4a): a cycle is refused with its own code, and a pass reading its own output is NOT one.
 */
TEST(PlanTest, ACycleIsRefusedWithItsCodeAndFeedbackIsNotACycle)
{
    PassDesc a;
    a.name        = u8"a";
    a.output_name = u8"a-out";
    a.input_names = { String(u8"b-out") };
    PassDesc b;
    b.name        = u8"b";
    b.output_name = u8"b-out";
    b.input_names = { String(u8"a-out") };
    Plan cycle;
    cycle.passes.push_back(a);
    cycle.passes.push_back(b);
    EXPECT_TRUE(carries(validatePlan(cycle), PlanProblemCode::DependencyCycle));

    PassDesc feedback;
    feedback.name        = u8"feedback";
    feedback.output_name = u8"self";
    feedback.input_names = { String(u8"self") };
    Plan allowed;
    allowed.passes.push_back(feedback);
    EXPECT_TRUE(validatePlan(allowed).empty()) << "reading what you wrote is the feedback pattern, not a cycle";
}

/*
 * @brief T30 (4/4b): more passes than kMaxPlanPasses is refused whole, never truncated.
 */
TEST(PlanTest, TooManyPassesIsRefusedWithItsCode)
{
    Plan shape;
    for (std::size_t index = 0; index <= kMaxPlanPasses; ++index) {
        PassDesc pass;
        pass.name = numbered(u8"p", index);
        shape.passes.push_back(pass);
    }

    EXPECT_TRUE(carries(validatePlan(shape), PlanProblemCode::TooManyPasses));
}

/*
 * @brief A refused plan changes NOTHING: the shape is applied whole or not at all.
 */
TEST(PlanTest, ARefusedPlanLeavesTheShapeUntouched)
{
    auto engine  = intrusive_ptr<RenderEngine>(new RenderEngine());
    auto backend = intrusive_ptr<HeadlessBackend>(new HeadlessBackend());
    engine->setDevice(backend);
    ASSERT_TRUE(engine->initialize());

    auto camera = vn::make_intrusive<Camera>();

    Plan good;
    good.passes.push_back(namedPass(u8"main", colourTarget(), 0, camera));
    ASSERT_TRUE(engine->applyPlan(good).empty());

    Plan bad;
    bad.passes.push_back(namedPass(u8"hud", colourTarget(), 60, camera));
    bad.passes.push_back(namedPass(u8"hud", colourTarget(), 70, camera));
    EXPECT_FALSE(engine->applyPlan(bad).empty());

    const std::vector<String> names = namesOf(engine->plan());
    ASSERT_EQ(names.size(), 1u) << "the half-applied shape would be a shape nobody declared";
    EXPECT_EQ(names[0], u8"main");
    EXPECT_EQ(engine->diagnosticCount(DiagnosticCategory::PassProtocolViolation), 1u) << "the refusal is said once per call";
}

/*
 * @brief The code SPELLING is part of the contract a log and a test read.
 */
TEST(PlanTest, TheCodeSpellingsAreStable)
{
    EXPECT_EQ(planCodeText(PlanProblemCode::EmptyPassName), u8"empty-pass-name");
    EXPECT_EQ(planCodeText(PlanProblemCode::DuplicatePassName), u8"duplicate-pass-name");
    EXPECT_EQ(planCodeText(PlanProblemCode::UnknownInput), u8"unknown-input");
    EXPECT_EQ(planCodeText(PlanProblemCode::DependencyCycle), u8"dependency-cycle");
    EXPECT_EQ(planCodeText(PlanProblemCode::TooManyPasses), u8"too-many-passes");
    EXPECT_EQ(planCodeText(PlanProblemCode::SessionBusy), u8"session-busy");
}

/*
 * @brief The read-back is greppable: one line per pass, with the facts a diff turns on.
 */
TEST(PlanTest, TheDescriptionNamesEveryFactADiffTurnsOn)
{
    auto camera = vn::make_intrusive<Camera>();

    Plan shape;
    shape.passes.push_back(namedPass(u8"main", colourTarget(), 0, camera));

    const String text = shape.describe();
    EXPECT_NE(text.find(u8"main"), String::npos);
    EXPECT_NE(text.find(u8"order=0"), String::npos);
    EXPECT_NE(text.find(u8"target=explicit"), String::npos);
    EXPECT_EQ(Plan{}.describe(), u8"(empty plan)");
}
