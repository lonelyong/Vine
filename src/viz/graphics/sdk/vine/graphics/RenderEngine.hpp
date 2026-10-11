#pragma once
#include "graphics_global.hpp"

#include <cstddef>
#include <cstdint>
#include <map>
#include <optional>
#include <set>
#include <vector>

#include <vine/Object.hpp>
#include <vine/RefCounted.hpp>
#include <vine/intrusive_ptr.hpp>
#include <vine/raw_ptr.hpp>

#include "Diagnostics.hpp"
#include "FrameContext.hpp"
#include "FramePlan.hpp"
#include "FrameStats.hpp"
#include "PassDesc.hpp"
#include "RenderDiagnostic.hpp"
#include "BuiltinShaders.hpp"

VN_GRAPHICS_NS_BEGIN

class Light;
class Scene;
class RenderPass;
class ImageRef;
class RenderTarget;
class RenderBackend;

/**
 * @brief High-level render engine managing the frame loop and render state.
 *
 * RenderEngine does not impose a fixed pipeline: nothing is rendered unless
 * the caller registers render passes with addPass(). Each frame, frame() runs
 * the registered passes in ascending order, then ends and swaps buffers. A
 * top / HUD pass (an axis gizmo, a minimap, a crosshair) is just a pass
 * registered with a higher order than the main view; there is no separate
 * overlay concept. The engine is a pure scheduler: it carries no content
 * scene and no camera state - every registered pass draws the content bound
 * to it explicitly (addPass(pass, content, order)), or nothing when it has
 * none. It owns no camera and forwards no mouse / scroll / key input. The
 * primary interactive view (its camera, content scene and navigation) lives
 * in a SceneView that borrows this engine; the pass presenting that view's
 * camera
 * to the window is simply a registered pass carrying the view's camera with
 * a null render target. Which pass draws into which SURFACE is a fact the
 * surface owns (see RenderSurface), not something the engine infers. The
 * engine is
 * platform-independent and delegates actual drawing to a RenderBackend
 * supplied by the caller via setBackend().
 *
 * The engine may be given a host native window (setWindowHandle) so the
 * backend can attach its render surface to it; the host reports surface
 * resizes via resize() and drives camera input through its SceneView. The
 * engine stays platform-independent and owns none of the window objects.
 */
class VN_GRAPHICS_API RenderEngine : public Object, public RefCounted<RenderEngine> {
    VN_OBJECT_META_DECL;

  public:
    /** @brief Constructs an empty engine with no backend attached yet.
     *
     * The engine starts with no registered pass; the caller configures the
     * pipeline explicitly (see addPass()) or through a RenderPipelineBuilder.
     * Call setBackend() before initialize().
     */
    RenderEngine();
    ~RenderEngine();

  public:
    /** @brief Gets the bound render backend, or nullptr when unset. */
    raw_ptr<RenderBackend> backend() const;

    /** @brief Sets the render backend used for drawing.
     *
     * The engine keeps a reference to the backend for as long as it is set,
     * so the backend stays alive at least until the engine is destroyed or
     * a different backend (or nullptr) is set. Call before initialize().
     * Setting the same backend instance again is a no-op. The new backend is
     * handed this engine's ONE diagnostic route (see Diagnostics.hpp), so a
     * host's sink and its single diagnosticCount() cover the backend too.
     *
     * @param backend Backend used for drawing, or null to clear.
     */
    void setBackend(intrusive_ptr<RenderBackend> backend);

    /** @brief Installs the sink that receives diagnostics.
     *
     * The backend reports what it could not serve (rejected geometry, dropped
     * channel, shader fallback, off-screen target it could not build) and the
     * engine reports what it could not wire, both instead of degrading
     * silently; this is the host's programmatic channel to them (log / overlay
     * / telemetry). The sink lives in the engine's ONE route (see
     * Diagnostics.hpp) and the engine hands that route to the backend it
     * drives, so installing it covers both - and a sink installed before
     * setBackend() is in place for the backend the moment it is set.
     *
     * @param sink Callback invoked for every diagnostic, or empty.
     */
    void setDiagnosticSink(DiagnosticSink sink);

