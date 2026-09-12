#include <vine/graphics/RenderEngine.hpp>

#include <algorithm>
#include <functional>

#include <vine/graphics/ImageRef.hpp>
#include <vine/graphics/RenderBackend.hpp>
#include <vine/graphics/RenderPass.hpp>
#include <vine/graphics/RenderTarget.hpp>
#include <vine/graphics/ScreenPass.hpp>
#include <vine/graphics/Scene.hpp>

V_GRAPHICS_NS_BEGIN

namespace
{

/** @brief Names a pass for a diagnostic, standing in for a pass that was never named.
 *
 * @param pass Pass to name (may be null).
 * @return The pass name, or "(unnamed)" when it has none.
 */
String reportedPassName(raw_ptr<const RenderPass> pass)
{
    if (pass == nullptr || pass->name().empty()) {
        return String(u8"(unnamed)");
    }
    return pass->name();
}

}  // namespace

V_OBJECT_META_IMPL(RenderEngine, vine::Object);

RenderEngine::RenderEngine()
{
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
    // A backend set after the sink was installed still gets it: the engine is
    // the stable place a host holds, not the backend instance.
    if (backend_ != nullptr) {
        backend_->setDiagnosticSink(diagnostic_sink_);
    }
}

void RenderEngine::setDiagnosticSink(DiagnosticSink sink)
{
    diagnostic_sink_ = std::move(sink);
    if (backend_ != nullptr) {
        backend_->setDiagnosticSink(diagnostic_sink_);
    }
}

std::size_t RenderEngine::diagnosticCount() const
{
    return backend_ != nullptr ? backend_->diagnosticCount() : 0u;
}

std::size_t RenderEngine::engineDiagnosticCount() const noexcept
{
    return engine_diagnostic_count_;
}

void RenderEngine::reportEngineProblem(vine::graphics::DiagnosticSeverity severity,
                                       vine::graphics::DiagnosticCategory category,
                                       const String&                message)
{
    ++engine_diagnostic_count_;
    if (diagnostic_sink_) {
        diagnostic_sink_(vine::graphics::RenderDiagnostic{ severity, category, message });
    }
}

