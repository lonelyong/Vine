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
#include <vine/graphics/Scene.hpp>
#include <vine/graphics/WindowPresentTarget.hpp>

#include <vine/intrusive_ptr.hpp>
#include <vine/raw_ptr.hpp>

#include <gtest/gtest.h>

#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <thread>
#include <vector>

#include "PlanTestSupport.hpp"

using namespace vn::graphics;
using vn::graphics::detail::planCodeText;
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

    // Counted so a case can prove a REMOVED pass still reached the device (see the take-back cases): the release is
    // the half of a removal that a refusal used to skip in silence.
    void releasePass(raw_ptr<const RenderPass> pass) override
    {
        ++released;
        RenderDevice::releasePass(pass);
    }

    bool                            up       = true;
    int                             frames   = 0;
    int                             released = 0;
    raw_ptr<const FrameDescription> last     = nullptr;
};

/**
 * @brief A headless device whose frame can be held INSIDE the engine half (the busy window).
 *
 * A shape change is refused while a frame is being recorded, and that refusal is a precondition rather than a defect
 * of the plan. The gate makes the window deterministic: `waitEntered()` returns once the engine thread is inside the
 * device call, and `release()` lets the frame finish.
 */
class GatedBackend : public HeadlessBackend {
  public:
    void arm()
    {
        const std::lock_guard<std::mutex> lock(gate_);
        armed_    = true;
        released_ = false;
        entered_  = false;
    }

    PresentOutcome executeFrame(const FrameDescription& frame, PresentTarget* present_to = nullptr) override
    {
        {
            std::unique_lock<std::mutex> lock(gate_);
            if (!armed_) {
                return HeadlessBackend::executeFrame(frame, present_to);
            }
            entered_ = true;
            changed_.notify_all();
            changed_.wait(lock, [this] { return released_; });
        }
        return HeadlessBackend::executeFrame(frame, present_to);
    }

    void waitEntered()
    {
        std::unique_lock<std::mutex> lock(gate_);
        changed_.wait(lock, [this] { return entered_; });
    }

    void release()
    {
        const std::lock_guard<std::mutex> lock(gate_);
        released_ = true;
        changed_.notify_all();
    }

  private:
    std::mutex              gate_;
    std::condition_variable changed_;
    bool                    armed_{ false };
    bool                    entered_{ false };
    bool                    released_{ false };
};

/** @brief A scene declaration with a name, a target and an order (the fields a plan diffs on). */
PassDesc namedPass(const char8_t* name, intrusive_ptr<RenderTarget> target, int order, intrusive_ptr<Camera> camera)
{
    PassDesc desc;
    desc.name   = name;
    desc.stage  = PassStage::Shading;
    desc.order  = order;
    desc.kind   = DrawKind::Scene;
    desc.camera = std::move(camera);
    desc.target = vn::graphics::Target::explicitTarget(std::move(target));
    return desc;
}

/**
 * @brief A pass whose behaviour is its overridden describe(): the escape hatch a plan cannot spell (W4).
 *
 * It is deliberately not registered through VN_OBJECT_META_DECL: the refusal asks the C++ DYNAMIC type, because
 * that is what "makePassNode would build a different object" means.
 */
class CustomPass : public RenderPass {
  public:
    int describes = 0;

    bool describe(vn::raw_ptr<Scene> scene, PassDraw& out) override
    {
        ++describes;
        return RenderPass::describe(scene, out);
    }
};

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

    EXPECT_TRUE(engine->applyPlan(shape).applied()) << "a valid shape is applied and answers no problem";
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
    ASSERT_TRUE(engine->applyPlan(shape).applied());

    const Plan read_back = engine->plan();
    ASSERT_EQ(read_back.passes.size(), 2u);
    EXPECT_EQ(read_back.passes[0].name, u8"early") << "the table is ordered, not declaration-ordered";
    EXPECT_EQ(read_back.passes[0].order.value_or(0), passStageOrder(PassStage::Depth));

    EXPECT_TRUE(engine->applyPlan(read_back).applied());
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
    ASSERT_TRUE(engine->applyPlan(first).applied());

    Plan second;
    second.passes.push_back(namedPass(u8"b", colourTarget(), 10, camera));
    second.passes.push_back(namedPass(u8"c", colourTarget(), 20, camera));
    EXPECT_TRUE(engine->applyPlan(second).applied());

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
    ASSERT_TRUE(engine->applyPlan(initial).applied());

    const raw_ptr<const RenderPass> before = engine->frameTable().passes.at(0).pass;
    ASSERT_NE(before, nullptr);

    Plan restated;
    restated.passes.push_back(namedPass(u8"main", second, 0, camera));
    EXPECT_TRUE(engine->applyPlan(restated).applied());

    const FrameTable after = engine->frameTable();
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
    main.name   = u8"main";
    main.inputs = { ImageInput::byName(String(u8"composite")) };
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
    a.output.name = u8"a-out";
    a.inputs      = { ImageInput::byName(String(u8"b-out")) };
    PassDesc b;
    b.name        = u8"b";
    b.output.name = u8"b-out";
    b.inputs      = { ImageInput::byName(String(u8"a-out")) };
    Plan cycle;
    cycle.passes.push_back(a);
    cycle.passes.push_back(b);
    EXPECT_TRUE(carries(validatePlan(cycle), PlanProblemCode::DependencyCycle));

    PassDesc feedback;
    feedback.name        = u8"feedback";
    feedback.output.name = u8"self";
    feedback.inputs      = { ImageInput::byName(String(u8"self")) };
    Plan allowed;
    allowed.passes.push_back(feedback);
    EXPECT_TRUE(validatePlan(allowed).empty()) << "reading what you wrote is the feedback pattern, not a cycle";
}

