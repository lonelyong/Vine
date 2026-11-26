#include <algorithm>
#include <functional>
#include <utility>
#include <vine/graphics/RenderEngine.hpp>

#include <vine/graphics/BuiltinShaders.hpp>
#include <vine/graphics/ImageRef.hpp>
#include <vine/graphics/RenderBackend.hpp>
#include <vine/graphics/RenderPass.hpp>
#include <vine/graphics/RenderArea.hpp>
#include <vine/graphics/RenderTarget.hpp>
#include <vine/graphics/Scene.hpp>
#include <vine/graphics/FullscreenPass.hpp>

#include "EngineDiagnostics.hpp"
#include "EngineFrame.hpp"
#include "FrameExpand.hpp"
#include "FramePump.hpp"
#include "FrameResources.hpp"
#include "FrameRunner.hpp"
#include "FrameScheduler.hpp"
#include "PublicationStore.hpp"
#include "SessionShape.hpp"
#include "SurfaceLedger.hpp"

VN_GRAPHICS_NS_BEGIN

VN_OBJECT_META_IMPL(RenderEngine, vn::Object);
RenderEngine::RenderEngine()
  : surface_(std::make_unique<detail::SurfaceLedger>(diagnostics_))
  , resources_(std::make_unique<detail::FrameResources>())
  , pump_(std::make_unique<detail::FramePump>())
  , publications_(std::make_unique<detail::PublicationStore>())
  , shape_(std::make_unique<detail::SessionShape>(diagnostics_, *publications_, *resources_))
  // THE PUBLICATION IS THE SOURCE (see the design's 2.11): every scene pass draws the content's PUBLISHED value, so a
  // frame's draws name nothing the content owns - which is what lets the engine half move to another thread. The
  // per-view memo it needs (one camera drawn by two passes collects once) is the shared DrawListMemo.
{
    // The ONE default in the shading path, and it is a named program: content with no program of its
    // own is shaded with the engine's forward program until the host says otherwise. (Null would mean
    // "draw such content not at all"; nothing is ever shaded with a program nobody named.)
    default_content_program_ = forwardProgram();
    // The validity bit every lease reads (see lease): alive from here on, and cleared in the destructor before
    // anything the engine owns goes away.
    lease_cell_              = std::make_shared<vn::detail::LeaseCell<RenderEngine>>();
    lease_cell_->target      = this;
    // No implicit chain: the engine starts empty. The caller registers the
    // scene passes explicitly (addPass()) or through PassChain::build;
    // the primary interactive view (camera + content + navigation) lives in a
    // SceneView borrowing this engine.
}

Lease<RenderEngine> RenderEngine::lease() noexcept
{
    return Lease<RenderEngine>(lease_cell_);
}

RenderEngine::~RenderEngine()
{
    shutdown();
    // The area is the SURFACE LEDGER's (createArea), and the ledger outlives this destructor's body by design: the area
    // is released while the engine is still an object, its lease reads empty, and a host still holding a handle keeps
    // a surface whose engine is gone rather than a dangling one.
    // Every lease this engine handed out stops referring to it HERE, before anything it owns is torn down: a holder
    // that outlives the engine (a pipeline a host kept) then has nothing to call into, instead of a pointer into
    // freed memory.
    if (lease_cell_ != nullptr) {
        lease_cell_->target = nullptr;
    }
    // THE FRAME DRIVER GOES BACK FIRST: the device owns it (RenderDevice::createFrameDriver) and may destroy its
    // machinery at once, so the borrowed reference must not outlive this engine.
    if (device_ != nullptr && session_ != nullptr) {
        device_->releaseFrameDriver(*session_);
        session_ = nullptr;
    }
    // The route this engine installed on the device goes with it: the device BORROWS it (see
    // RenderDevice::setDiagnosticsRoute), so a route that outlived its engine would be a dangling pointer the next
    // report walks into.
    if (device_ != nullptr) {
        device_->removeDiagnosticsRoute(diagnostics_);
    }
}

raw_ptr<RenderBackend> RenderEngine::device() const
{
    return device_.get();
}

