#include <vine/graphics/RenderEngine.hpp>

#include <algorithm>
#include <vector>

#include <vine/graphics/PassDesc.hpp>
#include <vine/graphics/RenderBackend.hpp>
#include <vine/graphics/RenderPass.hpp>
#include <vine/graphics/ScreenPass.hpp>

VN_GRAPHICS_NS_BEGIN

namespace
{
/// @brief Whether two declared transient shapes are the same shape (see PassDesc::writes_shape).
///
/// A shape is what the target is BUILT from, so a difference here is a different target rather than an edit to
/// this one: the attachment set cannot change under a built target (see RenderTarget).
bool sameTransientShape(const TransientShape& left, const TransientShape& right) noexcept
{
    return left.colors == right.colors && left.depth == right.depth &&
           left.depth_promotion == right.depth_promotion;
}
}  // namespace

void RenderEngine::compileFrame()
{
    // THIS IS THE FRAME. Everything the frame needs to know is derived here, from the declarations as they
    // stand right now: which pass runs when (the slot list is already ordered), what each declared input
    // resolves to (what ran BEFORE it - the same rule the runtime used to apply as it went), which transient
    // targets exist, and what is wrong with the set. The tables are reused, so a steady frame allocates
    // nothing beyond what the compile's own bookkeeping already has capacity for.
    plan_problems_.clear();
    plan_rows_.clear();
    plan_produced_.clear();
    plan_published_.clear();
    unusable_inputs_this_frame_.clear();
    // From here until the list is complete, every report the checks make is COLLECTED, not emitted: the
    // episode rule is applied once, at the end (see reportPlanProblems).
    collecting_problems_ = true;

    // A host binding is filled by the HOST: it is there from the start of the frame and no pass owns its
    // hand-off, so it seeds what this frame has already produced (an object-typed input addressing one is
    // answered like one addressing a pass' target).
    for (const auto& binding : wiring_.host_outputs_) {
        if (binding.second != nullptr) {
            plan_produced_.push_back(binding.second.get());
        }
    }

    // Transients first: a slot that declares one gets its target before anything reads a target, and one
    // nobody declares this frame is let go of.
    for (TransientTarget& transient : transients_) {
        transient.declared_now = false;
    }
    for (Slot& slot : slots_) {
        if (!slot.writes_image.empty()) {
            resolveTransientTarget(slot);
        }
    }
    for (auto transient = transients_.begin(); transient != transients_.end();) {
        if (transient->declared_now) {
            ++transient;
            continue;
        }
        releaseTransientTarget(transient->target);
        transient = transients_.erase(transient);
    }

    // The structural checks read DECLARATIONS only, so they run before anything is resolved: a wire the check
    // already spoke about is one problem, and the resolution below stays quiet about it (see hasPlanProblem).
    collectPlanProblems();

    plan_rows_.reserve(slots_.size());
    for (Slot& slot : slots_) {
        PlanRow row;
        row.pass    = slot.pass.get();
        row.content = slot.content.get();
        row.order   = slot.order;
        if (row.pass != nullptr && row.pass->isEnabled()) {
            // What this pass reads: everything produced BEFORE it, which is exactly what "resolved at draw
            // time" meant - a producer registered later is not part of the frame this pass can see.
            resolveRowInputs(row.pass, row.inputs);
            // What it publishes: the name serves the rest of the frame (the latest publisher wins the name,
            // as the per-frame registry did).
            publishRowOutput(row.pass);
            // What it fills: drawing into a target fills every image it has, which is what an object-typed
            // declared input is answered from. Linear and idempotent - several passes may fill one target.
            if (raw_ptr<RenderTarget> drawn_into = row.pass->renderTarget(); drawn_into != nullptr) {
                const raw_ptr<const RenderTarget> filled = drawn_into;
                if (std::find(plan_produced_.begin(), plan_produced_.end(), filled) == plan_produced_.end()) {
                    plan_produced_.push_back(filled);
                }
            }
        }
        plan_rows_.push_back(std::move(row));
    }

    collecting_problems_ = false;
}

