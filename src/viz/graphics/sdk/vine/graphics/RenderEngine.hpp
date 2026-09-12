#pragma once
#include "graphics_global.hpp"

#include <cstddef>
#include <cstdint>
#include <map>
#include <set>
#include <vector>

#include <vine/Object.hpp>
#include <vine/RefCounted.hpp>
#include <vine/intrusive_ptr.hpp>
#include <vine/raw_ptr.hpp>

#include "FrameContext.hpp"
#include "RenderDiagnostic.hpp"
#include "ShaderPreset.hpp"

V_GRAPHICS_NS_BEGIN

class Camera;
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
 * a null render target (see hasWindowPass). The engine is
 * platform-independent and delegates actual drawing to a RenderBackend
 * supplied by the caller via setBackend().
 *
 * The engine may be given a host native window (setWindowHandle) so the
 * backend can attach its render surface to it; the host reports surface
 * resizes via resize() and drives camera input through its SceneView. The
 * engine stays platform-independent and owns none of the window objects.
 */
class V_GRAPHICS_API RenderEngine : public Object, public RefCounted<RenderEngine> {
    V_OBJECT_META_DECL;

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
     * Setting the same backend instance again is a no-op. The installed
     * diagnostic sink (setDiagnosticSink) is applied to the new backend too.
     *
     * @param backend Backend used for drawing, or null to clear.
     */
    void setBackend(intrusive_ptr<RenderBackend> backend);

    /** @brief Installs the sink that receives backend diagnostics.
     *
     * The backend reports what it could not serve (rejected geometry, dropped
     * channel, shader fallback, off-screen target it could not build) instead
     * of degrading silently; this is the host's programmatic channel to it
     * (log / overlay / telemetry). The sink is stored by the engine and
     * applied to the current backend immediately and to any backend set
     * later, so a host can install it before setBackend().
     *
     * @param sink Callback invoked for every backend diagnostic, or empty.
     */
    void setDiagnosticSink(DiagnosticSink sink);

    /** @brief Gets the installed diagnostic sink (empty when unset). */
    const DiagnosticSink& diagnosticSink() const { return diagnostic_sink_; }

    /** @brief Gets how many diagnostics the current backend reported.
     *
     * 0 without a backend. Counted by the backend whether or not a sink is
     * installed, so a host can gate on it without listening.
     *
     * @return Number of reported diagnostics (0 when no backend is set).
     */
    std::size_t diagnosticCount() const;

    /** @brief Gets how many diagnostics the ENGINE itself reported.
     *
     * The backend reports what it could not draw; this counts what the ENGINE
     * could not wire — currently a pass that declared an input no pass
     * published this frame, which means the pass draws nothing (the wiring is
     * something the backend cannot see, and it used to be silent). Reported to
     * the same host sink as the backend's diagnostics.
     *
     * @return Number of diagnostics the engine reported.
     */
    [[nodiscard]] std::size_t engineDiagnosticCount() const noexcept;

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

    /** @brief Returns whether a pass currently presents @p camera to the
     * window.
     *
     * A pass presents the camera to the window when it is enabled, carries
     * @p camera and renders to the default framebuffer (null render target).
     * A SceneView uses this (see SceneView::ensureWindowPass) to decide
     * whether it must register its default window pass: a pipeline that only
     * registers helper / HUD passes (which draw through their own cameras)
     * still needs one, while a pipeline that already presents the view's
     * camera keeps full control.
     *
     * @param camera The view camera to test for window presentation.
     * @return true when an enabled pass presents @p camera to the window.
     */
    bool hasWindowPass(raw_ptr<Camera> camera) const;

    /** @brief Sets the shading-model preset for scene geometry.
     *
     * Forwarded to the backend before initialize(). Presets without a backend
     * implementation (Pbr / ShadowedPhong) fall back to StandardPhong until
     * their slice lands. The default is StandardPhong.
     *
     * @param preset Shading-model preset.
     */
    void setShaderPreset(ShaderPreset preset);

    /** @brief Gets the shading-model preset. */
    ShaderPreset shaderPreset() const;

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

    /** @brief Removes a previously added pass.
     *
     * The pass is dropped from the ordered list. Its backend resources are
     * released: the retained per-pass GPU state (RenderBackend::releasePass,
     * keyed by the pass itself), the legacy window layer keyed by the pass's
     * camera (RenderBackend::releaseWindowLayer), plus any off-screen render
     * target the pass owns (RenderBackend::releaseRenderTarget).
     *
     * @param pass Pass to remove (by pointer).
     */
    void removePass(raw_ptr<RenderPass> pass);