    /** @brief Gets the installed diagnostic sink (empty when unset). */
    const DiagnosticSink& diagnosticSink() const { return diagnostics_.sink(); }

    /** @brief Gets how many diagnostics were reported through this engine's route.
     *
     * ONE NUMBER, because there is one route: what the engine itself could not wire (a pass that
     * declared an input nothing published this frame, and the like) and what the backend could not serve
     * are counted in the same place. A host that used to add two counts reads this one.
     *
     * Counted whether or not a sink is installed, so a host can gate on "did anything unexpected
     * happen" without listening.
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
     * The gate a phase asserts on: a steady run that is expected to be uneventful asks this instead of
     * reading the log.
     *
     * @return true when no diagnostic was reported.
     */
    [[nodiscard]] bool diagnosticsClean() const noexcept;

    /** @brief Reports one problem the engine, or a helper acting for it, found.
     *
     * The engine owns the route, so the message reaches the host whether or
     * not a backend is set (a wiring problem exists before any backend draws).
     * Counted in diagnosticCount(). Public because the engine's own builders
     * report through it too — RenderPipelineBuilder tells the host that the content
     * it was given asks for shadows (Light::castShadow) while no shadow pass is
     * built yet, so the picture is unshadowed.
     *
     * @param severity How bad the situation is.
     * @param category What it is about.
     * @param message  Human-readable detail, naming the pass and the slot.
     */
    void reportEngineProblem(vn::graphics::DiagnosticSeverity severity,
                             vn::graphics::DiagnosticCategory category,
                             const String&                message);

    /** @brief Initializes the backend.
     *
     * @return true when the backend initialized successfully.
     */
    bool initialize();

    /** @brief Releases backend resources. */
    void shutdown();

    /** @brief Renders one frame (begin, ordered passes, end, swap).
     *
     * Nothing is drawn when no pass is registered. Each registered pass is
     * skipped when disabled (RenderPass::setEnabled); otherwise it resolves
     * its declared inputs, executes (drawing its bound content when set,
     * nothing otherwise), and publishes its named output.
     *
     * @param dt Seconds elapsed since the previous frame (recorded in
     *           frameContext(); reserved for future per-pass updates).
     */
    void frame(double dt = 0.0);

    /** @brief Gets this frame's shared context (elapsed time, surface size).
     *
     * Updated by frame() and the surface size reported via resize(); it is
     * the per-frame data shared across the pass pipeline (to be extended
     * later with previous-frame view-projection matrices and active lights).
     *
     * @return The current frame context.
     */
    const FrameContext& frameContext() const;

    /** @brief Gets the counters the backend kept for the frame that just ended.
     *
     * The backend is the only place that knows how the frame went, so a host (a HUD, a tool, a test)
     * asks through the engine rather than reaching for the backend object (see
     * RenderBackend::frameCounters for what the numbers are).
     *
     * @param counters Receives the counters when the backend keeps them.
     * @return true when @p counters was written; false when no backend is set or it keeps none.
     */
    bool frameCounters(FrameCounters& counters) const;

    /** @brief Gets the backend's cumulative retention numbers (objects released, device idles).
     *
     * @param stats Receives the numbers when the backend keeps them.
     * @return true when @p stats was written; false when no backend is set or it keeps none.
     */
    bool retentionStats(RetentionStats& stats) const;

    /** @brief Gets where the backend's frame clock is (submitted vs provably complete).
     *
     * The question a host asks before it decides to skip work or to read a frame back: how far behind
     * the GPU is. The backend owns the proof of completion, so the engine only forwards (see
     * RenderBackend::frameProgress).
     *
     * @param progress Receives the watermarks when the backend keeps them.
     * @return true when @p progress was written; false when no backend is set or it keeps no clock.
     */
    bool frameProgress(FrameProgress& progress) const;

