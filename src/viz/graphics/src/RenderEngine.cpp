#include <vine/graphics/RenderEngine.hpp>

#include <algorithm>
#include <chrono>
#include <functional>
#include <utility>

#include <vine/graphics/ImageRef.hpp>
#include <vine/graphics/RenderBackend.hpp>
#include <vine/graphics/RenderPass.hpp>
#include <vine/graphics/RenderTarget.hpp>
#include <vine/graphics/ScreenPass.hpp>
#include <vine/graphics/Scene.hpp>

VN_GRAPHICS_NS_BEGIN

namespace
{

}  // namespace

String RenderEngine::reportedPassName(raw_ptr<const RenderPass> pass)
{
    if (pass == nullptr || pass->name().empty()) {
        return String(u8"(unnamed)");
    }
    return pass->name();
}

VN_OBJECT_META_IMPL(RenderEngine, vn::Object);

RenderEngine::RenderEngine()
{
    // The ONE default in the shading path, and it is a named program: content with no program of its
    // own is shaded with the engine's forward program until the host says otherwise. (Null would mean
    // "draw such content not at all"; nothing is ever shaded with a program nobody named.)
    default_content_program_ = forwardProgram();
    // No implicit pipeline: the engine starts empty. The caller registers the
    // scene passes explicitly (addPass()) or through a RenderPipelineBuilder;
    // the primary interactive view (camera + content + navigation) lives in a
    // SceneView borrowing this engine.
}

RenderEngine::~RenderEngine()
{
    shutdown();
}

raw_ptr<RenderBackend> RenderEngine::backend() const
{
    return backend_.get();
}

void RenderEngine::setBackend(intrusive_ptr<RenderBackend> backend)
{
    // Re-assigning the same backend is a no-op (idempotent).
    if (backend_ == backend) {
        return;
    }
    backend_ = std::move(backend);
    // The backend reports through THIS engine's one route (see Diagnostics.hpp): attaching it is what
    // makes the engine's count the whole truth instead of half of it, and it is why a backend set after
    // the sink was installed needs nothing else done to it - the sink lives in the route, not in a copy.
    if (backend_ != nullptr) {
        backend_->setDiagnosticsRoute(diagnostics_);
    }
}

void RenderEngine::setDiagnosticSink(DiagnosticSink sink)
{
    diagnostics_.setSink(std::move(sink));
}

std::size_t RenderEngine::diagnosticCount() const noexcept
{
    return static_cast<std::size_t>(diagnostics_.total());
}

std::size_t RenderEngine::diagnosticCount(DiagnosticCategory category) const noexcept
{
    return static_cast<std::size_t>(diagnostics_.count(category));
}

bool RenderEngine::diagnosticsClean() const noexcept
{
    return diagnostics_.clean();
}

void RenderEngine::reportEngineProblem(vn::graphics::DiagnosticSeverity severity,
                                       vn::graphics::DiagnosticCategory category,
                                       const String&                message)
{
    if (collecting_problems_) {
        // A compile is running: what the checks FOUND is collected, and reportPlanProblems() decides what is
        // said out loud - once per episode, by comparing this list with the previous compile's (see it). The
        // checks therefore keep reporting the way they always did, and the episode rule lives in one place.
        noteProblem(message);
        return;
    }
    diagnostics_.report(severity, category, message);
}

bool RenderEngine::frameCounters(FrameCounters& counters) const
{
    return backend_ != nullptr && backend_->frameCounters(counters);
}

bool RenderEngine::retentionStats(RetentionStats& stats) const
{
    return backend_ != nullptr && backend_->retentionStats(stats);
}

bool RenderEngine::frameProgress(FrameProgress& progress) const
{
    return backend_ != nullptr && backend_->frameProgress(progress);
}

bool RenderEngine::initialize()
{
    if (backend_ == nullptr) {
        return false;
    }
    if (native_handle_ != nullptr) {
        backend_->setWindowHandle(native_handle_);
    }
    // Forward the default content program before the backend builds its shader sets.
    backend_->setDefaultContentProgram(default_content_program_);
    initialized_ = backend_->initialize();
    if (initialized_) {
        // A size announced before initialization (a host sizes its widget before it has a backend)
        // never reached the backend: it can only honour the announcement once it has a surface.
        // Hand it over BEFORE the warm-up, so the content compiled there is built at the right size
        // instead of the backend's default.
        if (frame_ctx_.surface_width > 0 && frame_ctx_.surface_height > 0) {
            backend_->resize(frame_ctx_.surface_width, frame_ctx_.surface_height);
        }
        // Pre-frame warm-up: execute every enabled, non-clearing pass once so
        // the backend builds and compiles its content before the first frame
        // is submitted. Such passes (top / HUD content that draws over the
        // main view, e.g. the axis gizmo) carry their own scene and window
        // layer; compiling content first encountered mid-frame (i.e. inside
        // frame()) has proven unreliable in the vsg backend, and the main
        // content is pre-compiled during backend initialize. Re-execution in
        // later frames is a no-op for already-built content. Each pass is
        // opened as a pass scope and announced with its explicit order so the
        // backend can key / stack its retained per-pass state correctly even
        // though the warm-up runs non-clearing passes ahead of the clearing
        // ones.
        for (const auto& slot : slots_) {
            RenderPass* pass   = slot.pass.get();
            Scene*      content = slot.content.get();
            if (pass == nullptr || content == nullptr || !pass->isEnabled() || pass->isClearEnabled()) {
                continue;
            }
            backend_->beginPass(pass);
            backend_->setPassOrder(slot.order);
            pass->execute(content, backend_.get());
            backend_->endPass();
        }
    }
    return initialized_;
}

