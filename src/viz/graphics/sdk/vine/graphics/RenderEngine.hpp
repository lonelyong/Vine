#pragma once
#include "graphics_global.hpp"

#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <vector>

#include <vine/Buffer.hpp>
#include <vine/Lease.hpp>
#include <vine/Object.hpp>
#include <vine/RefCounted.hpp>
#include <vine/intrusive_ptr.hpp>
#include <vine/raw_ptr.hpp>

#include "CameraSnapshot.hpp"
#include "Capabilities.hpp"
#include "Diagnostics.hpp"
#include "DiagnosticsRoute.hpp"
#include "EngineNotReady.hpp"
#include "FrameDescription.hpp"
#include "FrameResult.hpp"
#include "FrameStats.hpp"
#include "FrameTable.hpp"
#include "OutputRegistry.hpp"
#include "PassDesc.hpp"
#include "PassRegistry.hpp"
#include "Plan.hpp"
#include "PrepareReport.hpp"
#include "RenderDiagnostic.hpp"
#include "RenderSurface.hpp"
#include "ReportOnce.hpp"
#include "SceneSnapshotRef.hpp"

VN_GRAPHICS_NS_BEGIN

namespace detail
{

class FrameResources;
class FrameScheduler;
class PublicationStore;              // The content side of the publication channel (see src/PublicationStore.hpp).
struct EngineFrame;                  // One frame as it travels between the two halves (see src/EngineFrame.hpp).
class FrameRunner;                   // The thread a frame's engine half runs on (see src/FrameRunner.hpp).

} // namespace detail

class Light;
class Scene;
class RenderPass;
class ImageRef;
class RenderTarget;
class RenderBackend;
class PresentTarget;

/**
 * @brief What one RenderEngine::frame() call did.
 *
 * @details
 * - TWO OUTCOMES, one per price the call paid (`G4`): #Status::Ran means this call compiled AND answered the frame
 *   before returning (there is no frame thread), #Status::Submitted means it compiled and handed it over, so the
 *   picture follows one frame later (see waitForFrame). Nothing else is a frame outcome.
 * - #Status::NotReady is not a third frame outcome: it says the call drove NOTHING, and @ref reason says which
 *   precondition failed. It shares #EngineNotReady with `prepare()` so a host answers "is this engine ready" once.
 * - What it deliberately does not answer: whether anything was drawn (RenderEngine::frameReport) or where the picture
 *   went (RenderEngine::presentRecord / #PresentOutcome). Both describe the frame, not the call's readiness.
 *
 * @note
 * - The value describes the CALL; it is not the per-frame result the frame pipeline design calls `FrameResult`
 *   (`graphics-frame-pipeline.md` F12), which carries the snapshot, view, poses and diagnostics a host queries with.
 */
struct VN_GRAPHICS_API FrameOutcome {
    /// What became of the call (see the struct's own note).
    enum class Status : std::uint8_t
    {
        Ran,       // Compiled and answered before the call returned (no frame thread).
        Submitted, // Compiled and handed to the frame thread; see waitForFrame.
        NotReady,  // Nothing was driven: see reason.
    };

    Status         status{ Status::NotReady };            ///< Which answer this is.
    EngineNotReady reason{ EngineNotReady::NoDevice };     ///< Why, when @ref status is #Status::NotReady.

    /**
     * @brief Gets whether the frame was answered before this call returned.
     *
     * @return true when @ref status is #Status::Ran.
     */
    [[nodiscard]] bool ran() const noexcept
    {
        return status == Status::Ran;
    }

    /**
     * @brief Gets whether the frame was handed to the engine thread (see waitForFrame).
     *
     * @return true when @ref status is #Status::Submitted.
     */
    [[nodiscard]] bool submitted() const noexcept
    {
        return status == Status::Submitted;
    }

    /**
     * @brief Gets whether this call drove a frame at all.
     *
     * @return true when the frame ran here or was submitted; false when the engine was not ready.
     */
    [[nodiscard]] bool driven() const noexcept
    {
        return status != Status::NotReady;
    }

    /**
     * @brief Gets the same answer as @ref driven, for a caller that tests the outcome directly.
     *
     * @return true when the frame ran here or was submitted.
     */
    explicit operator bool() const noexcept
    {
        return driven();
    }
};

/**
 * @brief Where the engine half of a frame runs (see frame(); the content half always runs on the caller's thread).
 *
 * @details
 * - Inline (the default): both halves run inside frame(), on the calling thread - the behaviour every existing host and
 *   test has, and the one that needs no thread at all.
 * - EngineThread: frame() builds the content half here and hands the engine half (the expansion, the execute/present
 *   call) to a thread this engine owns. frame() then returns Submitted, the picture follows one frame later, and
 *   waitForFrame(n) is how a host asks "is frame n on screen yet" (screenshot, benchmark, shutdown).
 */
enum class FrameThreading : std::uint8_t
{
    Inline,      // Both halves on the calling thread; frame() returns Ran.
    EngineThread // The engine half on this engine's own thread; frame() returns Submitted.
};

/**
 * @brief High-level render engine managing the frame loop and render state.
 *
 * @details
 * - It imposes NO fixed pipeline: nothing renders unless the caller states passes (applyPlan, or a chain). Each
 *   frame() runs the registered passes in ascending order, then ends and swaps; a top / HUD pass is just a
 *   pass registered with a higher order - there is no separate overlay concept.
 * - A pure scheduler: it carries no content scene, no camera and no input forwarding. Every pass draws the
 *   content its own declaration states (PassDesc::content) or nothing; the primary interactive view
 *   lives in a SceneView that borrows this engine. Which pass draws into which SURFACE is the surface's own
 *   declaration (see RenderSurface), never something the engine infers.
 * - THE ENGINE KNOWS NO WINDOW: the window belongs to the AREA - the area holds the TARGET its device made
 *   (RenderSurface::bindTarget) and the size a host reports (resize), and the engine holds neither. It
 *   drives the frame: with no area target it asks the device (one that presents nothing answers none), it
 *   names the area's target in the frame it hands over, and it presents through it at the frame's tail
 *   (PresentTarget::present). A device that presents nothing presents nothing - a frame is complete
 *   without one.
 * - Drawing itself is delegated to the RenderDevice supplied via setDevice().
 */
class VN_GRAPHICS_API RenderEngine : public Object, public RefCounted<RenderEngine> {
    VN_OBJECT_META_DECL;

  public:
    /** @brief Constructs an empty engine (no device, no passes).
     *
     * Configure the chain explicitly (applyPlan) or through PassChain::build, then setDevice()
     * before initialize().
     */
    RenderEngine();
    ~RenderEngine();

  public:
    /** @brief Gets a borrowed handle that stops referring to this engine once it is destroyed.
     *
     * @details
     * - WHAT IT IS FOR. An object that is not owned by the engine but may outlive it - a PassChain holds passes
     *   registered HERE, and a host may keep a chain after the control that owned the engine is gone - needs a
     *   borrow it can CHECK, and it must not take an owning reference to the engine to get one: an owning reference
     *   from a child to its parent keeps the parent alive from below, which is how a "released" engine used to live
     *   on for as long as anything it once handed a chain to (see Lease).
     * - WHAT IT DOES NOT DO. It extends nothing: the engine still dies exactly when its last owner drops it, and a
     *   lease taken before that becomes empty rather than dangling.
     * - The cell lives as long as the engine and as long as any lease, so reading a lease after the engine is gone
     *   is safe and answers nullptr.
     *
     * @return A lease of this engine (empty never; invalidated by this engine's destructor).
     */
    [[nodiscard]] Lease<RenderEngine> lease() noexcept;

    /** @brief Gets the pass registry: the ONE table a frame runs from (see PassRegistry).
     *
     * @details
     * - The narrow contract a recipe (PassChain) depends on: registration, order, content access, and the owner
     *   relation that makes "the chain takes back what it registered" one call. The flat methods below
     *   (`passCount`, `passOrder`, ...) are shorthands for this handle.
     * - The handle carries a lease, so it may outlive the engine: `isValid()` then answers false and every call is
     *   a no-op.
     *
     * @return A handle over this engine's pass table.
     */
    [[nodiscard]] PassRegistry passes() noexcept;

    /** @brief Gets the named-output table (see OutputRegistry).
     *
     * @details
     * - The narrow contract for `publish` / `resolve` / `unpublish` and the compile's transients; the flat methods
     *   below are shorthands for this handle.
     *
     * @return A handle over this engine's outputs.
     */
    [[nodiscard]] OutputRegistry outputs() noexcept;

    /** @brief Gets the engine's one diagnostic route (see DiagnosticsRoute).
     *
     * @details
     * - The narrow contract for counting and reporting; a recipe holds this instead of the whole engine, and the
     *   compile-time episode rule is preserved because the handle forwards to the engine's own report path.
     *
     * @return A handle over this engine's diagnostic route.
     */
    [[nodiscard]] DiagnosticsRoute diagnostics() noexcept;

  public:
    /** @brief Gets the bound device.
     *
     * @return The device, or nullptr when unset.
     */
    raw_ptr<RenderBackend> device() const;