/*
 * @brief T56: the description spells the WHOLE declaration list, whatever identity each entry names.
 */
TEST(PlanTest, TheDescriptionSpellsEveryDeclaredInput)
{
    auto target = intrusive_ptr<RenderTarget>(new RenderTarget());
    target->setName(u8"composite");
    auto image = intrusive_ptr<ImageRef>(new ImageRef(u8"albedo"));
    image->bind(target, 1);

    PassDesc pass;
    pass.name   = u8"light";
    pass.inputs = { ImageInput::byTarget(target), ImageInput::byImage(image), ImageInput::byName(String(u8"Host")) };

    const String text = Plan{ { pass } }.describe();
    EXPECT_NE(text.find(u8"inputs=[target:composite, image:albedo#1, name:Host]"), String::npos) << text.as_std_str();
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
    ASSERT_TRUE(engine->applyPlan(good).applied());

    Plan bad;
    bad.passes.push_back(namedPass(u8"hud", colourTarget(), 60, camera));
    bad.passes.push_back(namedPass(u8"hud", colourTarget(), 70, camera));
    EXPECT_TRUE(engine->applyPlan(bad).invalid()) << "two rows with one name is a shape defect";

    const std::vector<String> names = namesOf(engine->plan());
    ASSERT_EQ(names.size(), 1u) << "the half-applied shape would be a shape nobody declared";
    EXPECT_EQ(names[0], u8"main");
    EXPECT_EQ(engine->diagnosticCount(DiagnosticCategory::PassProtocolViolation), 1u) << "the refusal is said once per call";
}

/*
 * @brief An unnamed pass has no plan spelling: the shape is refused, not guessed at.
 *
 * The plan identifies each pass by its NAME (that is what a diff compares), so an empty name is a shape that cannot
 * be expressed - `EmptyPassName`, with the table untouched. Naming it is the one thing a declaration must state.
 */
TEST(PlanTest, AnUnnamedPassIsNotRoundTrippable)
{
    auto engine  = intrusive_ptr<RenderEngine>(new RenderEngine());
    auto backend = intrusive_ptr<HeadlessBackend>(new HeadlessBackend());
    engine->setDevice(backend);
    ASSERT_TRUE(engine->initialize());

    PassDesc unnamed;
    unnamed.camera                          = vn::make_intrusive<Camera>();
    const PlanResult result                 = engine->applyPlan(Plan{ { unnamed } });
    ASSERT_TRUE(result.invalid()) << "a shape that cannot be diffed is refused, not guessed at";
    ASSERT_FALSE(result.problems.empty());
    EXPECT_EQ(result.problems.front().code, PlanProblemCode::EmptyPassName);
    EXPECT_EQ(engine->passCount(), 0u) << "and the refusal changes nothing";

    // A named restatement of the same declaration is the expressible shape.
    unnamed.name = u8"named";
    EXPECT_TRUE(engine->applyPlan(Plan{ { unnamed } }).applied());
    EXPECT_EQ(engine->passCount(), 1u);
    ASSERT_EQ(engine->plan().passes.size(), 1u);
    EXPECT_EQ(engine->plan().passes.front().name, u8"named");
}

/*
 * @brief A field the kind's execution never reads is refused, not silently ignored.
 *
 * A Fullscreen pass draws one program over its resolved inputs and never walks a scene; a Scene pass shades through
 * `program_override` and its per-geometry programs and never reads `program`. A descriptor mixing the two would draw
 * something other than what it declares, with no report - so the shape is refused and nothing of it is applied.
 */