void RenderEngine::shutdown()
{
    if (backend_ != nullptr && initialized_) {
        backend_->shutdown();
    }
    initialized_ = false;
}

raw_ptr<const RenderPass> RenderEngine::passNeedingAnUnsupportedTarget() const
{
    if (backend_ == nullptr || backend_->supportsRenderTargets()) {
        return nullptr;
    }
    // Only ENABLED passes matter: a disabled pass is skipped, so its target is never asked for.
    for (const Slot& slot : slots_) {
        if (slot.pass != nullptr && slot.pass->isEnabled() && slot.pass->renderTarget() != nullptr) {
            return slot.pass.get();
        }
    }
    return nullptr;
}

void RenderEngine::frame(double dt)
{
    if (!initialized_ || backend_ == nullptr) {
        return;
    }
    frame_ctx_.dt = dt;
    backend_->beginFrame();

    // Announce the content frame BEFORE the passes run: every scene this frame
    // renders may then memoise the command list it collects (see
    // Scene::setContentFrame), so the multi-pass pipelines that draw one scene
    // through one camera several times per frame (G-buffer + lighting + ...)
    // walk the tree once instead of once per pass. The token is idempotent, so
    // a scene shared by several slots is announced any number of times.
    ++content_frame_;
    for (const auto& slot : slots_) {
        if (slot.content != nullptr) {
            slot.content->setContentFrame(content_frame_);
        }
    }

    // A frame starts by COMPILING its declarations: which pass runs when, what its declared inputs resolve
    // to, which transient target it draws into, and what is wrong with the set (see compileFrame). Nothing
    // tells the engine that a declaration moved - reading the declarations IS the compile, so a re-wired
    // pass lands in THIS frame and there is no revision to keep in step, no gate to forget, and no second
    // table (the old wiring registry) that had to be rebuilt per frame to answer the same questions.
    compileFrame();

    // The problems the compile found are reported once per episode: what the previous compile already
    // found is not said again, and what is gone is forgotten (see reportPlanProblems).
    reportPlanProblems();

    // A backend reporting no off-screen target support IGNORES a non-null target, so a pass staged
    // through one is recorded where the pipeline never put it — a WRONG PICTURE rather than a slow one,
    // and silent unless the engine asks (see RenderBackend::supportsRenderTargets). Asked here, once per
    // frame, and reported as an EPISODE: the pass is still run (its content is the host's to place), but
    // the host is told that the pipeline it built cannot mean what it says.
    if (raw_ptr<const RenderPass> unsupported = passNeedingAnUnsupportedTarget(); unsupported != nullptr) {
        if (!unsupported_target_reported_) {
            unsupported_target_reported_ = true;
            reportEngineProblem(vn::graphics::DiagnosticSeverity::Warning,
                                vn::graphics::DiagnosticCategory::TargetBuildFailed,
                                String(u8"the backend reports no off-screen render target support, so pass '") +
                                    reportedPassName(unsupported) +
                                    String(u8"' (and any other pass rendering into a target) is not drawn where "
                                           u8"the pipeline expects it: the backend ignores the target and draws "
                                           u8"into the window"));
        }
    }
    else {
        // The pipeline stopped asking for what the backend cannot do, so a later ask is a new problem.
        unsupported_target_reported_ = false;
    }

    // Ordered pipeline in ascending order: negative orders run first (shadow
    // / depth / g-buffer pre-pass), the window-present pass (master camera,
    // null render target) conventionally sits at order 0, and positive orders
    // run after (post-processing / compositing, then top / HUD passes). The
    // pipeline is exactly what the caller registered - the engine auto-
    // registers nothing - and the COMPILED plan says what each pass will do
    // (its resolved inputs and target included), so this loop decides nothing:
    // it runs the rows. Disabled passes are skipped by the plan's own `enabled`
    // answer, and their row keeps saying they cost nothing (see
    // FramePlan::PassPlan::record_micros).
    for (std::size_t index = 0; index < plan_rows_.size(); ++index) {
        Slot&         slot = slots_[index];
        const PlanRow& row = plan_rows_[index];
        RenderPass*   pass = row.pass;
        if (pass == nullptr || !pass->isEnabled()) {
            // A pass that does not run this frame has no cost this frame: its row keeps saying so instead of
            // showing what it cost the last time it ran.
            slot.record_micros = 0;
            continue;
        }
        // The pass' own record cost, measured here for the frame plan: this is the host-visible half of a frame
        // (the GPU half is the backend's, see RenderEngine::frameCounters), and it is what "where does the first
        // frame's build go" is answered with. One clock read per pass, not per draw.
        const auto pass_started = std::chrono::steady_clock::now();
        // Content that updates itself per frame (an FPS readout, a mirrored gizmo) does it here, through the
        // hook its declaration carried: it writes CONTENT, so it happens before the pass states anything.
        if (slot.before_draw) {
            slot.before_draw();
        }
        // Pass scope: the backend is told which pass is running (its identity
        // for retained per-pass GPU state, and that it is active this frame),
        // runs the whole pass, then the scope is closed so per-pass state a
        // pass did not consume cannot leak into the next pass.
        backend_->beginPass(pass);
        backend_->setPassOrder(row.order);
        // The pass' inputs were resolved by the compile and are announced before it draws: they are a property
        // of the pass, so every draw call in it sees the same list (and a pass that declared none announces
        // none). The pass is told the same list, because the inputs are what an effect reads.
        pass->resolveInputTextures(row.inputs);
        backend_->setPassInputs(row.inputs);
        drawScenePass(pass, row.content);
        backend_->endPass();
        slot.record_micros = static_cast<std::uint64_t>(
            std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - pass_started)
                .count());
    }

    backend_->endFrame();
    backend_->swapBuffers();
}

