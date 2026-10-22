#pragma once
#include "graphics_global.hpp"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <set>
#include <vector>

#include <vine/Lease.hpp>
#include <vine/Object.hpp>
#include <vine/RefCounted.hpp>
#include <vine/intrusive_ptr.hpp>
#include <vine/raw_ptr.hpp>

#include "Capabilities.hpp"
#include "Diagnostics.hpp"
#include "DiagnosticsRoute.hpp"
#include "FrameDescription.hpp"
#include "FramePlan.hpp"
#include "FrameStats.hpp"
#include "OutputRegistry.hpp"
#include "PassDesc.hpp"
#include "PassRegistry.hpp"
#include "RenderDiagnostic.hpp"
#include "RenderSurface.hpp"
#include "ReportOnce.hpp"

VN_GRAPHICS_NS_BEGIN

namespace detail
{
class FrameResources;
class FrameScheduler;
}  // namespace detail

class Light;
class Scene;
class RenderPass;
class ImageRef;
class RenderTarget;
class RenderBackend;
class PresentTarget;

/**
 * @brief High-level render engine managing the frame loop and render state.
 *
 * @details
 * - It imposes NO fixed pipeline: nothing renders unless the caller registers passes (addPass). Each
 *   frame() runs the registered passes in ascending order, then ends and swaps; a top / HUD pass is just a
 *   pass registered with a higher order - there is no separate overlay concept.
 * - A pure scheduler: it carries no content scene, no camera and no input forwarding. Every pass draws the
 *   content bound to it (addPass(pass, content, order)) or nothing; the primary interactive view lives in a
 *   SceneView that borrows this engine. Which pass draws into which SURFACE is the surface's own
 *   declaration (see RenderSurface), never something the engine infers.
 * - THE ENGINE KNOWS NO WINDOW: the window belongs to the AREA - the area holds the TARGET its device made
 *   (RenderSurface::bindTarget) and the size a host reports (resize), and the engine holds neither. It
 *   drives the frame: with no area target it asks the device (one that presents nothing answers none), it
 *   names the area's target in the frame it hands over, and it presents through it at the frame's tail
 *   (PresentTarget::present). A device that presents nothing presents nothing - a frame is complete
 *   without one.
 * - Drawing itself is delegated to the RenderDevice supplied via setDevice().
 */
class VN_GRAPHICS_API RenderEngine : public Object, public RefCounted<RenderEngine>
{
    VN_OBJECT_META_DECL;

  public:
    /** @brief Constructs an empty engine (no device, no passes).
     *
     * Configure the chain explicitly (addPass) or through PassChain::build, then setDevice()
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
     * - The narrow contract a recipe (PassChain) depends on: registration, order, content binding, and the owner
     *   relation that makes "the chain takes back what it registered" one call. The flat methods below
     *   (`addPass`, `removePass`, `passCount`, ...) are shorthands for this handle.
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
    const DiagnosticSink& diagnosticSink() const { return diagnostics_.sink(); }

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
    void reportEngineProblem(vn::graphics::DiagnosticSeverity severity,
                             vn::graphics::DiagnosticCategory category,
                             const String&                message);

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
     */
    void frame();

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
     * - The order lives in the ENGINE (addPass places the pass), not on the pass, so this is the one way to
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
     * - The plan is COMPILED from the registered passes (see compileFrame); every frame compiles its own,
     *   so the answer is the last frame's plan - the table the ENGINE announced for it - or, before any
     *   frame, the plan the current declarations would compile to. Both are the same compile, so a report
     *   cannot disagree with what the engine hands over.
     * - The order here is the DECLARATION order (order, then registration); the engine's scheduler turns it
     *   into execution order for the frame it hands a backend (see frameDescription), so the two differ
     *   exactly where a dependency overrides the announced priority. A report that claims "what actually
     *   ran" reads the executor's own plan.
     * - What it is for: a test asserting the SHAPE of a frame instead of counting passes or comparing
     *   pointers, a dev switch printing one table, and a host checking what a pipeline actually registered.
     *
     * @return The plan, in announcement order; empty when nothing is registered.
     */
    [[nodiscard]] FramePlan framePlan() const;

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