TEST(PlanTest, AFieldTheKindDoesNotReadIsRefused)
{
    auto engine = intrusive_ptr<RenderEngine>(new RenderEngine());

    // `content` is NOT one of them: a full-screen pass reads it for the scene's LIGHT SET (see FullscreenPass::describe),
    // so a composite pass naming its scene is the ordinary deferred recipe.
    PassDesc fullscreen_with_content;
    fullscreen_with_content.name    = u8"composite";
    fullscreen_with_content.kind    = DrawKind::Fullscreen;
    fullscreen_with_content.camera  = vn::make_intrusive<Camera>();
    fullscreen_with_content.content = vn::make_intrusive<Scene>();

    // What the kind really never reads: the scene pass' program override, and a layer mask that would select
    // content the full-screen execution never walks.
    PassDesc fullscreen_with_override = fullscreen_with_content;
    fullscreen_with_override.program_override = vn::make_intrusive<ShaderProgram>();
    PlanResult result                         = engine->applyPlan(Plan{ { fullscreen_with_override } });
    ASSERT_TRUE(result.invalid()) << "a full-screen pass draws its `program`; an override is never read";
    EXPECT_EQ(result.problems.front().code, PlanProblemCode::FieldUnusedByKind);
    EXPECT_EQ(engine->passCount(), 0u) << "a refused plan applies nothing";

    PassDesc fullscreen_with_mask = fullscreen_with_content;
    fullscreen_with_mask.layer_mask = 1U;
    result                          = engine->applyPlan(Plan{ { fullscreen_with_mask } });
    ASSERT_TRUE(result.invalid()) << "a layer mask selects content, and the full-screen execution walks none";
    EXPECT_EQ(result.problems.front().code, PlanProblemCode::FieldUnusedByKind);

    // The other direction: `program` is the Fullscreen kind's field, and a Scene pass states its shading elsewhere.
    PassDesc scene_with_program;
    scene_with_program.name    = u8"main";
    scene_with_program.camera  = vn::make_intrusive<Camera>();
    scene_with_program.program = vn::make_intrusive<ShaderProgram>();
    result                     = engine->applyPlan(Plan{ { scene_with_program } });
    ASSERT_TRUE(result.invalid());
    EXPECT_EQ(result.problems.front().code, PlanProblemCode::FieldUnusedByKind);

    // The same declaration with the field on the kind that reads it is the expressible shape.
    scene_with_program.program = nullptr;
    EXPECT_TRUE(engine->applyPlan(Plan{ { scene_with_program } }).applied());
}

/*
 * @brief The code SPELLING is part of the contract a log and a test read.
 */
TEST(PlanTest, TheCodeSpellingsAreStable)
{
    EXPECT_EQ(planCodeText(PlanProblemCode::EmptyPassName), u8"empty-pass-name");
    EXPECT_EQ(planCodeText(PlanProblemCode::DuplicatePassName), u8"duplicate-pass-name");
    EXPECT_EQ(planCodeText(PlanProblemCode::InvalidTarget), u8"invalid-target");
    EXPECT_EQ(planCodeText(PlanProblemCode::InvalidOutput), u8"invalid-output");
    EXPECT_EQ(planCodeText(PlanProblemCode::FieldUnusedByKind), u8"field-unused-by-kind");
    EXPECT_EQ(planCodeText(PlanProblemCode::UnknownInput), u8"unknown-input");
    EXPECT_EQ(planCodeText(PlanProblemCode::DependencyCycle), u8"dependency-cycle");
    EXPECT_EQ(planCodeText(PlanProblemCode::TooManyPasses), u8"too-many-passes");
}

/*
 * @brief A frame in flight is a REFUSAL, not a problem with the plan: the shape is not even examined.
 *
 * `SessionBusy` used to live in PlanProblemCode, which made "my plan is malformed" and "try again between frames" one
 * vocabulary. The refused answer carries the reason and an empty problem list, so a host knows to retry the SAME plan.
 */