void RenderEngine::setDevice(intrusive_ptr<RenderBackend> device)
{
    // Re-assigning the same device is a no-op (idempotent).
    if (device_ == device) {
        return;
    }
    // A FRAME IN FLIGHT IS FINISHED FIRST. The driver about to go back is the one the engine half may be executing
    // through, and `session_` is about to be replaced: every other session change already obeys this rule
    // (`applyPlan` refuses while busy, `setFrameThreading(false)` and `shutdown` wait for the pump), so the one
    // lifecycle swap that frees a driver must not be the exception (design `graphics-threading.md` §2.6).
    pump_->waitIdle();
    // The route this engine installed on the OLD device goes with the old device: it was borrowed there (see
    // RenderDevice::setDiagnosticsRoute) and is about to stop being valid for it.
    // THE OLD DRIVER GOES BACK TO THE OLD DEVICE, before that reference is replaced.
    if (device_ != nullptr && session_ != nullptr) {
        device_->releaseFrameDriver(*session_);
    }
    session_ = nullptr;
    if (device_ != nullptr) {
        device_->removeDiagnosticsRoute(diagnostics_);
    }
    device_ = std::move(device);
    // A different device is a different answer: the "not up" episode starts over (see initialize).
    device_not_up_.rearm();
    // The device reports through THIS engine's one route (see Diagnostics.hpp): attaching it is what
    // makes the engine's count the whole truth instead of half of it, and it is why a device set after
    // the sink was installed needs nothing else done to it - the sink lives in the route, not in a copy.
    if (device_ != nullptr) {
        device_->setDiagnosticsRoute(diagnostics_);
        // A DRIVER OF THIS ENGINE'S OWN (RenderDevice::createFrameDriver): one device may serve several engines, and this
        // is the object that drives THIS one. A device that cannot hand one out leaves the engine without a driver,
        // and its frames answer NotInitialized rather than guess one.
        session_ = device_->createFrameDriver();
        // The default content program travels with the DRIVER, not with the attach: the program shades frames, and
        // the frame driver is what resolves it. A device that runs its own frames hands the same object back, so a
        // direct driver keeps the shape it had.
        if (session_ != nullptr) {
            session_->setDefaultContentProgram(default_content_program_);
        }
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

std::size_t RenderEngine::diagnosticCount(const DiagnosticOrigin origin) const noexcept
{
    return static_cast<std::size_t>(diagnostics_.total(origin));
}

std::size_t RenderEngine::diagnosticCount(DiagnosticCategory category) const noexcept
{
    return static_cast<std::size_t>(diagnostics_.count(category));
}

bool RenderEngine::diagnosticsClean() const noexcept
{
    return diagnostics_.clean();
}

void RenderEngine::reportEngineProblem(vn::graphics::DiagnosticSeverity severity, vn::graphics::DiagnosticCategory category, const String& message)
{
    // THE HOST'S FORM (see the header): the caller named no finer cause than the category, and the report is
    // marked Host so the engine's own count (diagnosticCount) stays decidable (A-06).
    vn::graphics::RenderDiagnostic diagnostic;
    diagnostic.origin   = vn::graphics::DiagnosticOrigin::Host;
    diagnostic.severity = severity;
    diagnostic.category = category;
    diagnostic.message  = message;
    reportEngineProblem(diagnostic);
}

void RenderEngine::reportEngineProblem(const vn::graphics::RenderDiagnostic& diagnostic)
{
    // ONE DOOR: the host's own reports and the engine's land on the same route. The compile's checks do not come
    // through here - they collect their findings in the session shape, which decides what becomes an episode (see
    // detail::SessionShape::reportPlanProblems) and emits the survivors through this same route.
    diagnostics_.report(diagnostic);
}

Capabilities RenderEngine::capabilities() const noexcept
{
    // No device, nothing can be claimed - and a field that is false is the honest answer rather than a
    // missing one (see Capabilities; it is also what a device that cannot do it answers).
    if (device_ == nullptr) {
        return Capabilities{};
    }
    // One answer, from the party that knows: the DEVICE describes itself, and whether it can present at all
    // (Capabilities::presentation) is part of that description - which is what makes a display-less device a
    // first-class state rather than a failure.
    return device_->capabilities();
}

FrameReport RenderEngine::frameReport() const noexcept
{
    // No device, nothing is measured: every part stays disengaged rather than reading as zeros (see
    // FrameReport - absent is not zero). The frame half of the device is what has the numbers.
    return session_ != nullptr ? session_->frameReport() : FrameReport{};
}

bool RenderEngine::initialize()
{
    if (device_ == nullptr) {
        return false;
    }
    // THE DEVICE IS BORROWED, so whether it is up is its CREATOR's doing (RenderDevice::initialize): an engine never
    // brings it up - that would take a device shared with another engine down under it - and never takes it down.
    // A device that is not up fails the attach and is said ONCE per episode: a frame driven through it would look
    // like a running engine drawing nothing.
    if (!device_->initialized()) {
        initialized_ = false;
        if (device_not_up_.shouldReport()) {
            reportEngineProblem(detail::engineProblem(u8"engine.device.not_up",
                                                      DiagnosticCategory::UnsupportedRequest,
                                                      u8"RenderEngine::initialize(): the device this engine borrows is not up - the party "
                                                      u8"that created it brings it up (RenderDevice::initialize); this engine only drives "
                                                      u8"frames through it"));
        }
        return false;
    }
    // A device that brings up a window of its OWN (the host named none) is asked for that target here, once (see
    // detail::SurfaceLedger::attach): the frame path has to have something to present through, and the device is the
    // party that knows whether it presents at all. A device that presents nothing answers nullptr with NoPresentation
    // and NO report - a display-less device is a first-class state, and the frame path then presents nowhere.
    surface_->attach(*device_);
    // A size announced before initialization (a host sizes its widget before it has a device) is NOT replayed here:
    // the first frame hands it to the target at its head (see frame), the same path every later size takes - one
    // place that resizes a swapchain instead of two, and nothing to keep in step when the target object changes.
    // NO PRE-FRAME WARM-UP. It used to execute every enabled, non-clearing pass once before the first frame, so
    // a backend could see its whole pass set before presenting - which mattered when a frame arrived as a
    // sequence of calls and content was built lazily inside a draw packet. A frame now arrives as ONE value
    // (RenderFrameExecutor::executeFrame), so a backend has the whole pass set before it does anything, and the
    // engine's own path to the backend is the same one the first frame takes (see frame()). What is left to
    // pre-build is the backend's own business: the main content is compiled during device initialize().
    initialized_ = true;
    return true;
}

void RenderEngine::shutdown()
{
    // THE DEVICE IS NOT SHUT DOWN: this engine borrows it (see the declaration), and another engine may still be
    // driving it. What is this engine's own is the area's binding - and it goes FIRST, because the target is one of
    // the device's objects (see RenderDevice::createSurfaceTarget): nothing is left naming something the device's
    // creator may destroy.
    //
    // NOTHING IS ANSWERED AFTER THIS POINT, so the frame thread goes FIRST: a frame in flight is finished (the thread is
    // joined, never detached) and the pump is left inline - a frame still executing while this call hands the device
    // back would touch objects its creator may already own (design F10 / N11).
    pump_->stop();
    // THE TRANSIENT TARGETS THIS ENGINE DECLARED GO BACK TOO. The device owns their GPU storage and may outlive this
    // engine (it is borrowed, and one device may serve several engines), so a table dropped in silence would leave
    // the device holding storage for targets nothing will ever ask about again - and `releaseAll` is the
    // announcement that lets it park each release against the frame that last used it (see FrameResources).
    if (device_ != nullptr) {
        resources_->releaseAll(device_.get());
    }
    // The target this engine was resizing is gone (the area let go of it, or the device will): a re-initialized engine
    // asks its device for a new one, whose size is the device's own until the next frame hands it the announced one.
    surface_->detach();
    initialized_ = false;
}

raw_ptr<const RenderPass> RenderEngine::passNeedingAnUnsupportedTarget() const
{
    if (device_ == nullptr || device_->capabilities().render_targets) {
        return nullptr;
    }
    // Only ENABLED passes matter (see detail::SessionShape): a disabled pass is skipped, so its target is never
    // asked for.
    return shape_->firstEnabledPassWithATarget();
}

void RenderEngine::bindSnapshot(Scene& scene, const SceneSnapshotRef& snapshot)
{
    // NO DEVICE AND NO FRAME IS CONSULTED: this states what this engine draws the scene with, and the next frame
    // reads it. A scene the engine has not drawn before gets its source here, exactly as an implicit publication
    // would have created one.
    publications_->bindSnapshot(scene, snapshot);
}

PrepareReport RenderEngine::prepare()
{
    // The two reasons nothing can be prepared are the same two answers frame() gives, and they are values rather
    // than silence: a host gating a load on this call has to be able to tell "no backend" from "not up yet".
    if (device_ == nullptr) {
        return PrepareReport{ PrepareReport::Outcome::NotReady, EngineNotReadyReason::NoDevice };
    }
    if (!initialized_) {
        return PrepareReport{ PrepareReport::Outcome::NotReady, EngineNotReadyReason::NotInitialized };
    }
    if (session_ == nullptr) {
        // A device that hands out no driver cannot be prepared through: the same answer as "not attached", because
        // there is nothing this call could build.
        return PrepareReport{ PrepareReport::Outcome::NotReady, EngineNotReadyReason::NotInitialized };
    }

    // A REQUEST FROM INSIDE THIS ENGINE'S OWN FRAME IS REFUSED, NOT WAITED OUT (see FramePump::onEngineThread): the
    // frame in flight IS the caller (a hook or a diagnostic sink driving the same engine), so `waitIdle` below would be
    // the thread waiting for itself. The answer is a value, like every other refusal this module makes.
    if (pump_->onEngineThread()) {
        return PrepareReport{ PrepareReport::Outcome::NotReady, EngineNotReadyReason::Reentrant };
    }

    // ONE FRAME IN FLIGHT: the engine half that owns the other slot is finished first, exactly as frame() does -
    // the expansion below writes the per-view memos, and those are the half's state (design 2.12).
    pump_->waitIdle();
    if (detail::EngineFrame* const finished = pump_->takePendingMicros()) {
        publishEngineMicros(*finished);
    }

    // THE FRAME THE NEXT frame() WOULD BUILD, built and expanded the same way, and handed over as a QUERY. The slot
    // is NOT advanced and no frame number is taken: nothing here is a frame, so the next frame() builds this same
    // slot again from the declarations AS THEY STAND THEN - a camera that moved in between is in it, which is what
    // keeps "prepared" a key rather than a frozen picture.
    detail::EngineFrame& frame = pump_->currentFrame();
    buildContentHalf(frame);
    // THE EXPANSION IS PART OF THE ANSWER. A compiled half is keyed per DRAW PACKET (its vertex layout, variant and
    // counts are the command's), so a frame value whose draws carry no collected commands would describe nothing to
    // build - the answer would be trivially zero and the first real frame would still compile. This is the same
    // value step the engine half runs (see FrameExpand): no device call and no submission.
    expandFrameValue(frame);
    return session_->prepareFrame(frame.description, frame.target);
}

FrameCallOutcome RenderEngine::frame()
{
    // A FRAME IS TWO HALVES (design 2.12): one built where the content is, one answered where the device is. Without a
    // frame thread they run one after the other, here; with one, this call builds and hands over, and the picture
    // follows one frame later.

    // The two reasons a frame cannot be driven are different answers, and both used to look like a frame that ran
    // and drew nothing: no device to hand anything to, or a device the engine is not attached to. The caller gets
    // the reason as a value (see FrameCallOutcome); "ran but issued no draw" stays a FrameReport fact.
    if (device_ == nullptr) {
        return FrameCallOutcome{ FrameCallOutcome::Status::NotReady, EngineNotReadyReason::NoDevice };
    }
    if (!initialized_) {
        return FrameCallOutcome{ FrameCallOutcome::Status::NotReady, EngineNotReadyReason::NotInitialized };
    }
    if (session_ == nullptr) {
        // A device that handed out no driver has nothing to run this frame on (see setDevice).
        return FrameCallOutcome{ FrameCallOutcome::Status::NotReady, EngineNotReadyReason::NotInitialized };
    }

    // A FRAME ASKED FROM INSIDE THIS ENGINE'S OWN FRAME IS REFUSED, NOT QUEUED (see FramePump::onEngineThread): the
    // content half is built on the caller's thread and `beginFrame` waits for the frame in flight, which - on the frame
    // thread - is the caller itself. A hook or a diagnostic sink driving the same engine gets this answer and the
    // frame it interrupted continues.
    if (pump_->onEngineThread()) {
        return FrameCallOutcome{ FrameCallOutcome::Status::NotReady, EngineNotReadyReason::Reentrant };
    }

    // The frame will really run, so this is where the frame clock takes its one reading (see frameContext): a
    // NotReady frame is not a frame's interval, and counting it would report a stalled engine as one slow frame.
    advanceFrameClock();

    // ONE FRAME IN FLIGHT: with a frame thread, the previous frame's engine half is finished before this frame touches
    // the mirrors or a slot. That is the whole pipeline (1 + 1), and it is why a caller never queues frames: the wait
    // is for the frame BEFORE the one it is about to build, which is normally already done (design 2.12, D12).
    detail::EngineFrame& next = pump_->beginFrame();
    // The frame that just finished reported its per-pass cost on its own value; the plan is the content half's, so it
    // is written here - the one moment nobody else is in it.
    if (detail::EngineFrame* const finished = pump_->takePendingMicros()) {
        publishEngineMicros(*finished);
    }
    buildContentHalf(next);

    if (!pump_->threaded()) {
        answerEngineHalf(next);
        return FrameCallOutcome{ FrameCallOutcome::Status::Ran };
    }
    detail::EngineFrame* const submitted = &next;
    pump_->submit([this, submitted]() { answerEngineHalf(*submitted); });
    return FrameCallOutcome{ FrameCallOutcome::Status::Submitted };
}

void RenderEngine::setFrameThreading(FrameThreading mode)
{
    pump_->setThreaded(mode == FrameThreading::EngineThread);
}

FrameThreading RenderEngine::frameThreading() const noexcept
{
    return pump_->threaded() ? FrameThreading::EngineThread : FrameThreading::Inline;
}

std::uint64_t RenderEngine::frameNumber() const noexcept
{
    return pump_->lastNumber();
}

void RenderEngine::advanceFrameClock() noexcept
{
    // ONE READING PER FRAME, KEPT HERE (see frameContext): the interval between two frames is what "how fast is this
    // loop running" means, and measuring it once is what stops two overlays from windowing the same frame
    // differently. A frame that never ran (NotReady) takes no reading, so a stalled engine does not report a
    // spurious interval on its first frame afterwards.
    const auto now = std::chrono::steady_clock::now();
    if (frame_tick_taken_) {
        frame_context_.elapsed_seconds = std::chrono::duration<double>(now - last_frame_tick_).count();
    }
    last_frame_tick_ = now;
    frame_tick_taken_ = true;
}

FrameContext RenderEngine::frameContext() const noexcept
{
    // THE FACTS ARE READ LIVE, the time is the head of the last frame: a host that asks twice gets the same answer,
    // and the size it sees is the one the passes are laid out on right now rather than the one the previous frame
    // used.
    FrameContext context = frame_context_;
    context.frame_number = frameNumber() + 1;
    context.area_width   = areaWidth();
    context.area_height  = areaHeight();
    context.pixel_ratio  = surface_ != nullptr ? surface_->pixelRatio() : 1.0;
    return context;
}

bool RenderEngine::waitForFrame(std::uint64_t number, std::chrono::milliseconds timeout)
{
    return pump_->wait(number, timeout);
}

bool RenderEngine::waitForFrame(std::chrono::milliseconds timeout)
{
    return waitForFrame(frameNumber(), timeout);
}

std::optional<CameraSnapshot> RenderEngine::presentedCamera() const
{
    const FrameResult answer = pump_->result();
    return answer.query.view.camera.present ? std::optional<CameraSnapshot>(answer.query.view.camera) : std::nullopt;
}

FrameResult RenderEngine::frameResult() const
{
    return pump_->result();
}

RenderEngine::PresentRecord RenderEngine::presentRecord() const noexcept
{
    const std::uint64_t packed = pump_->packedPresent();
    PresentRecord       record;
    record.frame   = packed >> 8U;
    record.outcome = static_cast<PresentOutcome>(packed & 0xFFU);
    return record;
}

void RenderEngine::compileFrame(detail::EngineFrame& frame)
{
    // The compile belongs to the session shape (see detail::SessionShape): the engine lends the device for the
    // transients a declaration may release, and the shape owns the declarations-to-frame translation.
    shape_->compile(frame, device_.get());
}

void RenderEngine::buildContentHalf(detail::EngineFrame& frame)
{
    // Everything below describes into THIS slot (see detail::SessionShape::compile): the frame is passed explicitly,
    // and the engine half takes its own as an argument - so neither half reaches for the other's slot.
    // THE FRAME'S SIZE AND TARGET ARE TAKEN ONCE, when the frame starts: the passes below lay themselves out on these
    // numbers, and the engine half applies THAT size to THAT target - so an announcement arriving while the frame is
    // being answered belongs to the next frame instead of tearing this one in half (design 2.12).
    frame.width         = surface_->width();
    frame.height        = surface_->height();
    frame.target        = surface_->frameTarget();
    frame.pixel_ratio   = surface_->area() != nullptr ? surface_->area()->pixelRatio() : 1.0;
    frame.content_frame = content_frame_;
    // THE CAMERA THE USER WILL BE LOOKING AT, taken now as the same value `describe` puts in the frame's draws: the pass
    // that presents this engine's area is the picture, so its camera is the one a pointer query must aim with once this
    // frame is answered (design N17). Nothing presents the area (an offline bake, a device with no window) => empty,
    // and a query keeps aiming with the live camera.
    frame.presented_view = View{};
    if (raw_ptr<RenderArea> area = surface_->area(); area != nullptr) {
        if (raw_ptr<RenderPass> presenting = area->presentingPass();
            presenting != nullptr && presenting->camera() != nullptr) {
            // The SAME VIEW the pass describes: its camera as a value, the device-pixel rectangle it lands in, and
            // the layers it draws. A query that aims with this value asks about the picture the user is looking at.
            frame.presented_view.camera     = CameraSnapshot::of(*presenting->camera());
            frame.presented_view.layer_mask = presenting->layerMask();
            Viewport viewport               = presenting->viewport();
            if (!viewport.isValid()) {
                if (raw_ptr<RenderTarget> target = presenting->renderTarget(); target != nullptr) {
                    viewport = Viewport{ 0, 0, target->width(), target->height() };
                }
                else {
                    viewport = presenting->drawSize();
                }
            }
            frame.presented_view.viewport = viewport;
        }
    }
    // THE PUBLICATION THE PICTURE IS DRAWN FROM, taken the same way and at the same moment as the camera: the presenting
    // pass' content, as this frame resolves it (a bound publication wins over the scene's latest, see bindSnapshot), or
    // empty when that pass draws no content at all.
    frame.presented_snapshot = SceneSnapshotRef{};
    if (raw_ptr<RenderArea> area = surface_->area(); area != nullptr) {
        if (raw_ptr<RenderPass> presenting = area->presentingPass()) {
            if (raw_ptr<Scene> scene = presenting->content()) {
                frame.presented_snapshot = publications_->publicationFor(*scene);
            }
        }
    }
    // The diagnostics THIS frame produces are the delta of the engine's one counter (see FrameResult): taken before the
    // compile, because everything the frame reports - wiring problems here, the device's during the engine half - lands
    // between these two reads.
    frame.diagnostics_before = diagnosticCount();

    // Announce the content frame BEFORE the passes run: every scene this frame
    // renders may then memoise the command list it collects (see
    // Scene::setContentFrame), so the multi-pass pipelines that draw one scene
    // through one camera several times per frame (G-buffer + lighting + ...)
    // walk the tree once instead of once per pass. The token is idempotent, so
    // a scene shared by several slots is announced any number of times.
    ++content_frame_;
    frame.content_frame = content_frame_;
    for (const detail::SessionShape::Slot& slot : shape_->slots()) {
        if (slot.pass == nullptr) {
            continue;
        }
        if (raw_ptr<Scene> scene = slot.pass->content()) {
            scene->setContentFrame(content_frame_);
        }
    }

    // A frame starts by COMPILING its declarations: which pass runs when, what its declared inputs resolve
    // to, which transient target it draws into, and what is wrong with the set (see detail::SessionShape). Nothing
    // tells the engine that a declaration moved - reading the declarations IS the compile, so a re-wired
    // pass lands in THIS frame and there is no revision to keep in step, no gate to forget, and no second
    // table (the old wiring registry) that had to be rebuilt per frame to answer the same questions.
    compileFrame(frame);

    // A backend whose capabilities say "no off-screen targets" IGNORES a non-null target, so a pass staged
    // through one is recorded where the pipeline never put it — a WRONG PICTURE rather than a slow one,
    // and silent unless the engine asks (see RenderDevice::capabilities). Asked here, once per
    // frame, and reported as an EPISODE: the pass is still run (its content is the host's to place), but
    // the host is told that the pipeline it built cannot mean what it says.
    if (raw_ptr<const RenderPass> unsupported = passNeedingAnUnsupportedTarget(); unsupported != nullptr) {
        if (unsupported_target_reported_.shouldReport()) {
            reportEngineProblem(detail::engineProblem(u8"engine.target.unsupported_offscreen",
                                                      vn::graphics::DiagnosticCategory::TargetBuildFailed,
                                                      String(u8"the backend reports no off-screen render target support, so pass '") +
                                                          detail::reportedPassName(unsupported) +
                                                          String(u8"' (and any other pass rendering into a target) is not drawn where "
                                                                 u8"the pipeline expects it: the backend ignores the target and draws "
                                                                 u8"into the window")));
        }
    }
    else {
        // The pipeline stopped asking for what the backend cannot do, so a later ask is a new problem.
        unsupported_target_reported_.rearm();
    }

    // The half ends with a COMPLETE VALUE: the compile above ends by assembling what every pass described (see
    // buildFrameValue, called from compileFrame), and the size and target were taken when the frame started. What
    // runs from here on is runEngineHalf - the half that touches the mirrors and the device.
}

void RenderEngine::runEngineHalf(detail::EngineFrame& frame)
{
    // THE FRAME'S ENGINE HALF, IN ORDER: the extent the frame was laid out on goes to the window half, then the
    // described draws are answered, then the whole frame is executed (acquire, record, submit, present).
    //
    // THE WINDOW HALF OF THE SIZE FOLLOWS THE FRAME (design 2.12): the size is the FRAME's (taken when it started, see
    // buildContentHalf), so a host resizing while this frame is answered costs the NEXT frame its resize and leaves
    // this one whole (see detail::SurfaceLedger::applyFrameSize).
    surface_->applyFrameSize(frame.target, frame.width, frame.height);

    expandFrameValue(frame);

    // The WHOLE FRAME happens inside this ONE call (see RenderFrameExecutor::executeFrame): the device acquires the
    // presentable image, the record submits, and the frame's tail presents it through the target it names. The engine
    // does not present afterwards - presenting twice would be presenting somebody else's frame (or nothing at all).
    frame.outcome = session_->executeFrame(frame.description, frame.target);

    // A LOST PICTURE IS THE SURFACE ASKING TO BE REBUILT, and the size is how that request is made: a swapchain is
    // rebuilt by re-announcing the extent it hangs off (PresentTarget::resize -> the session follows the surface and
    // builds a new one, counted as a device wait). So the engine FORGETS what it told the target, and the next frame
    // hands it the same numbers again - one repair, on the side that owns the swapchain's size. If that repair does
    // not take (the window itself is gone), the next frame answers Lost again, and the HOST's move is the attach
    // (a new handle, a new target) - never both at once, and never a repair loop inside the frame.
    if (frame.outcome == PresentOutcome::Lost) {
        surface_->forgetFrameSize();
    }

    // The frame is answered: what a host's waitForFrame is actually waiting for (see design 2.12 - the completion is a
    // fact of the frame, not a callback into anybody's thread).
    //   The per-pass cost still has to reach the plan a host reads, and the PLAN belongs to the content half (it is
    // written by addPass, removePass and the compile). Inline that is this thread, so it is published right here;
    // with a frame thread it is published by the next frame(), which by then has waited for this half - so the engine
    // half never touches the engine's own tables (design 2.12: the halves share no mutable state).
    if (!pump_->threaded()) {
        publishEngineMicros(frame);
    }
    else {
        pump_->notePendingMicros(frame);
    }
    publishFrameAnswer(frame);
}

void RenderEngine::answerEngineHalf(detail::EngineFrame& frame)
{
    // A FRAME IS ALWAYS ANSWERED (design 2.12). An engine half that throws used to leave the frame unanswered - inline
    // the exception reached the host, and on a frame thread it left the thread function, which is `std::terminate`.
    // Neither is an answer, so a throw becomes one here: the same completion a host's waitForFrame blocks on, plus a
    // coded report saying the frame was not drawn.
    try {
        runEngineHalf(frame);
    }
    catch (const std::exception& error) {
        reportEngineHalfFailure(frame, error.what());
    }
    catch (...) {
        reportEngineHalfFailure(frame, "an exception that is not a std::exception");
    }
}

void RenderEngine::reportEngineHalfFailure(detail::EngineFrame& frame, const char* what)
{
    // ONE REPORT, THEN A DEFINITE OUTCOME. Severity is Error: unlike the wiring problems this route usually carries,
    // this frame drew nothing at all - and a host that only counted warnings would never learn why its picture stopped.
    RenderDiagnostic diagnostic =
        detail::engineProblem(u8"engine.frame.failed",
                              DiagnosticCategory::SubmissionFailed,
                              String(u8"the frame's engine half threw, so the frame was not drawn: ") + String::fromUtf8(what),
                              String(u8"frame ") + String::fromUtf8(std::to_string(frame.number).c_str()));
    diagnostic.severity = DiagnosticSeverity::Error;
    diagnostics_.report(diagnostic);

    frame.outcome = PresentOutcome::Failed;
    publishFrameAnswer(frame);
}

void RenderEngine::publishFrameAnswer(detail::EngineFrame& frame)
{
    // The picture's fate and the frame's id travel as ONE word (see detail::FramePump), and the frame's whole answer
    // lands beside it: which publication and camera the picture was taken from, what the frame consisted of, and what
    // it reported. ONE function for an answered frame and a failed one: a host reading the answer must not have to
    // know which of the two produced it.
    pump_->publishPresent(frame.number, frame.outcome);
    // THE FRAME'S WHOLE ANSWER, in one value. A snapshot handle and a camera value are all it shares, so the pump's
    // lock is held for a copy in both directions - a host never sees half of this frame's answer beside half of the
    // previous one.
    FrameResult result;
    result.frame          = frame.number;
    result.present        = frame.outcome;
    result.query.snapshot = frame.presented_snapshot;
    result.query.view     = frame.presented_view;
    for (const FrameDescription::Pass& pass : frame.description.passes) {
        ++result.passes; // every pass the compiled value CARRIED (a clear-only or idle pass is still one)
        if (!pass.draw.has_value()) {
            continue;
        }
        for (const DrawPacket& draw : pass.draw->packets) {
            if (!packetDraws(draw)) {
                continue; // a packet with no view / no program / no commands issues nothing
            }
            ++result.draw_packets;
            result.draws += drawItemsOf(draw);
        }
    }
    const std::size_t reported = diagnosticCount();
    result.diagnostics = reported >= frame.diagnostics_before ? static_cast<std::uint64_t>(reported - frame.diagnostics_before) : 0U;
    pump_->publishResult(std::move(result));
    pump_->markCompleted(frame.number);
}

void RenderEngine::publishEngineMicros(const detail::EngineFrame& frame)
{
    // `record_micros` keeps meaning "describe + collect": each half measured its own share, and this adds the engine
    // half's (see detail::SessionShape::addEngineMicros). Called only when the content half is not looking (see the
    // two call sites), so it takes no lock.
    for (std::size_t index = 0; index < frame.engine_order.size() && index < frame.engine_micros.size(); ++index) {
        shape_->addEngineMicros(frame.engine_order[index], frame.engine_micros[index]);
    }
}

std::uint64_t RenderEngine::contentCollectCount() const noexcept
{
    return publications_->memoStores();
}

std::uint64_t RenderEngine::contentCollectReuseCount() const noexcept
{
    return publications_->memoHits();
}

int RenderEngine::areaWidth() const noexcept
{
    return surface_->width();
}

int RenderEngine::areaHeight() const noexcept
{
    return surface_->height();
}

std::optional<int> RenderEngine::passOrder(raw_ptr<RenderPass> pass) const
{
    return shape_->passOrder(pass);
}

FrameTable RenderEngine::frameTable() const
{
    return shape_->frameTable();
}

const FrameDescription& RenderEngine::frameDescription() const noexcept
{
    // What the last compile produced. Before the first frame it is empty: nothing has been decided yet, and an
    // invented value would be a second answer to "what does this frame do".
    return pump_->currentFrame().description;
}

bool RenderEngine::sessionChangeAllowed(const char* verb)
{
    if (!pump_->busy()) {
        // Nothing is recording: the change is applied and the episode re-arms,
        // so a later, separate mistake still speaks once.
        session_change_episode_.rearm();
        return true;
    }
    if (session_change_episode_.shouldReport()) {
        reportEngineProblem(detail::engineProblem(u8"engine.pass.changed_during_frame",
                                                  DiagnosticCategory::PassProtocolViolation,
                                                  String(u8"a session's passes cannot change while a frame is being recorded: ") + String::fromUtf8(verb) +
                                                      String(u8" was refused (wait for the frame with RenderEngine::waitForFrame, or change "
                                                             "passes between frames)")));
    }
    return false;
}

void RenderEngine::removePassesOwnedBy(const void* owner) noexcept
{
    // The rule is the engine's (the pump knows whether a frame is recording); taking the rows back is the shape's.
    if (!sessionChangeAllowed("removePass")) {
        return;
    }
    shape_->removePassesOwnedBy(owner, device_.get());
}

PlanResult RenderEngine::applyPlan(const Plan& plan)
{
    return applyPlan(plan, nullptr);
}

PlanResult RenderEngine::applyPlan(const Plan& plan, const void* owner)
{
    // THE SHAPE IS A VALUE, so applying it is one call - but the table it replaces is what a frame may be walking,
    // so the one rule every shape change obeys still holds here (see sessionChangeAllowed). A busy engine is a
    // REFUSAL, not a problem with the shape: the plan was not even examined, and the same call works between frames.
    if (!sessionChangeAllowed("applyPlan")) {
        PlanResult refused;
        refused.outcome = PlanResult::Outcome::Refused;
        refused.refusal = PlanRefusal::Busy;
        return refused;
    }
    return shape_->applyPlan(plan, owner, device_.get());
}

bool RenderEngine::releaseContent(raw_ptr<const Object> content)
{
    raw_ptr<RenderBackend> target = device();
    return target != nullptr && target->releaseContent(content);
}

bool RenderEngine::releaseContent(const vn::Buffer<float>& buffer)
{
    raw_ptr<RenderBackend> target = device();
    return target != nullptr && target->releaseContent(buffer);
}

bool RenderEngine::releaseContent(const vn::Buffer<std::uint32_t>& buffer)
{
    raw_ptr<RenderBackend> target = device();
    return target != nullptr && target->releaseContent(buffer);
}

std::size_t RenderEngine::passCount() const
{
    return shape_->passCount();
}

bool RenderEngine::bindPassContent(raw_ptr<RenderPass> pass, intrusive_ptr<Scene> content)
{
    return shape_->bindPassContent(pass, std::move(content));
}

raw_ptr<Scene> RenderEngine::contentOf(raw_ptr<RenderPass> pass) const
{
    return shape_->contentOf(pass);
}

raw_ptr<RenderPass> RenderEngine::pass(const String& name) const
{
    return shape_->pass(name);
}

raw_ptr<RenderPass> RenderEngine::passFor(const String& name, const void* owner) const
{
    return shape_->passFor(name, owner);
}

Plan RenderEngine::plan() const
{
    return shape_->plan();
}

Plan RenderEngine::planFor(const void* owner) const
{
    return shape_->planFor(owner);
}

std::size_t RenderEngine::transientTargetCount() const noexcept
{
    return resources_->transientCount();
}

raw_ptr<RenderTarget> RenderEngine::transientTarget(const String& identity) const
{
    return resources_->transient(identity);
}

void RenderEngine::expandFrameValue(detail::EngineFrame& frame)
{
    // A FRAME IS OPEN: the token every per-view memo is keyed by (a list outlives one frame, but nothing is memoised
    // outside one - see DrawListMemo). Opened HERE and not where the frame was built, because it writes the publications'
    // memos - the very state this half reads - and because the token travels on the frame (see detail::EngineFrame):
    // a frame answered late opens its OWN number, never the one the content has moved on to.
    publications_->beginFrame(frame.content_frame);

    // THE ENGINE HALF'S EXPANSION: every draw of the described frame is answered here, one expansion per pass, in
    // run order (see design 2.12 - the two halves are the seam a thread crosses, so they are two functions and not one
    // loop). Nothing in this function reads content: every scene draw was bound to its content's publication by the
    // content half (see detail::bindSceneDraws) and `expandPass` asks that publication, which is exactly what makes it
    // safe to run away from the content's thread.
    //
    // record_micros keeps meaning "describe + collect": the content half measured its part per pass, and the
    // expansion adds its own here - one clock read per pass, not per draw.
    std::size_t entry = 0;
    frame.engine_micros.clear();
    for (const std::uint32_t index : frame.engine_order) {
        FrameDescription::Pass& described = frame.description.passes[entry++];
        const auto              started   = std::chrono::steady_clock::now();
        if (described.draw.has_value()) {
            detail::expandPass(*described.draw);
        }
        frame.engine_micros.push_back(
            static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - started).count()));
    }
}