    /** @brief Gets the order a registered pass runs at in this engine's frame.
     *
     * The order lives in the ENGINE (addPass places the pass in its slot list), not on the pass, so this is the
     * one way to ask where a pass sits in the frame - and an answer doubles as the liveness query a caller that
     * remembers a pass needs: a pass that was removed (its pipeline was dropped, the host called removePass)
     * answers nothing, so an answer IS the pass being in the frame.
     *
     * The caller this exists for is RenderSurface, whose presentation table is ordered by this schedule.
     *
     * @param pass Pass to look up (by pointer; null, like an unregistered pass, answers nothing).
     * @return The pass' order, or nullopt when no such pass is registered (or @p pass is null).
     */
    std::optional<int> passOrder(raw_ptr<RenderPass> pass) const;

    /** @brief Gets what a frame of this engine consists of: the passes it will run, in the order it runs them.
     *
     * The plan is COMPILED from the registered passes (see compileFrame): every frame compiles its own, so the
     * answer here is the last frame's plan - the table that frame actually ran from - or, before any frame has
     * run, the plan the current declarations would compile to. Both are the same compile, so a report written
     * against a pipeline cannot disagree with what the engine draws.
     *
     * What it is for: a test that wants to assert the SHAPE of a frame instead of counting passes or comparing
     * pointers (see `test_graphics`, which does both), a dev switch that prints one table, and a host that wants
     * to know what a pipeline it was handed actually registered.
     *
     * @return The plan, in execution order; empty when nothing is registered.
     */
    [[nodiscard]] FramePlan framePlan() const;

    /** @brief Gets how many transient targets the current plan hands out (see PassDesc::writes_image).
     *
     * The witness of the compile owning transient storage: the number is one per DISTINCT transient a frame
     * declares (the same identity and shape is ONE target, shared), it does not grow with the frame count, and
     * it replaces rather than adds when a declared shape changes.
     *
     * @return Number of live transient targets.
     */
    [[nodiscard]] std::size_t transientTargetCount() const noexcept;

    /** @brief Sets the DEFAULT program for content: what a drawable naming none of its own gets.
     *
     * A default, not an override: a drawable's own program (RenderCommand::program,
     * Geometry::setProgram, StateNode::setProgram) still wins. This is the answer for everything
     * else, and the name says so — "setContentProgram" would read as "shade all content with this".
     *
     * Forwarded to the backend (before initialize() it is the session's decision; on a running
     * session the backend re-bakes the shading side, see RenderBackend::setDefaultContentProgram).
     *
     * The engine's default is forwardProgram(): a NAMED program, chosen once in the constructor, not
     * a lookup that hides which shading a scene gets. Hand it flatForwardProgram() to shade such
     * content flat, or null to decline it outright — content without its own program is then reported
     * and not drawn, never shaded with something the host did not name.
     *
     * @param program Program to use, or null for none.
     */
    void setDefaultContentProgram(intrusive_ptr<const ShaderProgram> program);

    /** @brief Gets the default content program (null when content without its own program is declined). */
    intrusive_ptr<const ShaderProgram> defaultContentProgram() const;

