#pragma once
#include "graphics_global.hpp"

#include <vine/math/Matrix4x4.hpp>
#include <vine/math/Point3.hpp>
#include <vine/math/Rect3.hpp>
#include <vine/raw_ptr.hpp>
#include <vine/intrusive_ptr.hpp>

#include "RenderPass.hpp"
#include "RenderPipeline.hpp"
#include "RenderTarget.hpp"

VN_GRAPHICS_NS_BEGIN

class Camera;
class Light;
class RenderEngine;
class Scene;
class ScreenPass;

/**
 * @brief Thin, ergonomic recipe layer for assembling a render pipeline.
 *
 * RenderPipelineBuilder produces the exact same objects you would build by
 * hand through the RenderEngine / RenderPass public API, but packages the
 * recurring "recipes" (a main-window forward / deferred pipeline via build(),
 * an off-screen render-to-texture + screen compositing via
 * addOffscreenToScreen(), ...) so applications do not repeat the wiring.
 *
 * It is intentionally thin: it does NOT own the per-frame scheduling,
 * content resolution, lighting or shadow logic — those stay in RenderEngine
 * and in the shaders the recipes bind (the builder only fills in order /
 * content / output / input names and the pass topology). Every add*() /
 * build() call applies immediately to the target engine, and created passes
 * are kept alive both by the builder (or its returned Pipeline) and by the
 * engine.
 *
 * RenderEngine itself auto-registers nothing (its pipeline is fully
 * explicit); this builder is the convenient, reusable way to assemble common
 * configurations, and the SceneView default viewer is assembled through the
 * same Forward preset so the whole codebase shares one main-pipeline recipe.
 */
class VN_GRAPHICS_API RenderPipelineBuilder {
  public:
    /** @brief Constructs a builder targeting an engine.
     *
     * @param engine Engine the assembled passes are applied to (borrowed;
     *               must outlive the builder).
     */
    explicit RenderPipelineBuilder(raw_ptr<RenderEngine> engine);

    /** @brief Binds the content scene for produced scene passes.
     *
     * @param content Scene used by the produced passes, or null to fall back
     *                to the engine's default content scene.
     */
    RenderPipelineBuilder& setContent(intrusive_ptr<Scene> content);

    /** @brief Binds an optional forward-only (transparent / overlay) scene.
     *
     * When set, the produced main-window preset also renders this scene as
     * transparent content composited over the lit opaque content WITH depth
     * occlusion (alpha-blended geometry half behind the opaque scene is
     * clipped correctly) instead of being drawn depth-less on top:
     *
     *  - Forward: a depth-on pass stacked right after the main content pass in
     *    the same window render pass.
     *  - Deferred: the lighting pass shades into an off-screen composite
     *    target together with the opaque depth; this scene is drawn depth-on
     *    over it and the composite is presented to the window.
     *
     * The scene is lit by its own lights (like any forward pass). Without a
     * transparent scene the presets behave exactly as before.
     *
     * @param transparent Transparent / overlay scene, or null for none.
     */
    RenderPipelineBuilder& setTransparentContent(intrusive_ptr<Scene> transparent);

    /** @brief Binds the camera used by the produced scene passes.
     *
     * Recipes that produce scene passes need a camera; it is provided
     * explicitly by the caller (typically a SceneView's camera).
     *
     * @param camera Camera (borrowed), or null to leave unset (scene-pass
     *               recipes then refuse to build).
     */
    RenderPipelineBuilder& setCamera(raw_ptr<Camera> camera);

    /** @brief Assembles a pipeline from @p options.
     *
     * Registers the passes on the target engine immediately and returns a
     * Pipeline handle that owns them: dropping the handle unregisters them
     * (see Pipeline). Passes are placed by PipelineStage rather than by hand-
     * picked order numbers, so an effect added later enters relative to the
     * passes it serves instead of competing for a number with them.
     *
     * The forward path builds one window scene pass drawing this builder's
     * content through its camera (PipelineStage::Shading). The deferred path
     * additionally builds the canonical G-buffer (offscreen MRT, published as
     * "GBuffer") at PipelineStage::Geometry and uses a fullscreen lighting
     * ScreenPass at PipelineStage::Shading as the window pass, so the view
     * camera is presented to the window and RenderControl / SceneView do not
     * add a second forward pass.
     *
     * A transparent scene (see setTransparentContent) is composited over the
     * lit opaque content at PipelineStage::Transparent - off-screen through a
     * target that shares the G-buffer's depth, so forward-only content is
     * occluded by the opaque depth instead of being drawn depth-less on top.
     * The HUD overlays stack at PipelineStage::Overlay.
     *
     * SHADOWS ARE NOT A PRESET HERE: a shadow is requested by the light that
     * casts it (Light::castShadow, with its own resolution and bias in
     * ShadowSettings), and BOTH paths honour that request through the one pass
     * builder (buildShadowPass): a depth-only pass at PipelineStage::Depth
     * framing the content with a single orthographic light camera
     * (directionalShadowMatrix), whose view-projection is stated on the map, and
     * which the pass that shades with it declares as an INPUT so the backend
     * binds it (see .ai/design/render-pipeline.md §9). Deferred declares it on
     * the fullscreen lighting pass, forward on the content pass itself.
     *
     * What a pipeline still cannot do is shade with a map it was given no
     * program for: a host that supplied its own deferred lighting program gets no
     * shadow pass (the shading is theirs), and THAT is reported once per build
     * (DiagnosticCategory::UnsupportedRequest) rather than silently handing back
     * a picture without the shadow that was asked for. The same complaint is made
     * one layer down by the backend, for a content PROGRAM that declares no
     * shadow_map while its pass declared a shadow.
     *
     * Deferred requires a content scene and a camera; when either is missing
     * nothing is registered and null is returned (no silent substitution; the backend
     * applies the same rule, see "服务与拒绝" in `src/plugins/gfx_backend_vsg/docs/backend.md`).
     *
     * @param options The pipeline's description (see PipelineOptions).
     * @return The built pipeline, or null when the requested path cannot be built.
     */
    intrusive_ptr<Pipeline> build(const PipelineOptions& options);