    /** @brief Registers a scene render pass executed every frame (no content bound).
     *
     * @details
     * - Passes run in ascending @p order (equal orders keep insertion order): negative orders first (a
     *   shadow-map or depth pre-pass); the pass presenting the primary view to the window conventionally at
     *   0; positive orders after it (post / compositing); top / HUD passes at the highest orders.
     * - This overload registers @p pass WITHOUT bound content: a base scene pass then draws nothing, while
     *   content-agnostic passes (a ScreenPass compositing inputs, an AxisGizmo) still execute. Bind a scene
     *   with the addPass(pass, content, order) overload. Nothing is auto-registered; registering the same
     *   pass instance twice is ignored.
     * - Two DIFFERENT passes may share a camera and an order: the backend keys retained state by the pass,
     *   so they stay separate content (the order only decides their stacking). Give distinct orders (and a
     *   sub-viewport / clear policy) when they must layer.
     *
     * @param pass  Pass to add (the engine keeps a reference).
     * @param order Execution order (ascending; any integer allowed).
     * @return The pass the engine registered, or null when it was already registered.
     */
    intrusive_ptr<RenderPass> addPass(intrusive_ptr<RenderPass> pass, int order);

    /** @brief Registers a scene render pass bound to explicit content.
     *
     * @details
     * - The pass renders @p content each frame. The association is stored by the ENGINE, not on the pass, so
     *   a RenderPass stays a reusable stage; registering the same instance twice is ignored (use
     *   bindPassContent() to change its content).
     *
     * @param pass    Pass to add (the engine keeps a reference).
     * @param content Scene the pass renders (a reference is kept); null leaves it contentless (draws nothing).
     * @param order   Execution order (ascending; any integer allowed).
     * @return The pass the engine registered, or null when it was already registered.
     */
    intrusive_ptr<RenderPass> addPass(intrusive_ptr<RenderPass> pass, intrusive_ptr<Scene> content, int order);

    /** @brief Registers a pass declared as a VALUE (see PassDesc).
     *
     * @details
     * - The declaration is turned into the execution node of its kind ONCE (`PassKind::Scene` -> RenderPass,
     *   `PassKind::Screen` -> ScreenPass) and registered like any other pass: nothing downstream knows which
     *   way a pass arrived, and no second mechanism has to be kept in step.
     * - What the descriptor adds over the setters: the STAGE it belongs to (its order follows from intent,
     *   see passStageOrder), a TAG for the plan, the TRANSIENT it needs (no host owns the target - see
     *   PassDesc::writes_image), and the per-frame hook its content updates itself through.
     * - What the return is for: a declaration says what a pass IS; the node it became is how a host CHANGES
     *   what it does without re-declaring - a program override, a viewport that follows the surface, a
     *   restated clear policy. That is the one handle a caller needs (a PassChain keeps the node for exactly
     *   this).
     *
     * @param desc The declaration (see PassDesc; a null camera draws nothing, as any pass does).
     * @return The execution node the declaration became, or null when that pass was already registered.
     */
    intrusive_ptr<RenderPass> addPass(const PassDesc& desc);

    /** @brief Removes a previously added pass.
     *
     * @details
     * - The pass is dropped from the ordered list and its backend resources are released: the retained
     *   per-pass GPU state (RenderDevice::releasePass, keyed by the pass itself) and any off-screen render
     *   target the pass owns (RenderDevice::releaseRenderTarget).
     *
     * @param pass Pass to remove (by pointer).
     */
    void removePass(raw_ptr<RenderPass> pass);

    /** @brief Removes all registered passes (each one as removePass: resources released). */
    void clearPasses();

    /** @brief Gets the number of registered scene passes.
     *
     * @return Number of passes added via addPass().
     */
    std::size_t passCount() const;

    /** @brief Rebinds which scene a registered pass renders.
     *
     * @details
     * - The binding is managed by the engine, not stored on the pass object.
     *
     * @param pass    Pass registered via addPass() (by pointer).
     * @param content New content scene; null clears the binding (a base scene pass then draws nothing).
     */
    void bindPassContent(raw_ptr<RenderPass> pass, intrusive_ptr<Scene> content);

    /** @brief Gets the content scene a registered pass renders.
     *
     * @param pass Pass registered via addPass() (by pointer).
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
    friend class RenderSurface;  // its one area: announces its size, and asks whether it is the one

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
        raw_ptr<Object>           owner{ nullptr };
        /// The role that owner gave this pass (empty = anonymous).
        String                    role;
        /// How long the last frame's record of this pass took, in microseconds (0: it did not run then).
        /// Measured by the compile, where that record happens (see buildFrameValue); reported through the frame
        /// plan (see FramePlan::PassPlan::record_micros).
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
        raw_ptr<const RenderTarget> target{ nullptr };      // set once the image is bound
        int                         attachment = 0;          // colour index; kWholeTarget = the whole target
        bool                        depth = false;           // a depth attachment is NOT attachment 0
        raw_ptr<const ImageRef>     declared{ nullptr };    // identity while unbound

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
            return target == other.target && attachment == other.attachment && depth == other.depth &&
                   declared == other.declared;
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
        raw_ptr<RenderPass>                pass{ nullptr };
        raw_ptr<Scene>                     content{ nullptr };
        int                                order{ 0 };
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
     */
    struct PlanProblem {
        String message;   ///< the sentence a report would carry
    };