    /** @brief Registers a scene render pass executed every frame.
     *
     * Passes run in ascending @p order each frame (equal orders keep
     * insertion order, stable):
     *
     *   - negative orders run first (e.g. a shadow-map pass driven by a light
     *     camera, or a depth / g-buffer pre-pass);
     *   - the pass presenting the primary view to the window (the view's
     *     camera, null render target) conventionally sits at order 0;
     *   - positive orders run after it (e.g. post-processing / compositing);
     *     top / HUD passes register with the highest orders so they draw
     *     last, over every earlier pass.
     *
     * This overload registers @p pass without bound content: a base scene
     * pass then draws nothing, while content-agnostic or self-contained
     * passes (a ScreenPass compositing its inputs, an AxisGizmo HUD) still
     * execute with their own content. Bind a scene with the addPass(pass,
     * content, order) overload. Nothing is auto-registered: without at least
     * one pass the engine draws nothing. Registering the same pass instance
     * twice is ignored.
     *
     * Two DIFFERENT passes may share a camera and an order: the backend keys
     * its retained state by the pass, so they stay separate content (the order
     * only decides their stacking inside the target). Give them distinct
     * orders (and a sub-viewport / clear policy) when they must layer.
     *
     * @param pass  Pass to add (the engine keeps a reference).
     * @param order Execution order (ascending; any integer allowed).
     */
    void addPass(intrusive_ptr<RenderPass> pass, int order);

    /** @brief Registers a scene render pass bound to explicit content.
     *
     * The pass renders @p content each frame. The content association is
     * stored by the engine, not on the pass object, so a RenderPass stays a
     * reusable stage. Registering the same pass instance twice is ignored
     * (use bindPassContent() to change its content).
     *
     * @param pass    Pass to add (the engine keeps a reference).
     * @param content Scene the pass renders (the engine keeps a reference);
     *                null leaves the pass without content (a base scene pass
     *                then draws nothing).
     * @param order   Execution order (ascending; any integer allowed).
     */
    void addPass(intrusive_ptr<RenderPass> pass, intrusive_ptr<Scene> content, int order);

    /** @brief Registers a pass declared as a VALUE (see PassDesc).
     *
     * The declaration is turned into the execution node of its kind ONCE (`PassKind::Scene` -> RenderPass,
     * `PassKind::Screen` -> ScreenPass) and registered like any other pass, so a descriptor-declared pass and a
     * hand-built one are the same thing to the frame by the time the frame runs: nothing downstream knows which
     * way it arrived, and no second mechanism has to be kept in step with this one.
     *
     * WHAT THE DESCRIPTOR ADDS OVER THE SETTERS. The declaration carries what a pass could not say before:
     * the STAGE it belongs to (so its order follows from intent, see pipelineStageOrder), a TAG for the plan,
     * the TRANSIENT it needs (so no host has to own the target - see PassDesc::writes_image) and the per-frame
     * hook its content updates itself through. A caller therefore adds a pass without adding a type.
     *
     * @param desc The declaration (see PassDesc; a null camera draws nothing, as any pass does).
     */
    void addPass(const PassDesc& desc);

    /** @brief Removes a previously added pass.
     *
     * The pass is dropped from the ordered list. Its backend resources are
     * released: the retained per-pass GPU state (RenderBackend::releasePass,
     * keyed by the pass itself), plus any off-screen render target the pass
     * owns (RenderBackend::releaseRenderTarget).
     *
     * @param pass Pass to remove (by pointer).
     */
    void removePass(raw_ptr<RenderPass> pass);

    /** @brief Removes all registered passes.
     *
     * Every registered pass is removed and its backend resources released
     * (the retained per-pass GPU state, plus any off-screen render target the
     * pass owns).
     */
    void clearPasses();

    /** @brief Gets the number of registered scene passes.
     *
     * @return Number of passes added via addPass().
     */
    std::size_t passCount() const;

    /** @brief Rebinds which scene a registered pass renders.
     *
     * The binding is managed by the engine, not stored on the pass object.
     *
     * @param pass    Pass registered via addPass() (by pointer).
     * @param content New content scene the pass renders; null clears the
     *                binding (a base scene pass then draws nothing).
     */
    void bindPassContent(raw_ptr<RenderPass> pass, intrusive_ptr<Scene> content);

    /** @brief Gets the content scene a registered pass renders.
     *
     * @param pass Pass registered via addPass() (by pointer).
     * @return The pass's bound content scene, or nullptr when the pass has no
     *         bound content or is not registered.
     */
    raw_ptr<Scene> contentOf(raw_ptr<RenderPass> pass) const;

