#pragma once
#include "graphics_global.hpp"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
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
#include "DiagnosticsHandle.hpp"
#include "EngineNotReadyReason.hpp"
#include "FrameContext.hpp"
#include "FrameDescription.hpp"
#include "FrameResult.hpp"
#include "FrameReport.hpp"
#include "FrameTable.hpp"
#include "OutputRegistry.hpp"
#include "PassDesc.hpp"
#include "PassRegistry.hpp"
#include "Plan.hpp"
#include "PrepareReport.hpp"
#include "RenderDiagnostic.hpp"
#include "RenderArea.hpp"
#include "ReportOnce.hpp"
#include "SceneSnapshotRef.hpp"

VN_GRAPHICS_NS_BEGIN

namespace detail
{

class FrameResources;
class FrameScheduler;
class FramePump;                     // The frame's numbers, its one-in-flight thread and its answers (see src/FramePump.hpp).
class PublicationStore;              // The content side of the publication channel (see src/PublicationStore.hpp).
class SessionShape;                  // The pass table, the plan diff and the compile (see src/SessionShape.hpp).
class SurfaceLedger;                 // The one area, its size and the target a frame presents through (see src/SurfaceLedger.hpp).
struct EngineFrame;                  // One frame as it travels between the two halves (see src/EngineFrame.hpp).
class FrameRunner;                   // The thread a frame's engine half runs on (see src/FrameRunner.hpp).

} // namespace detail

class Light;
class Scene;
class RenderPass;
class ImageRef;
class RenderTarget;
class RenderBackend;
class RenderFrameExecutor;
class PresentTarget;

/**
 * @brief What one RenderEngine::frame() call did.
 *
 * @details
 * - TWO OUTCOMES, one per price the call paid (`G4`): #Status::Ran means this call compiled AND answered the frame
 *   before returning (there is no frame thread), #Status::Submitted means it compiled and handed it over, so the
 *   picture follows one frame later (see waitForFrame). Nothing else is a frame outcome.
 * - #Status::NotReady is not a third frame outcome: it says the call drove NOTHING, and @ref reason says which
 *   precondition failed. It shares #EngineNotReadyReason with `prepare()` so a host answers "is this engine ready" once.
 * - What it deliberately does not answer: whether anything was drawn (RenderEngine::frameReport) or where the picture
 *   went (RenderEngine::presentRecord / #PresentOutcome). Both describe the frame, not the call's readiness.
 *
 * @note
 * - The value describes the CALL; it is not the per-frame result the frame pipeline design calls `FrameResult`
 *   (`graphics-frame-pipeline.md` F12), which carries the snapshot, view, poses and diagnostics a host queries with.
 */
struct VN_GRAPHICS_API FrameCallOutcome {
    /// What became of the call (see the struct's own note).
    enum class Status : std::uint8_t
    {
        Ran,       // Compiled and answered before the call returned (no frame thread).
        Submitted, // Compiled and handed to the frame thread; see waitForFrame.
        NotReady,  // Nothing was driven: see reason.
    };

    Status         status{ Status::NotReady };            ///< Which answer this is.
    EngineNotReadyReason reason{ EngineNotReadyReason::NoDevice };     ///< Why, when @ref status is #Status::NotReady.

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
 *   declaration (see RenderArea), never something the engine infers.
 * - THE ENGINE KNOWS NO WINDOW: the window belongs to the AREA - the area holds the TARGET its device made
 *   (RenderArea::bindTarget) and the size a host reports (resize), and the engine holds neither. It
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

    /** @brief Gets the engine's one diagnostic route (see DiagnosticsHandle).
     *
     * @details
     * - The narrow contract for counting and reporting; a recipe holds this instead of the whole engine, and the
     *   compile-time episode rule is preserved because the handle forwards to the engine's own report path.
     *
     * @return A handle over this engine's diagnostic route.
     */
    [[nodiscard]] DiagnosticsHandle diagnostics() noexcept;

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
     * - A frame already handed to the engine thread is FINISHED first: the driver it runs through is what this call
     *   gives back, and every other session change waits or refuses rather than racing a frame.
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

    /** @brief Gets a COPY of the installed diagnostic sink (empty when unset).
     *
     * @details
     * - A copy, because the route may be replaced from another thread at any moment (see Diagnostics): a reference
     *   into its storage would be a handle on something a report is writing.
     *
     * @return The sink.
     */
    DiagnosticSink diagnosticSink() const
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