const FrameContext& RenderEngine::frameContext() const
{
    return frame_ctx_;
}

std::optional<int> RenderEngine::passOrder(raw_ptr<RenderPass> pass) const
{
    for (const auto& slot : slots_) {
        if (slot.pass == pass) {
            return slot.order;
        }
    }
    return std::nullopt;
}

FramePlan RenderEngine::framePlan() const
{
    FramePlan plan;
    plan.passes.reserve(slots_.size());
    for (const Slot& slot : slots_) {
        const RenderPass* const pass = slot.pass.get();
        if (pass == nullptr) {
            continue;
        }
        PassPlan entry;
        entry.pass          = pass;
        entry.order         = slot.order;
        entry.name          = pass->name();
        entry.tag           = slot.tag;
        entry.content       = slot.content.get();
        entry.camera        = pass->camera();
        entry.target        = pass->renderTarget();
        entry.enabled       = pass->isEnabled();
        entry.clears        = pass->isClearEnabled();
        entry.clear_color   = pass->clearColor();
        entry.clears_depth  = pass->shouldClearDepth();
        entry.inputs        = pass->inputNames();
        entry.output        = pass->outputName();
        entry.record_micros = slot.record_micros;
        if (pass->hasViewport()) {
            entry.viewport = pass->viewport();
        }
        plan.passes.push_back(std::move(entry));
    }
    return plan;
}

void RenderEngine::addPass(intrusive_ptr<RenderPass> pass, int order)
{
    addPass(std::move(pass), nullptr, order);
}

void RenderEngine::addPass(intrusive_ptr<RenderPass> pass, intrusive_ptr<Scene> content, int order)
{
    Slot slot;
    slot.pass    = std::move(pass);
    slot.content = std::move(content);
    slot.order   = order;
    insertSlot(std::move(slot));
}

void RenderEngine::removePass(raw_ptr<RenderPass> pass)
{
    const auto old_size = slots_.size();
    // The order the pass ran at is not kept anywhere: the slot list IS the answer (see passOrder), and a pass
    // that is gone has none.
    raw_ptr<RenderTarget> transient_owned = nullptr;
    for (const Slot& slot : slots_) {
        if (slot.pass.get() == pass) {
            transient_owned = slot.transient.get();
            break;
        }
    }
    slots_.erase(std::remove_if(slots_.begin(), slots_.end(),
                                [pass](const Slot& slot) { return slot.pass.get() == pass; }),
                 slots_.end());

    // A pass is registered at most once, so any removal drops its only user:
    // release the backend state it retained — keyed by the pass itself (see
    // RenderBackend::releasePass) — plus any off-screen target the pass owns. A
    // TRANSIENT it declared is not its own (this engine owns it, and the next compile lets it go when nothing
    // declares it any more), so it is not released here: that would hand the backend a target the engine still
    // holds for another pass declaring the same one.
    if (backend_ != nullptr && pass != nullptr && slots_.size() != old_size) {
        backend_->releasePass(pass);
        if (raw_ptr<RenderTarget> target = pass->renderTarget(); target != nullptr && target != transient_owned) {
            backend_->releaseRenderTarget(target);
        }
    }
}

void RenderEngine::clearPasses()
{
    // Snapshot the registered passes (and the transients they declared), drop all slots, then release the
    // backend resources each removed pass owned (see RenderBackend::releasePass). A transient is this engine's,
    // not the pass': the compile lets it go when nothing declares it any more.
    std::vector<raw_ptr<RenderPass>> removed;
    removed.reserve(slots_.size());
    for (const auto& slot : slots_) {
        if (slot.pass != nullptr) {
            removed.push_back(slot.pass.get());
        }
    }
    std::vector<raw_ptr<RenderTarget>> transients;
    transients.reserve(transients_.size());
    for (const TransientTarget& transient : transients_) {
        transients.push_back(transient.target.get());
    }
    transients_.clear();

    slots_.clear();

    if (backend_ != nullptr) {
        for (RenderPass* pass : removed) {
            if (pass != nullptr) {
                backend_->releasePass(pass);
            }
            raw_ptr<RenderTarget> target = (pass != nullptr) ? pass->renderTarget() : nullptr;
            const bool            owned  = std::find(transients.begin(), transients.end(), target) != transients.end();
            if (target != nullptr && !owned) {
                backend_->releaseRenderTarget(target);
            }
        }
        for (const raw_ptr<RenderTarget> transient : transients) {
            backend_->releaseRenderTarget(transient);
        }
    }
}

std::size_t RenderEngine::passCount() const
{
    return slots_.size();
}

void RenderEngine::bindPassContent(raw_ptr<RenderPass> pass, intrusive_ptr<Scene> content)
{
    for (auto& slot : slots_) {
        if (slot.pass.get() == pass) {
            slot.content = std::move(content);
            return;
        }
    }
}

raw_ptr<Scene> RenderEngine::contentOf(raw_ptr<RenderPass> pass) const
{
    for (const auto& slot : slots_) {
        if (slot.pass.get() == pass) {
            return slot.content.get();
        }
    }
    return nullptr;
}

void RenderEngine::drawScenePass(raw_ptr<RenderPass> pass, raw_ptr<Scene> content)
{
    // The pass decides what a null content means: the base RenderPass draws
    // nothing (execute returns when the scene is null), while content-agnostic
    // passes such as ScreenPass still execute against their resolved inputs.
    if (pass != nullptr) {
        pass->execute(content, backend_.get());
    }
}