    /** @brief Sets the device used for drawing (see RenderDevice / RenderFrameExecutor).
     *
     * @details
     * - The device is BORROWED: the engine keeps a reference so the device outlives this engine's use of it, and
     *   never drives its lifecycle - the party that created the device brings it up and takes it down (see
     *   RenderDevice::initialize), which is what lets two engines drive one device. Call before initialize().
     *   Setting the same instance again is a no-op.
     * - The default content program is announced here, so a device brought up afterwards already knows what
     *   program-less content is drawn with - the device's own order asks for that before it comes up.
     * - The new device is handed this engine's ONE diagnostic route (see Diagnostics.hpp), so a host's
     *   sink and its single diagnosticCount() cover the device too.
     *
     * @param device Device used for drawing, or null to clear.
     */
    void setDevice(intrusive_ptr<RenderBackend> device);

    /** @brief Installs the sink that receives diagnostics.
     *
     * @details
     * - The backend reports what it could not serve (rejected geometry, dropped channel, shader fallback,
     *   target it could not build) and the engine reports what it could not wire - both instead of degrading
     *   silently; this is the host's programmatic channel to them (log / overlay / telemetry).
     * - The sink lives in the engine's ONE route (see Diagnostics.hpp), which the engine hands to the
     *   backend it drives - so installing it covers both, and a sink installed before setDevice() is in
     *   place the moment a device is set.
     *
     * @param sink Callback invoked for every diagnostic, or empty.
     */
    void setDiagnosticSink(DiagnosticSink sink);

    /** @brief Gets the installed diagnostic sink (empty when unset).
     *
     * @return The sink.
     */
    const DiagnosticSink& diagnosticSink() const
    {
        return diagnostics_.sink();
    }

    /**
     * @brief Gets how many command lists this engine's frames have COLLECTED (one per distinct view).
     *
     * @details
     * - The witness of the per-view memo on the engine's side: a frame whose passes share a camera collects once for it
     *   and answers the rest from the memo (see contentCollectReuseCount), which is what "one camera, two passes" must
     *   cost.
     * - It counts what the SOURCE answered - the content's published snapshot, since a frame's draws are answered from
     *   it - so the number means the same thing wherever the frame runs.
     *
     * @return Total number of lists collected since construction.
     */
    [[nodiscard]] std::uint64_t contentCollectCount() const noexcept;

    /**
     * @brief Gets how many collected lists this engine's frames answered from the per-view memo.
     *
     * @return Total number of reuses since construction.
     */
    [[nodiscard]] std::uint64_t contentCollectReuseCount() const noexcept;

    /** @brief Gets how many diagnostics were reported through this engine's route.
     *
     * @details
     * - ONE NUMBER, because there is one route: what the engine could not wire and what the backend could
     *   not serve are counted in the same place.
     * - Counted whether or not a sink is installed, so a host can gate on "did anything unexpected
     *   happen" without listening.
     *
     * @return Total number of diagnostics reported through this engine.
     */
    [[nodiscard]] std::size_t diagnosticCount() const noexcept;

    /** @brief Gets how many diagnostics of @p category were reported through this engine's route.
     *
     * @param category Category to count.
     * @return Number of reported diagnostics in that category.
     */
    [[nodiscard]] std::size_t diagnosticCount(DiagnosticCategory category) const noexcept;

    /** @brief Gets whether nothing at all was reported through this engine's route.
     *
     * @details
     * - The gate a phase asserts on: a steady run that is expected to be uneventful asks this instead of
     *   reading the log.
     *
     * @return true when no diagnostic was reported.
     */
    [[nodiscard]] bool diagnosticsClean() const noexcept;

    /** @brief Reports one problem the engine, or a helper acting for it, found.
     *
     * @details
     * - The engine owns the route, so the message reaches the host whether or not a backend is set (a
     *   wiring problem exists before any backend draws); counted in diagnosticCount().
     * - Public because the engine's own builders report through it too (e.g. the chain build tells)
     *   the host that content asks for shadows while no shadow pass is built).
     *
     * @param severity How bad the situation is.
     * @param category What it is about.
     * @param message  Human-readable detail, naming the pass and the slot.
     */
    void reportEngineProblem(vn::graphics::DiagnosticSeverity severity, vn::graphics::DiagnosticCategory category, const String& message);


    /** @brief Attaches this engine to the device it borrows: ready to drive frames.
     *
     * @details
     * - The DEVICE IS NOT INITIALIZED HERE. It belongs to its creator (see RenderDevice::initialize), which brings it
     *   up (and follows a re-announced window) before this call; an engine that brought it up would tear a shared
     *   device down under whoever else is drawing through it.
     * - A device that is not up fails the attach and is reported ONCE per episode: a frame driven through it would
     *   look like a running engine drawing nothing.
     * - What the attach does: asks the device for a target when this engine's area holds none (the device's own
     *   window, or "no presentation" for a display-less device), replays the announced surface size to the area's
     *   target, and marks the engine ready.
     *
     * @return true when the engine is ready to render; false when it has no device or the device is not up.
     */
    bool initialize();

    /** @brief Detaches from the device: unbinds the area's target and stops driving frames.
     *
     * @details
     * - THE DEVICE IS NOT SHUT DOWN: it is borrowed (see setDevice), and another engine may still be driving it. Its
     *   creator releases it - a host that also owns the device calls RenderDevice::shutdown() itself.
     * - The area's target is unbound FIRST, because it is one of the device's objects (see
     *   RenderDevice::createSurfaceTarget): nothing is left naming something the device may destroy.
     * - A host that comes back calls initialize() again, asks the device for a target and binds it - the
     *   "device lost, rebuild from the bottom" path.
     */
    void shutdown();

    /**
     * @brief States which publication of @p scene this engine draws from now on.
     *
     * @details
     * - THE EXPLICIT HALF OF PUBLISHING. `Scene::publish()` produces the value; this says which value THIS engine's
     *   frames draw. Together they are the whole handoff: a host publishes once, hands the answer over, and a frame
     *   then draws exactly that picture - not whatever the content happened to say when the frame ran.
     * - IT IS PER ENGINE AND PER SCENE. Two engines drawing one scene each hold their own publication, so an editor
     *   view and an offscreen bake can be one publication apart without either telling the other what to draw.
     * - An EMPTY handle clears the binding: every view then answers an empty list, which is the same answer as a
     *   scene whose content draws nothing.
     * - IT IS AN OVERRIDE, NOT A REQUIRED STEP: a frame takes the scene's latest publication by default
     *   (`Scene::publishedSnapshot`), so a host that publishes and stops there always draws its own content. Bind when
     *   you want an OLDER or a DIFFERENT picture - an offscreen bake drawing the state an editor has moved past, two
     *   engines drawing one scene one publication apart.
     * - A BIND SURVIVES later publications: every frame built until the next bind draws the publishing value you named.
     *
     * @note
     * - A frame pins the publication it took when it was BUILT, so a bind (or a publish) landing while a frame is being
     *   answered never changes that frame's picture (see detail::PublicationStore).
     *
     * @param scene    The scene whose publication is being handed over.
     * @param snapshot The publication to draw (see ScenePublishResult::snapshot), or an empty handle to draw nothing.
     */
    void bindSnapshot(Scene& scene, const SceneSnapshotRef& snapshot);

    /**
     * @brief Prepares the frame the next frame() would build, without running it: a warm-up a host can budget.
     *
     * @details
     * - WHAT IT IS. The frame is COMPILED exactly as frame() compiles it (the declarations are read as they stand,
     *   the same pass value comes out) and its draws are collected the same way, and then the compiled value is
     *   handed to the backend's `prepareFrame` - one wire, two verbs: what `prepare` builds is keyed by the same
     *   identities the next frame uses, so that frame finds them and builds nothing (`R23`).
     * - WHAT IT IS NOT. It is not a frame: nothing is acquired, recorded into a surface or presented, no counters of
     *   handled frames move, and `presentRecord()` / `presentedCamera()` keep describing the last frame that really
     *   ran. It is also not the "pre-frame warm-up" that used to live in initialize(): that one EXECUTED passes; this
     *   one is a query that may build what a frame would need.
     * - WHY A HOST CALLS IT. Compiling the first pipelines and uploading the first streams is where the first
     *   frames' cost is; a host that has just loaded content can pay it here, once, and see from the report what it
     *   cost. A caller that ignores it loses nothing: the next frame() does the same work inline, as before.
     * - IT DOES NOT CHANGE WHAT THE NEXT FRAME DRAWS. The declarations are re-read when that frame runs, so content,
     *   cameras and sizes that moved in between reach it as usual; what was prepared is a key, not a frozen picture.
     *
     * @warning
     * - Never inside a `before_draw` hook or while a frame is being answered: like every entry of this class it
     *   compiles the declarations, and it waits for a frame in flight on the engine thread first (between frames).
     * - A pass' `before_draw` hook RUNS, because describing a pass is what a frame does and the hook is part of it:
     *   a warm-up is therefore not free of side effects, and a hook that counts its calls sees one more. A hook that
     *   animates off its own wall clock (the intended shape) is unaffected.
     * - A prepared identity only helps while it stays the same one: an edit between the call and the frame is a new
     *   identity, and that frame compiles it (correct, and the price of the edit).
     *
     * @return What the request did, with what it built (see PrepareReport); `outcome` is `NotReady` with `reason`
     *         (`NoDevice` / `NotInitialized`) when this engine cannot prepare anything at all.
     */
    PrepareReport prepare();