    /** @brief Gets how many diagnostics the ENGINE and its DEVICE reported through this engine's route.
     *
     * @details
     * - ONE NUMBER, because there is one route: what the engine could not wire and what the backend could
     *   not serve are counted in the same place.
     * - THE HOST IS NOT PART OF IT (see DiagnosticOrigin): a report the layer above injected through
     *   reportEngineProblem is counted by the route but not here, so "how many problems did the engine find"
     *   stays decidable - a host bug and a wiring bug no longer read as one number (A-06 / CAP-87).
     * - Counted whether or not a sink is installed, so a host can gate on "did anything unexpected
     *   happen" without listening.
     *
     * @return Total number of diagnostics the engine and its device reported.
     */
    [[nodiscard]] std::size_t diagnosticCount() const noexcept;

    /** @brief Gets how many diagnostics of @p origin were reported through this engine's route.
     *
     * @param origin Origin to count.
     * @return Number of reported diagnostics with that origin.
     */
    [[nodiscard]] std::size_t diagnosticCount(DiagnosticOrigin origin) const noexcept;

    /** @brief Gets how many diagnostics of @p category the ENGINE and its DEVICE reported.
     *
     * @details
     * - The same scope as @ref diagnosticCount(): the per-category answer is about what the engine and its
     *   device ran into, not about what the host itself injected.
     *
     * @param category Category to count.
     * @return Number of reported diagnostics in that category.
     */
    [[nodiscard]] std::size_t diagnosticCount(DiagnosticCategory category) const noexcept;

    /** @brief Gets whether neither the engine nor its device reported anything.
     *
     * @details
     * - The gate a phase asserts on: a steady run that is expected to be uneventful asks this instead of
     *   reading the log. It uses the same scope as @ref diagnosticCount(), so the two answers agree.
     *
     * @return true when no diagnostic was reported by the engine or its device.
     */
    [[nodiscard]] bool diagnosticsClean() const noexcept;

    /** @brief Reports one problem THE HOST found: counted, and marked with origin Host.
     *
     * @details
     * - THE HOST'S ENTRY, and that is what the origin records: a problem the layer above noticed is real
     *   and belongs on the same route (so the host's sink and the engine's log agree), but it is not an
     *   engine finding - diagnosticCount() leaves it out and diagnosticCount(DiagnosticOrigin::Host) counts
     *   it (A-06).
     * - NAMING NO CODE: the caller's sentence is all there is, so this is the migration form the gate
     *   ratchets; a report whose cause is known carries it as a RenderDiagnostic instead.
     *
     * @param severity How bad the situation is.
     * @param category What it is about.
     * @param message  Human-readable detail, naming the object it is about.
     */
    void reportEngineProblem(vn::graphics::DiagnosticSeverity severity, vn::graphics::DiagnosticCategory category, const String& message);

    /** @brief Reports one problem as a whole diagnostic (see RenderDiagnostic).
     *
     * @details
     * - THE FORM THAT CARRIES A CODE: a compile problem names its cause (`engine.compile.*`), its subject (the pass)
     *   and its own severity, so a host can act on it and a test can assert on it rather than on the sentence.
     * - DURING A COMPILE the value is COLLECTED, not emitted: the episode rule decides what is said out loud, and it
     *   now keeps the whole diagnostic - so a problem the compile found is no longer flattened to one category on the
     *   way out.
     *
     * @param diagnostic What to report.
     */
    void reportEngineProblem(const vn::graphics::RenderDiagnostic& diagnostic);


    /** @brief Attaches this engine to the device it borrows: ready to drive frames.
     *
     * @details
     * - The DEVICE IS NOT INITIALIZED HERE. It belongs to its creator (see RenderDevice::initialize), which brings it
     *   up (and follows a re-announced window) before this call; an engine that brought it up would tear a shared
     *   device down under whoever else is drawing through it.
     * - A device that is not up fails the attach and is reported ONCE per episode: a frame driven through it would
     *   look like a running engine drawing nothing.
     * - What the attach does: asks the device for a target when this engine's area holds none (the device's own
     *   window, or "no presentation" for a display-less device), replays the announced area size to the area's
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
     * - Never while a frame is being answered: like every entry of this class it compiles the declarations, and
     *   it waits for a frame in flight on the engine thread first (between frames).
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
     *   animates measures its own time (an overlay's own wall clock, or a host that calls FrameUpdate::onFrame()
     *   before frame()), which is what the one `dt` a host had to hand over used to be - a number nothing in the
     *   frame read.
     *
     * @return What the call did (see FrameCallOutcome): `status.ran()` when this engine has no frame thread and the frame
     *         was answered before returning, `status.submitted()` when it was handed to one; `NotReady` with
     *         `reason` (`NoDevice` / `NotInitialized`) when nothing was compiled or handed over.
     */
    FrameCallOutcome frame();