    /** @brief Binds a render target to a named output slot (a standing host binding).
     *
     * The engine keeps a reference so the target stays alive while bound, and the binding KEEPS until
     * unpublish() removes it (or another publish() replaces it) — unlike a pass' output, which is
     * published once per frame and disappears when the pass stops running. A host has no per-frame
     * hook to re-publish from, so its binding must not be tied to a frame.
     *
     * Consumers declared by name resolve it (and an object-typed input addressing the same target is
     * available too: a host binding counts as produced every frame). Publishing twice under one name
     * replaces the binding — that is a host swapping what it offers, not a collision.
     *
     * A name can only be served by a target, so a null @p target is a request the engine cannot
     * honour: it is reported (once per name, until a real target is published or unpublish()
     * withdraws the name) instead of being ignored, which would leave the host believing the name is
     * served.
     *
     * @param name   Slot name consumers resolve against.
     * @param target Target to bind (the engine keeps a reference; null is reported).
     */
    void publish(const String& name, intrusive_ptr<RenderTarget> target);

    /** @brief Looks up a published render target by slot name.
     *
     * A pass' publication from THIS frame wins over a host binding of the same name (the pass ran and
     * its content is this frame's); otherwise a standing host binding answers.
     *
     * @param name Slot name to look up.
     * @return The published target, or nullptr when nothing is published
     *         under @p name.
     */
    raw_ptr<RenderTarget> resolve(const String& name) const;

    /** @brief Removes an output slot (a host binding and this frame's publication alike).
     *
     * @param name Slot name to remove (no-op when not published).
     */
    void unpublish(const String& name);

    /** @brief Resizes the rendering surface.
     *
     * Records the surface size in the shared frame context and rebuilds the
     * backend swapchain (only when the backend is initialized). The engine
     * never manages camera / target / pass-viewport layout: those are
     * maintained by their creators on the new surface size (e.g.
     * SceneView::addSurfaceLayout and SceneView::onSurfaceResized for the
     * view camera).
     *
     * @param width  New surface width in pixels.
     * @param height New surface height in pixels.
     */
    void resize(int width, int height);

    /** @brief Supplies the native window the backend attaches to.
     *
     * Stored until initialize(); the backend reads the handle from it to
     * attach its render surface (e.g. a Qt QWindow). The handle value is
     * captured at call time, so the host must call it again before a
     * re-initialize whenever the native window was recreated. Pass nullptr
     * to clear.
     *
     * @param native_handle Native window handle (HWND on Windows), or nullptr.
     */
    void setWindowHandle(void* native_handle);

  private:

    /** @brief One registered draw slot in the engine's ordered pass list.
     *
     * A slot draws its bound @p content each frame; content may be null (a
     * base scene pass then draws nothing). Disabled passes
     * (RenderPass::setEnabled) are skipped. Top / HUD passes are ordinary
     * slots whose @p order sits above the main view.
     *
     * The last four fields are what a PassDesc declared and a hand-built pass cannot say: where the pass
     * belongs in a human-reading table (@p tag), the transient it needs (@p writes_image / @p writes_shape) and
     * the content update it runs once per frame (@p before_draw).
     */
    struct Slot {
        intrusive_ptr<RenderPass> pass;
        intrusive_ptr<Scene>      content;   // may be null (draws nothing)
        int                       order = 0;
        /// How long the last frame's record of this pass took, in microseconds (0: it did not run then).
        /// Measured by frame(); reported through the frame plan (see FramePlan::PassPlan::record_micros).
        std::uint64_t record_micros = 0;
        /// A label for the plan and for a human reading it ("" when nothing was declared).
        String tag;
        /// The transient image this pass declares it needs ("" = it writes what it holds).
        String writes_image;
        /// The shape writes_image is built at.
        TransientShape writes_shape;
        /// The transient the compile allocated for this pass (null when it declares none).
        intrusive_ptr<RenderTarget> transient;
        /// The content update this pass runs once per frame, before it draws.
        std::function<void()> before_draw;
    };