    /** @brief Renders one frame (compile, then announce the compiled value, end, swap).
     *
     * @details
     * - Nothing is drawn when no pass is registered. The frame is COMPILED first (compileFrame): the
     *   declarations are read as they stand - which pass runs when, what its declared inputs resolve to,
     *   what is wrong with the set - and that same pass produces the frame's VALUE (FrameDescription).
     * - What the frame RUNS is that value: a disabled pass (RenderPass::setEnabled) has no entry and does
     *   not run, and no pass is described or executed a second time. A pass whose work must run therefore
     *   puts it where the compile looks - see RenderPass::execute.
     *
     * - The frame is what the declarations ARE: there is no clock and no per-frame context to pass in. Content that
     *   animates measures its own time (a pass' `before_draw` hook, an overlay's own wall clock), which is what the
     *   one `dt` a host had to hand over used to be - a number nothing in the frame read.
     *
     * @return What the call did (see FrameOutcome): `status.ran()` when this engine has no frame thread and the frame
     *         was answered before returning, `status.submitted()` when it was handed to one; `NotReady` with
     *         `reason` (`NoDevice` / `NotInitialized`) when nothing was compiled or handed over.
     */
    FrameOutcome frame();

    /** @brief Chooses where the engine half of each frame runs (see FrameThreading).
     *
     * @details
     * - A LIFETIME DECISION, made once: switching to EngineThread starts this engine's frame thread, switching back to
     *   Inline stops and joins it. Both wait for a frame in flight first, so a host may switch between frames (not
     *   from inside a `before_draw` hook).
     * - The thread is told nothing about the content: `frame()` still builds the content half on the caller's thread,
     *   and only the publications, the surface's window half and the device are touched on the other side.
     *
     * @param mode Where to run the engine half.
     */
    void setFrameThreading(FrameThreading mode);

    /** @brief Gets where the engine half of a frame runs (Inline until setFrameThreading says otherwise). */
    [[nodiscard]] FrameThreading frameThreading() const noexcept;

    /** @brief Gets the id of the last frame this engine built (0 before the first one).
     *
     * @details
     * - The number a host hands to waitForFrame: it is the same number the frame carried, so "wait for the frame I just
     *   asked for" is `waitForFrame(frameNumber())`.
     */
    [[nodiscard]] std::uint64_t frameNumber() const noexcept;

    /** @brief Waits until frame @p number has been answered (executed and presented, or refused by the device).
     *
     * @details
     * - The ONE explicit wait of this class (design 2.12): `frame()` never waits for the device, so a host that needs
     *   "this frame is on screen" - a screenshot, a benchmark, a resize settled, shutdown - says so here.
     * - Inline it is a no-op that answers true for every frame already built, because those were answered inside
     *   `frame()`.
     * - It does not start a frame, and it does not spin: it waits on the frame thread's own completion signal.
     *
     * @param number  The frame id to wait for (see frameNumber).
     * @param timeout How long to wait.
     * @return true when that frame has been answered, false on timeout (or when it was never built).
     */
    bool waitForFrame(std::uint64_t number, std::chrono::milliseconds timeout = std::chrono::milliseconds(5000));

    /** @brief Waits until the last frame this engine built has been answered (see waitForFrame).
     *
     * @param timeout How long to wait.
     * @return true when it has been answered, false on timeout.
     */
    bool waitForFrame(std::chrono::milliseconds timeout = std::chrono::milliseconds(5000));

    /** @brief What became of the picture of the last frame this engine ANSWERED (see PresentOutcome).
     *
     * @details
     * - ONE VALUE, TWO FACTS: which frame (its id, so a host can tell whether the frame it is waiting for has been
     *   answered yet) and what became of its picture. It is written by whichever half answers frames - the caller's
     *   inline, the engine thread otherwise - and may be read from any thread at any time.
     * - #PresentOutcome::Lost is the one answer a host must ACT on: the surface was there and the picture did not reach
     *   it (the window was destroyed under a frame, the swapchain could not serve it), so the move is to re-attach -
     *   which must happen BETWEEN frames (the device verbs are the host's, and they need it quiet).
     * - The frame's id never goes backwards and never skips: `frame` is the id of the frame that produced this answer.
     *
     * @return The last answered frame's id and picture outcome; frame 0 means no frame has been answered yet.
     */
    struct PresentRecord {
        std::uint64_t  frame{ 0 };                         ///< The frame this answer belongs to (0: none yet).
        PresentOutcome outcome{ PresentOutcome::Skipped }; ///< What became of its picture.
    };

    /** @brief Gets what became of the last answered frame's picture (see PresentRecord). */
    [[nodiscard]] PresentRecord presentRecord() const noexcept;

    /** @brief Gets what the last ANSWERED frame was: its picture, the world it saw, and what it cost (see FrameResult).
     *
     * @details
     * - ONE ANSWER FOR ONE FRAME. Every field describes the frame that produced it, and the whole value is written once,
     *   when that frame is answered - so a host reading while the next frame is being recorded never sees this frame's
     *   id beside the last frame's picture.
     * - THE VALUE A QUERY AIMS WITH: `query.snapshot` is the publication the picture was drawn from and `query.view`
     *   the camera it was taken with, both taken when the frame was built. A pick, an anchor or a fit that asks about
     *   "what the user sees" reads them here instead of the live scene and camera (`I7`, design N17).
     * - `frame` stays 0 until something is answered (`answered()`), and `present` is the same answer `presentRecord()`
     *   carries - the one field a host must ACT on (`Lost` -> re-attach, between frames).
     *
     * @note
     * - `presentedCamera()` is the `query.view` half of this value, kept as its own entry for callers that only aim.
     *
     * @return The last answered frame's result; an empty value (frame 0) before the first answer.
     */
    [[nodiscard]] FrameResult frameResult() const;

    /** @brief Gets the camera the last ANSWERED frame's picture was taken with, when something presents this area.
     *
     * @details
     * - The value a pointer query should aim with (design graphics-threading.md N17): the camera moves on the content's
     *   side and takes effect at once, while the picture on screen is the frame answered last - one frame behind when
     *   the engine half runs elsewhere. Aiming with the LIVE camera asks about a pose nobody has seen yet, which shows
     *   up as a pivot or a selection that is off by exactly that frame's camera delta.
     * - It is the presenting pass' camera, taken when the frame was BUILT (the same conversion the frame's draws carry),
     *   and published when the frame is answered - beside presentRecord(), and read from any thread.
     * - Absent when nothing presents this engine's area (an offline bake, a device that presents nothing): a query then
     *   keeps using the live camera, which is the right answer when there is no picture to disagree with.
     * - It is the `query.view` half of RenderEngine::frameResult, kept as its own entry for callers that only aim.
     *
     * @return The snapshot, or nullopt (see above).
     */
    [[nodiscard]] std::optional<CameraSnapshot> presentedCamera() const;

    /** @brief Gets the width the frame is laid out on, as the host announced it (see RenderSurface::resize).
     *
     * @details
     * - The surface's shape is the one number a builder or a host needs from the engine while it is assembling
     *   passes (PassChain::build lays its targets out on it). It is not a per-frame value: it changes when the
     *   surface is resized, and everything that reads it reads the same fact.
     *
     * @return Width in the units the host announced (logical pixels for the Qt host; 0 until the first resize).
     */
    [[nodiscard]] int surfaceWidth() const noexcept;

    /** @brief Gets the height the frame is laid out on (see surfaceWidth).
     *
     * @return Height in the units the host announced (0 until the first resize).
     */
    [[nodiscard]] int surfaceHeight() const noexcept;

    /** @brief Gets what the device can do, as one value (see RenderDevice::capabilities).
     *
     * @details
     * - The answer is the DEVICE's own, `presentation` included: "can this session show frames somewhere"
     *   is a device property, so a host reads ONE value from ONE object. The engine itself asks the same
     *   value where a capability decides how a frame is driven (see frame()).
     *
     * @return The device's capabilities, whole; everything false when no device is set (nothing can be
     *         claimed).
     */
    [[nodiscard]] Capabilities capabilities() const noexcept;

    /** @brief Gets the device's own report about its frames (see RenderFrameExecutor::frameReport).
     *
     * @details
     * - The frame half of the device is the only place that knows how a frame went; one question, one
     *   answer - the frame that just ended, what is retained, and where the frame clock is, with the parts
     *   this device does not keep left disengaged rather than reading as zeros.
     *
     * @return The report; all parts disengaged when no backend is set (nothing is measured).
     */
    [[nodiscard]] FrameReport frameReport() const noexcept;