    /** @brief Chooses where the engine half of each frame runs (see FrameThreading).
     *
     * @details
     * - A LIFETIME DECISION, made once: switching to EngineThread starts this engine's frame thread, switching back to
     *   Inline stops and joins it. Both wait for a frame in flight first, so a host may switch between frames (not
     *   from inside a FrameUpdate::onFrame() call).
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
     * - #PresentOutcome::Failed is answered when the frame's engine half threw: it also means "the picture did not reach
     *   the surface", but the move is NOT a re-attach - read the `engine.frame.failed` diagnostic and fix what threw.
     *   The frame is answered either way, so `waitForFrame` never waits for a frame that will never finish.
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
     *   the view it was taken with (camera value, device-pixel area, layer mask), both taken when the frame was
     *   built. A pick, an anchor or a fit that asks about "what the user sees" reads them here instead of the live
     *   scene and camera (`I7`, design N17).
     * - `frame` stays 0 until something is answered (`answered()`), and `present` is the same answer `presentRecord()`
     *   carries - the one field a host must ACT on (`Lost` -> re-attach, between frames; `Failed` -> the engine half
     *   threw, see `engine.frame.failed` in the diagnostics: the picture will not come back by re-attaching).
     *
     * @note
     * - `presentedCamera()` is the camera half of `query.view`, kept as its own entry for callers that only aim.
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
     * - It is the camera half of `query.view` in RenderEngine::frameResult, kept as its own entry for callers that
     *   only aim.
     *
     * @return The snapshot, or nullopt (see above).
     */
    [[nodiscard]] std::optional<CameraSnapshot> presentedCamera() const;

    /** @brief Gets the width the frame is laid out on, as the host announced it (see RenderArea::resize).
     *
     * @details
     * - The surface's shape is the one number a builder or a host needs from the engine while it is assembling
     *   passes (PassChain::build lays its targets out on it). It is not a per-frame value: it changes when the
     *   surface is resized, and everything that reads it reads the same fact.
     *
     * @return Width in the units the host announced (logical pixels for the Qt host; 0 until the first resize).
     */
    [[nodiscard]] int areaWidth() const noexcept;

    /** @brief Gets the height the frame is laid out on (see areaWidth).
     *
     * @return Height in the units the host announced (0 until the first resize).
     */
    [[nodiscard]] int areaHeight() const noexcept;

    /** @brief Gets the context of the frame the engine is about to build (see FrameContext).
     *
     * @details
     * - THE ONE VALUE THE PER-FRAME WORK SHARES: the host composes nothing, it asks once and hands the answer to
     *   `SceneView::update` / `PassChain::update`, which pass the same value to every object of that frame
     *   (`FrameUpdate::onFrame`).
     * - A PURE READ, AND THE SAME ANSWER ALL FRAME: the frame id, the announced size and the ratio are read live, so
     *   a host that asks twice gets the same context; only `elapsed_seconds` advances, once per `frame()`.
     * - THE TIME IS ONE FRAME BEHIND: it is the interval between the two most recent frames, measured at the head of
     *   `frame()` - the value a consumer of "how fast is the loop running" wants, and the price of keeping this call
     *   free of side effects. It is wall-clock; a host that needs a fixed step keeps its own clock.
     *
     * @return The frame's context; `frame_number` is 0 before any frame ran, and the fields are empty when no host
     *         announced a surface yet.
     */
    [[nodiscard]] FrameContext frameContext() const noexcept;

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
     *   (RenderArea::present), and this is how a host finds out whether that pass is still in the frame
     *   (see RenderArea::presentingPass).
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
     * - WHAT "IN PLACE" DOES NOT COVER: a pass whose KIND changed (Scene -> Fullscreen) cannot become the other
     *   object, so that name is removed and re-added under the same owner and role. Every other field a
     *   PassDesc states is adopted as the pass' new declaration.
     * - THE PLAN'S VOCABULARY IS `DrawKind` (Scene / Screen), and the node built from a declaration is the built-in
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