void RenderEngine::resolveTransientTarget(Slot& slot)
{
    // The extent: what the declaration states, or the surface this frame is presenting (a transient is
    // normally the size of the picture that consumes it). A surface nobody has sized yet is not a reason to
    // build nothing: a 1x1 target keeps the pass' shape defined until the host reports a size.
    const TransientShape& shape  = slot.writes_shape;
    int                   width  = shape.width > 0 ? shape.width : frame_ctx_.surface_width;
    int                   height = shape.height > 0 ? shape.height : frame_ctx_.surface_height;
    if (width <= 0 || height <= 0) {
        width  = 1;
        height = 1;
    }

    for (TransientTarget& transient : transients_) {
        if (transient.identity != slot.writes_image) {
            continue;
        }
        if (!sameTransientShape(transient.shape, shape) || transient.target->width() != width ||
            transient.target->height() != height) {
            // A different shape is a different TARGET: the attachments of a built one cannot change (see
            // RenderTarget), so the compile replaces it and tells the backend the old one is done with.
            releaseTransientTarget(transient.target);
            transient.shape  = shape;
            transient.target = makeTransientTarget(slot.writes_image, width, height, shape);
        }
        transient.declared_now = true;
        slot.transient         = transient.target;
        slot.pass->setRenderTarget(slot.transient);
        return;
    }

    TransientTarget created;
    created.identity     = slot.writes_image;
    created.shape        = shape;
    created.target       = makeTransientTarget(slot.writes_image, width, height, shape);
    created.declared_now = true;
    transients_.push_back(created);
    slot.transient = created.target;
    slot.pass->setRenderTarget(slot.transient);
}

intrusive_ptr<RenderTarget> RenderEngine::makeTransientTarget(const String& identity, int width, int height,
                                                              const TransientShape& shape)
{
    auto target = intrusive_ptr<RenderTarget>(new RenderTarget());
    target->setName(identity);
    for (const RenderTarget::ColorFormat format : shape.colors) {
        target->attachColor(format);
    }
    if (shape.depth.has_value()) {
        target->attachDepth(*shape.depth);
    }
    target->setDepthPromotion(shape.depth_promotion);
    target->setSize(width, height);
    return target;
}

void RenderEngine::releaseTransientTarget(const intrusive_ptr<RenderTarget>& target)
{
    if (target == nullptr || backend_ == nullptr) {
        return;
    }
    // The SDK's announcement that the caller may destroy it now: the backend drops what it built for it (and
    // retires anything still in flight), exactly as removePass() does for a pass' own target.
    backend_->releaseRenderTarget(target.get());
}

std::size_t RenderEngine::transientTargetCount() const noexcept
{
    return transients_.size();
}

raw_ptr<RenderTarget> RenderEngine::transientTarget(const String& identity) const
{
    if (identity.empty()) {
        return nullptr;
    }
    for (const TransientTarget& transient : transients_) {
        if (transient.identity == identity) {
            return transient.target.get();
        }
    }
    return nullptr;
}

void RenderEngine::resolveRowInputs(raw_ptr<RenderPass> pass, std::vector<raw_ptr<RenderTarget>>& out)
{
    out.clear();
    if (pass == nullptr) {
        return;
    }

    // The object-typed declarations ARE the wiring; a name is the sugar over it. When a pass declares images
    // or targets, they decide what it gets - one resolved entry per declaration, in declaration order, null
    // where nothing produced it - and the names are not consulted at all: one wire stated twice must not be
    // resolved twice.
    if (!pass->inputs().empty() || !pass->inputTargets().empty()) {
        out.reserve(pass->inputs().size() + pass->inputTargets().size());
        for (const auto& image : pass->inputs()) {
            out.push_back(image != nullptr ? resolveDeclaredImage(pass, *image) : nullptr);
        }
        for (const auto& target : pass->inputTargets()) {
            out.push_back(target != nullptr ? resolveDeclaredTarget(pass, target.get()) : nullptr);
        }
        return;
    }

    const auto& names = pass->inputNames();
    if (names.empty()) {
        return;
    }
    out.reserve(names.size());
    for (const auto& name : names) {
        out.push_back(resolveCompiled(name));
    }

    // A declared input nobody published this frame means the pass draws NOTHING (ScreenPass keeps the first
    // non-null input and returns early when there is none), so the frame silently misses its content: the
    // wiring is the engine's job, so it says so. A pass may declare several alternative names (a chain that
    // falls back), so this only fires when NONE of them resolved - and the compile forgets the problem the
    // moment one of them resolves, which is what re-arms it.
    const bool any_resolved = std::any_of(out.begin(), out.end(),
                                         [](raw_ptr<RenderTarget> target) { return target != nullptr; });
    if (any_resolved) {
        return;
    }
    reportEngineProblem(vn::graphics::DiagnosticSeverity::Warning,
                        vn::graphics::DiagnosticCategory::ContentSkipped,
                        String(u8"pass '") + reportedPassName(pass) + String(u8"' declared input '") +
                            names.front() +
                            String(u8"' but no pass published it this frame; the pass draws nothing"
                                   u8" (check the producer's order, its enabled state and its output name)"));
}