bool RenderEngine::initialize()
{
    if (backend_ == nullptr) {
        return false;
    }
    if (native_handle_ != nullptr) {
        backend_->setWindowHandle(native_handle_);
    }
    // Forward the shading preset before the backend builds its shader sets.
    backend_->setShaderPreset(shader_preset_);
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
            if (pass == nullptr || content == nullptr || !pass->enabled() || pass->clearEnabled()) {
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

    // Fresh named-output registry per frame: every producer publishes during
    // the ordered pass run, so a consumer only ever samples this frame's
    // output and stale entries from removed producers disappear automatically.
    outputs_.clear();
    duplicate_outputs_seen_this_frame_.clear();
    unpublishable_passes_seen_this_frame_.clear();
    // What this frame produces is per TARGET, not per name: a pass that runs fills the target it
    // draws into (all of its images), and that is what a declared input is answered from. A host
    // binding is available every frame, so it seeds the set.
    produced_targets_.clear();
    for (const auto& binding : host_outputs_) {
        if (binding.second != nullptr) {
            produced_targets_.insert(binding.second.get());
        }
    }
    unproduced_inputs_seen_this_frame_.clear();

    // Structural wiring problems are visible from the DECLARATIONS, so they are reported before
    // anything runs — not left to show up as "the pass drew nothing" (design §14.4).
    validateWiring();

    // Ordered pipeline in ascending order: negative orders run first (shadow
    // / depth / g-buffer pre-pass), the window-present pass (master camera,
    // null render target) conventionally sits at order 0, and positive orders
    // run after (post-processing / compositing, then top / HUD passes). The
    // pipeline is exactly what the caller registered - the engine auto-
    // registers nothing. Each pass resolves its declared inputs just before
    // it runs and publishes its named output right after; disabled passes are
    // skipped. The pass's explicit order is announced to the backend before
    // it runs so the backend can stack its retained content slots by that
    // order (equal orders keep registration order).
    for (const auto& slot : slots_) {
        RenderPass* pass = slot.pass.get();
        if (pass == nullptr || !pass->enabled()) {
            continue;
        }
        raw_ptr<Scene> content = slot.content.get();
        // Pass scope: the backend is told which pass is running (its identity
        // for retained per-pass GPU state, and that it is active this frame),
        // runs the whole pass, then the scope is closed so per-pass state a
        // pass did not consume cannot leak into the next pass.
        backend_->beginPass(pass);
        backend_->setPassOrder(slot.order);
        resolvePassInputs(pass);
        drawScenePass(pass, content);
        // What it just drew into is produced for the rest of the frame: this is the fact, where a
        // promise is only the claim (a pass may promise a target it draws into — the normal case —
        // and what a consumer reads is what actually ran).
        if (raw_ptr<RenderTarget> drawn_into = pass->renderTarget(); drawn_into != nullptr) {
            produced_targets_.insert(drawn_into);
        }
        publishPassOutput(pass);
        backend_->endPass();
    }

    // A collision that did NOT happen this frame is over: re-arm its report, so a name that breaks
    // again later is reported again (the same episode rule the unresolved inputs follow).
    duplicate_outputs_reported_ = duplicate_outputs_seen_this_frame_;
    unpublishable_passes_reported_ = unpublishable_passes_seen_this_frame_;

    // Same for the declared inputs nothing produced this frame: what was broken stays reported (one
    // message), what recovered drops out of the set, so a wire that breaks again is reported again.
    unproduced_inputs_reported_ = std::move(unproduced_inputs_seen_this_frame_);

    backend_->endFrame();
    backend_->swapBuffers();
}

const FrameContext& RenderEngine::frameContext() const
{
    return frame_ctx_;
}

bool RenderEngine::hasWindowPass(raw_ptr<Camera> camera) const
{
    for (const auto& slot : slots_) {
        RenderPass* pass = slot.pass.get();
        if (pass != nullptr && pass->enabled() && pass->camera() == camera
            && pass->renderTarget() == nullptr) {
            return true;
        }
    }
    return false;
}

void RenderEngine::addPass(intrusive_ptr<RenderPass> pass, int order)
{
    addPass(std::move(pass), nullptr, order);
}

void RenderEngine::addPass(intrusive_ptr<RenderPass> pass, intrusive_ptr<Scene> content, int order)
{
    if (pass == nullptr) {
        return;
    }
    // Registering the same pass instance twice is ignored: it would otherwise
    // run twice per frame. To rebind content use bindPassContent(); to change
    // the order remove the pass and re-add it.
    for (const auto& existing : slots_) {
        if (existing.pass.get() == pass.get()) {
            return;
        }
    }
    // Keep the slots ascending by order; equal orders preserve insertion
    // order (stable), so ties are resolved by the addPass() call sequence.
    const auto it = std::find_if(slots_.begin(), slots_.end(),
                                 [order](const Slot& slot) { return slot.order > order; });
    slots_.insert(it, Slot{ std::move(pass), std::move(content), order });
}

void RenderEngine::removePass(raw_ptr<RenderPass> pass)
{
    const auto old_size = slots_.size();
    // Capture the removed pass's order — the content-slot key of the window
    // content it drew through — before the slot is dropped.
    int order = 0;
    for (const auto& slot : slots_) {
        if (slot.pass.get() == pass) {
            order = slot.order;
            break;
        }
    }
    slots_.erase(std::remove_if(slots_.begin(), slots_.end(),
                                [pass](const Slot& slot) { return slot.pass.get() == pass; }),
                 slots_.end());
    // A pass that leaves the list cannot be reported through again: an address
    // kept here would be reused by a NEW pass and silence its first report.
    unresolved_inputs_reported_.erase(pass);

    // A pass is registered at most once, so any removal drops its only user:
    // release the backend state it retained — keyed by the pass itself (the
    // primary contract, releasePass) and by (pass camera, pass order) (the
    // legacy contract) — plus any off-screen target the pass owns.
    if (backend_ != nullptr && pass != nullptr && slots_.size() != old_size) {
        if (raw_ptr<Camera> camera = pass->camera(); camera != nullptr) {
            backend_->releaseWindowLayer(camera, order);
        }
        backend_->releasePass(pass);
        if (raw_ptr<RenderTarget> target = pass->renderTarget(); target != nullptr) {
            backend_->releaseRenderTarget(target);
        }
    }
}

void RenderEngine::clearPasses()
{
    // Snapshot every registered (pass, order) pair — the order is the
    // content-slot key of the window content each pass drew — drop all slots,
    // then release the backend resources each removed pass owned.
    std::vector<std::pair<raw_ptr<RenderPass>, int>> removed;
    removed.reserve(slots_.size());
    for (const auto& slot : slots_) {
        if (slot.pass != nullptr) {
            removed.emplace_back(slot.pass.get(), slot.order);
        }
    }

    slots_.clear();
    unresolved_inputs_reported_.clear();
    duplicate_outputs_seen_this_frame_.clear();
    duplicate_outputs_reported_.clear();

    if (backend_ != nullptr) {
        for (const auto& entry : removed) {
            RenderPass* pass = entry.first;
            if (raw_ptr<Camera> camera = (pass != nullptr) ? pass->camera() : nullptr; camera != nullptr) {
                backend_->releaseWindowLayer(camera, entry.second);
            }
            if (pass != nullptr) {
                backend_->releasePass(pass);
            }
            if (raw_ptr<RenderTarget> target = (pass != nullptr) ? pass->renderTarget() : nullptr; target != nullptr) {
                backend_->releaseRenderTarget(target);
            }
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
    if (produced_targets_.count(target) != 0) {
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
    if (depth_usable && produced_targets_.count(target) != 0) {
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
    // A wire the structural check already reported (no producer at all, or one registered too late)
    // is ONE problem: the runtime does not repeat it in frame terms.
    const auto key = std::make_pair(raw_ptr<const RenderPass>(pass), identity);
    unproduced_inputs_seen_this_frame_.insert(key);
    if (unusable_inputs_reported_.count(key) != 0) {
        return;
    }
    if (unproduced_inputs_reported_.count(key) != 0) {
        return;   // the same input, still not produced: one message for this episode
    }
    reportEngineProblem(vine::graphics::DiagnosticSeverity::Warning,
                        vine::graphics::DiagnosticCategory::ContentSkipped,
                        String(u8"pass '") + reportedPassName(pass) + String(u8"' declares ") + what +
                            String(u8" but nothing produced it this frame, so the pass draws nothing"
                                   u8" (check the producer's order and its enabled state)"));
}

void RenderEngine::resolvePassInputs(raw_ptr<RenderPass> pass)
{
    if (pass == nullptr) {
        return;
    }

    // The object-typed declarations ARE the wiring; a name is the sugar over it (design §14.3). When
    // a pass declares images or targets, they decide what it gets — one resolved entry per
    // declaration, in declaration order, null where nothing produced it — and the names are not
    // consulted at all: one wire stated twice must not be resolved twice.
    if (!pass->inputs().empty() || !pass->inputTargets().empty()) {
        std::vector<raw_ptr<RenderTarget>> resolved;
        resolved.reserve(pass->inputs().size() + pass->inputTargets().size());
        for (const auto& image : pass->inputs()) {
            resolved.push_back(image != nullptr ? resolveDeclaredImage(pass, *image) : nullptr);
        }
        for (const auto& target : pass->inputTargets()) {
            resolved.push_back(target != nullptr ? resolveDeclaredTarget(pass, target.get()) : nullptr);
        }
        pass->resolveInputTextures(resolved);
        return;
    }

    const auto& names = pass->inputNames();
    if (names.empty()) {
        return;
    }
    std::vector<raw_ptr<RenderTarget>> resolved;
    resolved.reserve(names.size());
    for (const auto& name : names) {
        resolved.push_back(resolve(name));
    }
    pass->resolveInputTextures(resolved);

    // A declared input nobody published this frame means the pass draws
    // NOTHING (ScreenPass keeps the first non-null input and returns early when
    // there is none), so the frame silently misses its content: the wiring is
    // the engine's job, so it says so. A pass may declare several alternative
    // names (a chain that falls back), so this only fires when NONE of them
    // resolved.
    const bool any_resolved = std::any_of(resolved.begin(), resolved.end(),
                                          [](raw_ptr<RenderTarget> target) { return target != nullptr; });
    if (any_resolved) {
        // Re-arm: if this pass loses its producer later, that is a new problem.
        unresolved_inputs_reported_.erase(pass);
        return;
    }
    if (unresolved_inputs_reported_.insert(pass).second) {
        // The frontend has no printf-style helper of its own: the message is
        // assembled from String pieces (a formatting utility is the backend's).
        reportEngineProblem(vine::graphics::DiagnosticSeverity::Warning,
                            vine::graphics::DiagnosticCategory::ContentSkipped,
                            String(u8"pass '") + reportedPassName(pass) + String(u8"' declared input '") +
                                names.front() +
                                String(u8"' but no pass published it this frame; the pass draws nothing"
                                       u8" (check the producer's order, its enabled state and its output name)"));
    }
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

void RenderEngine::validateWiring()
{
    // Every pass on its own is valid, so only the engine can see these; and they are properties
    // of the DECLARATION, not of a frame (design §14.4). Reported once per episode: the sets
    // below are pruned to the problems still present, so one that is fixed re-arms the report.
    // Two phases, because a consumer may be registered before its producer: the producers have to
    // be collected before any consumer is judged against them.
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
    std::set<std::pair<raw_ptr<const RenderPass>, OutputIdentity>> mismatched_promises;
    std::set<raw_ptr<const RenderPass>>   input_less_passes;
    std::set<raw_ptr<const RenderPass>>   program_without_camera;
    std::set<raw_ptr<const ImageRef>>     unbound_declared_images;
    // image -> (first pass that declares it, whether as its OUTPUT) — reported in phase 4, after the
    // collision check had its say about the same image.
    std::map<raw_ptr<const ImageRef>, std::pair<raw_ptr<const RenderPass>, bool>> unbound_declared_by;
    std::set<std::pair<raw_ptr<const RenderPass>, OutputIdentity>> unusable_inputs;

    // Phase 1: who fills and who claims. The fillers come from ALL passes (a declaration is a
    // declaration), the claims from the ENABLED ones only — what the collision report is about is
    // "whoever runs last silently wins", and a disabled pass runs nothing.
    //
    // A host binding (publish) is filled by the HOST: it is there from the start of the frame and no
    // pass owns its hand-off, so a declared input addressing it is answered like one addressing a
    // pass' target.
    for (const auto& binding : host_outputs_) {
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

        if (!pass->enabled()) {
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
                mismatched_promises.emplace(pass, identity);
                if (mismatched_promises_reported_.count(std::make_pair(raw_ptr<const RenderPass>(pass), identity)) == 0) {
                    reportEngineProblem(vine::graphics::DiagnosticSeverity::Warning,
                                        vine::graphics::DiagnosticCategory::ContentSkipped,
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
                if (output_collisions_reported_.insert(identity).second) {
                    reportEngineProblem(vine::graphics::DiagnosticSeverity::Warning,
                                        vine::graphics::DiagnosticCategory::ContentSkipped,
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
            if (!output->bound()) {
                // Recorded, not reported yet: if another pass also claims this image, the collision
                // report below names both passes and the image, and binding the image alone would not
                // fix a double claim — one mistake, one message. Phase 4 reports the rest.
                unbound_declared_images.insert(output);
                unbound_declared_by.emplace(output, std::make_pair(raw_ptr<const RenderPass>(pass), true));
            }
            // Same rule as the coarse form: a fine promise is about a target this pass has to write.
            raw_ptr<RenderTarget> promised_target = output->target();
            if (promised_target != nullptr && pass->renderTarget() != promised_target) {
                const OutputIdentity identity = OutputIdentity::of(*output);
                mismatched_promises.emplace(pass, identity);
                if (mismatched_promises_reported_.count(std::make_pair(raw_ptr<const RenderPass>(pass), identity)) == 0) {
                    reportEngineProblem(vine::graphics::DiagnosticSeverity::Warning,
                                        vine::graphics::DiagnosticCategory::ContentSkipped,
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
                if (output_collisions_reported_.insert(identity).second) {
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
                    reportEngineProblem(vine::graphics::DiagnosticSeverity::Warning,
                                        vine::graphics::DiagnosticCategory::ContentSkipped,
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
            unusable_inputs.emplace(pass, identity);
            if (unusable_inputs_reported_.emplace(pass, identity).second) {
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
                reportEngineProblem(vine::graphics::DiagnosticSeverity::Warning,
                                    vine::graphics::DiagnosticCategory::ContentSkipped,
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
            if (!image->bound()) {
                unbound_declared_images.insert(image.get());
                unbound_declared_by.emplace(image.get(), std::make_pair(raw_ptr<const RenderPass>(pass), false));
                unusable_inputs.emplace(pass, identity);
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

            unusable_inputs.emplace(pass, identity);
            if (unusable_inputs_reported_.emplace(pass, identity).second) {
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
                reportEngineProblem(vine::graphics::DiagnosticSeverity::Warning,
                                    vine::graphics::DiagnosticCategory::ContentSkipped,
                                    message);
            }
        }
    }

    // Phase 3: a ScreenPass that declares NO input at all (no image, no target, no name) can never
    // draw — it samples one input and returns without it. Nothing else reports this: "no input" is
    // not "an input that failed to resolve" (resolvePassInputs), nor "an input nobody fills"
    // (phase 2).
    for (const auto& slot : slots_) {
        RenderPass* pass = slot.pass.get();
        if (pass == nullptr || !pass->enabled()) {
            continue;
        }

        if (dynamic_cast<ScreenPass*>(pass) != nullptr && pass->inputs().empty() && pass->inputTargets().empty() &&
            pass->inputNames().empty()) {
            input_less_passes.insert(pass);
            if (missing_inputs_reported_.insert(pass).second) {
                reportEngineProblem(vine::graphics::DiagnosticSeverity::Warning,
                                    vine::graphics::DiagnosticCategory::ContentSkipped,
                                    String(u8"pass '") + reportedPassName(pass) +
                                        String(u8"' is a ScreenPass that declares no input, so it can never draw"
                                               u8" (give it addInput / addInputName)"));
            }
        }

        // A ScreenPass WITH a program draws through the fullscreen program path, which builds the
        // pass' view from its camera and pushes the scene's lights through that view space: with no
        // camera there is no view, so ScreenPass::execute returns before asking the backend for
        // anything and the pass draws nothing at all. Reported at wiring time like the input-less
        // case above: the fix is static (setCamera), and a host would otherwise see a post-process
        // that never appears with no reason for it.
        auto* screen = dynamic_cast<ScreenPass*>(pass);
        if (screen != nullptr && screen->program() != nullptr && pass->camera() == nullptr) {
            program_without_camera.insert(pass);
            if (program_without_camera_reported_.insert(pass).second) {
                reportEngineProblem(vine::graphics::DiagnosticSeverity::Warning,
                                    vine::graphics::DiagnosticCategory::ContentSkipped,
                                    String(u8"pass '") + reportedPassName(pass) +
                                        String(u8"' has a fullscreen program but no camera, so it can never draw: the"
                                               u8" program path builds its view from the pass camera (and pushes the"
                                               u8" scene's lights through it) — give the pass a camera (setCamera)"));
            }
        }
    }

    // Phase 3b: a ScreenPass WITHOUT a program copies ONE COLOUR attachment (see attachmentToSample),
    // so a declaration that names images but no colour image cannot drive it: the pass would sample
    // the attachment it was left with (sourceAttachment(), 0 by default) while the host declared —
    // typically — the depth. Nothing else reports this: the wire is fine (a producer exists), the
    // declaration is simply not one this pass can sample. The program path is different
    // (drawScreenProgram binds every colour attachment of the source plus its depth), so a pass with
    // a program is left alone, and a coarse or name-only declaration is the documented fallback.
    std::set<raw_ptr<const RenderPass>> unsampleable_screen_inputs;
    for (const auto& slot : slots_) {
        RenderPass* pass = slot.pass.get();
        if (pass == nullptr || !pass->enabled() || pass->inputs().empty()) {
            continue;
        }
        auto* screen = dynamic_cast<ScreenPass*>(pass);
        if (screen == nullptr || screen->program() != nullptr) {
            continue;
        }
        bool any_colour = false;
        for (const auto& image : pass->inputs()) {
            if (image != nullptr && image->kind() != ImageRef::Kind::Depth) {
                any_colour = true;
                break;
            }
        }
        if (any_colour) {
            continue;
        }
        // A depth declaration whose image the wiring already reported (the target HAS no depth) is
        // the same declaration: the host gets the actionable message first, and this one only once
        // the depth exists and the pass still cannot sample it — one mistake, one message.
        bool already_reported = false;
        for (const auto& image : pass->inputs()) {
            if (image != nullptr &&
                unusable_inputs.count(std::make_pair(raw_ptr<const RenderPass>(pass), OutputIdentity::of(*image))) != 0) {
                already_reported = true;
                break;
            }
        }
        if (already_reported) {
            continue;
        }
        unsampleable_screen_inputs.insert(pass);
        if (unsampleable_screen_inputs_reported_.insert(pass).second) {
            const String& declared = pass->inputs().front() != nullptr ? pass->inputs().front()->label()
                                                                      : String(u8"(unnamed)");
            reportEngineProblem(vine::graphics::DiagnosticSeverity::Warning,
                                vine::graphics::DiagnosticCategory::ContentSkipped,
                                String(u8"pass '") + reportedPassName(pass) +
                                    String(u8"' declares the depth image '") + declared +
                                    String(u8"' as its input, but a ScreenPass without a program samples ONE"
                                           u8" COLOUR attachment of its source: it samples the attachment it was left"
                                           u8" with (sourceAttachment(), 0 by default) instead — declare the colour"
                                           u8" image you mean (an ImageRef bound to that attachment), or give the"
                                           u8" pass a program (its path receives every colour attachment and the"
                                           u8" depth of its source)"));
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
        if (!unbound_declared_images_reported_.insert(image).second) {
            continue;   // already reported for this episode
        }
        const auto& [pass, as_output] = declarer;
        reportEngineProblem(vine::graphics::DiagnosticSeverity::Warning,
                            vine::graphics::DiagnosticCategory::ContentSkipped,
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

    output_collisions_reported_  = std::move(colliding_images);
    mismatched_promises_reported_ = std::move(mismatched_promises);
    unusable_inputs_reported_    = std::move(unusable_inputs);
    missing_inputs_reported_     = std::move(input_less_passes);
    unsampleable_screen_inputs_reported_ = std::move(unsampleable_screen_inputs);
    program_without_camera_reported_     = std::move(program_without_camera);
    unbound_declared_images_reported_    = std::move(unbound_declared_images);
}

void RenderEngine::publishPassOutput(raw_ptr<RenderPass> pass)
{
    if (pass == nullptr) {
        return;
    }
    const String&         name   = pass->outputName();
    raw_ptr<RenderTarget> target = pass->renderTarget();
    if (name.empty()) {
        return;   // an empty name is how a pass says "do not publish me": not a problem to report
    }
    if (target == nullptr) {
        // The registry hands out a SAMPLEABLE target, and a pass rendering into the window has none.
        // Reported once per episode instead of dropping the declaration: a consumer of that name
        // would otherwise be told "nothing produced it this frame", pointing at the consumer for the
        // producer's mistake.
        unpublishable_passes_seen_this_frame_.insert(pass);
        if (unpublishable_passes_reported_.insert(pass).second) {
            reportEngineProblem(vine::graphics::DiagnosticSeverity::Warning,
                                vine::graphics::DiagnosticCategory::ContentSkipped,
                                String(u8"pass '") + reportedPassName(pass) +
                                    String(u8"' publishes the output name '") + name +
                                    String(u8"' but renders into the window (no render target), so nothing is"
                                           u8" published and a consumer of '") +
                                    name + String(u8"' finds nothing — give the pass a render target, or drop the"
                                                u8" output name"));
        }
        return;
    }
    publishFrameOutput(name, intrusive_ptr<RenderTarget>(target));
}

void RenderEngine::publishFrameOutput(const String& name, intrusive_ptr<RenderTarget> target)
{
    if (name.empty() || target == nullptr) {
        return;
    }
    const auto existing = outputs_.find(name);
    if (existing != outputs_.end() && existing->second.get() != target.get()) {
        // Two passes published DIFFERENT targets under one name: every consumer of that name
        // silently gets whichever pass ran last, and only the engine can see the collision (each
        // pass is individually valid). Reported once per episode — a scene that keeps the wiring
        // bug must not produce one message per frame.
        duplicate_outputs_seen_this_frame_.insert(name);
        if (duplicate_outputs_reported_.insert(name).second) {
            const String first  = (existing->second != nullptr && !existing->second->name().empty())
                                      ? existing->second->name()
                                      : String(u8"(unnamed)");
            const String second = target->name().empty() ? String(u8"(unnamed)") : target->name();
            reportEngineProblem(vine::graphics::DiagnosticSeverity::Warning,
                                vine::graphics::DiagnosticCategory::ContentSkipped,
                                String(u8"pass output '") + name +
                                    String(u8"' is published by two passes this frame (targets '") + first +
                                    String(u8"' and '") + second + String(u8"'); every consumer of '") + name +
                                    String(u8"' samples whichever pass ran last"));
        }
    }
    outputs_[name] = std::move(target);
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
        if (unpublishable_host_names_.insert(name).second) {
            reportEngineProblem(vine::graphics::DiagnosticSeverity::Warning,
                                vine::graphics::DiagnosticCategory::ContentSkipped,
                                String(u8"publish('") + name +
                                    String(u8"') was given no render target, so the name serves nothing and its"
                                           u8" consumers find nothing — pass the target to hand out, or unpublish()"
                                           u8" the name"));
        }
        return;
    }
    unpublishable_host_names_.erase(name);
    // A HOST binding, not a pass publication: it keeps until unpublish() removes it (or another
    // publish replaces it), because a host has no per-frame hook to re-publish from — tying it to a
    // frame is what made this documented capability unusable (the registry is cleared per frame, so
    // the binding died before any consumer could resolve it). Publishing twice under one name is a
    // host swapping what it offers, not the collision two PASSES produce.
    host_outputs_[name] = std::move(target);
}

raw_ptr<RenderTarget> RenderEngine::resolve(const String& name) const
{
    // This frame's pass publication wins: that pass ran, so its content is this frame's.
    const auto published = outputs_.find(name);
    if (published != outputs_.end()) {
        return published->second.get();
    }
    const auto bound = host_outputs_.find(name);
    return (bound != host_outputs_.end()) ? bound->second.get() : nullptr;
}

void RenderEngine::unpublish(const String& name)
{
    outputs_.erase(name);
    host_outputs_.erase(name);
    // Withdrawing the name ends the "cannot serve it" episode: publishing it again with no target is
    // a new mistake, not the same one.
    unpublishable_host_names_.erase(name);
}

void RenderEngine::setShaderPreset(ShaderPreset preset)
{
    shader_preset_ = preset;
}

ShaderPreset RenderEngine::shaderPreset() const
{
    return shader_preset_;
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

V_GRAPHICS_NS_END