    /** @brief Gets how many transient targets the current plan hands out (see PassDesc::target.name).
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
     * (PassDesc::target.name).
     *
     * @details
     * - A declared transient is owned by the compile, so this is how its OWNER reaches it (to preview it,
     *   read it back, or state a relationship the declaration cannot - "this target borrows that transient's
     *   depth" is a composition, RenderTarget::shareDepth its mechanism).
     * - The answer is the instance live NOW: re-ask after a shape change, because a transient is REPLACED
     *   when its declared shape moves (PassDesc::target.shape) - a handle kept across a resize describes a
     *   target the compile has already let go.
     *
     * @param identity Identity a pass declared (its target name).
     * @return The target, or null when nothing declares that identity (including every call before the
     *         first compiled frame - a transient is the compile's product, not the build's).
     */
    [[nodiscard]] raw_ptr<RenderTarget> transientTarget(const String& identity) const;

    /** @brief Sets the DEFAULT program for content: what a drawable naming none of its own gets.
     *
     * @details
     * - A default, not an override: a drawable's own program (DrawItem::program, Geometry::setProgram,
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

    /** @brief Creates the ONE render area this engine drives (see RenderArea).
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
    [[nodiscard]] intrusive_ptr<RenderArea> createArea();

  private:
    friend class RenderArea; // its one area: announces its size, and asks whether it is the one

    /** @brief Answers whether @p area is the one area this engine drives (see createArea).
     *
     * @details
     * - The question `RenderArea` asks before it lets a declaration through: an area another engine drives would
     *   present nothing at all, so the area says so once instead of silently dropping the picture (design: one
     *   engine, one area).
     *
     * @param area Area to check (null answers false).
     * @return true when @p area is this engine's own.
     */
    [[nodiscard]] bool drivesArea(raw_ptr<const RenderArea> area) const noexcept;

    /** @brief Records the area's size for the frame and replays it to the target once a session can honour it.
     *
     * @details
     * - The engine's ONE size announcement (see RenderArea::resize): recorded even with no device yet
     *   (the frame context is what the passes read), replayed by initialize(). Only the engine's own area is
     *   heard (see createArea) - a frame laid out on another window's size would be a silent disagreement.
     *
     * @param from   Area announcing (the engine's own is the only one it acts on).
     * @param width  Area width in pixels (> 0; the caller refuses the rest).
     * @param height Area height in pixels (> 0).
     */
    void announceAreaSize(raw_ptr<const RenderArea> from, int width, int height);

    /** @brief Compiles this frame's declarations into the table the frame runs from (see FrameTable).
     *
     * @details
     * - THE ONE PLACE A FRAME IS DECIDED (see detail::SessionShape::compile): compiled every frame - no declaration
     *   tells the engine it moved, because reading the declarations IS the compile. The ORDER and WIRING and this
     *   set's PROBLEMS are the shape's; the engine half's expansion follows below.
     *
     * @param frame The frame value to compile into (the pump's storage, borrowed for the call).
     */
    void compileFrame(detail::EngineFrame& frame);

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

    /** @brief Runs one frame's engine half, turning a throw into a frame answer (see @ref runEngineHalf).
     *
     * @details
     * - THIS is what the pump is handed, in both modes, so a frame is answered exactly once whatever happens inside:
     *   inline the exception used to reach the host mid-`frame()`, and on a frame thread it left the thread function,
     *   which is `std::terminate` (design 2.12 - a frame is a value and is always answered).
     * - The failure is reported as `engine.frame.failed` and answered with #PresentOutcome::Failed: the frame is
     *   complete, the waiter wakes, and the picture is known not to have been drawn.
     *
     * @param frame The frame value to answer (borrowed; one of this engine's slots).
     */
    void answerEngineHalf(detail::EngineFrame& frame);

    /** @brief Reports why an engine half threw and answers its frame with #PresentOutcome::Failed.
     *
     * @param frame The frame that was being answered.
     * @param what  What was thrown, as the report's sentence says it.
     */
    void reportEngineHalfFailure(detail::EngineFrame& frame, const char* what);

    /** @brief Publishes one answered frame's outcome, its whole answer, and its completion.
     *
     * @details
     * - ONE WRITER for both an answered frame and a failed one: a host reading the answer must not have to know which
     *   produced it, and the completion a `waitForFrame` blocks on happens exactly here.
     *
     * @param frame The frame whose engine half is over (borrowed; `outcome` is already its answer).
     */
    void publishFrameAnswer(detail::EngineFrame& frame);

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

    friend class PassRegistry;