    /** @brief What one pass declared about one image or target, as the wiring checks compare it.
     *
     * @details
     * - The pass count of a frame is small (single digits in every pipeline the framework builds), so the tables below
     *   are FLAT and searched linearly: a red-black tree per compile would allocate a node per pass per frame for no
     *   lookup benefit, and a vector keeps its capacity across frames (see collectPlanProblems).
     */
    struct Declaration {
        raw_ptr<const ImageRef>     image{ nullptr };    ///< The fine declaration, when there was one.
        raw_ptr<const RenderTarget> target{ nullptr };   ///< The target the declaration is about.
        raw_ptr<const RenderPass>   pass{ nullptr };     ///< Who declared it (null: the host filled it).
        std::size_t                 index{ 0 };          ///< Its position in the draw order.
        OutputIdentity              identity{};          ///< The image it claims (the claimed table's key).
    };

    /** @brief A declared image nobody bound to a target, and who declared it (see collectPlanProblems phase 4). */
    struct UnboundDeclaration {
        raw_ptr<const ImageRef>   image{ nullptr };      ///< The unbound identity.
        raw_ptr<const RenderPass> pass{ nullptr };       ///< Who declared it.
        bool                      as_output{ false };    ///< Declared as an output (else as an input).
    };

    /** @brief Compiles this frame's declarations into the table the frame runs from (see FramePlan).
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

    /** @brief Collects the problems this frame's declarations carry (see PlanProblem).
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

    // ---- Registration by owner (see PassChain): one slot table, an owner pointer per row ----

    friend class PassRegistry;

    /** @brief Registers @p desc on behalf of @p owner under @p role (the PassChain::addPass implementation).
     *
     * @param desc  Declaration of the pass to register.
     * @param owner Object that keeps the pass and takes it back when it dies (null = no owner).
     * @param role  Name the owner gives this pass (empty = anonymous).
     * @return The registered node, or null when it was already registered.
     */
    intrusive_ptr<RenderPass> addPassOwnedBy(const PassDesc& desc, raw_ptr<Object> owner, const String& role);

    /** @brief Removes every pass whose slot is owned by @p owner (each one as removePass).
     *
     * @param owner Owner whose passes are taken back (null is a no-op: unowned passes belong to whoever added them).
     */
    void removePassesOwnedBy(const Object* owner) noexcept;

    /** @brief Answers the passes owned by @p owner, in engine order.
     *
     * @param owner Owner whose passes are asked for (null answers nothing).
     * @return The handles; empty when nothing is registered for that owner.
     */
    [[nodiscard]] std::vector<intrusive_ptr<RenderPass>> passesOwnedBy(const Object* owner) const;

    /** @brief Answers the pass @p owner registered as @p role, or null.
     *
     * @param owner Owner whose pass is asked for (null answers nothing).
     * @param role  Role the pass was registered under.
     * @return The pass, or null when that owner registered no pass with that role.
     */
    [[nodiscard]] raw_ptr<RenderPass> passOwnedBy(const Object* owner, const String& role) const;

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
    [[nodiscard]] bool sameProblem(const PlanProblem& other) const noexcept;