    /** @brief WHICH image a declared output is, as the wiring check compares it (design §14.4).
     *
     * The rule is "one image, at most one producer", so the check must catch a second producer of
     * the SAME IMAGE — including when the two passes each declared their own ImageRef for it. The
     * identity is therefore the ADDRESS (target + attachment) once the image is bound, not the
     * declared object.
     *
     * Two passes writing one target with DIFFERENT attachments are NOT a collision: a legitimate
     * pipeline renders several passes into one target (RenderPipelineBuilder's light and
     * transparent passes both write `composite`) and an MRT pass hands out several images of one
     * target at once.
     *
     * An image that is not bound yet has no address, so its identity is the declared object: the
     * host may declare an image before it binds it, and until then "same object" is the whole
     * truth that exists.
     */
    struct OutputIdentity {
        raw_ptr<const RenderTarget> target{ nullptr };      // set once the image is bound
        int                         attachment = 0;          // colour index; kWholeTarget = the whole target
        bool                        depth = false;           // a depth attachment is NOT attachment 0
        raw_ptr<const ImageRef>     declared{ nullptr };    // identity while unbound

        /// @brief The coarse form: a whole target, as a bundle declaration names it.
        static constexpr int kWholeTarget = -1;

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

    /// The transient targets this engine allocated for a frame that declared one (see resolveTransientTarget).
    /// Keyed by the identity the declaration names plus the shape it asked for: two passes declaring the same
    /// transient SHARE one target, and a shape change REPLACES rather than adds.
    struct TransientTarget {
        String                      identity;          ///< the declared identity (PassDesc::writes_image)
        TransientShape              shape;             ///< the shape it was built at
        intrusive_ptr<RenderTarget> target;            ///< the target itself
        bool                        declared_now{false}; ///< whether this compile saw the declaration
    };

    /** @brief One row of the compiled frame: what a pass will do, decided before the frame runs.
     *
     * The row is the frame's OWN answer, not a reason to re-read the declarations: the inputs are resolved
     * once, here, and the frame loop hands them to the pass and to the backend. The vector lives in a REUSED
     * list (one row per slot) and each row's input list is assigned into, so a steady frame allocates nothing.
     */
    struct PlanRow {
        raw_ptr<RenderPass>                pass{ nullptr };
        raw_ptr<Scene>                     content{ nullptr };
        int                                order{ 0 };
        /// The declared inputs, resolved for this frame: one entry per declaration, in declaration order,
        /// null where nothing produced it (exactly what the pass and the backend are handed).
        std::vector<raw_ptr<RenderTarget>> inputs;
    };

    /** @brief One problem a compile found, as the sentence a report would carry.
     *
     * The problem carries its own sentence because that IS the problem's identity as far as a host is concerned:
     * the episode rule compares sentences (see reportPlanProblems), and nothing has to be kept in step with the
     * wording. The cost is paid only by a frame that has a problem - a healthy frame builds none, which is the
     * case the frame loop must not pay for - and the message names its subject ("pass 'lighting' ..."), so two
     * frames saying the same sentence really are the same problem.
     */
    struct PlanProblem {
        String message;   ///< the sentence a report would carry
    };