    /** @brief Gets the order a registered pass runs at in this engine's frame.
     *
     * @details
     * - The order lives in the ENGINE (a plan states it), not on the pass, so this is the one way to
     *   ask where it sits - and an answer doubles as the liveness query: a removed pass answers nothing, so
     *   an answer IS the pass being in the frame.
     * - The caller this exists for is a HOST (and tests): the area a pass presents is a declaration
     *   (RenderSurface::present), and this is how a host finds out whether that pass is still in the frame
     *   (see RenderSurface::presentingPass).
     *
     * @param pass Pass to look up (by pointer; null answers nothing).
     * @return The pass' order, or nullopt when no such pass is registered.
     */
    std::optional<int> passOrder(raw_ptr<RenderPass> pass) const;

    /** @brief Gets what a frame of this engine consists of: the passes it runs, in order.
     *
     * @details
     * - The table is COMPILED from the registered passes (see compileFrame); every frame compiles its own,
     *   so the answer is the last frame's table - what the ENGINE announced for it - or, before any
     *   frame, the table the current declarations would compile to. Both are the same compile, so a report
     *   cannot disagree with what the engine hands over.
     * - The order here is the DECLARATION order (order, then registration); the engine's scheduler turns it
     *   into execution order for the frame it hands a backend (see frameDescription), so the two differ
     *   exactly where a dependency overrides the announced priority. A report that claims "what actually
     *   ran" reads the executor's own table.
     * - What it is for: a test asserting the SHAPE of a frame instead of counting passes or comparing
     *   pointers, a dev switch printing one table, and a host checking what a pipeline actually registered.
     *
     * @return The table, in announcement order; empty when nothing is registered.
     */
    [[nodiscard]] FrameTable frameTable() const;

    /** @brief Applies @p plan as this engine's session shape, diffing it by pass name.
     *
     * @details
     * - WHAT CHANGES: a name the engine does not have is added, a name it has is updated IN PLACE (the pass
     *   node survives, so the backend state keyed by it - pipeline variant, retained set, timing - does not
     *   have to be rebuilt), and a name the plan no longer states is removed (its backend state and owned
     *   target released). No separate add/remove call is involved, which is the
     *   point: the shape is one value, and applying it is one call.
     * - WHAT "IN PLACE" DOES NOT COVER: a pass whose KIND changed (Scene -> Screen) cannot become the other
     *   object, so that name is removed and re-added under the same owner and role. Every other field a
     *   PassDesc states is adopted as the pass' new declaration.
     * - THE PLAN'S VOCABULARY IS `PassKind` (Scene / Screen), and the node built from a declaration is the built-in
     *   of its kind: a custom pass CLASS (a subclass overriding `describe()`) is not registrable - what is custom
     *   about a picture is stated as data (a program override, a viewport, a depth band, a hook). There is no
     *   custom-kind registry (`G8`).
     * - THE WHOLE PLAN IS CHECKED FIRST (see validatePlan) and a plan with ANY problem is applied ENTIRELY
     *   or not at all: a half-applied shape would be a shape nobody declared.
     * - A PASS THE PLAN DOES NOT NAME IS REMOVED (the whole-session form; a scoped plan removes nothing). A pass
     *   whose name is empty cannot be stated at all, which is why every plan row must be named.
     *
     * @warning
     * - A SESSION'S SHAPE IS EDITED BETWEEN FRAMES: a call that lands while a frame is being recorded is
     *   REFUSED with `PlanResult::refusal == PlanRefusal::Busy` (no partial change).
     *
     * @param plan The shape to apply (see Plan).
     * @return What the call did (see PlanResult): applied, invalid (with every problem), or refused with a reason.
     */
    [[nodiscard]] PlanResult applyPlan(const Plan& plan);

    /**
     * @brief Applies @p plan as @p owner's rows: an upsert that leaves every other contributor's rows alone.
     *
     * @details
     * - WHAT A SCOPED PLAN IS FOR. One engine is drawn by several contributors (a view's chain, a demo's own passes,
     *   a host's overlay), and each states its own rows. A scoped apply resolves the plan against the rows @p owner
     *   already has: a name it already owns is updated IN PLACE (the node, and every backend state keyed by it,
     *   survives), a name it does not own is ADDED, and every row owned by someone else is left exactly as it is.
     * - IT DOES NOT REMOVE: a scoped plan states what this contributor draws, not what the session is. Removal is
     *   either the whole-session form above or the owner going away (see RenderEngine::removePassesOwnedBy).
     * - A NAME ANOTHER OWNER HAS IS REFUSED (`DuplicatePassName`): one table, one row per name, so a scoped plan
     *   cannot take a pass somebody else draws.
     *
     * @warning
     * - A plan is still a shape change: refused with `PlanRefusal::Busy` while a frame is being recorded.
     *
     * @param plan  The rows this contributor states (name = identity, as in the whole-session form).
     * @param owner The contributor the rows belong to (an identity; null means the whole session, see above).
     * @return What the call did (see PlanResult): applied, invalid (with every problem), or refused with a reason.
     */
    [[nodiscard]] PlanResult applyPlan(const Plan& plan, const void* owner);

    /**
     * @brief Finds the registered pass whose name is @p name.
     *
     * @details
     * - THE LOOKUP A PLAN IS APPLIED WITH: a plan identifies a pass by its name, so this is how a host finds the node
     *   it stated (the handle the old addPass family used to return).
     *
     * @param name Pass name (a plan's identity).
     * @return The registered node, or null when no row carries that name.
     */
    [[nodiscard]] raw_ptr<RenderPass> pass(const String& name) const;

    /**
     * @brief Finds the pass @p owner stated under @p name.
     *
     * @details
     * - THE UNAMBIGUOUS LOOKUP. `pass(name)` answers the first row with that name; a plan's identity is
     *   `(owner, name)` on the scoped form (see applyPlan), so a contributor that reads its own rows back has to say
     *   whose they are - two views of one engine may each have a "main".
     *
     * @param name  Pass name.
     * @param owner The scope that stated it (null: any row, as pass(name)).
     * @return The node, or null.
     */
    [[nodiscard]] raw_ptr<RenderPass> passFor(const String& name, const void* owner) const;

    /** @brief Answers the session shape this engine runs, as a value (the inverse of applyPlan).
     *
     * @details
     * - Built from the registered passes, each one stating its EFFECTIVE order, so `applyPlan(engine.plan())`
     *   is a no-op: the shape read back is exactly the shape that reproduces it.
     * - THE ONE PRECONDITION IS A NAME. A plan identifies each pass by its name (`Plan`), and an empty name cannot be
     *   expressed - `validatePlan` refuses it, because a shape that cannot be diffed would be applied as "remove
     *   everything, add everything". A pass with no name cannot be expressed at all: name every pass.
     * - What is NOT here: the measured facts (the last frame's record time) and the owner/role bookkeeping - those
     *   belong to the compile and to whoever registered the pass, not to the declared shape (see FrameTable, which is
     *   the COMPILED frame and a different value).
     *
     * @return The shape, in engine order; empty when nothing is registered.
     */
    [[nodiscard]] Plan plan() const;

    /**
     * @brief Answers the rows one contributor stated, as a value (the scoped inverse of applyPlan(plan, owner)).
     *
     * @details
     * - A scoped plan speaks only for its own rows, so reading them back is `planFor(owner)` - `plan()` is the
     *   session's whole shape and cannot say which row belongs to whom.
     *
     * @param owner The scope whose rows are answered (null: same as plan(), the whole session).
     * @return The rows that scope stated, in engine order.
     */
    [[nodiscard]] Plan planFor(const void* owner) const;

    /** @brief Gets the frame the last compile produced, as a value (see FrameDescription).
     *
     * @details
     * - THE FRAME THE ENGINE IS ABOUT TO RUN, in the form a backend consumes: every enabled pass in
     *   EXECUTION order (the engine's own stable topological sort) with its identity, order, resolved inputs
     *   and what it issues - assembled by the compile, before anything is dispatched. Read it to see exactly
     *   what a frame is made of; a backend is handed the same value once this path drives the frame
     *   (RenderFrameExecutor::executeFrame).
     * - The value is a BORROWED snapshot (see FrameDescription): it is valid until the next compile, and
     *   its order IS the execution order - a backend keeps it, and a cyclic component is already removed and
     *   reported before the value is published.
     *
     * @warning With a frame thread (setFrameThreading) the value's command lists are filled by the ENGINE half, so
     *          reading them before `waitForFrame` is reading while another thread writes: wait for the frame first.
     *          Without one (the default) it is complete as soon as frame() returns.
     *
     * @return The value, valid until the next compile and borrowed throughout; empty before the first frame.
     */
    [[nodiscard]] const FrameDescription& frameDescription() const noexcept;

    /** @brief Gets how many transient targets the current plan hands out (see PassDesc::writes_image).
     *
     * @details
     * - The witness of the compile owning transient storage: one per DISTINCT transient a frame declares
     *   (same identity and shape = ONE shared target), it does not grow with the frame count, and a shape
     *   change replaces rather than adds.
     *
     * @return Number of live transient targets.
     */
    [[nodiscard]] std::size_t transientTargetCount() const noexcept;