    /** @brief The light camera a directional shadow is rendered with.
     *
     * Orthographic, looking along @p light's direction, framing @p bounds: the eye is pulled back
     * past the box along the light, the window covers the box's diagonal (a sphere of that radius
     * fits every orientation, so the fit does not have to know which way the light comes from), and
     * the depth range reaches from in front of the box to well behind it.
     *
     * It is PUBLIC because it has to be the only one: the pass that renders the map draws through
     * this camera, the target STATES its view-projection (RenderTarget::setProducerViewProjection),
     * and the shading maps its fragments through that matrix. A second derivation anywhere — in a
     * test, in a host, in a backend — is a second light camera that agrees only until one of them
     * is touched, and a mismatch shows up as a shadow in the wrong place, not as an error.
     *
     * @param light  Directional light the map is rendered along (the light shines ALONG its
     *               direction, so the eye is pulled back against it).
     * @param bounds Content bounds the map must cover (a box with an inverted axis maps a unit
     *               sphere at the origin instead).
     * @param camera Receives the light camera (also usable as the shadow pass' camera).
     * @return The light camera's projection * view matrix.
     */
    static vn::math::Mat4d directionalShadowMatrix(const Light& light, const vn::math::Aabbd& bounds,
                                                    Camera& camera);

    /** @brief Creates the built-in temporary G-buffer geometry program.
     *
     * One scene traversal writing the canonical four G-buffer outputs (albedo
     * / view normal + shininess / specular / view position). This is the
     * default program the Deferred preset uses, exposed as a single shared
     * source so deferred A/B previews can reuse it too. Backend-ABI specific
     * (vsg); it will move into the render backend once the backend ships its
     * own deferred shading.
     *
     * @return A fresh program instance.
     */
    static intrusive_ptr<ShaderProgram> defaultGbufferGeometryProgram();

    /** @brief Creates the built-in temporary deferred-lighting program.
     *
     * A fullscreen fragment program sampling the G-buffer attachments
     * (binding 0..3) with ambient + up to three directional lights from the
     * backend's view-space push block. This is the default program the
     * Deferred preset uses, exposed as a single shared source so deferred
     * A/B previews can reuse it too. Backend-ABI specific (vsg).
     *
     * @return A fresh program instance (fragment stage only).
     */
    static intrusive_ptr<ShaderProgram> defaultDeferredLightProgram();

    /** @brief Creates the canonical G-buffer MRT target of the Deferred
     * presets.
     *
     * Four colour attachments - albedo (RGBA8), view normal + shininess
     * (RGBA16F), specular (RGBA8), view position (RGBA16F) - plus depth
     * (D24). The Deferred presets build their G-buffer through this factory,
     * exposed so deferred A/B previews can share the same canonical layout
     * (their geometry program must match its attachment order).
     *
     * @param width  Target width in pixels (<= 0 uses 640).
     * @param height Target height in pixels (<= 0 uses 360).
     * @return A fresh target with the canonical colour + depth attachments.
     */
    static intrusive_ptr<RenderTarget> defaultGbufferTarget(int width, int height);

