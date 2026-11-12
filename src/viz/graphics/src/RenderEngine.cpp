#include <algorithm>
#include <functional>
#include <utility>
#include <vine/graphics/RenderEngine.hpp>

#include <vine/graphics/BuiltinShaders.hpp>
#include <vine/graphics/ImageRef.hpp>
#include <vine/graphics/RenderBackend.hpp>
#include <vine/graphics/RenderPass.hpp>
#include <vine/graphics/RenderSurface.hpp>
#include <vine/graphics/RenderTarget.hpp>
#include <vine/graphics/Scene.hpp>
#include <vine/graphics/ScreenPass.hpp>

#include "EngineFrame.hpp"
#include "FrameResources.hpp"
#include "FrameRunner.hpp"
#include "FrameScheduler.hpp"
#include "FrameWorker.hpp"

VN_GRAPHICS_NS_BEGIN

namespace
{} // namespace

String RenderEngine::reportedPassName(raw_ptr<const RenderPass> pass)
{
    if (pass == nullptr || pass->name().empty()) {
        return String(u8"(unnamed)");
    }
    return pass->name();
}

VN_OBJECT_META_IMPL(RenderEngine, vn::Object);

RenderEngine::RenderEngine()
  : resources_(std::make_unique<detail::FrameResources>())
  , scheduler_(std::make_unique<detail::FrameScheduler>())
  , frame_slots_{ std::make_unique<detail::EngineFrame>(), std::make_unique<detail::EngineFrame>() }
  , worker_(std::make_unique<detail::FrameWorker>())
  // THE PUBLISHED SNAPSHOT IS THE SOURCE (5b-1; see the design's 2.11): every scene pass is published, then expanded
  // from that content's snapshot source, so a frame's draws name nothing the content owns - which is what P3 will move
  // to another thread. The per-view memo it needs (one camera drawn by two passes collects once) is the shared
  // DrawListMemo.
  , frame_mode_(detail::FrameMode::Published)
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
    // The area is OURS (createSurface), so it is released HERE, while this engine is still an object and `area_` is
    // already null: the area's own destructor unnames its view, and a host still holding a handle keeps a surface
    // whose engine is gone rather than a dangling one.
    intrusive_ptr<RenderSurface> area = std::move(area_);
    // Every lease this engine handed out stops referring to it HERE, before anything it owns is torn down: a holder
    // that outlives the engine (a pipeline a host kept) then has nothing to call into, instead of a pointer into
    // freed memory.
    if (lease_cell_ != nullptr) {
        lease_cell_->target = nullptr;
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
    // The route this engine installed on the OLD device goes with the old device: it was borrowed there (see
    // RenderDevice::setDiagnosticsRoute) and is about to stop being valid for it.
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
        // The default content program travels with the device, not with the attach: the device is brought up by
        // its CREATOR (see RenderDevice::initialize), so the program has to be known before that - the engine is
        // the only party that has it, and this is the moment the two are introduced.
        device_->setDefaultContentProgram(default_content_program_);
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

void RenderEngine::reportEngineProblem(vn::graphics::DiagnosticSeverity severity, vn::graphics::DiagnosticCategory category, const String& message)
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
    return device_ != nullptr ? device_->executor().frameReport() : FrameReport{};
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
            reportEngineProblem(DiagnosticSeverity::Warning,
                                DiagnosticCategory::UnsupportedRequest,
                                u8"RenderEngine::initialize(): the device this engine borrows is not up - the party "
                                u8"that created it brings it up (RenderDevice::initialize); this engine only drives "
                                u8"frames through it");
        }
        return false;
    }
    // A device that brings up a window of its OWN (the host named none) is asked for that target here, once:
    // the frame path has to have something to present through, and the device is the party that knows whether
    // it presents at all (see RenderDevice::createSurfaceTarget). A device that presents nothing answers
    // nullptr with NoPresentation and NO report - a display-less device is a first-class state, and the frame
    // path then presents nowhere. An AREA that holds one keeps it (a host that bound a target is never
    // overridden); an engine with no area at all keeps the answer here, because its frames still have to name
    // a target when the device presents through a window of its own.
    if (area_ != nullptr && !area_->hasTarget()) {
        SurfaceTargetResult why = SurfaceTargetResult::Ok;
        if (raw_ptr<WindowPresentTarget> own = device_->createSurfaceTarget(nullptr, &why); own != nullptr) {
            area_->bindTarget(own);
        }
    }
    else if (area_ == nullptr) {
        SurfaceTargetResult why = SurfaceTargetResult::Ok;
        own_target_             = device_->createSurfaceTarget(nullptr, &why);
    }
    // A size announced before initialization (a host sizes its widget before it has a device) is NOT replayed here:
    // the first frame hands it to the target at its head (see frame), the same path every later size takes - one
    // place that resizes a swapchain instead of two, and nothing to keep in step when the target object changes.
    // NO PRE-FRAME WARM-UP. It used to execute every enabled, non-clearing pass once before the first frame, so
    // a backend could see its whole pass set before presenting - which mattered when a frame arrived as a
    // sequence of calls and content was built lazily inside a drawing call. A frame now arrives as ONE value
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
    // joined, never detached) and the runner is destroyed - a frame still executing while this call hands the device
    // back would touch objects its creator may already own (design F10 / N11).
    if (runner_ != nullptr) {
        runner_->waitIdle();
        runner_->stop();
        runner_.reset();
    }
    if (area_ != nullptr) {
        area_->bindTarget(nullptr);
    }
    own_target_  = nullptr;
    initialized_ = false;
    // The target this engine was resizing is gone (the area let go of it, or the device will): a re-initialized engine
    // asks its device for a new one, whose size is the device's own until the next frame hands it the announced one.
    applied_width_  = 0;
    applied_height_ = 0;
}