    /** @brief Gets the transient target a declaration asked for, by its declared identity
     * (PassDesc::writes_image).
     *
     * @details
     * - A declared transient is owned by the compile, so this is how its OWNER reaches it (to preview it,
     *   read it back, or state a relationship the declaration cannot - "this target borrows that transient's
     *   depth" is a composition, RenderTarget::shareDepth its mechanism).
     * - The answer is the instance live NOW: re-ask after a shape change, because a transient is REPLACED
     *   when its declared shape moves (PassDesc::writes_shape) - a handle kept across a resize describes a
     *   target the compile has already let go.
     *
     * @param identity Identity a pass declared (its `writes_image`).
     * @return The target, or null when nothing declares that identity (including every call before the
     *         first compiled frame - a transient is the compile's product, not the build's).
     */
    [[nodiscard]] raw_ptr<RenderTarget> transientTarget(const String& identity) const;

    /** @brief Sets the DEFAULT program for content: what a drawable naming none of its own gets.
     *
     * @details
     * - A default, not an override: a drawable's own program (RenderCommand::program, Geometry::setProgram,
     *   StateNode::setProgram) still wins. The name says so - "setContentProgram" would read as "shade all
     *   content with this".
     * - Forwarded to the backend (before initialize() it is the session's decision; on a running session
     *   the backend re-bakes the shading side).
     * - The engine's default is forwardProgram(): a NAMED program, chosen once in the constructor - not a
     *   lookup that hides which shading a scene gets. Hand it flatForwardProgram() to shade flat, or null to
     *   decline: content without its own program is then reported and not drawn, never shaded with something
     *   the host did not name.
     *
     * @param program Program to use, or null for none.
     */
    void setDefaultContentProgram(intrusive_ptr<const ShaderProgram> program);

    /** @brief Gets the default content program.
     *
     * @return The program, or null when content without its own program is declined.
     */
    intrusive_ptr<const ShaderProgram> defaultContentProgram() const;

    /** @brief Releases the device's GPU residency of a content object the host still owns.
     *
     * @details
     * - The object remains valid and keeps its revision; the next frame that names it uploads, compiles or
     *   builds its tables again. This is the explicit release for a host under memory pressure that still
     *   wants to keep its logical content.
     * - Recognized content: Geometry, Material (an asset or a MaterialInstance), ShaderProgram and Texture - and,
     *   through the overloads below, the stream buffers a geometry reads. The release is scheduled through the
     *   backend's retirement queue (see RenderDevice::releaseContent), not done synchronously.
     * - Call between frames, on the frame thread.
     *
     * @param content Content object to evict; null is refused.
     * @return true when the object was recognized and the release was accepted; false when it is not content,
     *         no device is bound, or the backend cannot park the release safely.
     */
    bool releaseContent(raw_ptr<const Object> content);

    /** @brief Releases the GPU residency of the streams a geometry reads from @p buffer (see RenderDevice).
     *
     * @details
     * - The geometry that reads the buffer is NOT touched: only the GPU copy goes, and the next frame that draws
     *   it uploads the stream again. To drop every stream of a geometry, release the Geometry.
     *
     * @param buffer Vertex or instance stream buffer.
     * @return true when the buffer was recognized and the release was accepted.
     */
    bool releaseContent(const vn::Buffer<float>& buffer);

    /** @brief Releases the GPU residency of the index stream read from @p buffer (see RenderDevice).
     *
     * @param buffer Index buffer.
     * @return true when the buffer was recognized and the release was accepted.
     */
    bool releaseContent(const vn::Buffer<std::uint32_t>& buffer);

    /** @brief Gets the number of registered passes.
     *
     * @return Number of rows in the engine's pass table.
     */
    std::size_t passCount() const;

    /** @brief Rebinds which scene a registered pass renders.
     *
     * @details
     * - The binding is written into the pass' declaration (`PassDesc::content`) - the same one storage a plan
     *   writes and the compile reads.
     *
     * @param pass    Pass in the engine's table (by pointer).
     * @param content New content scene; null clears the binding (a base scene pass then draws nothing).
     * @return true when @p pass is registered and its declaration now states @p content; false when no such pass is
     *         registered, so nothing changed.
     */
    bool bindPassContent(raw_ptr<RenderPass> pass, intrusive_ptr<Scene> content);

    /** @brief Gets the content scene a registered pass renders.
     *
     * @param pass Pass in the engine's table (by pointer).
     * @return The bound content, or nullptr when the pass has none or is not registered.
     */
    raw_ptr<Scene> contentOf(raw_ptr<RenderPass> pass) const;

    /** @brief Binds a render target to a named output slot (a standing host binding).
     *
     * @details
     * - The binding KEEPS until unpublish() (or another publish() replaces it) - unlike a pass' output,
     *   which is published once per frame: a host has no per-frame hook to re-publish from, so its binding
     *   must not be tied to a frame.
     * - Consumers declared by name resolve it, and an object-typed input addressing the same target is
     *   available too (a host binding counts as produced every frame). Publishing twice under one name
     *   replaces the binding - a host swapping what it offers, not a collision.
     * - A name can only be served by a target, so a null @p target is reported (once per name, until a real
     *   target is published or unpublish() withdraws the name) instead of ignored - the host would
     *   otherwise believe the name is served.
     *
     * @param name   Slot name consumers resolve against.
     * @param target Target to bind (a reference is kept; null is reported).
     */
    void publish(const String& name, intrusive_ptr<RenderTarget> target);

    /** @brief Looks up a published render target by slot name.
     *
     * @details
     * - A pass' publication from THIS frame wins over a host binding of the same name (the pass ran and its
     *   content is this frame's); otherwise a standing host binding answers.
     *
     * @param name Slot name to look up.
     * @return The published target, or nullptr when nothing is published under @p name.
     */
    raw_ptr<RenderTarget> resolve(const String& name) const;

    /** @brief Removes an output slot (a host binding and this frame's publication alike).
     *
     * @param name Slot name to remove (no-op when not published).
     */
    void unpublish(const String& name);

    /** @brief Creates the ONE render area this engine drives (see RenderSurface).
     *
     * @details
     * - ONE ENGINE, ONE AREA, OWNED HERE: the engine keeps the area it created, so the 1:1 relation has an owner
     *   instead of a paragraph of documentation, and the host holds a handle to the same object (dropping it does
     *   not destroy the area; the engine does). Filament's `Engine` and its `SwapChain`, and O3DE's `WindowContext`,
     *   are the same shape.
     * - A SECOND call answers null and says so once per episode: two areas on one engine would both declare the
     *   picture for one frame (they draw over each other, and only one's target is the frame's default framebuffer)
     *   while the other's size quietly drove the passes. A host that wants a second window builds a second engine; a
     *   device may serve several.
     * - The area outlives its host handle and never outlives the engine: an area a host kept after dropping the
     *   control that owned the engine dies with the engine, so no path here can reach freed memory.
     *
     * @return The area this engine now drives, or nullptr when it already drives one (reported once).
     */
    [[nodiscard]] intrusive_ptr<RenderSurface> createSurface();

  private:
    friend class RenderSurface; // its one area: announces its size, and asks whether it is the one

    /** @brief Records the area's size for the frame and replays it to the target once a session can honour it.
     *
     * @details
     * - The engine's ONE size announcement (see RenderSurface::resize): recorded even with no device yet
     *   (the frame context is what the passes read), replayed by initialize(). Only the engine's own area is
     *   heard (see createSurface) - a frame laid out on another window's size would be a silent disagreement.
     *
     * @param from   Area announcing (the engine's own is the only one it acts on).
     * @param width  Area width in pixels (> 0; the caller refuses the rest).
     * @param height Area height in pixels (> 0).
     */
    void announceAreaSize(raw_ptr<const RenderSurface> from, int width, int height);

    /** @brief One registered pass: the node, where it sits in the frame, and what a frame produced for it.
     *
     * @details
     * - THE DECLARATION IS NOT HERE. It is the pass' own (RenderPass::declaration()), so a descriptor-declared pass and the
     *   node it became cannot drift apart, and the compile reads one copy of every fact instead of two.
     * - What a slot adds is what only the ENGINE knows: the order the pass was registered at, the transient target
     *   the compile resolved for it, and how long its last record took. A disabled pass is skipped at run time; its
     *   declaration stays, because the wiring checks judge declarations.
     */
    struct Slot {
        intrusive_ptr<RenderPass> pass;
        int                       order = 0;
        /// The object that registered this pass and takes it back when it dies (null = registered without an owner,
        /// e.g. straight on the engine). Opaque on purpose: the engine never calls into it, only compares it.
        const void* owner{ nullptr };
        /// The role that owner gave this pass (empty = anonymous).
        String role;
        /// How long the last frame's record of this pass took, in microseconds (0: it did not run then).
        /// Measured by the compile, where that record happens (see buildFrameValue); reported through the frame
        /// plan (see PassRow::record_micros).
        std::uint64_t record_micros = 0;
        /// The transient the compile allocated for this pass (null when it declares none).
        intrusive_ptr<RenderTarget> transient;
    };

