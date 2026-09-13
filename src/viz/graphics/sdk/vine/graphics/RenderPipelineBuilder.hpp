#pragma once
#include "graphics_global.hpp"

#include <vector>

#include <vine/raw_ptr.hpp>
#include <vine/intrusive_ptr.hpp>

#include "RenderPass.hpp"
#include "RenderPipeline.hpp"
#include "RenderTarget.hpp"

V_GRAPHICS_NS_BEGIN

class Camera;
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
class V_GRAPHICS_API RenderPipelineBuilder {
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
     * casts it (Light::castShadow) and this builder builds no shadow pass yet,
     * so a content scene that declares one is REPORTED
     * (DiagnosticCategory::UnsupportedRequest) - the host learns that the
     * picture it gets is unshadowed instead of being left to wonder why nothing
     * changed. Building the passes themselves is the next slice (see
     * .ai/design/graphics-shadow.md §10 and .ai/design/render-pipeline.md §6).
     *
     * Deferred requires a content scene and a camera; when either is missing
     * nothing is registered and null is returned (no silent substitution, see
     * .ai/design/vsg-custom-shader.md §11.7).
     *
     * @param options The pipeline's description (see PipelineOptions).
     * @return The built pipeline, or null when the requested path cannot be built.
     */
    intrusive_ptr<Pipeline> build(const PipelineOptions& options);

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
     *     @p pip_... sub-viewport.
     * Both are added to the engine immediately; the returned ScreenPass lets
     * the caller re-anchor the PiP viewport once the surface size is known
     * (see RenderPass::setViewport). The pass keeps the target alive.
     *
     * @param output_slot Name the off-screen target is published under and
     *                    the screen pass resolves.
     * @param rt_width    Off-screen target width.
     * @param rt_height   Off-screen target height.
     * @param color_format Off-screen colour format.
     * @param depth_format Off-screen depth format.
     * @param pip_x/y/w/h  PiP sub-viewport on the output surface (device px).
     * @return The created ScreenPass (owned by the engine; do not delete).
     */
    raw_ptr<ScreenPass> addOffscreenToScreen(const String& output_slot,
                                             int rt_width, int rt_height,
                                             RenderTarget::ColorFormat color_format,
                                             RenderTarget::DepthFormat depth_format,
                                             int pip_x, int pip_y, int pip_w, int pip_h);

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

    /** @brief Adds the optional HUD overlays to @p pipeline.
     *
     * @param pipeline Pipeline the overlays are registered on.
     * @param options  Options naming which overlays to add.
     * @return true when every requested overlay was added.
     */
    bool applyOverlays(Pipeline& pipeline, const PipelineOptions& options);

    /** @brief Reports shadow-casting lights this builder cannot honour yet.
     *
     * The request lives on the light (Light::castShadow), so the check walks
     * the content scenes this builder binds: a host that asked for shadows gets
     * one report (per build) saying the picture is unshadowed, rather than a
     * frame that quietly differs from the one it asked for.
     */
    void reportRequestedShadows() const;

    raw_ptr<RenderEngine>      engine_;
    raw_ptr<Camera>            camera_      = nullptr;
    intrusive_ptr<Scene>       content_;
    intrusive_ptr<Scene>       transparent_; // optional forward-only / overlay scene
    // References kept by the builder for as long as it lives; the engine also
    // holds its own references after each add*() call. Records the passes the
    // ONE-SHOT recipes register outside a Pipeline handle (see
    // addOffscreenToScreen); a build() pipeline owns its own.
    std::vector<intrusive_ptr<RenderPass>> passes_;
};

V_GRAPHICS_NS_END