raw_ptr<RenderTarget> RenderEngine::resolveDeclaredTarget(raw_ptr<RenderPass> pass, raw_ptr<RenderTarget> target)
{
    // Resolved against what the frame has produced so far (plan_produced_): a target the frame fills LATER is
    // not something this pass can read, which is the same answer the per-pass resolution used to give.
    if (plan_produced_.end() != std::find(plan_produced_.begin(), plan_produced_.end(),
                                          static_cast<raw_ptr<const RenderTarget>>(target))) {
        return target;
    }
    // A pass reading what it draws into is the feedback pattern, and whether the actual draw call is
    // a feedback loop is the BACKEND's to judge (the vsg backend rejects source == destination and
    // says so): this layer stays quiet about it, so one mistake is reported once.
    if (pass->renderTarget() == target) {
        return nullptr;
    }
    reportUnproducedInput(pass, OutputIdentity::targetOf(*target), String(u8"the input target '") +
                                                                    (target->name().empty()
                                                                         ? String(u8"(unnamed target)")
                                                                         : target->name()) +
                                                                    u8"'");
    return nullptr;
}

raw_ptr<RenderTarget> RenderEngine::resolveDeclaredImage(raw_ptr<RenderPass> pass, const ImageRef& image)
{
    raw_ptr<RenderTarget> target = image.target();
    if (target == nullptr) {
        // An unbound declaration names nothing that can be read this frame. The wiring check already
        // reports the declaration itself; nothing more to say per frame.
        return nullptr;
    }
    const bool depth_usable = (image.kind() != ImageRef::Kind::Depth) || target->hasDepth();
    const bool produced     = plan_produced_.end() != std::find(plan_produced_.begin(), plan_produced_.end(),
                                                                static_cast<raw_ptr<const RenderTarget>>(target));
    if (depth_usable && produced) {
        return target;
    }
    // Same as above: a pass that reads the target it draws into is the feedback pattern, judged by
    // the backend (the depth case included — its layout is the backend's business).
    if (pass->renderTarget() == target) {
        return nullptr;
    }
    reportUnproducedInput(pass, OutputIdentity::of(image),
                          String(u8"the input image '") + image.label() + u8"'");
    return nullptr;
}

void RenderEngine::reportUnproducedInput(raw_ptr<RenderPass> pass, const OutputIdentity& identity,
                                         const String& what)
{
    // A wire the structural check already reported THIS compile (no producer at all, or one registered too
    // late) is ONE problem: the resolution does not repeat it in frame terms. Both are collected by the same
    // compile (see collectPlanProblems), so this is a question about this frame, not about the last episode.
    if (unusableInputReported(pass, identity)) {
        return;
    }
    reportEngineProblem(vn::graphics::DiagnosticSeverity::Warning,
                        vn::graphics::DiagnosticCategory::ContentSkipped,
                        String(u8"pass '") + reportedPassName(pass) + String(u8"' declares ") + what +
                            String(u8" but nothing produced it this frame, so the pass draws nothing"
                                   u8" (check the producer's order and its enabled state)"));
}

RenderEngine::OutputIdentity RenderEngine::OutputIdentity::of(const ImageRef& image) noexcept
{
    OutputIdentity identity;
    identity.target     = image.target();
    identity.attachment = image.attachment();
    identity.depth      = (image.kind() == ImageRef::Kind::Depth);
    // Only while there is no address: the object is all the identity that exists yet.
    identity.declared = (identity.target == nullptr) ? &image : nullptr;
    return identity;
}

RenderEngine::OutputIdentity RenderEngine::OutputIdentity::colorOf(const RenderTarget& target, int attachment) noexcept
{
    OutputIdentity identity;
    identity.target     = &target;
    identity.attachment = attachment;
    return identity;
}

RenderEngine::OutputIdentity RenderEngine::OutputIdentity::depthOf(const RenderTarget& target) noexcept
{
    OutputIdentity identity;
    identity.target     = &target;
    identity.attachment = 0;   // a depth attachment has no colour index; `depth` is what tells them apart
    identity.depth      = true;
    return identity;
}

RenderEngine::OutputIdentity RenderEngine::OutputIdentity::targetOf(const RenderTarget& target) noexcept
{
    OutputIdentity identity;
    identity.target     = &target;
    identity.attachment = kWholeTarget;
    return identity;
}

bool RenderEngine::OutputIdentity::operator<(const OutputIdentity& other) const noexcept
{
    if (target != other.target) {
        // std::less, not the built-in `<`: comparing unrelated pointers with `<` is unspecified,
        // and these keys are addresses of distinct objects (as CollisionPair does for pairs).
        return std::less<const void*>{}(target, other.target);
    }
    if (target == nullptr) {
        // Both unbound: the declared object is the only identity there is.
        return std::less<const void*>{}(declared, other.declared);
    }
    if (depth != other.depth) {
        return depth < other.depth;
    }
    return attachment < other.attachment;
}