    /** @brief Records a problem the compile found, once per subject (see PlanProblem).
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
    [[nodiscard]] bool unusableInputReported(raw_ptr<const RenderPass> pass,
                                             const OutputIdentity& identity) const noexcept;

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
    intrusive_ptr<RenderBackend>        device_;
    // The validity bit behind every Lease this engine handed out (see lease): set to this in the constructor and
    // cleared in the destructor, so a holder of a lease (a PassChain, say) answers "the engine is gone" instead
    // of calling into freed memory - and never has to keep the engine alive to be safe.
    std::shared_ptr<vn::detail::LeaseCell<RenderEngine>> lease_cell_;
    // The engine's ONE diagnostic route (see Diagnostics.hpp): the host's sink lives inside it, and the
    // backend the engine drives reports through this very instance (setDiagnosticsRoute), which is what
    // makes diagnosticCount() the whole truth instead of half of it.
    Diagnostics                         diagnostics_;
    // The shading a drawable that names no program gets. A named program, not an enum: set once in
    // the constructor to forwardProgram(), replaced by setDefaultContentProgram(), and null means "decline".
    intrusive_ptr<const ShaderProgram>  default_content_program_;
    std::vector<Slot>                   slots_;         // uniform ordered draw registry
    // The wiring checks' tables (see collectPlanProblems). MEMBERS, cleared every compile: the pass count is small, a
    // flat scan beats a tree, and a vector keeps its capacity - so a steady frame allocates nothing here.
    std::vector<Declaration>            wiring_filled_;
    std::vector<Declaration>            wiring_claimed_;
    std::vector<OutputIdentity>         wiring_colliding_;
    std::vector<UnboundDeclaration>     wiring_unbound_;
    std::vector<OutputIdentity>         wiring_promised_images_;
    // The shape the frame is laid out on, as the host announced it (see surfaceWidth / RenderSurface::resize):
    // recorded even before a device exists, because the passes and their targets are built from it.
    int                                 surface_width_      = 0;
    int                                 surface_height_     = 0;
    // Monotonic content-frame token, announced to every rendered scene each
    // frame (Scene::setContentFrame): the passes of one frame that draw the same
    // scene through the same camera then share one tree walk instead of walking
    // it once per pass. 0 = no frame announced yet, which is also what a caller
    // that never renders sees (no memoising at all).
    std::uint64_t                       content_frame_      = 0;
    bool                                initialized_        = false;
    // The one area this engine drives (see createSurface): OWNED, because a 1:1 relation needs one of its two ends
    // to own it. It is the only place the engine reads a target from - the engine owns no window and no handle (see
    // the class contract) - and a host holding the same object cannot outlive it: dropping the host's handle leaves
    // the engine's, and the engine releases it while it is still an object (see ~RenderEngine).
    intrusive_ptr<RenderSurface>        area_               = nullptr;
    /// "A second area was asked for" is one condition, not a per-call event (see createSurface).
    ReportOnce                          area_refused_{};
    /// "The borrowed device is not up" is one condition too (see initialize): re-armed when a device is set, so the
    /// same device answering the same thing does not say it twice, and a different one gets its own say.
    ReportOnce                          device_not_up_{};
    // The target the frame path presents through when this engine has NO area (see initialize): what the device
    // made for the window it brought up itself, or nullptr when it presents nothing. Borrowed - the device owns it.
    raw_ptr<PresentTarget>              own_target_         = nullptr;
    // The episode of "the backend cannot draw the off-screen targets the pipeline asks for" (see frame()):
    // said once, re-armed the moment no enabled pass needs a target. The shared helper, like every other
    // one-condition episode in this class.
    ReportOnce                          unsupported_target_reported_{};

    /// "A pass names a layered depth view" is one condition, not a per-pass event (see addPass): layered targets
    /// are not built yet, so the declaration is refused once rather than once per declaration.
    ReportOnce                          layered_depth_reported_{};
    /// The depth-view conflicts already said, keyed by (target, the image it was refused): a repeated identical
    /// declaration stays silent, while a different second image gets its own sentence (see addPass).
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
    /// This frame as a value, rebuilt by every compile (see buildFrameValue). Reused, so a steady frame keeps its
    /// storage: the rows of the previous frame are dropped, not their capacity.
    FrameDescription frame_value_;
    /// This frame's problems and the previous frame's, swapped after each compile: the episode rule.
    std::vector<PlanProblem>     plan_problems_;
    std::vector<PlanProblem>     previous_problems_;
    /// Whether a compile is running: while it is, the checks' reports are COLLECTED rather than emitted (see
    /// reportPlanProblems). One flag, because every check keeps calling reportEngineProblem() as it always did
    /// - the compile decides what becomes an episode, not each check.
    bool                         collecting_problems_ = false;
    /// The declared inputs the structural check spoke about in THIS compile (see unusableInputReported).
    std::set<std::pair<raw_ptr<const RenderPass>, OutputIdentity>> unusable_inputs_this_frame_;
    /// The passes whose picture shape was reported, and this compile's rebuild of that list (see
    /// collectPictureShapeMismatches). The rebuild IS the episode rule for this check: an entry that is not
    /// carried over re-arms its pass, so the same mismatch after it was fixed is a new report.
    struct ShapeMismatchEpisode {
        raw_ptr<const RenderPass> pass     = nullptr;   ///< the pass whose rectangle and camera disagree
        ReportOnce                reported;             ///< whether that pass' mismatch has been said
    };
    std::vector<ShapeMismatchEpisode> shape_mismatches_this_frame_;
    std::vector<ShapeMismatchEpisode> shape_mismatches_last_frame_;
};

VN_GRAPHICS_NS_END