void RenderEngine::publishRowOutput(raw_ptr<RenderPass> pass)
{
    if (pass == nullptr) {
        return;
    }
    const String& name = pass->outputName();
    if (name.empty()) {
        return;   // an empty name is how a pass says "do not publish me": not a problem to report
    }
    raw_ptr<RenderTarget> target = pass->renderTarget();
    if (target == nullptr) {
        // The registry hands out a SAMPLEABLE target, and a pass rendering into the window has none. Reported
        // instead of dropping the declaration: a consumer of that name would otherwise be told "nothing
        // produced it this frame", pointing at the consumer for the producer's mistake.
        reportEngineProblem(vn::graphics::DiagnosticSeverity::Warning,
                            vn::graphics::DiagnosticCategory::ContentSkipped,
                            String(u8"pass '") + reportedPassName(pass) +
                                String(u8"' publishes the output name '") + name +
                                String(u8"' but renders into the window (no render target), so nothing is"
                                       u8" published and a consumer of '") +
                                name + String(u8"' finds nothing — give the pass a render target, or drop the"
                                              u8" output name"));
        return;
    }
    for (auto& published : plan_published_) {
        if (published.first == nullptr || published.first->outputName() != name) {
            continue;
        }
        if (published.second != nullptr && published.second != target) {
            // Two passes published DIFFERENT targets under one name: every consumer of that name silently gets
            // whichever pass ran last, and only the whole table can see the collision (each pass is
            // individually valid).
            const String first =
                published.second->name().empty() ? String(u8"(unnamed)") : published.second->name();
            const String second = target->name().empty() ? String(u8"(unnamed)") : target->name();
            reportEngineProblem(vn::graphics::DiagnosticSeverity::Warning,
                                vn::graphics::DiagnosticCategory::ContentSkipped,
                                String(u8"pass output '") + name +
                                    String(u8"' is published by two passes this frame (targets '") + first +
                                    String(u8"' and '") + second + String(u8"'); every consumer of '") + name +
                                    String(u8"' samples whichever pass ran last"));
        }
        published.second = target;   // the latest publication serves the name for the rest of the frame
        return;
    }
    plan_published_.emplace_back(pass, target);
}

raw_ptr<RenderTarget> RenderEngine::resolveCompiled(const String& name) const
{
    for (const auto& published : plan_published_) {
        if (published.first != nullptr && published.first->outputName() == name) {
            return published.second;
        }
    }
    const auto bound = wiring_.host_outputs_.find(name);
    return (bound != wiring_.host_outputs_.end()) ? bound->second.get() : nullptr;
}

void RenderEngine::noteProblem(String message)
{
    // One problem per sentence per frame: the checks visit a subject once, but a subject can be reached from
    // more than one direction (a second and a third claimant of one image), and one mistake should read as one
    // message. The episode rule decides whether it is said out loud at all (see reportPlanProblems).
    const auto known = std::find_if(plan_problems_.begin(), plan_problems_.end(),
                                    [&message](const PlanProblem& problem) { return problem.message == message; });
    if (known == plan_problems_.end()) {
        plan_problems_.push_back(PlanProblem{ std::move(message) });
    }
}

bool RenderEngine::unusableInputReported(raw_ptr<const RenderPass> pass,
                                         const OutputIdentity& identity) const noexcept
{
    const auto key = std::make_pair(pass, identity);
    return std::any_of(unusable_inputs_this_frame_.begin(), unusable_inputs_this_frame_.end(),
                       [&key](const std::pair<raw_ptr<const RenderPass>, OutputIdentity>& entry) {
                           return entry.first == key.first && entry.second.target == key.second.target &&
                                  entry.second.attachment == key.second.attachment &&
                                  entry.second.depth == key.second.depth &&
                                  entry.second.declared == key.second.declared;
                       });
}