void RenderEngine::collectPlanProblems()
{
    // Every pass on its own is valid, so only the whole table can see these; and they are properties of the
    // DECLARATION, not of a frame's outcome. Run as part of the compile, so the answer is about THIS frame;
    // what is REPORTED is decided once, by comparing this list with the previous compile's (see
    // reportPlanProblems) - the checks below only say what they found.
    //
    // Two phases, because a consumer may be registered before its producer: the producers have to be collected
    // before any consumer is judged against them.
    struct Declaration {
        raw_ptr<const ImageRef>     image  = nullptr;   // the fine declaration, when there was one
        raw_ptr<const RenderTarget> target = nullptr;   // the target the declaration is about
        raw_ptr<const RenderPass>   pass   = nullptr;   // who declared it
        std::size_t                 index  = 0;         // its position in the draw order
    };

    const auto target_name = [](raw_ptr<const RenderTarget> target) -> String {
        if (target == nullptr) {
            return String(u8"(no target)");
        }
        return target->name().empty() ? String(u8"(unnamed target)") : target->name();
    };
    /// What a pass writes, for a message about a promise it cannot keep (null render target = the
    /// window, which is a wire of its own).
    const auto drawn_into_name = [&target_name](raw_ptr<const RenderPass> pass) -> String {
        raw_ptr<const RenderTarget> drawn_into = pass->renderTarget();
        return (drawn_into == nullptr) ? String(u8"the window") : target_name(drawn_into);
    };
    const auto identity_name = [&target_name](const OutputIdentity& identity) -> String {
        if (identity.attachment == OutputIdentity::kWholeTarget) {
            return String(u8"the images of target '") + target_name(identity.target) + u8"'";
        }
        if (identity.depth) {
            return String(u8"the depth of target '") + target_name(identity.target) + u8"'";
        }
        return String(u8"an image of target '") + target_name(identity.target) + u8"'";
    };

    /// Who fills what: the lowest-indexed pass that renders into a target or promises the whole of
    /// it. Keyed by TARGET, because filling a target fills every image it has. A DISABLED pass still
    /// declares its wire: toggling a pass off is not a wiring mistake, it is a paused pass.
    std::map<raw_ptr<const RenderTarget>, Declaration> filled;
    /// Who claims which image: a fine promise, and the images of a coarse one. Two claimants of one
    /// image is what the collision report is about.
    std::map<OutputIdentity, Declaration> claimed;

    std::set<OutputIdentity>              colliding_images;
    // Who fills what / who claims what: the two maps above. The other sets the checks used to fill only for
    // the episode memory are gone - a problem is the sentence the collect holds (see reportPlanProblems), and
    // the one cross-check that needs a subject (the structural check speaking before the resolution does) uses
    // unusable_inputs_this_frame_ directly.
    // image -> (first pass that declares it, whether as its OUTPUT) — reported in phase 4, after the
    // collision check had its say about the same image.
    std::map<raw_ptr<const ImageRef>, std::pair<raw_ptr<const RenderPass>, bool>> unbound_declared_by;

    // Phase 1: who fills and who claims. The fillers come from ALL passes (a declaration is a
    // declaration), the claims from the ENABLED ones only — what the collision report is about is
    // "whoever runs last silently wins", and a disabled pass runs nothing.
    //
    // A host binding (publish) is filled by the HOST: it is there from the start of the frame and no
    // pass owns its hand-off, so a declared input addressing it is answered like one addressing a
    // pass' target.
    for (const auto& binding : wiring_.host_outputs_) {
        if (binding.second != nullptr) {
            filled.emplace(binding.second.get(), Declaration{ nullptr, binding.second.get(), nullptr, 0 });
        }
    }

    for (std::size_t index = 0; index < slots_.size(); ++index) {
        RenderPass* pass = slots_[index].pass.get();
        if (pass == nullptr) {
            continue;
        }

        // Drawing into a target fills it: every attachment it has holds this pass' content
        // afterwards. This is the physical fact the input rules below are answered from.
        if (raw_ptr<RenderTarget> drawn_into = pass->renderTarget(); drawn_into != nullptr) {
            filled.emplace(drawn_into, Declaration{ nullptr, drawn_into, pass, index });
        }

        if (!pass->isEnabled()) {
            continue;
        }

        // A promise that names a WHOLE target: it claims every image that target has (the
        // declaration stays shape-agnostic; only the comparison is per image).
        if (raw_ptr<RenderTarget> promised = pass->outputTarget(); promised != nullptr) {
            filled.emplace(promised, Declaration{ nullptr, promised, pass, index });

            // A promise is a claim about the content a consumer gets, so it has to be about the
            // target this pass WRITES: promising a target the pass never draws into is a claim about
            // nothing, and it would make the collision report fire on wires that do not exist.
            if (pass->renderTarget() != promised) {
                const OutputIdentity identity = OutputIdentity::targetOf(*promised);
                {
                    reportEngineProblem(vn::graphics::DiagnosticSeverity::Warning,
                                        vn::graphics::DiagnosticCategory::ContentSkipped,
                                        String(u8"pass '") + reportedPassName(pass) + String(u8"' promises target '") +
                                            target_name(promised) + String(u8"' as its output but renders into '") +
                                            drawn_into_name(pass) +
                                            String(u8"', so the promise is about content this pass never writes"));
                }
            }

            std::vector<OutputIdentity> promised_images;
            promised_images.reserve(static_cast<std::size_t>(promised->colorCount()) + 1);
            for (int attachment = 0; attachment < promised->colorCount(); ++attachment) {
                promised_images.push_back(OutputIdentity::colorOf(*promised, attachment));
            }
            if (promised->hasDepth()) {
                // The depth of a promised target is part of the promise, and it is a DIFFERENT
                // image from colour attachment 0 (which is why the identity carries the kind).
                promised_images.push_back(OutputIdentity::depthOf(*promised));
            }

            for (const auto& identity : promised_images) {
                const auto entry = claimed.find(identity);
                if (entry == claimed.end()) {
                    claimed.emplace(identity, Declaration{ nullptr, promised, pass, index });
                    continue;
                }
                if (entry->second.pass == pass) {
                    continue;   // one pass claiming twice is one claim
                }
                colliding_images.insert(identity);
                {
                    reportEngineProblem(vn::graphics::DiagnosticSeverity::Warning,
                                        vn::graphics::DiagnosticCategory::ContentSkipped,
                                        String(u8"two passes ('") + reportedPassName(entry->second.pass) +
                                            String(u8"' and '") + reportedPassName(pass) + String(u8"') claim ") +
                                            identity_name(identity) +
                                            String(u8" as their output, so a consumer of it gets whichever pass"
                                                   u8" runs last"));
                }
            }
        }

        // A fine promise that names ONE image: the address is the claim, so two passes that each
        // declared their OWN ImageRef for one image are caught as well — the host may build its
        // wiring out of two references to the same attachment and never notice.
        if (raw_ptr<ImageRef> output = pass->output(); output != nullptr) {
            // An UNBOUND promise cannot be checked at all: there is no target to compare with the
            // one this pass renders into, so a promise about nothing would pass unnoticed — and no
            // consumer could resolve it either (an unbound identity has no address). Reported once
            // per image, here (the producer side) so the consumer side below stays quiet about the
            // same object.
            if (!output->isBound()) {
                // Recorded, not reported yet: if another pass also claims this image, the collision
                // report below names both passes and the image, and binding the image alone would not
                // fix a double claim — one mistake, one message. Phase 4 reports the rest.
                unbound_declared_by.emplace(output, std::make_pair(raw_ptr<const RenderPass>(pass), true));
            }
            // Same rule as the coarse form: a fine promise is about a target this pass has to write.
            raw_ptr<RenderTarget> promised_target = output->target();
            if (promised_target != nullptr && pass->renderTarget() != promised_target) {
                const OutputIdentity identity = OutputIdentity::of(*output);
                {
                    reportEngineProblem(vn::graphics::DiagnosticSeverity::Warning,
                                        vn::graphics::DiagnosticCategory::ContentSkipped,
                                        String(u8"pass '") + reportedPassName(pass) + String(u8"' promises the image '") +
                                            output->label() + String(u8"' of target '") + target_name(promised_target) +
                                            String(u8"' as its output but renders into '") + drawn_into_name(pass) +
                                            String(u8"', so the promise is about content this pass never writes"));
                }
            }
            const OutputIdentity identity = OutputIdentity::of(*output);
            const auto           entry    = claimed.find(identity);
            if (entry == claimed.end()) {
                claimed.emplace(identity, Declaration{ output, output->target(), pass, index });
            }
            else if (entry->second.pass != pass) {
                colliding_images.insert(identity);
                {
                    const String first  = reportedPassName(entry->second.pass);
                    const String second = reportedPassName(pass);
                    // One format string per branch: the cases do not print the same thing (one image
                    // declared twice, two images that turn out to be one attachment, or an image
                    // claimed next to a whole target).
                    const String message =
                        (entry->second.image == output)
                            ? String(u8"image '") + output->label() +
                                  String(u8"' is declared as the output of two passes ('") + first +
                                  String(u8"' and '") + second +
                                  String(u8"'); a consumer of it gets whichever pass runs last")
                            : (entry->second.image != nullptr)
                                  ? String(u8"two passes ('") + first + String(u8"' and '") + second +
                                        String(u8"') declared DIFFERENT images ('") +
                                        entry->second.image->label() + String(u8"' and '") + output->label() +
                                        String(u8"') that are the same attachment of one target; a consumer of it"
                                               u8" gets whichever pass runs last")
                                  : String(u8"two passes ('") + first + String(u8"' and '") + second +
                                        String(u8"') claim the image '") + output->label() +
                                        String(u8"' of target '") + target_name(output->target()) +
                                        String(u8"', one of them as part of the whole target; a consumer of it gets"
                                               u8" whichever pass runs last");
                    reportEngineProblem(vn::graphics::DiagnosticSeverity::Warning,
                                        vn::graphics::DiagnosticCategory::ContentSkipped,
                                        message);
                }
            }
        }
    }

    // Phase 2: every declared input its consumer cannot draw from — nobody fills the image (or the
    // whole target), the only filler is registered after the consumer, or the image asked for is a
    // depth the target does not have. This is the structural half of D37 (design §14.4); its
    // name-based counterpart can only be judged once a frame has run, because a name resolves per
    // frame.
    for (std::size_t index = 0; index < slots_.size(); ++index) {
        RenderPass* pass = slots_[index].pass.get();
        if (pass == nullptr) {
            continue;
        }

        for (const auto& target : pass->inputTargets()) {
            if (target == nullptr) {
                continue;
            }
            const OutputIdentity identity = OutputIdentity::targetOf(*target);
            const auto           entry    = filled.find(target.get());
            // A pass reading what it draws into is a feedback loop the backend supports on purpose
            // (an overlay reading the frame it is about to composite into): never a bogus wire.
            if (entry != filled.end() && entry->second.pass == pass) {
                continue;
            }
            if (entry != filled.end() && entry->second.index < index) {
                continue;   // filled before this pass draws
            }
            // The structural check speaks for this wire; the collect keeps its subject so the RESOLUTION of the
            // same input stays quiet about it (see unusableInputReported / reportUnproducedInput).
            unusable_inputs_this_frame_.emplace(pass, identity);
            {
                const String consumer = reportedPassName(pass);
                // One format string per branch: "nobody writes it" and "its producer runs too late"
                // are different problems with different fixes.
                const String message =
                    (entry == filled.end())
                        ? String(u8"pass '") + consumer + String(u8"' declares the input target '") +
                              target_name(target.get()) +
                              String(u8"' but no pass writes it, so nothing fills it")
                        : String(u8"pass '") + consumer + String(u8"' declares the input target '") +
                              target_name(target.get()) + String(u8"' but its producer '") +
                              reportedPassName(entry->second.pass) +
                              String(u8"' is registered after it, so the target is still a frame behind when this"
                                     u8" pass draws");
                reportEngineProblem(vn::graphics::DiagnosticSeverity::Warning,
                                    vn::graphics::DiagnosticCategory::ContentSkipped,
                                    message);
            }
        }

        for (const auto& image : pass->inputs()) {
            if (image == nullptr) {
                continue;
            }
            const OutputIdentity identity = OutputIdentity::of(*image);
            // An UNBOUND image can never be delivered (resolveDeclaredImage returns null for it, and
            // deliberately says nothing — declaring it is what is wrong). Reported once per IMAGE:
            // by the producer side above when a pass declares it as its output, otherwise here.
            if (!image->isBound()) {
                unbound_declared_by.emplace(image.get(), std::make_pair(raw_ptr<const RenderPass>(pass), false));
                continue;
            }
            // An unbound image has no address, so no target account can say anything about it: only
            // a promise of that same object can.
            const auto claim = claimed.find(identity);
            const auto entry = (identity.target != nullptr) ? filled.find(identity.target) : filled.end();

            // A pass that promises the very image it reads, or draws into its target, is a feedback
            // loop the backend supports on purpose (an overlay reading the frame it composites
            // into): never a bogus wire.
            if ((claim != claimed.end() && claim->second.pass == pass) ||
                (entry != filled.end() && entry->second.pass == pass)) {
                continue;
            }

            // Whoever can fill the image: the EARLIEST of the pass that promises this very image and
            // the pass that draws into its target (drawing into a target fills all of its images).
            // A filler may have NO pass at all: a host binding is filled by the host.
            std::size_t               producer_index = slots_.size();
            raw_ptr<const RenderPass> producer       = nullptr;
            if (claim != claimed.end() && claim->second.index < producer_index) {
                producer       = claim->second.pass;
                producer_index = claim->second.index;
            }
            if (entry != filled.end() && entry->second.index < producer_index) {
                producer       = entry->second.pass;
                producer_index = entry->second.index;
            }
            const bool has_filler = (producer_index < slots_.size());

            if (has_filler && producer_index < index &&
                (!identity.depth || identity.target->hasDepth())) {
                continue;   // filled in time
            }

            // The structural check speaks for this wire; the collect keeps its subject so the RESOLUTION of the
            // same input stays quiet about it (see unusableInputReported / reportUnproducedInput).
            unusable_inputs_this_frame_.emplace(pass, identity);
            {
                const String consumer = reportedPassName(pass);
                // One format string per branch: "nobody declares it", "the target has no depth" and
                // "the producer runs too late" are different problems with different fixes.
                const String message =
                    !has_filler
                        ? String(u8"pass '") + consumer + String(u8"' declares the input image '") +
                              image->label() +
                              String(u8"' but no pass declares it as an output, so nothing fills it")
                        : (identity.depth && !identity.target->hasDepth())
                              ? String(u8"pass '") + consumer + String(u8"' declares the input image '") +
                                    image->label() + String(u8"' (the depth of target '") +
                                    target_name(identity.target) +
                                    String(u8"') but that target has no depth attachment, so nothing fills it")
                              : String(u8"pass '") + consumer + String(u8"' declares the input image '") +
                                    image->label() + String(u8"' but its producer '") + reportedPassName(producer) +
                                    String(u8"' is registered after it, so the image is still a frame behind when"
                                           u8" this pass draws");
                reportEngineProblem(vn::graphics::DiagnosticSeverity::Warning,
                                    vn::graphics::DiagnosticCategory::ContentSkipped,
                                    message);
            }
        }
    }

    // Phase 3: a ScreenPass that cannot draw what it declares. Every one of these is a property of
    // the DECLARATION (nothing else can see them: the backend only ever sees "a pass with nothing to
    // draw"), they are found by the compile (so the frame they belong to is the frame that says so), and
    // they are checked in the order a host has to fix them — each check `continue`s, because the next one is
    // moot until this one is answered and one mistake should read as one message.
    for (const auto& slot : slots_) {
        auto* pass = dynamic_cast<ScreenPass*>(slot.pass.get());
        if (pass == nullptr || !pass->isEnabled()) {
            continue;
        }

        // 1. No program: the shading is a CHOICE the host makes (a ScreenPass has no implicit one —
        // there is no "plain copy" a backend fills in), so a pass without a program draws nothing.
        // This is the first thing to answer, and it is the one a host is most likely to hit: the
        // picture a pass draws is exactly what it named.
        if (pass->program() == nullptr) {
            {
                reportEngineProblem(vn::graphics::DiagnosticSeverity::Warning,
                                    vn::graphics::DiagnosticCategory::ContentSkipped,
                                    String(u8"pass '") + reportedPassName(pass) +
                                        String(u8"' is a ScreenPass with no program, so it can never draw: name one"
                                               u8" (BuiltinShaders::screenCopyProgram() for a plain copy of the"
                                               u8" source's colour attachment, deferredLightProgram() for deferred"
                                               u8" lighting, or your own fragment stage)"));
            }
            continue;
        }

        // 2. No input at all (no image, no target, no name): it samples one input and returns without
        // it. Not "an input that failed to resolve" (resolvePassInputs), nor "an input nobody fills"
        // (phase 2).
        if (pass->inputs().empty() && pass->inputTargets().empty() && pass->inputNames().empty()) {
            {
                reportEngineProblem(vn::graphics::DiagnosticSeverity::Warning,
                                    vn::graphics::DiagnosticCategory::ContentSkipped,
                                    String(u8"pass '") + reportedPassName(pass) +
                                        String(u8"' is a ScreenPass that declares no input, so it can never draw"
                                               u8" (give it addInput / addInputName)"));
            }
            continue;
        }

        // 3. No camera: the fullscreen path builds the pass' view from it and pushes the scene's
        // lights through that view space, so without one ScreenPass::execute returns before asking the
        // backend for anything. The fix is static (setCamera), and a host would otherwise see a
        // post-process that never appears with no reason for it.
        if (pass->camera() == nullptr) {
            {
                reportEngineProblem(vn::graphics::DiagnosticSeverity::Warning,
                                    vn::graphics::DiagnosticCategory::ContentSkipped,
                                    String(u8"pass '") + reportedPassName(pass) +
                                        String(u8"' has a fullscreen program but no camera, so it can never draw: the"
                                               u8" fullscreen path builds its view from the pass camera (and pushes the"
                                               u8" scene's lights through it) — give the pass a camera (setCamera)"));
            }
        }
    }

    // Phase 4: a declared image nobody bound to a target. An unbound identity has no address, so
    // nothing can be delivered through it (resolveDeclaredImage() returns null and says nothing,
    // because declaring it is what is wrong) and the promise check had nothing to compare with the
    // target the pass renders into. One report per IMAGE: a producer and its consumers share the same
    // unbound object. An image whose double claim the collision check already reported is skipped —
    // that message names both passes and the fix, and one mistake gets one message.
    for (const auto& [image, declarer] : unbound_declared_by) {
        if (colliding_images.count(OutputIdentity::of(*image)) != 0) {
            continue;
        }
        const auto& [pass, as_output] = declarer;
        reportEngineProblem(vn::graphics::DiagnosticSeverity::Warning,
                            vn::graphics::DiagnosticCategory::ContentSkipped,
                            as_output
                                ? String(u8"pass '") + reportedPassName(pass) +
                                      String(u8"' declares the output image '") + image->label() +
                                      String(u8"' but that image is not bound to a target, so no consumer can resolve"
                                             u8" it and the promise cannot be checked against what the pass renders"
                                             u8" into — bind it (ImageRef::bind(target, attachment))")
                                : String(u8"pass '") + reportedPassName(pass) +
                                      String(u8"' declares the input image '") + image->label() +
                                      String(u8"' but that image is not bound to a target, so nothing can fill it —"
                                             u8" bind it (ImageRef::bind(target, attachment))"));
    }
}