    /** @brief WHICH image a declared output is, as the wiring check compares it (design §14.4).
     *
     * @details
     * - The rule is "one image, at most one producer", so the check must catch a second producer of the
     *   SAME IMAGE - including when the two passes each declared their own ImageRef for it. The identity is
     *   therefore the ADDRESS (target + attachment) once bound, not the declared object.
     * - Two passes writing one target with DIFFERENT attachments are NOT a collision: a legitimate pipeline
     *   renders several passes into one target (light and transparent both write `composite`) and an MRT
     *   pass hands out several images of one target at once.
     * - An image not bound yet has no address, so its identity is the declared object: a host may declare an
     *   image before binding it, and until then "same object" is the whole truth that exists.
     */
    struct OutputIdentity {
        raw_ptr<const RenderTarget> target{ nullptr };   // set once the image is bound
        int                         attachment = 0;      // colour index; kWholeTarget = the whole target
        bool                        depth      = false;  // a depth attachment is NOT attachment 0
        raw_ptr<const ImageRef>     declared{ nullptr }; // identity while unbound

        /// @brief The coarse form: a whole target, as a bundle declaration names it.
        static constexpr int kWholeTarget = -1;

        /** @brief Compares two identities for being the SAME image.
         *
         * @details
         * - Field-wise, because the identity is a value: the wiring tables look an image up by identity, and two
         *   declarations of one attachment answer the same.
         *
         * @param other Identity to compare with.
         * @return true when both name the same image (or the same declared object, while unbound).
         */
        [[nodiscard]] bool operator==(const OutputIdentity& other) const noexcept
        {
            return target == other.target && attachment == other.attachment && depth == other.depth && declared == other.declared;
        }

        /** @brief Builds the identity of a declared output image.
         *
         * @param image Image a pass declared as its output.
         * @return The image's address when bound, otherwise its declared object.
         */
        static OutputIdentity of(const ImageRef& image) noexcept;

        /** @brief Builds the identity of one colour attachment of a target.
         *
         * @param target     Target the image belongs to.
         * @param attachment Colour attachment index.
         * @return The image's address.
         */
        static OutputIdentity colorOf(const RenderTarget& target, int attachment) noexcept;

        /** @brief Builds the identity of a target's depth attachment.
         *
         * @param target Target the depth belongs to.
         * @return The image's address.
         */
        static OutputIdentity depthOf(const RenderTarget& target) noexcept;

        /** @brief Builds the identity of a whole target (a coarse declaration).
         *
         * @param target Target the declaration names.
         * @return The coarse identity of @p target.
         */
        static OutputIdentity targetOf(const RenderTarget& target) noexcept;

        /** @brief Orders identities so they can key a map / set.
         *
         * @param other Identity to compare against.
         * @return true when this identity sorts before @p other.
         */
        bool operator<(const OutputIdentity& other) const noexcept;
    };

    /** @brief One row of the compiled frame: what a pass will do, decided before the frame runs.
     *
     * @details
     * - The row is the frame's OWN answer, not a reason to re-read the declarations: inputs are resolved
     *   once, here, and the frame loop hands them to the pass and to the backend. The vector lives in a
     *   REUSED list (one row per slot) and each row's input list is assigned into, so a steady frame
     *   allocates nothing.
     */
    struct PlanRow {
        raw_ptr<RenderPass> pass{ nullptr };
        raw_ptr<Scene>      content{ nullptr };
        int                 order{ 0 };
        /// The declared inputs, resolved for this frame: one entry per declaration, in declaration order,
        /// Kind::None where nothing produced it (exactly what the pass and the backend are handed).
        std::vector<ImageSource> inputs;
    };

    /** @brief One problem a compile found, as the sentence a report would carry.
     *
     * @details
     * - The problem carries its own sentence because that IS its identity to a host: the episode rule
     *   compares sentences (see reportPlanProblems), so nothing has to be kept in step with the wording.
     *   The cost is paid only by a frame that has a problem (a healthy frame builds none), and the message
     *   names its subject ("pass 'lighting' ..."), so two frames saying the same sentence really are the
     *   same problem.
     * - It is NOT the public `PlanProblem`: that one is a shape's defect with a stable code, answered before
     *   anything is applied; this one is a sentence about a compiled frame, compared by its wording.
     */
    struct CompileProblem {
        String message; // the sentence a report would carry
    };

    /** @brief What one pass declared about one image or target, as the wiring checks compare it.
     *
     * @details
     * - The pass count of a frame is small (single digits in every pipeline the framework builds), so the tables below
     *   are FLAT and searched linearly: a red-black tree per compile would allocate a node per pass per frame for no
     *   lookup benefit, and a vector keeps its capacity across frames (see collectPlanProblems).
     */
    struct Declaration {
        raw_ptr<const ImageRef>     image{ nullptr };  // The fine declaration, when there was one.
        raw_ptr<const RenderTarget> target{ nullptr }; // The target the declaration is about.
        raw_ptr<const RenderPass>   pass{ nullptr };   // Who declared it (null: the host filled it).
        std::size_t                 index{ 0 };        // Its position in the draw order.
        OutputIdentity              identity{};        // The image it claims (the claimed table's key).
    };

    /** @brief A declared image nobody bound to a target, and who declared it (see collectPlanProblems phase 4). */
    struct UnboundDeclaration {
        raw_ptr<const ImageRef>   image{ nullptr };   // The unbound identity.
        raw_ptr<const RenderPass> pass{ nullptr };    // Who declared it.
        bool                      as_output{ false }; // Declared as an output (else as an input).
    };

    /** @brief Compiles this frame's declarations into the table the frame runs from (see FrameTable).
     *
     * @details
     * - THE ONE PLACE A FRAME IS DECIDED. Compiled every frame - no declaration tells the engine it moved,
     *   because reading the declarations IS the compile (a steady frame costs the same walk, nothing kept
     *   in step with a revision). Two things come out, both the frame's own: the ORDER and WIRING (which
     *   pass runs when, what declared inputs resolve to, which transients allocate - PassDesc::writes_image)
     *   and this set's PROBLEMS, reported once per episode by comparing with the previous compile's.
     */
    void compileFrame();

    /** @brief Orders this frame's rows: dependencies first, announced order as the tie-break (see FrameScheduler).
     *
     * @details
     * - The ONE scheduler: the engine sees every producer and consumer, so a backend never re-derives the order.
     *   The result lands in frame_order_ (indices into plan_rows_), and a cyclic component is left out and reported
     *   instead of being run in call order.
     */
    void scheduleRows();

    /** @brief Collects the problems this frame's declarations carry (see CompileProblem).
     *
     * @details
     * - Every pass on its own is valid, so only the whole table can see a promise about a target never
     *   written, two claimants of one image, a declared input nobody fills, or a screen pass that can never
     *   draw. Run as part of the compile, so the answer is about THIS frame.
     */
    void collectPlanProblems();

    /** @brief Collects this frame's declared picture shapes against the cameras that project them.
     *
     * @details
     * - The one check that needs both halves: the RECTANGLE a pass draws into (viewport, or the whole
     *   target - the surface for the window) against the SHAPE its camera is built for (see Camera). Where
     *   they disagree the picture is stretched and nothing downstream can see it (the backend gets a
     *   rectangle and a camera snapshot that are each legal). Reported once per episode per pass, because a
     *   dragged pane changes the rectangle every frame while the mistake stays the same.
     */
    void collectPictureShapeMismatches();

    /** @brief Assembles the frame's value from this compile's rows (see FrameDescription).
     *
     * @details
     * - What every pass issues, before anything is dispatched: asks each pass to describe itself
     *   (RenderPass::describe - cheap, needs nothing from the backend) and adds the three facts the engine
     *   owns (identity, order, resolved inputs). A pass that issues nothing keeps its entry with no
     *   description; a disabled pass has no entry.
     */
    void buildFrameValue();

    /** @brief Runs the frame's CONTENT half: the token, the compile, its reports, and the described passes.
     *
     * @details
     * - Everything here reads or writes CONTENT (hooks, `Scene` rows, `Scene::setContentFrame`) and the declarations,
     *   and nothing here touches the device or a publication's state - which is what lets the other half run elsewhere.
     * - It ends with the frame being a complete value (see `detail::EngineFrame`): a target, a size and a list of
     *   described passes.
     */
    void buildContentHalf();

    /** @brief Builds ONE frame's content half into @p frame (also the slot `frame_` names while it runs). */
    void buildContentHalf(detail::EngineFrame& frame);