    /** @brief Removes every pass whose slot is owned by @p owner (each one as removePass).
     *
     * @details
     * - Ownership is ONE SLOT TABLE with an owner pointer per row (see PassChain): a row is removed by its own name inside that owner's scope, and taking a whole owner back is one walk.
     *
     * @param owner Owner whose passes are taken back (null is a no-op: unowned passes belong to whoever added them).
     */
    void removePassesOwnedBy(const void* owner) noexcept;

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

    intrusive_ptr<RenderBackend> device_;
    // THIS ENGINE'S FRAME DRIVER: borrowed from the device (RenderDevice::createFrameDriver) and returned to it
    // (RenderDevice::releaseFrameDriver) when the device is replaced or this engine dies. The device owns it, so a NULL
    // SESSION means the device could not hand one out - frames then answer NotInitialized rather than guess a driver.
    RenderFrameExecutor* session_{ nullptr };
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
    // The shape the frame is laid out on, as the host announced it (see areaWidth / RenderArea::resize) lives with the
    // surface ledger (see detail::SurfaceLedger): the engine holds neither the size nor the area, only the handle that
    // composes them.
    // Monotonic content-frame token, announced to every rendered scene each
    // frame (Scene::setContentFrame): the passes of one frame that draw the same
    // scene through the same camera then share one tree walk instead of walking
    // it once per pass. 0 = no frame announced yet, which is also what a caller
    // that never renders sees (no memoising at all).
    std::uint64_t content_frame_       = 0;
    bool          initialized_         = false;
    // The frame clock (see frameContext): the head of frame() takes one reading and keeps the interval between the two
    // most recent frames in `frame_context_`, which is what a consumer asking "how fast is this loop running" wants.
    // Measured here and nowhere else, so two overlays cannot window the same frame differently (BC-25).
    FrameContext                           frame_context_{};
    std::chrono::steady_clock::time_point   last_frame_tick_{};
    bool                                    frame_tick_taken_ = false;
    /// Takes the frame clock's one reading and records the interval since the previous one (see frameContext).
    void advanceFrameClock() noexcept;
    // The one area this engine drives, its announced size and the target a frame presents through: OWNED by the
    // surface ledger, because a 1:1 relation needs one of its two ends to own it. The engine reads a target from it -
    // the engine owns no window and no handle (see the class contract).
    std::unique_ptr<detail::SurfaceLedger> surface_;
    /// "The borrowed device is not up" is one condition too (see initialize): re-armed when a device is set, so the
    /// same device answering the same thing does not say it twice, and a different one gets its own say.
    ReportOnce device_not_up_{};
    // The episode of "the backend cannot draw the off-screen targets the pipeline asks for" (see frame()):
    // said once, re-armed the moment no enabled pass needs a target. The shared helper, like every other
    // one-condition episode in this class.
    ReportOnce unsupported_target_reported_{};

    /// The frame's image tables: host bindings, this frame's productions/publications, and transients. Kept
    /// out of the engine so passes, ordering and the compile stay the engine's only business (see
    /// FrameResources).
    std::unique_ptr<detail::FrameResources> resources_;

    /// THE FRAME PUMP: the frame numbers, the value being built beside the one being answered, the thread that may run
    /// the engine half, and the answer a host waits for (see detail::FramePump).
    std::unique_ptr<detail::FramePump> pump_;

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
    /// A frame whose engine half finished on the frame thread and whose per-pass cost still has to reach the plan.
    /// The pump keeps this (see detail::FramePump): the plan is the content half's, so it is written at the one moment
    /// nobody else is in it, right after the next beginFrame waited.
    /// The last ANSWERED frame's whole answer and its lock live with the pump too, beside the completion number they
    /// are published with - one unit owns "what a host reads about the frame that just finished".
    // THE PUBLICATION CHANNEL. Its verbs are the content side's (see detail::PublicationStore): publish, bind, and the
    // lookup the frame's bind step does - so a frame value crosses with publications in it, never with live content
    // (G2/G12), and the engine half's only call on it is `beginFrame` (the token the per-view memos share).
    std::unique_ptr<detail::PublicationStore> publications_;
    /// THE SESSION SHAPE: the pass table, the plan diff and the compile (see detail::SessionShape). It borrows the
    /// route, the publication store and the image tables above, so it is constructed after them - and the engine
    /// composes it while holding none of its tables.
    std::unique_ptr<detail::SessionShape> shape_;
};

VN_GRAPHICS_NS_END