void RenderEngine::publish(const String& name, intrusive_ptr<RenderTarget> target)
{
    if (name.empty()) {
        return;   // no name: nothing to serve, and nothing a consumer could have asked for
    }
    if (target == nullptr) {
        // The host-facing half of the rule above: publish() promises a target to by-name consumers,
        // and a null one means there is nothing to hand out. The episode is the NAME: it ends when a
        // publish() hands over a real target (or unpublish() withdraws it), because a host has no
        // frame to re-publish from.
        if (wiring_.unpublishable_host_names_.insert(name).second) {
            reportEngineProblem(vn::graphics::DiagnosticSeverity::Warning,
                                vn::graphics::DiagnosticCategory::ContentSkipped,
                                String(u8"publish('") + name +
                                    String(u8"') was given no render target, so the name serves nothing and its"
                                           u8" consumers find nothing — pass the target to hand out, or unpublish()"
                                           u8" the name"));
        }
        return;
    }
    wiring_.unpublishable_host_names_.erase(name);
    // A HOST binding, not a pass publication: it keeps until unpublish() removes it (or another
    // publish replaces it), because a host has no per-frame hook to re-publish from — tying it to a
    // frame is what made this documented capability unusable (the registry is cleared per frame, so
    // the binding died before any consumer could resolve it). Publishing twice under one name is a
    // host swapping what it offers, not the collision two PASSES produce.
    //
    // Nothing else has to be told: the compile reads the bindings every frame, so a binding is part of the
    // NEXT frame's plan by definition - there is no revision for the host to bump and none to forget.
    wiring_.host_outputs_[name] = std::move(target);
}