    /** @brief Recipe: render content into an off-screen target and composite
     * it back as a picture-in-picture screen pass.
     *
     * Creates:
     *   - an order < 0 scene pass rendering into a @p rt_width x @p rt_height
     *     RenderTarget (publishing its colour as @p output_slot), and
     *   - an order > 0 ScreenPass that samples @p output_slot into the
     *     @p pip sub-viewport.
     *
     * OWNERSHIP: both passes are registered on the engine IMMEDIATELY and the
     * engine keeps them alive — they are NOT part of any Pipeline handle, so a
     * Pipeline built by this builder does not own them and dropping it leaves
     * them running (that RAII covers the passes build() planned; see Pipeline).
     * Only this ScreenPass is handed back: the off-screen pass it samples is
     * not addressable through this API, so a host that wants the recipe gone
     * calls RenderEngine::clearPasses() (removePass(screen) alone would leave
     * the off-screen pass drawing into a target nobody samples). The builder
     * keeps no state of its own: it may be destroyed at once (the demo builds
     * one, wires a PiP and drops it).
     * The returned ScreenPass lets the caller re-anchor the PiP viewport once
     * the surface size is known (see RenderPass::setViewport).
     *
     * @param output_slot Name the off-screen target is published under and
     *                    the screen pass resolves.
     * @param rt_width    Off-screen target width.
     * @param rt_height   Off-screen target height.
     * @param color_format Off-screen colour format.
     * @param depth_format Off-screen depth format.
     * @param pip          PiP sub-viewport on the output surface (device px). Taken as the SDK's own
     *                     rectangle rather than four coordinates in a row: `pip` is applied to a pass
     *                     without a viewport of its own (see RenderPass::setViewport).
     * @return The created ScreenPass (owned by the engine; do not delete).
     */
    raw_ptr<ScreenPass> addOffscreenToScreen(const String& output_slot,
                                             int rt_width, int rt_height,
                                             RenderTarget::ColorFormat color_format,
                                             RenderTarget::DepthFormat depth_format,
                                             const Viewport& pip);

    /** @brief Manual escape hatch: adds an arbitrary pass to the engine.
     *
     * @param pass  Pass to add (the engine keeps a reference).
     * @param order Execution order relative to the main pass.
     */
    void addPass(intrusive_ptr<RenderPass> pass, int order);

  private:
    /** @brief Builds the forward path's window pass into @p pipeline.
     *
     * @param pipeline Pipeline the pass is registered on.
     * @return true when the pass was registered.
     */
    bool buildForwardPath(Pipeline& pipeline);

    /** @brief Builds the deferred path (G-buffer + lighting) into @p pipeline.
     *
     * @param pipeline Pipeline the passes are registered on.
     * @param options  Options the path reads (G-buffer size, programs).
     * @return true when the passes were registered.
     */
    bool buildDeferredPath(Pipeline& pipeline, const PipelineOptions& options);

    /** @brief Builds the depth-only shadow pass a castShadow light asks for.
     *
     * Both paths honour a shadow request through this ONE builder: the pass frames the content with
     * directionalShadowMatrix, states that matrix on the map (RenderTarget::setProducerViewProjection)
     * and draws the content at PipelineStage::Depth, so it runs before anything that samples it. The
     * caller declares the map as an INPUT on the pass that shades with it, which is what makes the
     * backend bind it (RenderEngine::resolvePassInputs -> RenderBackend::setPassInputs).
     *
     * @param pipeline      Pipeline the pass is registered on.
     * @param shadow_light  The light that casts it (enabled, castShadow, directional).
     * @return The map the pass renders into (never null on this path).
     */
    /**
     * @brief Builds the depth-only shadow pass for @p shadow_light_owner and returns its map.
     *
     * The map states whose shadow it is (RenderTarget::setShadowOf) and HOLDS that light: a retained
     * target can outlive the scene that built it, and a consumer reads the light through the map.
     *
     * @param pipeline          Pipeline the pass is added to.
     * @param shadow_light_owner The light that casts the shadow (borrowed ownership is the map's).
     * @return The shadow map target.
     */
    intrusive_ptr<RenderTarget> buildShadowPass(Pipeline& pipeline, const intrusive_ptr<const Light>& shadow_light_owner);

    /** @brief Adds the optional HUD overlays to @p pipeline.
     *
     * @param pipeline Pipeline the overlays are registered on.
     * @param options  Options naming which overlays to add.
     * @return true when every requested overlay was added.
     */
    bool applyOverlays(Pipeline& pipeline, const PipelineOptions& options);

    /** @brief Reports the shadow-casting lights this build did NOT honour.
     *
     * The request lives on the light (Light::castShadow), so the check walks
     * the content scenes this builder binds: a host that asked for a shadow the
     * pipeline did not build gets one report (per build) naming how many, rather
     * than a frame that quietly differs from the one it asked for. A shadow the
     * pipeline DID build is not reported — the count above is what it built.
     */
    void reportRequestedShadows() const;

    raw_ptr<RenderEngine>      engine_;
    raw_ptr<Camera>            camera_      = nullptr;
    intrusive_ptr<Scene>       content_;
    intrusive_ptr<Scene>       transparent_; // optional forward-only / overlay scene
    /// Shadow passes THIS build created: a requested shadow that WAS built is not a gap to report
    /// (see reportRequestedShadows).
    std::size_t shadows_built_ = 0;
};

VN_GRAPHICS_NS_END