void RenderEngine::reportPlanProblems()
{
    for (const PlanProblem& problem : plan_problems_) {
        const auto previous = std::find_if(previous_problems_.begin(), previous_problems_.end(),
                                           [&problem](const PlanProblem& earlier) {
                                               return problem.message == earlier.message;
                                           });
        if (previous != previous_problems_.end()) {
            continue;   // the same problem, in the same words: this episode has already been said
        }
        reportEngineProblem(vn::graphics::DiagnosticSeverity::Warning,
                            vn::graphics::DiagnosticCategory::ContentSkipped, problem.message);
    }
    // What this compile found becomes what the next compile compares against: a problem that is gone drops out,
    // so the same breakage later is a new episode.
    previous_problems_.swap(plan_problems_);
    plan_problems_.clear();
}

intrusive_ptr<RenderPass> RenderEngine::insertSlot(Slot slot)
{
    if (slot.pass == nullptr) {
        return nullptr;
    }
    // Registering the same pass instance twice is ignored: it would otherwise run twice per frame. To rebind
    // content use bindPassContent(); to change the order remove the pass and re-add it.
    for (const auto& existing : slots_) {
        if (existing.pass.get() == slot.pass.get()) {
            return nullptr;
        }
    }
    // Keep the slots ascending by order; equal orders preserve insertion order (stable), so ties are resolved
    // by the addPass() call sequence.
    const int order = slot.order;
    const auto it   = std::find_if(slots_.begin(), slots_.end(),
                                   [order](const Slot& existing) { return existing.order > order; });
    const auto registered = slots_.insert(it, std::move(slot));
    return registered->pass;
}

intrusive_ptr<RenderPass> RenderEngine::addPass(const PassDesc& desc)
{
    // The declaration is turned into the execution node of its kind ONCE, and that node is the only storage of
    // it: a descriptor-declared pass and a hand-built one cannot drift apart, because by the time the engine
    // holds it, it IS a hand-built pass (which is also what keeps the escape hatch the same thing).
    intrusive_ptr<RenderPass> node;
    if (desc.kind == PassKind::Screen) {
        auto screen = make_intrusive<ScreenPass>();
        screen->setProgram(desc.program);
        node = std::move(screen);
    }
    else {
        node = make_intrusive<RenderPass>();
    }

    node->setName(desc.name);
    node->setCamera(desc.camera);
    if (desc.writes != nullptr) {
        node->setRenderTarget(desc.writes);
    }
    // The clear policy is applied only where the declaration states one: an unstated policy leaves the kind's
    // own (a Screen pass never clears, a Scene pass does), which is what keeps a descriptor and a hand-built
    // pass the same pass (see PassDesc::clear_enabled).
    if (desc.clear_enabled.has_value()) {
        node->setClearEnabled(*desc.clear_enabled);
    }
    if (desc.clear_color.has_value()) {
        node->setClearColor(*desc.clear_color);
    }
    if (desc.clear_depth.has_value()) {
        node->setShouldClearDepth(*desc.clear_depth);
    }
    node->setDepthMode(desc.depth_mode);
    node->setProgramOverride(desc.program_override);
    if (desc.polygon_mode.has_value()) {
        node->setPolygonModeOverride(*desc.polygon_mode);
    }
    if (desc.viewport.has_value()) {
        node->setViewport(*desc.viewport);
    }
    node->setOutputName(desc.output_name);
    node->setOutputTarget(desc.output_target);
    for (const auto& image : desc.inputs) {
        node->addInput(image);
    }
    for (const auto& target : desc.input_targets) {
        node->addInputTarget(target);
    }
    for (const auto& name : desc.input_names) {
        node->addInputName(name);
    }
    node->setEnabled(desc.enabled);

    Slot slot;
    slot.pass         = std::move(node);
    slot.content      = desc.content;
    slot.order        = desc.order.value_or(pipelineStageOrder(desc.stage));
    slot.tag          = desc.tag;
    slot.writes_image = desc.writes_image;
    slot.writes_shape = desc.writes_shape;
    slot.before_draw  = desc.before_draw;
    return insertSlot(std::move(slot));
}

VN_GRAPHICS_NS_END