    /** @brief Removes all registered passes.
     *
     * Every registered pass is removed and its backend resources released
     * (the retained per-pass GPU state, the legacy camera-keyed window layer,
     * plus any off-screen render target the pass owns).
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
    /** @brief Executes a scene pass against the given content scene. */
    void drawScenePass(raw_ptr<RenderPass> pass, raw_ptr<Scene> content);

    /** @brief Resolves a pass's declared inputs from the named-output registry.
     *
     * Called just before executing a pass: for every name in
     * pass->inputNames() the matching published target is handed to
     * pass->resolveInputTextures() (missing names resolve to nullptr).
     *
     * @param pass Pass whose inputs to resolve.
     */
    void resolvePassInputs(raw_ptr<RenderPass> pass);

    /** @brief Reports the structural wiring problems the pass declarations themselves carry.
     *
     * Every pass on its own is valid, so nothing else can see these, and they are all properties of
     * the DECLARATION (design §14.4):
     *   * an image (or an output name) TWO passes declare as their output;
     *   * a declared input image nobody produces, or whose producer is registered after its
     *     consumer (the structural half of "the pass draws nothing");
     *   * a ScreenPass that declares no input at all and therefore can never draw.
     *
     * Reported once per episode, from the declarations rather than from a frame — the backend only
     * ever sees "a pass with nothing to draw".
     */
    void validateWiring();

    /** @brief Publishes a pass's output target under its output name.
     *
     * Called just after executing a pass. When the pass declares a non-empty
     * output name (RenderPass::setOutputName) and renders into a non-null
     * RenderTarget, that target is registered for THIS frame's consumers — a pass publication is
     * per frame, unlike a host binding (publish).
     *
     * @param pass Pass whose output to publish.
     */
    void publishPassOutput(raw_ptr<RenderPass> pass);

    /** @brief Publishes a pass's output for this frame (the per-frame half of the registry).
     *
     * @param name   Slot name the pass declares.
     * @param target Target the pass rendered into.
     */
    void publishFrameOutput(const String& name, intrusive_ptr<RenderTarget> target);

    /** @brief Reports one problem the engine found in the pass wiring.
     *
     * The engine owns the host's sink, so the message reaches the host whether
     * or not a backend is set (a wiring problem exists before any backend
     * draws). Counted in engineDiagnosticCount().
     *
     * @param severity How bad the situation is.
     * @param category What it is about.
     * @param message  Human-readable detail, naming the pass and the slot.
     */
    void reportEngineProblem(vine::graphics::DiagnosticSeverity severity,
                             vine::graphics::DiagnosticCategory category,
                             const String&                message);

    /** @brief One registered draw slot in the engine's ordered pass list.
     *
     * A slot draws its bound @p content each frame; content may be null (a
     * base scene pass then draws nothing). Disabled passes
     * (RenderPass::setEnabled) are skipped. Top / HUD passes are ordinary
     * slots whose @p order sits above the main view.
     */
    struct Slot {
        intrusive_ptr<RenderPass> pass;
        intrusive_ptr<Scene>      content;   // may be null (draws nothing)
        int                       order = 0;
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
    // Stored by the engine (not only forwarded) so a backend set later still
    // receives the host's diagnostics.
    DiagnosticSink                      diagnostic_sink_;
    ShaderPreset                        shader_preset_{ ShaderPreset::StandardPhong };
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

    /// This frame's pass publications: slot name -> published target. Cleared at
    /// the start of every frame and rebuilt as the ordered passes publish (a pass
    /// that stops running stops publishing).
    std::map<String, intrusive_ptr<RenderTarget>> outputs_;
    /// Standing host bindings (RenderEngine::publish): they have no producer to re-publish them each
    /// frame, so they survive the per-frame clear until unpublish() removes them. Seeded into the
    /// frame's produced targets, so an object-typed input addressing one is answered as well.
    std::map<String, intrusive_ptr<RenderTarget>> host_outputs_;

    /// Output names TWO passes published with DIFFERENT targets this frame (a collision: every
    /// consumer resolving the name silently gets whichever pass ran last). Rebuilt per frame and
    /// copied into the reported set below, which is what makes the message an episode instead of
    /// a per-frame stream.
    std::set<String> duplicate_outputs_seen_this_frame_;
    /// Output names whose collision has already been reported: pruned to the names that are still
    /// colliding, so a name that breaks again after a clean frame is reported again.
    std::set<String> duplicate_outputs_reported_;