raw_ptr<RenderTarget> RenderEngine::resolve(const String& name) const
{
    // The last compiled frame's answer: a pass publication (that pass ran, so its content is this frame's),
    // else a standing host binding. Both are the frame's own table - see resolveCompiled.
    return resolveCompiled(name);
}

void RenderEngine::unpublish(const String& name)
{
    // A pass' publication lives only inside the frame that compiled it (see compileFrame), so the only binding
    // there is to withdraw is the host's: the next compile stops answering the name on its own.
    wiring_.host_outputs_.erase(name);
    // Withdrawing the name ends the "cannot serve it" episode: publishing it again with no target is
    // a new mistake, not the same one.
    wiring_.unpublishable_host_names_.erase(name);
}

void RenderEngine::setDefaultContentProgram(intrusive_ptr<const ShaderProgram> program)
{
    default_content_program_ = std::move(program);
    if (backend_ != nullptr) {
        // Forwarded straight away: the backend treats a call before initialize() as the session
        // decision and one after it as a live switch (rebuild the window sets, drop the slots), so
        // the engine's accessor and the picture cannot disagree.
        backend_->setDefaultContentProgram(default_content_program_);
    }
}

intrusive_ptr<const ShaderProgram> RenderEngine::defaultContentProgram() const
{
    return default_content_program_;
}

void RenderEngine::resize(int width, int height)
{
    if (width <= 0 || height <= 0) {
        return;
    }
    // Recorded even when there is no backend yet: the frame context is what the passes lay
    // themselves out on, and the announcement is handed to the backend at initialize().
    frame_ctx_.surface_width  = width;
    frame_ctx_.surface_height = height;
    if (initialized_ && backend_ != nullptr) {
        backend_->resize(width, height);
    }
    // No layout fan-out here: target sizes, pass viewports and camera
    // projections are maintained by their creators on the surface size
    // (e.g. SceneView::addSurfaceLayout). The engine only rebuilds its own
    // swapchain and records the surface size.
}

void RenderEngine::setWindowHandle(void* native_handle)
{
    native_handle_ = native_handle;
}

VN_GRAPHICS_NS_END