raw_ptr<const RenderPass> RenderEngine::passNeedingAnUnsupportedTarget() const
{
    if (device_ == nullptr || device_->capabilities().render_targets) {
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

PrepareReport RenderEngine::prepare()
{
    // The two reasons nothing can be prepared are the same two answers frame() gives, and they are values rather
    // than silence: a host gating a load on this call has to be able to tell "no backend" from "not up yet".
    if (device_ == nullptr) {
        return PrepareReport{ PrepareReport::Outcome::NoDevice };
    }
    if (!initialized_) {
        return PrepareReport{ PrepareReport::Outcome::NotInitialized };
    }

    // ONE FRAME IN FLIGHT: the engine half that owns the other slot is finished first, exactly as frame() does -
    // the expansion below writes the per-view memos, and those are the half's state (design 2.12).
    if (runner_ != nullptr) {
        runner_->waitIdle();
        if (detail::EngineFrame* const finished = pending_micros_.exchange(nullptr, std::memory_order_acquire)) {
            publishEngineMicros(*finished);
        }
    }

    // THE FRAME THE NEXT frame() WOULD BUILD, built and expanded the same way, and handed over as a QUERY. The slot
    // is NOT advanced and no frame number is taken: nothing here is a frame, so the next frame() builds this same
    // slot again from the declarations AS THEY STAND THEN - a camera that moved in between is in it, which is what
    // keeps "prepared" a key rather than a frozen picture.
    detail::EngineFrame& frame = *frame_slots_[frame_slot_];
    buildContentHalf(frame);
    // THE EXPANSION IS PART OF THE ANSWER. A compiled half is keyed per DRAWING CALL (its vertex layout, variant and
    // counts are the command's), so a frame value whose draws carry no collected commands would describe nothing to
    // build - the answer would be trivially zero and the first real frame would still compile. This is the same
    // value step the engine half runs (see FrameExpand): no device call and no submission.
    expandFrameValue(frame);
    return device_->executor().prepareFrame(frame.description, frame.target);
}

FrameOutcome RenderEngine::frame()
{
    // A FRAME IS TWO HALVES (design 2.12): one built where the content is, one answered where the device is. Without a
    // frame thread they run one after the other, here; with one, this call builds and hands over, and the picture
    // follows one frame later.

    // The two reasons a frame cannot be driven are different answers, and both used to look like a frame that ran
    // and drew nothing: no device to hand anything to, or a device the engine is not attached to. The caller gets
    // the reason as a value (see FrameOutcome); "ran but issued no draw" stays a FrameReport fact.
    if (device_ == nullptr) {
        return FrameOutcome::NoDevice;
    }
    if (!initialized_) {
        return FrameOutcome::NotInitialized;
    }

    // ONE FRAME IN FLIGHT: with a frame thread, the previous frame's engine half is finished before this frame touches
    // the mirrors or a slot. That is the whole pipeline (1 + 1), and it is why a caller never queues frames: the wait
    // is for the frame BEFORE the one it is about to build, which is normally already done (design 2.12, D12).
    if (runner_ != nullptr) {
        runner_->waitIdle();
        // The frame that just finished reported its per-pass cost on its own value; the plan is the content half's, so it
        // is written here - the one moment nobody else is in it.
        if (detail::EngineFrame* const finished = pending_micros_.exchange(nullptr, std::memory_order_acquire)) {
            publishEngineMicros(*finished);
        }
    }

    // The content half fills one slot; the other is what the frame thread holds (inline, it is only the spare).
    detail::EngineFrame& next = *frame_slots_[frame_slot_];
    frame_slot_               = (frame_slot_ + 1) % frame_slots_.size();
    next.number               = ++frame_number_;
    buildContentHalf(next);

    if (runner_ == nullptr) {
        runEngineHalf(next);
        return FrameOutcome::Ran;
    }
    detail::EngineFrame* const submitted = &next;
    runner_->submit([this, submitted]() { runEngineHalf(*submitted); });
    return FrameOutcome::Submitted;
}

void RenderEngine::setFrameThreading(FrameThreading mode)
{
    const bool wanted = mode == FrameThreading::EngineThread;
    if (wanted == (runner_ != nullptr)) {
        return; // already there
    }
    if (wanted) {
        // The thread exists only when a host asked for one: an engine that never does allocates nothing and starts
        // nothing, and the inline path stays literally the same code.
        runner_ = std::make_unique<detail::FrameRunner>(detail::RunnerMode::Threaded);
        return;
    }
    // Back to inline: a frame in flight is finished first (the thread is joined, not detached), so nothing is being
    // answered while the host goes on to describe frames here.
    runner_->waitIdle();
    runner_->stop();
    runner_.reset();
}

FrameThreading RenderEngine::frameThreading() const noexcept
{
    return runner_ != nullptr ? FrameThreading::EngineThread : FrameThreading::Inline;
}

std::uint64_t RenderEngine::frameNumber() const noexcept
{
    return frame_number_;
}

bool RenderEngine::waitForFrame(std::uint64_t number, std::chrono::milliseconds timeout)
{
    if (number == 0U) {
        return false; // no frame was ever built, so nothing can be "on screen"
    }
    if (frame_completed_.load(std::memory_order_acquire) >= number) {
        return true;
    }
    if (runner_ == nullptr) {
        // Inline: every frame this engine built was answered before frame() returned, so a number this engine does not
        // know is simply not built yet.
        return frame_completed_.load(std::memory_order_acquire) >= number;
    }
    (void)runner_->wait(timeout);
    return frame_completed_.load(std::memory_order_acquire) >= number;
}

bool RenderEngine::waitForFrame(std::chrono::milliseconds timeout)
{
    return waitForFrame(frame_number_, timeout);
}

std::optional<CameraSnapshot> RenderEngine::presentedCamera() const
{
    const std::lock_guard<std::mutex> lock(presented_camera_mutex_);
    return presented_camera_;
}

RenderEngine::PresentRecord RenderEngine::presentRecord() const noexcept
{
    const std::uint64_t packed = present_record_.load(std::memory_order_acquire);
    PresentRecord       record;
    record.frame   = packed >> 8U;
    record.outcome = static_cast<PresentOutcome>(packed & 0xFFU);
    return record;
}

void RenderEngine::buildContentHalf(detail::EngineFrame& frame)
{
    // Everything below describes into THIS slot: `frame_` is the compile's way to reach it (see buildFrameValue), and
    // it is reset by the next frame - which is why the engine half takes its frame as an argument instead.
    frame_ = &frame;

    // THE FRAME'S SIZE AND TARGET ARE TAKEN ONCE, when the frame starts: the passes below lay themselves out on these
    // numbers, and the engine half applies THAT size to THAT target - so an announcement arriving while the frame is
    // being answered belongs to the next frame instead of tearing this one in half (design 2.12).
    frame.width         = surface_width_;
    frame.height        = surface_height_;
    frame.target        = (area_ != nullptr && area_->target() != nullptr) ? area_->target() : own_target_;
    frame.content_frame = content_frame_;
    // THE CAMERA THE USER WILL BE LOOKING AT, taken now as the same value `describe` puts in the frame's draws: the pass
    // that presents this engine's area is the picture, so its camera is the one a pointer query must aim with once this
    // frame is answered (design N17). Nothing presents the area (an offline bake, a device with no window) => empty,
    // and a query keeps aiming with the live camera.
    frame.presented_camera = CameraSnapshot{};
    if (area_ != nullptr) {
        if (raw_ptr<RenderPass> presenting = area_->presentingPass();
            presenting != nullptr && presenting->camera() != nullptr) {
            frame.presented_camera = CameraSnapshot::of(*presenting->camera());
        }
    }

    // Announce the content frame BEFORE the passes run: every scene this frame
    // renders may then memoise the command list it collects (see
    // Scene::setContentFrame), so the multi-pass pipelines that draw one scene
    // through one camera several times per frame (G-buffer + lighting + ...)
    // walk the tree once instead of once per pass. The token is idempotent, so
    // a scene shared by several slots is announced any number of times.
    ++content_frame_;
    frame.content_frame = content_frame_;
    for (const auto& slot : slots_) {
        if (slot.pass == nullptr) {
            continue;
        }
        if (raw_ptr<Scene> scene = slot.pass->content()) {
            scene->setContentFrame(content_frame_);
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

    // A backend whose capabilities say "no off-screen targets" IGNORES a non-null target, so a pass staged
    // through one is recorded where the pipeline never put it — a WRONG PICTURE rather than a slow one,
    // and silent unless the engine asks (see RenderDevice::capabilities). Asked here, once per
    // frame, and reported as an EPISODE: the pass is still run (its content is the host's to place), but
    // the host is told that the pipeline it built cannot mean what it says.
    if (raw_ptr<const RenderPass> unsupported = passNeedingAnUnsupportedTarget(); unsupported != nullptr) {
        if (unsupported_target_reported_.shouldReport()) {
            reportEngineProblem(vn::graphics::DiagnosticSeverity::Warning,
                                vn::graphics::DiagnosticCategory::TargetBuildFailed,
                                String(u8"the backend reports no off-screen render target support, so pass '") + reportedPassName(unsupported) +
                                    String(u8"' (and any other pass rendering into a target) is not drawn where "
                                           u8"the pipeline expects it: the backend ignores the target and draws "
                                           u8"into the window"));
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
    // THE WINDOW HALF OF THE SIZE FOLLOWS THE FRAME (design 2.12). The size is the FRAME's (taken when it started, see
    // buildContentHalf), so a host resizing while this frame is answered costs the NEXT frame its resize and leaves
    // this one whole. The swapchain is told here, at the frame's head, once per changed size - it is the one place a
    // frame waits on the device, and a host resizing twice between two frames costs one resize instead of two.
    if (frame.target != nullptr && frame.width > 0 && frame.height > 0
        && (frame.width != applied_width_ || frame.height != applied_height_)) {
        frame.target->resize(frame.width, frame.height);
        applied_width_  = frame.width;
        applied_height_ = frame.height;
    }

    expandFrameValue(frame);

    // The WHOLE FRAME happens inside this ONE call (see RenderFrameExecutor::executeFrame): the device acquires the
    // presentable image, the record submits, and the frame's tail presents it through the target it names. The engine
    // does not present afterwards - presenting twice would be presenting somebody else's frame (or nothing at all).
    frame.outcome = device_->executor().executeFrame(frame.description, frame.target);

    // A LOST PICTURE IS THE SURFACE ASKING TO BE REBUILT, and the size is how that request is made: a swapchain is
    // rebuilt by re-announcing the extent it hangs off (PresentTarget::resize -> the session follows the surface and
    // builds a new one, counted as a device wait). So the engine FORGETS what it told the target, and the next frame
    // hands it the same numbers again - one repair, on the side that owns the swapchain's size. If that repair does
    // not take (the window itself is gone), the next frame answers Lost again, and the HOST's move is the attach
    // (a new handle, a new target) - never both at once, and never a repair loop inside the frame.
    if (frame.outcome == PresentOutcome::Lost) {
        applied_width_  = 0;
        applied_height_ = 0;
    }

    // The frame is answered: what a host's waitForFrame is actually waiting for (see design 2.12 - the completion is a
    // fact of the frame, not a callback into anybody's thread).
    //   The per-pass cost still has to reach the plan a host reads, and the PLAN belongs to the content half (it is
    // written by addPass, removePass and the compile). Inline that is this thread, so it is published right here;
    // with a frame thread it is published by the next frame(), which by then has waited for this half - so the engine
    // half never touches the engine's own tables (design 2.12: the halves share no mutable state).
    if (runner_ == nullptr) {
        publishEngineMicros(frame);
    }
    else {
        pending_micros_.store(&frame, std::memory_order_release);
    }
    // The picture's fate and the frame's id travel as ONE word: a host reading presentRecord() while the next frame is
    // being answered must never see this frame's outcome beside that frame's id.
    present_record_.store((frame.number << 8U) | static_cast<std::uint64_t>(frame.outcome), std::memory_order_release);
    // The picture's camera travels with the answer for the same reason its outcome does: a query must aim with the frame
    // the user can see (design N17), and it is a value, so the copy under a lock is all the sharing there is.
    {
        const std::lock_guard<std::mutex> lock(presented_camera_mutex_);
        presented_camera_ = frame.presented_camera.present ? std::optional<CameraSnapshot>(frame.presented_camera)
                                                           : std::nullopt;
    }
    frame_completed_.store(frame.number, std::memory_order_release);
}

void RenderEngine::publishEngineMicros(const detail::EngineFrame& frame)
{
    // `record_micros` keeps meaning "describe + collect": each half measured its own share, and this adds the engine
    // half's. Called only when the content half is not looking (see the two call sites), so it takes no lock.
    for (std::size_t index = 0; index < frame.engine_order.size() && index < frame.engine_micros.size(); ++index) {
        const std::uint32_t slot = frame.engine_order[index];
        if (slot < slots_.size()) {
            slots_[slot].record_micros += frame.engine_micros[index];
        }
    }
}

std::uint64_t RenderEngine::contentCollectCount() const noexcept
{
    return worker_ != nullptr ? worker_->memoStores() : 0U;
}

std::uint64_t RenderEngine::contentCollectReuseCount() const noexcept
{
    return worker_ != nullptr ? worker_->memoHits() : 0U;
}

int RenderEngine::surfaceWidth() const noexcept
{
    return surface_width_;
}

int RenderEngine::surfaceHeight() const noexcept
{
    return surface_height_;
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
        entry.tag           = pass->declaration().tag;
        entry.content       = pass->content();
        entry.camera        = pass->camera();
        entry.layer_mask    = pass->layerMask();
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
        entry.depth_range = pass->depthRange();
        plan.passes.push_back(std::move(entry));
    }
    return plan;
}

const FrameDescription& RenderEngine::frameDescription() const noexcept
{
    // What the last compile produced. Before the first frame it is empty: nothing has been decided yet, and an
    // invented value would be a second answer to "what does this frame do".
    return frame_->description;
}

bool RenderEngine::sessionChangeAllowed(const char* verb)
{
    if (runner_ == nullptr || !runner_->busy()) {
        // Nothing is recording: the change is applied and the episode re-arms,
        // so a later, separate mistake still speaks once.
        session_change_episode_.rearm();
        return true;
    }
    if (session_change_episode_.shouldReport()) {
        reportEngineProblem(DiagnosticSeverity::Warning,
                            DiagnosticCategory::PassProtocolViolation,
                            String(u8"a session's passes cannot change while a frame is being recorded: ") +
                                String::fromUtf8(verb) +
                                String(u8" was refused (wait for the frame with RenderEngine::waitForFrame, or change "
                                        "passes between frames)"));
    }
    return false;
}

intrusive_ptr<RenderPass> RenderEngine::addPass(intrusive_ptr<RenderPass> pass, int order)
{
    return addPass(std::move(pass), nullptr, order);
}

intrusive_ptr<RenderPass> RenderEngine::addPass(intrusive_ptr<RenderPass> pass, intrusive_ptr<Scene> content, int order)
{
    if (!sessionChangeAllowed("addPass")) {
        return nullptr;
    }
    Slot slot;
    slot.pass  = std::move(pass);
    slot.order = order;
    // THE ADD HAPPENS FIRST, THE CONTENT SECOND. Content is a field of the pass' declaration (PassDesc::content),
    // so writing it before insertSlot() would edit a pass that this call then refuses to register - a duplicate
    // add would rebind the content of a pass it did not add. Registering first makes "ignored" mean ignored.
    intrusive_ptr<RenderPass> registered = insertSlot(std::move(slot));
    if (registered == nullptr) {
        return nullptr;
    }
    // Passing none leaves what the pass already states alone - a pass built with its own scene (an overlay) keeps
    // it on registration. Clearing is bindPassContent(pass, nullptr).
    if (content != nullptr) {
        registered->setContent(std::move(content));
    }
    // What comes back is the node the engine registered - the same instance, or null when it was already there
    // (see addPass(PassDesc), which explains why a caller wants a handle at all).
    return registered;
}

void RenderEngine::removePass(raw_ptr<RenderPass> pass)
{
    if (!sessionChangeAllowed("removePass")) {
        return;
    }
    // THE INVOCATION MAY BE THE PASS' LAST REFERENCE (its slot is one, and a host that just added a pass and forgot
    // its handle has nothing else), so hold it for the length of this call: the erase below can drop the last count,
    // and everything after it reads the pass - the same rule clearPasses states for the same reason (measured: a
    // heap-use-after-free of the pass' render target, found by the thread sanitizer because it checks liveness too).
    const intrusive_ptr<RenderPass> keep_alive{ pass };
    const auto                      old_size = slots_.size();
    // The order the pass ran at is not kept anywhere: the slot list IS the answer (see passOrder), and a pass
    // that is gone has none.
    raw_ptr<RenderTarget> transient_owned = nullptr;
    for (const Slot& slot : slots_) {
        if (slot.pass.get() == pass) {
            transient_owned = slot.transient.get();
            break;
        }
    }
    slots_.erase(std::remove_if(slots_.begin(), slots_.end(), [pass](const Slot& slot) { return slot.pass.get() == pass; }), slots_.end());

    // A pass is registered at most once, so any removal drops its only user:
    // release the backend state it retained — keyed by the pass itself (see
    // RenderDevice::releasePass) — plus any off-screen target the pass owns. A
    // TRANSIENT it declared is not its own (this engine owns it, and the next compile lets it go when nothing
    // declares it any more), so it is not released here: that would hand the backend a target the engine still
    // holds for another pass declaring the same one.
    if (device_ != nullptr && pass != nullptr && slots_.size() != old_size) {
        device_->releasePass(pass);
        if (raw_ptr<RenderTarget> target = pass->renderTarget(); target != nullptr && target != transient_owned) {
            device_->releaseRenderTarget(target);
        }
    }
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

void RenderEngine::clearPasses()
{
    if (!sessionChangeAllowed("clearPasses")) {
        return;
    }
    // Keep the passes alive while their retained backend state is released: a pass may be owned only by its
    // slot, so dropping the slots first and then dereferencing a raw pointer would be a use-after-free.
    std::vector<intrusive_ptr<RenderPass>> removed;
    removed.reserve(slots_.size());
    for (const auto& slot : slots_) {
        if (slot.pass != nullptr) {
            removed.push_back(slot.pass);
        }
    }
    slots_.clear();

    if (device_ != nullptr) {
        for (const intrusive_ptr<RenderPass>& pass : removed) {
            device_->releasePass(pass.get());
            raw_ptr<RenderTarget> target = pass->renderTarget();
            // A transient is the resources table's, not the pass': releaseAll() below lets it go once, after
            // every pass has stopped naming it.
            if (target != nullptr && !resources_->owns(target)) {
                device_->releaseRenderTarget(target);
            }
        }
    }
    resources_->releaseAll(device_.get());
}

std::size_t RenderEngine::passCount() const
{
    return slots_.size();
}

bool RenderEngine::bindPassContent(raw_ptr<RenderPass> pass, intrusive_ptr<Scene> content)
{
    for (auto& slot : slots_) {
        if (slot.pass.get() == pass) {
            // The declaration owns it (PassDesc::content), so there is one storage and the compile reads the same
            // scene this call bound.
            slot.pass->setContent(std::move(content));
            return true;
        }
    }
    // An unregistered pass (or null) is not a binding: say so instead of reporting success for a change that did
    // not happen. The caller can still register it and bind again.
    return false;
}

raw_ptr<Scene> RenderEngine::contentOf(raw_ptr<RenderPass> pass) const
{
    for (const auto& slot : slots_) {
        if (slot.pass.get() == pass) {
            return slot.pass->content();
        }
    }
    return nullptr;
}

raw_ptr<RenderTarget> RenderEngine::resolveDeclaredTarget(raw_ptr<RenderPass> pass, raw_ptr<RenderTarget> target)
{
    // Resolved against what the frame has produced so far (FrameResources): a target the frame fills LATER is
    // not something this pass can read, which is the same answer the per-pass resolution used to give.
    if (resources_->wasProduced(static_cast<raw_ptr<const RenderTarget>>(target))) {
        return target;
    }
    // A pass reading what it draws into is the feedback pattern, and whether the actual draw call is
    // a feedback loop is the BACKEND's to judge (the vsg backend rejects source == destination and
    // says so): this layer stays quiet about it, so one mistake is reported once.
    if (pass->renderTarget() == target) {
        return nullptr;
    }
    reportUnproducedInput(pass,
                          OutputIdentity::targetOf(*target),
                          String(u8"the input target '") + (target->name().empty() ? String(u8"(unnamed target)") : target->name()) + u8"'");
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
    const bool produced     = resources_->wasProduced(static_cast<raw_ptr<const RenderTarget>>(target));
    if (depth_usable && produced) {
        return target;
    }
    // Same as above: a pass that reads the target it draws into is the feedback pattern, judged by
    // the backend (the depth case included — its layout is the backend's business).
    if (pass->renderTarget() == target) {
        return nullptr;
    }
    reportUnproducedInput(pass, OutputIdentity::of(image), String(u8"the input image '") + image.label() + u8"'");
    return nullptr;
}

void RenderEngine::reportUnproducedInput(raw_ptr<RenderPass> pass, const OutputIdentity& identity, const String& what)
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
    identity.declared   = (identity.target == nullptr) ? &image : nullptr;
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
    identity.attachment = 0; // a depth attachment has no colour index; `depth` is what tells them apart
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
    /// Who claims which image: a fine promise, and the images of a coarse one. Two claimants of one
    /// image is what the collision report is about.
    /// Both tables (and the collision/unbound ones below) are the ENGINE's, cleared here: the pass count is small, so a
    /// flat scan both beats a tree and keeps its capacity - a steady frame allocates nothing while it checks the wiring.
    auto& filled           = wiring_filled_;
    auto& claimed          = wiring_claimed_;
    auto& colliding_images = wiring_colliding_;
    auto& unbound_declared = wiring_unbound_;
    filled.clear();
    claimed.clear();
    colliding_images.clear();
    unbound_declared.clear();

    // First wins in every table, exactly as the keyed containers did: a second filler of one target, or a second
    // claimant of one image, is a fact the reports below are about, not a replacement.
    const auto findFilled = [this](raw_ptr<const RenderTarget> target) -> const Declaration* {
        const auto it = std::find_if(wiring_filled_.begin(), wiring_filled_.end(), [target](const Declaration& entry) { return entry.target == target; });
        return it == wiring_filled_.end() ? nullptr : &*it;
    };
    const auto noteFilled = [this, &findFilled](Declaration declaration) {
        if (findFilled(declaration.target) == nullptr) {
            wiring_filled_.push_back(std::move(declaration));
        }
    };
    const auto findClaimed = [this](const OutputIdentity& identity) -> const Declaration* {
        const auto it =
            std::find_if(wiring_claimed_.begin(), wiring_claimed_.end(), [&identity](const Declaration& entry) { return entry.identity == identity; });
        return it == wiring_claimed_.end() ? nullptr : &*it;
    };
    const auto noteColliding = [this](const OutputIdentity& identity) {
        const auto known =
            std::any_of(wiring_colliding_.begin(), wiring_colliding_.end(), [&identity](const OutputIdentity& entry) { return entry == identity; });
        if (!known) {
            wiring_colliding_.push_back(identity);
        }
    };
    const auto isColliding = [this](const OutputIdentity& identity) {
        return std::any_of(wiring_colliding_.begin(), wiring_colliding_.end(), [&identity](const OutputIdentity& entry) { return entry == identity; });
    };
    const auto noteUnbound = [this](raw_ptr<const ImageRef> image, raw_ptr<const RenderPass> pass, bool as_output) {
        const auto known =
            std::any_of(wiring_unbound_.begin(), wiring_unbound_.end(), [image](const UnboundDeclaration& entry) { return entry.image == image; });
        if (!known) {
            wiring_unbound_.push_back(UnboundDeclaration{ image, pass, as_output });
        }
    };

    // Phase 1: who fills and who claims. The fillers come from ALL passes (a declaration is a
    // declaration), the claims from the ENABLED ones only — what the collision report is about is
    // "whoever runs last silently wins", and a disabled pass runs nothing.
    //
    // A host binding (publish) is filled by the HOST: it is there from the start of the frame and no
    // pass owns its hand-off, so a declared input addressing it is answered like one addressing a
    // pass' target.
    for (const auto& binding : resources_->hostBindings()) {
        if (binding.second != nullptr) {
            noteFilled(Declaration{ nullptr, binding.second.get(), nullptr, 0 });
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
            noteFilled(Declaration{ nullptr, drawn_into, pass, index });
        }

        // A content pass whose mask selects nothing. Legal (see PassDesc::layer_mask), and nearly always a
        // mistake, so it is said once per episode instead of showing up later as an empty picture with no
        // reason attached. The sentence states the declaration, not an outcome: what a pass does with a
        // content scene is the pass' own business (a screen pass uses one for its lights, not for a walk).
        if (pass->layerMask() == 0 && pass->content() != nullptr) {
            reportEngineProblem(vn::graphics::DiagnosticSeverity::Warning,
                                vn::graphics::DiagnosticCategory::ContentSkipped,
                                String(u8"pass '") + reportedPassName(pass) +
                                    String(u8"' has a layer mask of 0, so no layer of its content is selected —"
                                           u8" state the layers it draws (RenderPass::setLayerMask / Node::setLayers)"));
        }

        if (!pass->isEnabled()) {
            continue;
        }

        // A promise that names a WHOLE target: it claims every image that target has (the
        // declaration stays shape-agnostic; only the comparison is per image).
        if (raw_ptr<RenderTarget> promised = pass->outputTarget(); promised != nullptr) {
            noteFilled(Declaration{ nullptr, promised, pass, index });

            // A promise is a claim about the content a consumer gets, so it has to be about the
            // target this pass WRITES: promising a target the pass never draws into is a claim about
            // nothing, and it would make the collision report fire on wires that do not exist.
            if (pass->renderTarget() != promised) {
                const OutputIdentity identity = OutputIdentity::targetOf(*promised);
                {
                    reportEngineProblem(vn::graphics::DiagnosticSeverity::Warning,
                                        vn::graphics::DiagnosticCategory::ContentSkipped,
                                        String(u8"pass '") + reportedPassName(pass) + String(u8"' promises target '") + target_name(promised) +
                                            String(u8"' as its output but renders into '") + drawn_into_name(pass) +
                                            String(u8"', so the promise is about content this pass never writes"));
                }
            }

            std::vector<OutputIdentity>& promised_images = wiring_promised_images_;
            promised_images.clear();
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
                const Declaration* entry = findClaimed(identity);
                if (entry == nullptr) {
                    claimed.push_back(Declaration{ nullptr, promised, pass, index, identity });
                    continue;
                }
                if (entry->pass == pass) {
                    continue; // one pass claiming twice is one claim
                }
                noteColliding(identity);
                {
                    reportEngineProblem(vn::graphics::DiagnosticSeverity::Warning,
                                        vn::graphics::DiagnosticCategory::ContentSkipped,
                                        String(u8"two passes ('") + reportedPassName(entry->pass) + String(u8"' and '") + reportedPassName(pass) +
                                            String(u8"') claim ") + identity_name(identity) +
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
                noteUnbound(output, pass, true);
            }
            // Same rule as the coarse form: a fine promise is about a target this pass has to write.
            raw_ptr<RenderTarget> promised_target = output->target();
            if (promised_target != nullptr && pass->renderTarget() != promised_target) {
                const OutputIdentity identity = OutputIdentity::of(*output);
                {
                    reportEngineProblem(vn::graphics::DiagnosticSeverity::Warning,
                                        vn::graphics::DiagnosticCategory::ContentSkipped,
                                        String(u8"pass '") + reportedPassName(pass) + String(u8"' promises the image '") + output->label() +
                                            String(u8"' of target '") + target_name(promised_target) + String(u8"' as its output but renders into '") +
                                            drawn_into_name(pass) + String(u8"', so the promise is about content this pass never writes"));
                }
            }
            const OutputIdentity identity = OutputIdentity::of(*output);
            const Declaration*   entry    = findClaimed(identity);
            if (entry == nullptr) {
                claimed.push_back(Declaration{ output, output->target(), pass, index, identity });
            }
            else if (entry->pass != pass) {
                noteColliding(identity);
                {
                    const String first  = reportedPassName(entry->pass);
                    const String second = reportedPassName(pass);
                    // One format string per branch: the cases do not print the same thing (one image
                    // declared twice, two images that turn out to be one attachment, or an image
                    // claimed next to a whole target).
                    const String message =
                        (entry->image == output) ? String(u8"image '") + output->label() + String(u8"' is declared as the output of two passes ('") + first +
                                                       String(u8"' and '") + second + String(u8"'); a consumer of it gets whichever pass runs last")
                        : (entry->image != nullptr)
                            ? String(u8"two passes ('") + first + String(u8"' and '") + second + String(u8"') declared DIFFERENT images ('") +
                                  entry->image->label() + String(u8"' and '") + output->label() +
                                  String(u8"') that are the same attachment of one target; a consumer of it"
                                         u8" gets whichever pass runs last")
                            : String(u8"two passes ('") + first + String(u8"' and '") + second + String(u8"') claim the image '") + output->label() +
                                  String(u8"' of target '") + target_name(output->target()) +
                                  String(u8"', one of them as part of the whole target; a consumer of it gets"
                                         u8" whichever pass runs last");
                    reportEngineProblem(vn::graphics::DiagnosticSeverity::Warning, vn::graphics::DiagnosticCategory::ContentSkipped, message);
                }
            }
        }
    }

    // Phase 2: every declared input its consumer cannot draw from — nobody fills the image (or the
    // whole target), or the image asked for is a depth the target does not have. WHERE the producer was
    // registered is no longer a wiring fact: the scheduler orders a later-announced producer first (see
    // scheduleRows), so registration order says nothing about execution order. This is the structural half of
    // D37 (design §14.4); its name-based counterpart can only be judged once a frame has run, because a name
    // resolves per frame.
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
            const Declaration*   entry    = findFilled(target.get());
            // A pass reading what it draws into is a feedback loop the backend supports on purpose
            // (an overlay reading the frame it is about to composite into): never a bogus wire.
            if (entry != nullptr && entry->pass == pass) {
                continue;
            }
            if (entry != nullptr) {
                continue; // someone fills it; the scheduler puts that pass before this one
            }
            // The structural check speaks for this wire; the collect keeps its subject so the RESOLUTION of the
            // same input stays quiet about it (see unusableInputReported / reportUnproducedInput).
            unusable_inputs_this_frame_.emplace(pass, identity);
            {
                const String consumer = reportedPassName(pass);
                const String message  = String(u8"pass '") + consumer + String(u8"' declares the input target '") + target_name(target.get()) +
                                        String(u8"' but no pass writes it, so nothing fills it");
                reportEngineProblem(vn::graphics::DiagnosticSeverity::Warning, vn::graphics::DiagnosticCategory::ContentSkipped, message);
            }
        }

        for (const ImageInput& input : pass->inputs()) {
            raw_ptr<ImageRef> image = input.image();
            if (image == nullptr) {
                continue; // an imported texture has no producer/wiring rule to check
            }
            const OutputIdentity identity = OutputIdentity::of(*image);
            // An UNBOUND image can never be delivered (resolveDeclaredImage returns null for it, and
            // deliberately says nothing — declaring it is what is wrong). Reported once per IMAGE:
            // by the producer side above when a pass declares it as its output, otherwise here.
            if (!image->isBound()) {
                noteUnbound(image, pass, false);
                continue;
            }
            // An unbound image has no address, so no target account can say anything about it: only
            // a promise of that same object can.
            const Declaration* claim = findClaimed(identity);
            const Declaration* entry = (identity.target != nullptr) ? findFilled(identity.target) : nullptr;

            // A pass that promises the very image it reads, or draws into its target, is a feedback
            // loop the backend supports on purpose (an overlay reading the frame it composites
            // into): never a bogus wire.
            if ((claim != nullptr && claim->pass == pass) || (entry != nullptr && entry->pass == pass)) {
                continue;
            }

            // Whoever can fill the image: a pass that promises this very image, a pass that draws into its target
            // (drawing into a target fills all of its images), or the host (a host binding is filled by the host).
            // WHERE that pass sits in registration order is not a question any more: the scheduler orders it.
            const bool has_filler = claim != nullptr || entry != nullptr;

            if (has_filler && (!identity.depth || identity.target->hasDepth())) {
                continue; // filled, by a pass the scheduler orders before this one (or by the host)
            }

            // The structural check speaks for this wire; the collect keeps its subject so the RESOLUTION of the
            // same input stays quiet about it (see unusableInputReported / reportUnproducedInput).
            unusable_inputs_this_frame_.emplace(pass, identity);
            {
                const String consumer = reportedPassName(pass);
                // Two problems, two fixes: nobody declares it, or the target simply has no depth.
                const String message  = !has_filler ? String(u8"pass '") + consumer + String(u8"' declares the input image '") + image->label() +
                                                          String(u8"' but no pass declares it as an output, so nothing fills it")
                                                    : String(u8"pass '") + consumer + String(u8"' declares the input image '") + image->label() +
                                                          String(u8"' (the depth of target '") + target_name(identity.target) +
                                                          String(u8"') but that target has no depth attachment, so nothing fills it");
                reportEngineProblem(vn::graphics::DiagnosticSeverity::Warning, vn::graphics::DiagnosticCategory::ContentSkipped, message);
            }
        }
    }

    // Phase 3: a ScreenPass that cannot draw what it declares. Every one of these is a property of
    // the DECLARATION (nothing else can see them: the backend only ever sees "a pass with nothing to
    // draw"), they are found by the compile (so the frame they belong to is the frame that says so), and
    // they are checked in the order a host has to fix them — each check `continue`s, because the next one is
    // moot until this one is answered and one mistake should read as one message.
    for (const auto& slot : slots_) {
        auto* pass = obj_cast<ScreenPass>(slot.pass.get());
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
        // lights through that view space, so without one the pass describes nothing and the frame hands the
        // backend no draw at all. The fix is static (setCamera), and a host would otherwise see a
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
    for (const UnboundDeclaration& unbound : unbound_declared) {
        if (isColliding(OutputIdentity::of(*unbound.image))) {
            continue;
        }
        reportEngineProblem(vn::graphics::DiagnosticSeverity::Warning,
                            vn::graphics::DiagnosticCategory::ContentSkipped,
                            unbound.as_output
                                ? String(u8"pass '") + reportedPassName(unbound.pass) + String(u8"' declares the output image '") + unbound.image->label() +
                                      String(u8"' but that image is not bound to a target, so no consumer can resolve"
                                             u8" it and the promise cannot be checked against what the pass renders"
                                             u8" into — bind it (ImageRef::bind(target, attachment))")
                                : String(u8"pass '") + reportedPassName(unbound.pass) + String(u8"' declares the input image '") + unbound.image->label() +
                                      String(u8"' but that image is not bound to a target, so nothing can fill it —"
                                             u8" bind it (ImageRef::bind(target, attachment))"));
    }
}

void RenderEngine::publish(const String& name, intrusive_ptr<RenderTarget> target)
{
    const String problem = resources_->publish(name, std::move(target));
    if (!problem.empty()) {
        reportEngineProblem(vn::graphics::DiagnosticSeverity::Warning, vn::graphics::DiagnosticCategory::ContentSkipped, problem);
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
    if (device_ != nullptr) {
        // Forwarded straight away: the backend treats a call before initialize() as the session
        // decision and one after it as a live switch (rebuild the window sets, drop the slots), so
        // the engine's accessor and the picture cannot disagree.
        device_->setDefaultContentProgram(default_content_program_);
    }
}

intrusive_ptr<const ShaderProgram> RenderEngine::defaultContentProgram() const
{
    return default_content_program_;
}

void RenderEngine::announceAreaSize(raw_ptr<const RenderSurface> from, int width, int height)
{
    if (width <= 0 || height <= 0) {
        return;
    }
    // ONLY the engine's own area feeds the frame context (see createSurface): letting anything else through would lay
    // this frame's passes out on another window's size.
    if (from == nullptr || area_ == nullptr || area_.get() != from) {
        return;
    }
    // Recorded even when there is no device yet: the frame context is what the passes lay
    // themselves out on, and the announcement is replayed to the target at initialize().
    surface_width_  = width;
    surface_height_ = height;
    // No layout fan-out here, and no window half either: target sizes, pass viewports and camera
    // projections are maintained by their creators on the surface size
    // (e.g. SceneView::addSurfaceLayout), and the WINDOW's own half of the size belongs to the AREA's target,
    // which the area announces to directly (see RenderSurface::resize). The engine only records the surface
    // size the passes read.
}

intrusive_ptr<RenderSurface> RenderEngine::createSurface()
{
    // ONE engine drives ONE area, and the engine OWNS it (see the class note): the relation has an owner instead of
    // a paragraph of documentation, so "the area must die first" and "a second area is refused" stop being
    // conventions. A second area is refused rather than silently taking over - the first one would keep declaring
    // its picture into a frame that no longer resolves it, and its own size would stop being the one the passes lay
    // out on, both silent.
    if (area_ != nullptr) {
        if (area_refused_.shouldReport()) {
            reportEngineProblem(DiagnosticSeverity::Warning,
                                DiagnosticCategory::UnsupportedRequest,
                                u8"RenderEngine::createSurface: this engine already drives one render area, so a "
                                u8"second one was refused - an engine drives one area, and a second window wants "
                                u8"its own engine (one device may serve several engines)");
        }
        return nullptr;
    }
    auto area = intrusive_ptr<RenderSurface>(new RenderSurface(*this));
    area_     = area;
    return area;
}

VN_GRAPHICS_NS_END