    /** @brief Runs the frame's ENGINE half: the window half of the size, the expansion of every draw, execute/present.
     *
     * @details
     * - The seam `buildContentHalf` hands over (see `.ai/design/graphics-threading.md` 2.12): it touches the publications,
     *   the target and the device - never the content - so it is what runs on the engine thread.
     * - It takes the frame EXPLICITLY instead of reading `frame_`: by the time it runs, the content half may already
     *   be filling the other slot.
     * - The size it hands the window half is the one the frame was LAID OUT on, not the surface's current size: an
     *   announcement that arrived while the frame was being built belongs to the next frame.
     *
     * @param frame The frame value to answer (borrowed; it outlives the call because it is one of this engine's slots).
     */
    void runEngineHalf(detail::EngineFrame& frame);

    /** @brief Answers the frame's draws: one expansion per described pass.
     *
     * @details
     * - The ENGINE HALF of a frame (design 2.12): every draw `buildFrameValue` described is asked of its source here,
     *   after every pass has been described - so one frame answers ONE publication (N16), and nothing in this
     *   function reads content: the content half already bound every scene draw to its publication (see
     *   detail::bindSceneDraws), so this half reads values only.
     * - What it costs per pass lands in @p frame (its own `engine_micros`), never in the plan: the plan belongs to the
     *   content half, and the numbers are published into it by `publishEngineMicros`.
     *
     * @param frame The frame whose draws to answer.
     */
    void expandFrameValue(detail::EngineFrame& frame);

    /** @brief Adds the engine half's per-pass cost to the plan a host reads.
     *
     * @details
     * - Called only when the content half is NOT running (inline: right after the engine half; threaded: at the start
     *   of the next frame, after the runner went idle), because the plan's tables belong to the content half.
     *
     * @param frame The frame whose engine half just finished.
     */
    void publishEngineMicros(const detail::EngineFrame& frame);

    /** @brief Reports the problems the compile found, once per episode, then remembers them.
     *
     * @details
     * - A problem is an episode, not a per-frame stream: what the previous compile already found is not
     *   said again, a problem that is gone is forgotten, and the same breakage later is a new episode.
     */
    void reportPlanProblems();

    /** @brief Inserts a slot in the pass list, keeping it ascending by order.
     *
     * @details
     * - The one place the slot list is ordered (equal orders keep insertion order). A pass instance already
     *   registered is ignored: it would otherwise run twice.
     *
     * @param slot Slot to insert (moved in when it is not a duplicate).
     * @return The pass the engine registered, or null when that pass was already registered.
     */
    intrusive_ptr<RenderPass> insertSlot(Slot slot);

    /** @brief Turns a declaration into its execution node, adjudicating the depth edge first.
     *
     * @details
     * - THE ONE PLACE a PassDesc becomes a node: applyPlan updates an existing row through it and builds a new one
     *   with it, so a re-stated pass and an added pass cannot differ in kind mapping or depth-edge handling.
     * - A layered depth view and a second, different depth view for one target are REFUSED here (once per
     *   episode), because the SDK has one depth image per target and a declaration it cannot honour must not
     *   look accepted on the node.
     *
     * @param desc Declaration to adopt (moved into the node).
     * @return A new node of the declaration's kind (never null).
     */
    [[nodiscard]] intrusive_ptr<RenderPass> makePassNode(const PassDesc& desc);

    /** @brief Applies a plan that has already been validated (see applyPlan): update, add, remove, re-order.
     *
     * @param plan  The shape to apply (its problems were answered by validatePlan already).
     * @param owner The contributor whose rows the plan states (null: the whole session, which may remove).
     */
    void applyPlanRows(const Plan& plan, const void* owner);

    /** @brief Drops one registered pass and releases what the backend retained for it (the plan's own step). */
    void removePass(raw_ptr<RenderPass> pass);

    /** @brief Answers the names the HOST publishes (see publish), which a plan's named input may resolve against.
     *
     * @return The standing host binding names, in name order.
     */
    [[nodiscard]] std::vector<String> hostOutputNames() const;

    // ---- Registration by owner (see PassChain): one slot table, an owner pointer per row ----

    friend class PassRegistry;

    /** @brief Removes every pass whose slot is owned by @p owner (each one as removePass).
     *
     * @param owner Owner whose passes are taken back (null is a no-op: unowned passes belong to whoever added them).
     */
    void removePassesOwnedBy(const void* owner) noexcept;

    /** @brief Makes sure the transient target a slot declares exists, at the shape it declares.
     *
     * @param slot Slot whose declaration is served (its pass gets the target when it changed).
     */
    void resolveTransientTarget(Slot& slot);

    /** @brief Whether two problems are the same episode (see reportPlanProblems).
     *
     * @param other Problem to compare with.
     * @return true when both describe the same problem of the same subject.
     */
    [[nodiscard]] bool sameProblem(const CompileProblem& other) const noexcept;

    /** @brief Records a problem the compile found, once per subject (see CompileProblem).
     *
     * The one place a finding becomes data. Two checks can reach the same subject (a third pass claiming an
     * image two others already claim), and the frame should say it once: the episode rule then decides whether
     * it is said OUT LOUD at all (see reportPlanProblems).
     *
     * @param message The sentence a report would carry (it names its subject).
     */
    void noteProblem(String message);

    /** @brief Returns whether the structural check already spoke about this declared input.
     *
     * One wire, one message: a declared input the structural check already reported (no producer at all, or a
     * producer registered too late) is not repeated in frame terms by the resolution.
     *
     * @param pass     Consumer that declared the input.
     * @param identity Image the input addresses.
     * @return true when the structural check reported that wire for this frame.
     */
    [[nodiscard]] bool unusableInputReported(raw_ptr<const RenderPass> pass, const OutputIdentity& identity) const noexcept;

    /** @brief Resolves one pass' declared inputs into @p out, for this frame (see compileFrame).
     *
     * @param pass Pass whose declarations are resolved.
     * @param out  Receives the resolved inputs in declaration order (cleared first).
     */
    void resolveRowInputs(raw_ptr<RenderPass> pass, std::vector<ImageSource>& out);

    /** @brief Publishes a pass' output name for the rest of this frame (see compileFrame).
     *
     * @param pass Pass whose output to publish.
     */
    void publishRowOutput(raw_ptr<RenderPass> pass);

    /** @brief Returns the name a pass is reported under (its own, or a readable fallback).
     *
     * @param pass Pass to name.
     * @return The pass' name, or an unnamed-pass marker when it has none.
     */
    [[nodiscard]] static String reportedPassName(raw_ptr<const RenderPass> pass);

    /** @brief Returns the first enabled pass that needs a target this backend cannot draw into.
     *
     * A backend reporting no off-screen support IGNORES a non-null target (see
     * RenderDevice::capabilities), so such a pass draws into the window instead of where the
     * pipeline expects it, and nothing else in the frame would notice (see frame(), the one caller).
     *
     * @return The first enabled pass with a non-null render target, or null when the backend supports
     *         targets or no pass asks for one.
     */
    [[nodiscard]] raw_ptr<const RenderPass> passNeedingAnUnsupportedTarget() const;



    /** @brief Resolves one declared input image: the target it addresses, or null when nothing
     * produced it this frame.
     *
     * This is the frame-level half of "the pass draws nothing": the structural check sees the
     * declaration, only the frame knows whether the producer ran. It reports once per episode, and
     * not for a wire the structural check already reported (no producer at all, or one registered too
     * late) — that is one problem, and it is the wiring check's to state.
     *
     * @param pass  Pass that declared the input.
     * @param image Image the pass declared.
     * @return The target to hand over, or null when the image was not produced this frame.
     */
    raw_ptr<RenderTarget> resolveDeclaredImage(raw_ptr<RenderPass> pass, const ImageRef& image);

    /** @brief Resolves one declared input target, or null when nothing produced it this frame.
     *
     * @param pass   Pass that declared the input.
     * @param target Target the pass declared.
     * @return The target to hand over, or null when nothing drew into it this frame.
     */
    raw_ptr<RenderTarget> resolveDeclaredTarget(raw_ptr<RenderPass> pass, raw_ptr<RenderTarget> target);

    /** @brief Reports a declared input nothing produced this frame (once per episode).
     *
     * @param pass     Pass whose input is missing.
     * @param identity Image (or whole target) that was not produced.
     * @param what     Human-readable description of what the pass declared.
     */
    void reportUnproducedInput(raw_ptr<RenderPass> pass, const OutputIdentity& identity, const String& what);