void RenderEngine::publish(const String& name, intrusive_ptr<RenderTarget> target)
{
    const String problem = resources_->publish(name, std::move(target));
    if (!problem.empty()) {
        reportEngineProblem(detail::engineProblem(u8"engine.target.publish_refused", vn::graphics::DiagnosticCategory::ContentSkipped, problem));
    }
}

raw_ptr<RenderTarget> RenderEngine::resolve(const String& name) const
{
    // The last compiled frame's answer: a pass publication (that pass ran, so its content is this frame's),
    // else a standing host binding.
    return resources_->resolve(name);
}

void RenderEngine::unpublish(const String& name)
{
    // A pass' publication lives only inside the frame that compiled it (see compileFrame), so the only binding
    // there is to withdraw is the host's: the next compile stops answering the name on its own.
    resources_->unpublish(name);
}

void RenderEngine::setDefaultContentProgram(intrusive_ptr<const ShaderProgram> program)
{
    default_content_program_ = std::move(program);
    if (session_ != nullptr) {
        // Forwarded straight away: the driver treats a call before initialize() as the session
        // decision and one after it as a live switch (rebuild the window sets, drop the slots), so
        // the engine's accessor and the picture cannot disagree.
        session_->setDefaultContentProgram(default_content_program_);
    }
}

intrusive_ptr<const ShaderProgram> RenderEngine::defaultContentProgram() const
{
    return default_content_program_;
}

void RenderEngine::announceAreaSize(raw_ptr<const RenderArea> from, int width, int height)
{
    // The rules (only the engine's own area is heard, the number is recorded even before a device exists) live with
    // the ledger that owns the size: this is the door the area knocks on (see RenderArea::resize).
    surface_->announce(from, width, height);
}

bool RenderEngine::drivesArea(raw_ptr<const RenderArea> area) const noexcept
{
    // The question RenderArea::present asks before it lets a declaration through (see RenderArea): one engine drives
    // one area, and an area another engine drives would present nothing at all.
    return area != nullptr && surface_->area() == area;
}

intrusive_ptr<RenderArea> RenderEngine::createArea()
{
    // ONE engine drives ONE area, and the ledger owns it (see detail::SurfaceLedger): the relation has an owner
    // instead of a paragraph of documentation, so "the area must die first" and "a second area is refused" stop
    // being conventions.
    return surface_->createArea(*this);
}

VN_GRAPHICS_NS_END