    /**
     * @brief What the compile owns besides the plan itself: the host's bindings and the problems the PREVIOUS
     * compile found.
     *
     * The wiring logic used to be declaration-driven and therefore needed ten sets of "seen this frame /
     * already reported" to keep its reports episodes (see the deleted validateWiring). Now that the frame is
     * compiled from the declarations every frame, the episode rule is ONE comparison - the problems of this
     * compile against the problems of the last - and the problems are kept as the SENTENCES they would print:
     * a problem IS what the host would read, so "the same problem again" is "the same sentence again", and the
     * comparison needs no second vocabulary (no kinds, no subjects) that could drift from the message.
     *
     * What is left here is the state that has no frame to be rebuilt from: the host's standing bindings and the
     * last compile's problem list.
     */
    struct WiringState {
        /// Standing host bindings (RenderEngine::publish): they have no producer to re-publish them each
        /// frame, so they survive the per-frame rebuild until unpublish() removes them. They are the only
        /// entries of the name registry the compile does not derive from the passes.
        std::map<String, intrusive_ptr<RenderTarget>> host_outputs_;
        /// Host bindings currently refused because publish() was given no target. A host has no frame to
        /// re-publish from, so the episode is the name itself: it leaves the set when a publish() for it hands
        /// over a real target, or when unpublish() withdraws the name.
        std::set<String> unpublishable_host_names_;
    };
    /** @brief Executes a scene pass against the given content scene.
     *
     * @param pass    Pass to execute.
     * @param content Scene the pass draws.
     */
    void drawScenePass(raw_ptr<RenderPass> pass, raw_ptr<Scene> content);

    /** @brief Compiles this frame's declarations into the table the frame runs from (see FramePlan).
     *
     * THE ONE PLACE A FRAME IS DECIDED. Compiled every frame - no declaration tells the engine it moved,
     * because reading the declarations IS the compile (a steady frame costs the same walk, and nothing has to
     * be kept in step with a revision). Two things come out of it, and both are the frame's own:
     *
     *   * the ORDER and the WIRING: which pass runs when, which target a pass' declared inputs resolve to,
     *     which targets a transient declaration allocates (see PassDesc::writes_image);
     *   * the PROBLEMS this set of declarations has - reported once per episode, by comparing this compile's
     *     problems with the previous one's (see reportPlanProblems), which is the whole of the episode rule.
     */
    void compileFrame();

    /** @brief Collects the problems this frame's declarations carry (see PlanProblem).
     *
     * The checks that used to run when a declaration "moved" (the deleted validateWiring): every pass on its
     * own is valid, so only the whole table can see a promise about a target the pass never writes, two
     * claimants of one image, a declared input no one fills, or a screen pass that can never draw. Run as part
     * of the compile, so the answer is about THIS frame and nothing has to remember that it changed.
     */
    void collectPlanProblems();

    /** @brief Reports the problems the compile found, once per episode, then remembers them.
     *
     * A problem is an episode, not a per-frame stream: what was already found by the previous compile is not
     * said again, and a problem that is gone is forgotten, so the same breakage later is a new episode.
     */
    void reportPlanProblems();

    /** @brief Inserts a slot in the pass list, keeping it ascending by order.
     *
     * The one place the slot list is ordered (equal orders keep insertion order, so ties are resolved by the
     * registration sequence). A pass instance already registered is ignored: it would otherwise run twice.
     *
     * @param slot Slot to insert (moved in when it is not a duplicate).
     */
    void insertSlot(Slot slot);

    /** @brief Builds the target a transient declaration asks for (see resolveTransientTarget).
     *
     * @param identity Name to give the target (diagnostics and the plan read it).
     * @param width    Extent in device pixels.
     * @param height   Extent in device pixels.
     * @param shape    Attachment set the declaration asked for.
     * @return The target, configured but not yet built (the backend builds it when a pass draws into it).
     */
    [[nodiscard]] intrusive_ptr<RenderTarget> makeTransientTarget(const String& identity, int width, int height,
                                                                  const TransientShape& shape);

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
    void resolveRowInputs(raw_ptr<RenderPass> pass, std::vector<raw_ptr<RenderTarget>>& out);

    /** @brief Publishes a pass' output name for the rest of this frame (see compileFrame).
     *
     * @param pass Pass whose output to publish.
     */
    void publishRowOutput(raw_ptr<RenderPass> pass);