    // ---- Fields ----
    intrusive_ptr<RenderBackend> device_;
    // The validity bit behind every Lease this engine handed out (see lease): set to this in the constructor and
    // cleared in the destructor, so a holder of a lease (a PassChain, say) answers "the engine is gone" instead
    // of calling into freed memory - and never has to keep the engine alive to be safe.
    std::shared_ptr<vn::detail::LeaseCell<RenderEngine>> lease_cell_;
    // The engine's ONE diagnostic route (see Diagnostics.hpp): the host's sink lives inside it, and the
    // backend the engine drives reports through this very instance (setDiagnosticsRoute), which is what
    // makes diagnosticCount() the whole truth instead of half of it.
    Diagnostics diagnostics_;
    // The shading a drawable that names no program gets. A named program, not an enum: set once in
    // the constructor to forwardProgram(), replaced by setDefaultContentProgram(), and null means "decline".
    intrusive_ptr<const ShaderProgram> default_content_program_;
    std::vector<Slot>                  slots_; // uniform ordered draw registry
    // The wiring checks' tables (see collectPlanProblems). MEMBERS, cleared every compile: the pass count is small, a
    // flat scan beats a tree, and a vector keeps its capacity - so a steady frame allocates nothing here.
    std::vector<Declaration>        wiring_filled_;
    std::vector<Declaration>        wiring_claimed_;
    std::vector<OutputIdentity>     wiring_colliding_;
    std::vector<UnboundDeclaration> wiring_unbound_;
    std::vector<OutputIdentity>     wiring_promised_images_;
    // The shape the frame is laid out on, as the host announced it (see surfaceWidth / RenderSurface::resize):
    // recorded even before a device exists, because the passes and their targets are built from it.
    int surface_width_                 = 0;
    int surface_height_                = 0;
    // What the frame's target was LAST told (see RenderEngine::frame's head): the swapchain follows the frame, so a
    // frame only pays for a resize when the number it was laid out on differs from the one the target already has.
    // Reset by shutdown(), because the target a re-initialized engine asks for is a new object with the device's own
    // default size.
    int applied_width_                 = 0;
    int applied_height_                = 0;
    // Monotonic content-frame token, announced to every rendered scene each
    // frame (Scene::setContentFrame): the passes of one frame that draw the same
    // scene through the same camera then share one tree walk instead of walking
    // it once per pass. 0 = no frame announced yet, which is also what a caller
    // that never renders sees (no memoising at all).
    std::uint64_t content_frame_       = 0;
    bool          initialized_         = false;
    // The one area this engine drives (see createSurface): OWNED, because a 1:1 relation needs one of its two ends
    // to own it. It is the only place the engine reads a target from - the engine owns no window and no handle (see
    // the class contract) - and a host holding the same object cannot outlive it: dropping the host's handle leaves
    // the engine's, and the engine releases it while it is still an object (see ~RenderEngine).
    intrusive_ptr<RenderSurface> area_ = nullptr;
    /// "A second area was asked for" is one condition, not a per-call event (see createSurface).
    ReportOnce area_refused_{};
    /// "The borrowed device is not up" is one condition too (see initialize): re-armed when a device is set, so the
    /// same device answering the same thing does not say it twice, and a different one gets its own say.
    ReportOnce device_not_up_{};
    // The target the frame path presents through when this engine has NO area (see initialize): what the device
    // made for the window it brought up itself, or nullptr when it presents nothing. Borrowed - the device owns it.
    raw_ptr<PresentTarget> own_target_ = nullptr;
    // The episode of "the backend cannot draw the off-screen targets the pipeline asks for" (see frame()):
    // said once, re-armed the moment no enabled pass needs a target. The shared helper, like every other
    // one-condition episode in this class.
    ReportOnce unsupported_target_reported_{};

    /// "A pass names a layered depth view" is one condition, not a per-pass event (see PassDesc::depth_view): layered
    /// targets are not built yet, so the declaration is refused once rather than once per declaration.
    ReportOnce layered_depth_reported_{};
    /// The depth-view conflicts already said, keyed by (target, the image it was refused): a repeated identical
    /// declaration stays silent, while a different second image gets its own sentence (see PassDesc::depth_view).
    std::set<std::pair<raw_ptr<RenderTarget>, raw_ptr<RenderTarget>>> depth_view_conflicts_{};


    /// The frame's image tables: host bindings, this frame's productions/publications, and transients. Kept
    /// out of the engine so passes, ordering and the compile stay the engine's only business (see
    /// FrameResources).
    std::unique_ptr<detail::FrameResources> resources_;

    /// This frame's compiled rows, in the order they run (reused; one per slot).
    std::vector<PlanRow> plan_rows_;
    /// The execution order of plan_rows_ (indices), decided by scheduleRows(); also what buildFrameValue walks.
    std::vector<std::uint32_t> frame_order_;
    /// The announced sort key of each plan row, rebuilt by scheduleRows() (the scheduler reads a contiguous span).
    std::vector<int> frame_order_keys_;
    /// The scheduler's working memory (see FrameScheduler): one per engine, reused every frame.
    std::unique_ptr<detail::FrameScheduler> scheduler_;
    /// THIS frame, as one value: what the passes described plus the facts the engine half needs (the size it was laid
    /// out on, the target it resolves to, the frame token). Rebuilt by every compile, reused across frames so a
    /// steady frame keeps its storage - and, being one object, it is what the frame thread is handed.
    detail::EngineFrame* frame_ = nullptr;
    /// The frame values, TWO of them: with a frame thread, the half that runs elsewhere holds one while the content
    /// half fills the other. Both live as long as the engine, because each keeps the storage of its pass entries.
    std::array<std::unique_ptr<detail::EngineFrame>, 2> frame_slots_;
    /// Which slot frame_ names while a frame is being built (alternates every frame).
    std::size_t frame_slot_     = 0;
    /// How many frames this engine has built (monotonic; 0 before the first one). What waitForFrame names.
    std::uint64_t frame_number_ = 0;
    /// The id of the last frame whose engine half finished (0 before the first one). Read by waitForFrame.
    std::atomic<std::uint64_t> frame_completed_{ 0 };
    /// Where the engine half runs (see setFrameThreading); null means inline - the thread object exists only when a
    /// host asked for one, so an engine that never does allocates nothing and starts no thread.
    std::unique_ptr<detail::FrameRunner> runner_;

    /// The episode of the "a session's shape cannot change during a frame" report (see sessionChangeAllowed).
    ReportOnce session_change_episode_;

    /// @brief Answers whether a session-shape change may be applied now, reporting ONE diagnostic per episode when not.
    ///
    /// @details
    /// - THE ONE PLACE THE RULE IS SPELLED OUT (design graphics-threading.md 2.12, C20): the pass slots, the resources
    ///   table and the backend's retained state are what the engine thread walks while it records, so a change that
    ///   lands during a frame is REFUSED rather than applied to a list something is already reading.
    /// - Private on purpose: it is the engine's own guard, not a question a host asks.
    /// - ONE REPORT PER EPISODE: a host that forgot to wait hears it the first time; an accepted change re-arms it, so a
    ///   second, separate mistake still speaks.
    ///
    /// @param verb Name of the refused call, for the sentence.
    /// @return true when the caller may proceed.
    [[nodiscard]] bool sessionChangeAllowed(const char* verb);
    /// A frame whose engine half finished on the frame thread and whose per-pass cost still has to reach the plan: the
    /// next frame() publishes it, once it has waited for that half. Null when there is nothing to publish (the inline
    /// path publishes immediately - it IS the content half's thread).
    std::atomic<detail::EngineFrame*> pending_micros_{ nullptr };
    /// The last answered frame's id and picture outcome, packed into one word (`frame << 8 | outcome`) so a host can
    /// read both from any thread without a lock and without the two halves of the answer tearing apart.
    std::atomic<std::uint64_t> present_record_{ 0 };
    /// The last ANSWERED frame's whole answer (see frameResult): its picture, the world it saw and what it cost, plus
    /// the lock that makes it readable from the content's thread while the engine thread answers the next frame. A
    /// snapshot handle and a camera value are all it shares, so the lock is held for a copy in both directions.
    mutable std::mutex frame_result_mutex_;
    FrameResult        frame_result_;
    // THE PUBLICATION CHANNEL. Its verbs are the content side's (see detail::PublicationStore): publish, bind, and the
    // lookup the frame's bind step does - so a frame value crosses with publications in it, never with live content
    // (G2/G12), and the engine half's only call on it is `beginFrame` (the token the per-view memos share).
    std::unique_ptr<detail::PublicationStore> publications_;
    /// This frame's problems and the previous frame's, swapped after each compile: the episode rule.
    std::vector<CompileProblem> plan_problems_;
    std::vector<CompileProblem> previous_problems_;
    /// Whether a compile is running: while it is, the checks' reports are COLLECTED rather than emitted (see
    /// reportPlanProblems). One flag, because every check keeps calling reportEngineProblem() as it always did
    /// - the compile decides what becomes an episode, not each check.
    bool collecting_problems_ = false;
    /// The declared inputs the structural check spoke about in THIS compile (see unusableInputReported).
    std::set<std::pair<raw_ptr<const RenderPass>, OutputIdentity>> unusable_inputs_this_frame_;

    /// The passes whose picture shape was reported, and this compile's rebuild of that list (see
    /// collectPictureShapeMismatches). The rebuild IS the episode rule for this check: an entry that is not
    /// carried over re-arms its pass, so the same mismatch after it was fixed is a new report.
    struct ShapeMismatchEpisode {
        raw_ptr<const RenderPass> pass = nullptr; // the pass whose rectangle and camera disagree
        ReportOnce                reported;       // whether that pass' mismatch has been said
    };

    std::vector<ShapeMismatchEpisode> shape_mismatches_this_frame_;
    std::vector<ShapeMismatchEpisode> shape_mismatches_last_frame_;
};

VN_GRAPHICS_NS_END