TEST(PlanTest, ABusyEngineRefusesThePlanInsteadOfBlamingItsShape)
{
    auto backend = intrusive_ptr<GatedBackend>(new GatedBackend());
    auto engine  = intrusive_ptr<RenderEngine>(new RenderEngine());
    engine->setDevice(backend);
    ASSERT_TRUE(engine->initialize());

    auto camera = vn::make_intrusive<Camera>();

    Plan initial;
    initial.passes.push_back(namedPass(u8"main", colourTarget(), 0, camera));
    ASSERT_TRUE(engine->applyPlan(initial).applied());

    engine->setFrameThreading(FrameThreading::EngineThread);
    backend->arm();
    ASSERT_TRUE(engine->frame().submitted());
    backend->waitEntered();

    Plan next;
    next.passes.push_back(namedPass(u8"hud", colourTarget(), 60, camera));
    const PlanResult refused = engine->applyPlan(next);
    EXPECT_TRUE(refused.refused());
    EXPECT_EQ(refused.refusal, PlanRefusal::Busy);
    EXPECT_TRUE(refused.problems.empty()) << "busy is not a shape defect";
    EXPECT_EQ(engine->passCount(), 1u) << "the session keeps the shape the frame is drawing";
    EXPECT_EQ(engine->diagnosticCount(DiagnosticCategory::PassProtocolViolation), 1u) << "one report for the episode";

    backend->release();
    ASSERT_TRUE(engine->waitForFrame());
    EXPECT_TRUE(engine->applyPlan(next).applied()) << "the same plan is applicable between frames";
    EXPECT_EQ(engine->passCount(), 1u) << "the whole-session form replaced main with hud";
    EXPECT_NE(engine->pass(String(u8"hud")), nullptr);

    engine->setFrameThreading(FrameThreading::Inline);
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

/**
 * @brief A declaration that states two forms of one fact is refused, not resolved by precedence.
 *
 * `Target` says WHERE a pass draws and `Output` says WHAT it publishes; both are single values, so
 * "an explicit target beside a transient name" and "a whole-target promise beside one image" are
 * values the plan refuses instead of letting one of them win at compile time (see Target::isValid /
 * Output::isValid). The consistent value is accepted, which is what keeps the refusal about the
 * shape and not about the fields being used at all.
 */
TEST(PlanTest, ATargetValueAndAnOutputValueCannotBothBeStated)
{
    auto target = intrusive_ptr<RenderTarget>(new RenderTarget());
    target->setName(u8"gbuffer");
    target->attachColor(RenderTarget::ColorFormat::RGBA8);
    auto image = intrusive_ptr<ImageRef>(new ImageRef(u8"gbuffer.color"));
    image->bind(target, 0);

    PassDesc pass;
    pass.name = u8"mixed";
    // TWO FORMS OF ONE FACT: an explicit target and a transient identity for the same place.
    pass.target.kind   = Target::Kind::Transient;
    pass.target.name   = u8"gbuffer";
    pass.target.target = target;
    // TWO PROMISES FOR ONE NAME: the whole target and one of its images.
    pass.output.name         = u8"out";
    pass.output.whole_target = true;
    pass.output.image        = image;

    const std::vector<PlanProblem> problems = validatePlan(Plan{ { pass } });
    bool                           target_refused = false;
    bool                           output_refused = false;
    for (const PlanProblem& problem : problems) {
        target_refused = target_refused || problem.code == PlanProblemCode::InvalidTarget;
        output_refused = output_refused || problem.code == PlanProblemCode::InvalidOutput;
    }
    EXPECT_TRUE(target_refused) << "a Target that states an explicit target and a transient name was accepted";
    EXPECT_TRUE(output_refused) << "an Output that promises a whole target and one image was accepted";

    // The consistent spelling is accepted: one transient identity, one fine image claim.
    pass.target = Target::transient(u8"gbuffer");
    pass.output.whole_target = false;
    pass.output.image        = image;
    EXPECT_TRUE(validatePlan(Plan{ { pass } }).empty()) << "a consistent Target/Output pair must be a valid plan";
}

/*
 * @brief "Being recorded" starts with the COMPILE, not with the frame thread - and inline there is no frame thread.
 *
 * The gate used to ask the runner, so it was closed only for the engine half: an inline frame never closed it, and a
 * threaded frame's content half did not either. `SessionShape::applyPlan` erases and inserts slots, and the compile
 * walks those slots (rows, transients, and the raw `pass` pointers a `FrameDescription` carries into the backend), so
 * a plan that landed there was applied to a table something was already reading - a dangling pass in the frame value
 * and a target released under the recording. The sink below is where such a call really comes from: it is host code
 * invoked from inside `SessionShape::compile`, which is the case `EngineNotReadyReason::Reentrant` exists for.
 */
TEST(PlanTest, AFrameRefusesAPlanFromInsideItsOwnCompile)
{
    auto engine  = intrusive_ptr<RenderEngine>(new RenderEngine());
    auto backend = intrusive_ptr<HeadlessBackend>(new HeadlessBackend());
    engine->setDevice(backend);
    ASSERT_TRUE(engine->initialize());

    auto camera      = vn::make_intrusive<Camera>();
    // NEVER PUBLISHED, so this frame's compile reports it - from inside itself, which is the hook this case needs.
    auto unpublished = vn::make_intrusive<Scene>();

    Plan shape;
    shape.passes.push_back(namedPass(u8"main", colourTarget(), 0, camera));
    ASSERT_TRUE(engine->applyPlan(shape).applied());
    ASSERT_TRUE(engine->bindPassContent(engine->pass(u8"main"), unpublished).bound());

    PlanResult from_inside;
    bool       asked = false;
    engine->setDiagnosticSink([&](const RenderDiagnostic&) {
        if (asked) {
            return; // the refusal's own report arrives through the same sink
        }
        asked       = true;
        // THE WORST THING A HOST COULD DO HERE: replace the whole session shape while it is being compiled.
        from_inside = engine->applyPlan(Plan{});
    });

    const FrameCallOutcome outcome = engine->frame();
    EXPECT_TRUE(outcome.ran()) << "inline, the frame is answered before frame() returns";
    ASSERT_TRUE(asked) << "the case rests on the compile reporting from inside its own build";
    EXPECT_TRUE(from_inside.refused()) << "a plan was APPLIED to the table the compile was walking";
    EXPECT_EQ(from_inside.refusal, PlanRefusal::Busy);
    EXPECT_EQ(engine->passCount(), 1U) << "the shape the frame was built from is still the shape it has";
    EXPECT_EQ(engine->diagnosticCount(DiagnosticCategory::PassProtocolViolation), 1U) << "one report for the episode";
    EXPECT_TRUE(engine->applyPlan(Plan{}).applied()) << "and the same plan applies between frames";
}

/*
 * @brief The SECOND door onto a pass' declaration takes the same gate: a content bind is a shape change.
 *
 * `applyPlan` refused a change from inside a compile and `bindPassContent` did not, although it rewrites
 * `PassDesc::content` - "the same one storage a plan writes and the compile reads". A host re-binding a pass' scene at
 * the moment a frame was compiled got a silent race where the same host re-stating the plan got a value back (review 19
 * `Z2`): the two doors onto one storage answered the same question differently.
 */
TEST(PlanTest, AFrameRefusesAContentBindFromInsideItsOwnCompile)
{
    auto engine  = intrusive_ptr<RenderEngine>(new RenderEngine());
    auto backend = intrusive_ptr<HeadlessBackend>(new HeadlessBackend());
    engine->setDevice(backend);
    ASSERT_TRUE(engine->initialize());

    auto camera      = vn::make_intrusive<Camera>();
    // NEVER PUBLISHED, so this frame's compile reports it - from inside itself, which is the hook this case needs.
    auto bound       = vn::make_intrusive<Scene>();
    auto replacement = vn::make_intrusive<Scene>();

    Plan shape;
    shape.passes.push_back(namedPass(u8"main", colourTarget(), 0, camera));
    ASSERT_TRUE(engine->applyPlan(shape).applied());
    ASSERT_TRUE(engine->bindPassContent(engine->pass(u8"main"), bound).bound());

    ContentBindOutcome from_inside;
    bool              asked = false;
    engine->setDiagnosticSink([&](const RenderDiagnostic&) {
        if (asked) {
            return; // the refusal's own report arrives through the same sink
        }
        asked       = true;
        from_inside = engine->bindPassContent(engine->pass(u8"main"), replacement);
    });

    const FrameCallOutcome outcome = engine->frame();
    EXPECT_TRUE(outcome.ran()) << "inline, the frame is answered before frame() returns";
    ASSERT_TRUE(asked) << "the case rests on the compile reporting from inside its own build";
    EXPECT_TRUE(from_inside.refused()) << "the declaration a compile was walking was rewritten under it";
    EXPECT_EQ(engine->contentOf(engine->pass(u8"main")), bound.get()) << "the frame kept the declaration it was built from";
    EXPECT_EQ(engine->diagnosticCount(DiagnosticCategory::PassProtocolViolation), 1U) << "one report for the episode";

    EXPECT_TRUE(engine->bindPassContent(engine->pass(u8"main"), replacement).bound()) << "and the same bind works between frames";
    EXPECT_EQ(engine->contentOf(engine->pass(u8"main")), replacement.get());
}

/*
 * @brief Two passes publishing one name from different targets: one report, and the last one serves the name.
 *
 * The table now answers "what did this name serve before" from the SAME call that records the new target (see
 * FrameResources::recordPublication) instead of handing out a row reference across its lock - so the collision policy
 * reads its other half from the value, and this case is what proves the report still fires.
 */
TEST(PlanTest, TwoPassesPublishingOneNameAreReportedAndTheLastOneServesIt)
{
    auto engine  = intrusive_ptr<RenderEngine>(new RenderEngine());
    auto backend = intrusive_ptr<HeadlessBackend>(new HeadlessBackend());
    engine->setDevice(backend);
    ASSERT_TRUE(engine->initialize());

    auto camera = vn::make_intrusive<Camera>();

    std::vector<RenderDiagnostic> reported;
    engine->setDiagnosticSink([&reported](const RenderDiagnostic& diagnostic) { reported.push_back(diagnostic); });

    Plan shape;
    PassDesc writer_a = namedPass(u8"writer_a", colourTarget(), 0, camera);
    writer_a.output.name = u8"SceneColor";
    PassDesc writer_b = namedPass(u8"writer_b", colourTarget(), 10, camera);
    writer_b.output.name = u8"SceneColor";
    shape.passes.push_back(std::move(writer_a));
    shape.passes.push_back(std::move(writer_b));
    ASSERT_TRUE(engine->applyPlan(shape).applied());

    engine->frame();

    bool collision = false;
    for (const RenderDiagnostic& diagnostic : reported) {
        if (diagnostic.code == u8"engine.compile.duplicate_output") {
            collision = true;
        }
    }
    EXPECT_TRUE(collision) << "two passes serving one name from different targets is one report";
    EXPECT_EQ(engine->diagnosticCount(DiagnosticCategory::ContentSkipped), 1U) << "and it is said once for the episode";

    // The LATEST publisher serves the name for the rest of the frame (the rule the table states).
    EXPECT_EQ(engine->resolve(u8"SceneColor"), engine->pass(u8"writer_b")->renderTarget());
}

/*
 * @brief A take-back asked from inside a frame is ACCEPTED and lands at the next frame head (review 19 Z22).
 *
 * The one shape change whose caller cannot retry is the take-back (`~PassChain` / `~SceneView`), so it may not be
 * refused: a destructor asks once, and "refused" would mean the rows stay in the frame with a dead owner and the
 * per-pass backend release never runs. It is deferred instead - the row a compile is walking is untouched, the next
 * frame no longer announces it - and `engine.pass.changed_during_frame` is not reported, because its advice ("wait,
 * or change between frames") is exactly what a destructor cannot do.
 */
TEST(PlanTest, ATakeBackAskedInsideAFrameLandsOnTheNextFrame)
{
    auto engine  = intrusive_ptr<RenderEngine>(new RenderEngine());
    auto backend = intrusive_ptr<HeadlessBackend>(new HeadlessBackend());
    engine->setDevice(backend);
    ASSERT_TRUE(engine->initialize());

    auto camera = vn::make_intrusive<Camera>();
    // NEVER PUBLISHED, so this frame's compile reports it - and the sink below runs INSIDE that compile.
    auto scene  = vn::make_intrusive<Scene>();

    int  scope = 0; // stands in for the owner a chain / a view passes as "my rows"
    Plan shape;
    shape.passes.push_back(namedPass(u8"main", colourTarget(), 0, camera));
    ASSERT_TRUE(engine->applyPlan(shape, &scope).applied());
    ASSERT_TRUE(engine->bindPassContent(engine->pass(u8"main"), scene).bound());
    ASSERT_EQ(engine->passCount(), 1U);

    std::size_t after_ask = 0;
    bool        asked     = false;
    engine->setDiagnosticSink([&](const RenderDiagnostic&) {
        if (asked) {
            return; // later reports of the same frame are not this case's business
        }
        asked = true;
        engine->passes().clearOwnedBy(&scope);
        after_ask = engine->passCount();
    });

    const FrameCallOutcome outcome = engine->frame();
    EXPECT_TRUE(outcome.ran()) << "inline, the frame is answered before frame() returns";
    ASSERT_TRUE(asked) << "the case rests on the compile reporting from inside its own build";
    EXPECT_EQ(after_ask, 1U) << "the take-back waits: it mutates nothing a compile is walking";
    EXPECT_EQ(engine->diagnosticCount(DiagnosticCategory::PassProtocolViolation), 0U)
        << "an accepted take-back is not the 'wait for the frame' refusal";

    engine->frame(); // the head of this frame is where the waiting rows leave
    EXPECT_EQ(engine->passCount(), 0U);
    EXPECT_EQ(engine->pass(u8"main"), nullptr);
    EXPECT_EQ(backend->released, 1) << "the deferral still runs removePass' backend release";
}

/*
 * @brief A take-back that waited removes the rows its owner had when it asked - never a row a later object registers.
 *
 * An owner is a raw address, and `operator new` hands the next view the address the last one freed. So a deferred
 * take-back keyed by the address alone would delete the rows of the object that asked SECOND; it carries the
 * registration ticket of the instant it was asked instead.
 */
TEST(PlanTest, ATakeBackThatWaitedLeavesRowsRegisteredAfterTheAskAlone)
{
    auto engine  = intrusive_ptr<RenderEngine>(new RenderEngine());
    auto backend = intrusive_ptr<HeadlessBackend>(new HeadlessBackend());
    engine->setDevice(backend);
    ASSERT_TRUE(engine->initialize());

    auto camera = vn::make_intrusive<Camera>();
    auto scene  = vn::make_intrusive<Scene>();

    int  scope = 0;
    Plan first;
    first.passes.push_back(namedPass(u8"left", colourTarget(), 0, camera));
    first.passes.push_back(namedPass(u8"right", colourTarget(), 10, camera));
    ASSERT_TRUE(engine->applyPlan(first, &scope).applied());
    ASSERT_TRUE(engine->bindPassContent(engine->pass(u8"left"), scene).bound());

    bool        asked = false;
    std::size_t seen  = 0;
    engine->setDiagnosticSink([&](const RenderDiagnostic&) {
        if (asked) {
            return;
        }
        asked = true;
        engine->passes().clearOwnedBy(&scope);
        seen = engine->passCount();
    });
    engine->frame();
    ASSERT_TRUE(asked);
    ASSERT_EQ(seen, 2U) << "both rows wait";

    // THE ADDRESS IS REUSED: a host that destroys a view and builds the next one may land on the same memory.
    Plan second;
    second.passes.push_back(namedPass(u8"fresh", colourTarget(), 20, camera));
    ASSERT_TRUE(engine->applyPlan(second, &scope).applied());

    engine->frame(); // the head drains what the first owner asked for, and only that
    EXPECT_EQ(engine->pass(u8"left"), nullptr);
    EXPECT_EQ(engine->pass(u8"right"), nullptr);
    EXPECT_NE(engine->pass(u8"fresh"), nullptr) << "a row registered after the ask is the second object's";
    EXPECT_EQ(engine->passCount(), 1U);

    engine->passes().clearOwnedBy(&scope); // between frames the same call is immediate
    EXPECT_EQ(engine->passCount(), 0U);
}

/*
 * @brief A take-back still waiting when the engine stops is applied by shutdown, not dropped with the pump.
 *
 * Nothing will compile after `shutdown()`, so a waiting take-back would never land - and the device it should tell
 * about the release is exactly the object the host may destroy next.
 */
TEST(PlanTest, ATakeBackStillWaitingWhenTheEngineStopsIsApplied)
{
    auto engine  = intrusive_ptr<RenderEngine>(new RenderEngine());
    auto backend = intrusive_ptr<HeadlessBackend>(new HeadlessBackend());
    engine->setDevice(backend);
    ASSERT_TRUE(engine->initialize());

    auto camera = vn::make_intrusive<Camera>();
    auto scene  = vn::make_intrusive<Scene>();

    int  scope = 0;
    Plan shape;
    shape.passes.push_back(namedPass(u8"main", colourTarget(), 0, camera));
    ASSERT_TRUE(engine->applyPlan(shape, &scope).applied());
    ASSERT_TRUE(engine->bindPassContent(engine->pass(u8"main"), scene).bound());

    bool asked = false;
    engine->setDiagnosticSink([&](const RenderDiagnostic&) {
        if (asked) {
            return;
        }
        asked = true;
        engine->passes().clearOwnedBy(&scope);
    });
    engine->frame();
    ASSERT_TRUE(asked);
    ASSERT_EQ(engine->passCount(), 1U) << "still waiting for a frame head";

    engine->shutdown();
    EXPECT_EQ(engine->passCount(), 0U) << "a take-back never outlives the engine that held it";
    EXPECT_EQ(backend->released, 1);
}

/*
 * @brief The shipped host's own order: the view is destroyed while the engine half still answers the frame.
 *
 * In threaded mode `frame()` returns as soon as the job is submitted, and `SurfaceWindow`'s teardown destroys the view
 * immediately before the engine that stops the frame thread - so the take-back lands while `answering_` is true. It must
 * be recorded (not dropped) and applied at the next frame head, which is the same repair the inline case proves from
 * inside the compile.
 */
TEST(PlanTest, ATakeBackWhileAFrameIsAnsweredWaitsForTheNextHead)
{
    auto engine  = intrusive_ptr<RenderEngine>(new RenderEngine());
    auto backend = intrusive_ptr<GatedBackend>(new GatedBackend());
    engine->setDevice(backend);
    ASSERT_TRUE(engine->initialize());
    engine->setFrameThreading(FrameThreading::EngineThread);

    auto camera = vn::make_intrusive<Camera>();

    int  scope = 0;
    Plan shape;
    shape.passes.push_back(namedPass(u8"main", colourTarget(), 0, camera));
    ASSERT_TRUE(engine->applyPlan(shape, &scope).applied());
    ASSERT_EQ(engine->passCount(), 1U);

    // THE ENGINE HALF IS HELD INSIDE THE DEVICE CALL: `recording()` is true for the whole time it answers.
    backend->arm();
    ASSERT_TRUE(engine->frame().submitted());
    backend->waitEntered();

    // The host's destructor, mid-answer: it cannot wait and cannot retry, so this is the call that used to be dropped.
    engine->passes().clearOwnedBy(&scope);
    EXPECT_EQ(engine->passCount(), 1U) << "the frame being answered keeps the shape it compiled";

    backend->release();
    ASSERT_TRUE(engine->waitForFrame(std::chrono::milliseconds(2000)));
    EXPECT_EQ(engine->passCount(), 1U) << "still waiting: the next frame head is where a take-back lands";

    engine->frame();
    EXPECT_EQ(engine->passCount(), 0U);
    EXPECT_EQ(backend->released, 1) << "the deferred take-back still reached the device";
}

/*
 * @brief A PREPARE compiles too, so it is a build - the interval the runner never described.
 *
 * `prepare()` is not a frame (nothing is answered), which is why it was left outside the old predicate altogether. It
 * walks the same pass table, resolves the same transients and describes the same passes, so the same corrupting change
 * could land in it.
 */
TEST(PlanTest, APrepareRefusesAPlanFromInsideItsOwnCompile)
{
    auto engine  = intrusive_ptr<RenderEngine>(new RenderEngine());
    auto backend = intrusive_ptr<HeadlessBackend>(new HeadlessBackend());
    engine->setDevice(backend);
    ASSERT_TRUE(engine->initialize());

    auto camera      = vn::make_intrusive<Camera>();
    auto unpublished = vn::make_intrusive<Scene>();

    Plan shape;
    shape.passes.push_back(namedPass(u8"main", colourTarget(), 0, camera));
    ASSERT_TRUE(engine->applyPlan(shape).applied());
    ASSERT_TRUE(engine->bindPassContent(engine->pass(u8"main"), unpublished).bound());

    PlanResult from_inside;
    bool       asked = false;
    engine->setDiagnosticSink([&](const RenderDiagnostic&) {
        if (asked) {
            return;
        }
        asked       = true;
        from_inside = engine->applyPlan(Plan{});
    });

    const PrepareReport report = engine->prepare();
    EXPECT_NE(report.outcome, PrepareReport::Outcome::NotReady) << "the engine was ready to prepare";
    ASSERT_TRUE(asked) << "prepare() compiles, so it reports like a frame does";
    EXPECT_TRUE(from_inside.refused());
    EXPECT_EQ(from_inside.refusal, PlanRefusal::Busy);
    EXPECT_EQ(engine->passCount(), 1U);
}

/*
 * @brief A plan from ANOTHER thread is refused while the paint thread is inside the compile.
 *
 * The shape is edited by the content side, and an application's content assembly may run on its own pool (the demo's
 * does: "a layout step runs on a resize, and the content assembly runs on a pool"). With the check and the change as two
 * statements, that thread could decide "nothing is recording" and then apply while the compile had begun - which is a
 * data race on the slot table, not merely a wrong picture. The decision and the change are one step now, under the gate
 * the phase flip also takes.
 */
TEST(PlanTest, APlanFromAnotherThreadIsRefusedWhileThisOneCompiles)
{
    auto engine  = intrusive_ptr<RenderEngine>(new RenderEngine());
    auto backend = intrusive_ptr<HeadlessBackend>(new HeadlessBackend());
    engine->setDevice(backend);
    ASSERT_TRUE(engine->initialize());

    auto camera      = vn::make_intrusive<Camera>();
    auto unpublished = vn::make_intrusive<Scene>();

    Plan shape;
    shape.passes.push_back(namedPass(u8"main", colourTarget(), 0, camera));
    ASSERT_TRUE(engine->applyPlan(shape).applied());
    ASSERT_TRUE(engine->bindPassContent(engine->pass(u8"main"), unpublished).bound());

    std::mutex              gate;
    std::condition_variable changed;
    bool                    inside   = false;
    bool                    released = false;
    engine->setDiagnosticSink([&](const RenderDiagnostic&) {
        std::unique_lock<std::mutex> lock(gate);
        if (inside) {
            return; // the refusal's own report (it arrives on the applying thread)
        }
        inside = true;
        changed.notify_all();
        changed.wait(lock, [&] { return released; });
    });

    FrameCallOutcome outcome;
    std::thread      driver([&] { outcome = engine->frame(); });
    {
        std::unique_lock<std::mutex> lock(gate);
        changed.wait(lock, [&] { return inside; });
    }

    // THE OTHER THREAD, WHILE THE COMPILE IS STOPPED INSIDE ITSELF.
    const PlanResult while_building = engine->applyPlan(Plan{});
    EXPECT_TRUE(while_building.refused()) << "a plan landed inside another thread's compile";
    EXPECT_EQ(while_building.refusal, PlanRefusal::Busy);
    EXPECT_EQ(engine->passCount(), 1U);

    {
        const std::lock_guard<std::mutex> lock(gate);
        released = true;
    }
    changed.notify_all();
    driver.join();

    EXPECT_TRUE(outcome.ran());
    EXPECT_EQ(engine->passCount(), 1U) << "the frame kept the shape it was built from";
    EXPECT_TRUE(engine->applyPlan(Plan{}).applied()) << "the same plan applies once the frame is answered";
}