    /// Passes that asked to publish an output name while having NO render target (they render into
    /// the window): the registry maps a name to a sampleable target, so there is nothing to publish.
    /// Rebuilt per frame and pruned into the reported set below, like the collision above — and for
    /// the same reason: dropping the declaration silently leaves a consumer of that name reporting
    /// "nothing produced it", which accuses the consumer of the producer's mistake.
    std::set<raw_ptr<const RenderPass>> unpublishable_passes_seen_this_frame_;
    /// Those of them already reported (pruned to the passes still declaring it).
    std::set<raw_ptr<const RenderPass>> unpublishable_passes_reported_;
    /// Host bindings currently refused because publish() was given no target. A host has no frame
    /// to re-publish from, so the episode is the name itself: it leaves the set when a publish() for
    /// it hands over a real target, or when unpublish() withdraws the name.
    std::set<String> unpublishable_host_names_;

    /// Count of diagnostics this engine reported itself (see
    /// engineDiagnosticCount).
    std::size_t engine_diagnostic_count_ = 0;
    /// Passes already reported for an unresolved declared input, so a producer
    /// that stays absent does not produce one message per frame. A pass whose
    /// input resolves again is dropped from the set, so a later breakage is
    /// reported again (pruned with the pass list).
    std::set<raw_ptr<const RenderPass>> unresolved_inputs_reported_;
    /// Images two passes declared as their output (a structural collision), keyed by ADDRESS: pruned
    /// to the images still colliding each frame, so a collision that goes away is reported again if
    /// it comes back (see validateWiring).
    std::set<OutputIdentity> output_collisions_reported_;
    /// Promises about a target the pass does not write, keyed by (pass, declared image): a promise is
    /// a claim about the content a consumer gets, so it has to be about what the pass draws into.
    /// Pruned the same way (see validateWiring).
    std::set<std::pair<raw_ptr<const RenderPass>, OutputIdentity>> mismatched_promises_reported_;
    /// Declared input images a pass cannot draw from, keyed by (consumer, image): the image has no
    /// producer at all, or its producer is registered after the consumer. Pruned the same way, so a
    /// wire that is fixed and breaks again is reported again (see validateWiring).
    std::set<std::pair<raw_ptr<const RenderPass>, OutputIdentity>> unusable_inputs_reported_;

    /// Targets a pass actually drew into THIS frame: what a declared input is answered from (a
    /// promise says whose content a consumer gets, a filled target says what is there).
    std::set<raw_ptr<const RenderTarget>> produced_targets_;
    /// Declared inputs nothing produced this frame, collected while the passes run so the report is
    /// an episode: seen this frame / already reported (the same prune-and-re-arm rule as the rest).
    std::set<std::pair<raw_ptr<const RenderPass>, OutputIdentity>> unproduced_inputs_seen_this_frame_;
    std::set<std::pair<raw_ptr<const RenderPass>, OutputIdentity>> unproduced_inputs_reported_;
    /// ScreenPasses that declare no input at all and therefore can never draw (pruned the same
    /// way; see validateWiring).
    std::set<raw_ptr<const RenderPass>> missing_inputs_reported_;

    /// ScreenPasses (without a program) whose declared input images are ALL depth images: the
    /// texture path copies one COLOUR attachment, so such a declaration cannot drive it — the pass
    /// would sample the attachment it was left with (sourceAttachment(), 0 by default) while the
    /// host declared the depth. Pruned the same way as the reports above, so a pass that declares a
    /// colour image (or gains a program) is re-armed.
    std::set<raw_ptr<const RenderPass>> unsampleable_screen_inputs_reported_;

    /// ScreenPasses that carry a fullscreen program but no camera: the program path builds the
    /// pass' view from the camera, so such a pass draws nothing at all. Pruned the same way, so a
    /// pass that is given a camera is re-armed.
    std::set<raw_ptr<const RenderPass>> program_without_camera_reported_;

    /// Declared images (an output promise or a declared input) that nobody BOUND to a target: an
    /// unbound identity has no address, so nothing can be delivered through it, and the promise
    /// check has nothing to compare with the rendered target either. Keyed by the IMAGE, because a
    /// producer and its consumers share the same unbound object; pruned every frame (the set is
    /// rebuilt from what this frame saw), so binding it re-arms the report.
    std::set<raw_ptr<const ImageRef>> unbound_declared_images_reported_;
};

V_GRAPHICS_NS_END