    /** @brief Resolves a published name for this frame: a pass' publication, else the host's binding.
     *
     * @param name Slot name to resolve.
     * @return The target the name serves, or null when nothing produced it this frame.
     */
    [[nodiscard]] raw_ptr<RenderTarget> resolveCompiled(const String& name) const;

    /** @brief Releases a transient target nobody declared this frame.
     *
     * @param target Target to let go of (the backend is told, so it can retire what it built for it).
     */
    void releaseTransientTarget(const intrusive_ptr<RenderTarget>& target);

    /** @brief Returns the name a pass is reported under (its own, or a readable fallback).
     *
     * @param pass Pass to name.
     * @return The pass' name, or an unnamed-pass marker when it has none.
     */
    [[nodiscard]] static String reportedPassName(raw_ptr<const RenderPass> pass);

    /** @brief Returns the first enabled pass that needs a target this backend cannot draw into.
     *
     * A backend reporting no off-screen support IGNORES a non-null target (see
     * RenderBackend::supportsRenderTargets), so such a pass draws into the window instead of where the
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
    intrusive_ptr<RenderBackend>        backend_;
    // The engine's ONE diagnostic route (see Diagnostics.hpp): the host's sink lives inside it, and the
    // backend the engine drives reports through this very instance (setDiagnosticsRoute), which is what
    // makes diagnosticCount() the whole truth instead of half of it.
    Diagnostics                         diagnostics_;
    // The shading a drawable that names no program gets. A named program, not an enum: set once in
    // the constructor to forwardProgram(), replaced by setDefaultContentProgram(), and null means "decline".
    intrusive_ptr<const ShaderProgram>  default_content_program_;
    std::vector<Slot>                   slots_;         // uniform ordered draw registry
    FrameContext                        frame_ctx_;
    // Monotonic content-frame token, announced to every rendered scene each
    // frame (Scene::setContentFrame): the passes of one frame that draw the same
    // scene through the same camera then share one tree walk instead of walking
    // it once per pass. 0 = no frame announced yet, which is also what a caller
    // that never renders sees (no memoising at all).
    std::uint64_t                       content_frame_      = 0;
    void*                               native_handle_      = nullptr;
    bool                                initialized_        = false;
    // True once the frame reported that the backend cannot draw the off-screen targets the pipeline
    // asks for (see frame()): the report is an EPISODE, so it repeats only after the pipeline stopped
    // asking for one — the flag is cleared the moment no enabled pass needs a target.
    bool                                unsupported_target_reported_ = false;


    WiringState wiring_;


    /// This frame's compiled rows, in the order they run (reused; one per slot).
    std::vector<PlanRow> plan_rows_;
    /// The targets this frame had produced when the compile walked to a given row, in production order: what
    /// an object-typed declared input is answered from (a consumer sees what ran BEFORE it). Reused, and keyed
    /// by target only - a target is filled once however many passes draw into it.
    std::vector<raw_ptr<const RenderTarget>> plan_produced_;
    /// The publications this frame made so far, in publication order, keyed by the PASS that made them: the
    /// name is read off the pass, so a steady frame copies no strings to answer `resolve(name)`.
    std::vector<std::pair<raw_ptr<const RenderPass>, raw_ptr<RenderTarget>>> plan_published_;


    std::vector<TransientTarget> transients_;
    /// This frame's problems and the previous frame's, swapped after each compile: the episode rule.
    std::vector<PlanProblem>     plan_problems_;
    std::vector<PlanProblem>     previous_problems_;
    /// Whether a compile is running: while it is, the checks' reports are COLLECTED rather than emitted (see
    /// reportPlanProblems). One flag, because every check keeps calling reportEngineProblem() as it always did
    /// - the compile decides what becomes an episode, not each check.
    bool                         collecting_problems_ = false;
    /// The declared inputs the structural check spoke about in THIS compile (see unusableInputReported).
    std::set<std::pair<raw_ptr<const RenderPass>, OutputIdentity>> unusable_inputs_this_frame_;
};

VN_GRAPHICS_NS_END
