#include "ChainBuilder.hpp"

#include <vine/graphics/AxisGizmo.hpp>
#include <vine/graphics/BuiltinShaders.hpp>
#include <vine/graphics/Camera.hpp>
#include <vine/graphics/ChainPresets.hpp>
#include <vine/graphics/FpsOverlay.hpp>
#include <vine/graphics/Light.hpp>
#include <vine/math/Point3.hpp>
#include <vine/math/Rect3.hpp>
#include <vine/graphics/PassDesc.hpp>
#include <vine/graphics/RenderDiagnostic.hpp>
#include <vine/graphics/RenderEngine.hpp>
#include <vine/graphics/Scene.hpp>
#include <vine/graphics/ScreenPass.hpp>
#include <vine/graphics/ShaderProgram.hpp>

VN_GRAPHICS_NS_BEGIN

namespace detail
{

ChainBuilder::ChainBuilder(raw_ptr<RenderEngine> engine)
  : engine_(engine)
{
}

intrusive_ptr<PassChain> ChainBuilder::build(const ChainDesc& options)
{
    if (engine_ == nullptr) {
        return nullptr;
    }
    // The request is a value; the builder copies what it needs for THIS build once, so the recipes below read a
    // snapshot instead of a host's object that could change half way through.
    camera_      = options.camera;
    content_     = options.content;
    transparent_ = options.transparent;
    shadows_built_  = 0;
    content_layers_ = options.layer_mask;
    // The chain IS its registration: each pass is filed with it as owner, so a build that stops half way (or a host
    // that drops the handle) takes its passes back out of the frame. The handles are leases; the engine may die
    // first and the chain then answers "gone" instead of reaching into freed memory.
    auto pipeline = make_intrusive<PassChain>(engine_->passes(), engine_->diagnostics());
    // The chain the REQUEST assembles is not always the chain it names: a mode that needs the geometry pass
    // decides it (see effectivePath). The chain records what it was built as, because "can this chain
    // express that mode" is what a later setShadingMode() is answered from.
    const ShadingPath path = PassChain::effectivePath(options);
    const bool        ok   = (path == ShadingPath::Forward) ? buildForwardPath(*pipeline)
                                                            : buildDeferredPath(*pipeline, options);
    if (!ok) {
        return nullptr;
    }
    pipeline->path_ = path;
    reportRequestedShadows();
    if (!applyOverlays(*pipeline, options)) {
        return nullptr;
    }
    // The build's mode lands as the same override a live switch applies (one place knows what a mode means),
    // and it always succeeds here: the chain was chosen to be able to express it.
    (void)pipeline->setShadingMode(options.mode);
    return pipeline;
}

bool ChainBuilder::applyOverlays(PassChain& pipeline, const ChainDesc& options)
{
    // HUD overlays, stacked at one stage: what orders them against each other is
    // their own stacking offset inside it (see AxisGizmoOptions::order), not a
    // number that has to stay bigger than the passes below.
    const int overlay_base = passStageOrder(PassStage::Overlay);

    // An axis gizmo, mirroring the source camera. Re-anchored through
    // PassChain::resize.
    if (options.gizmo.source_camera != nullptr) {
        auto gizmo = make_intrusive<AxisGizmo>();
        gizmo->setSourceCamera(options.gizmo.source_camera);
        gizmo->setPixelRatio(options.gizmo.pixel_ratio);
        gizmo->setBoxSize(options.gizmo.box_size);
        gizmo->setAxisLength(options.gizmo.axis_length);
        gizmo->setThickness(options.gizmo.thickness);

        // The gizmo is CONTENT (its scene, its framing camera, its box); the pass that draws it is a
        // declaration like every other one - which is why the name, the HUD placement and the per-frame work
        // are stated here instead of being inherited from a pass subclass (see PassDesc).
        PassDesc desc;
        desc.name          = u8"axis_gizmo";
        desc.tag           = u8"Hud";
        desc.stage         = PassStage::Overlay;
        desc.order         = overlay_base + options.gizmo.order;
        desc.camera        = intrusive_ptr<Camera>(gizmo->camera());
        desc.content       = intrusive_ptr<Scene>(gizmo->content());
        desc.viewport      = gizmo->boxViewport();
        // A HUD draws over the main content each frame: never clears, and no depth occlusion (always on top).
        desc.clear_enabled = false;
        desc.depth_mode    = DepthMode::Disabled;
        // The one thing that must happen before it draws: mirror the source camera's orientation onto the
        // framing one.
        desc.before_draw   = [gizmo] { gizmo->update(); };
        // Registered through the same public entry a host uses (see PassChain::addOverlay): the recipe states the
        // overlay's CONTENT, the label its pass carries, and how it re-anchors itself - a third overlay is another
        // call, not a change to PassChain.
        pipeline.addOverlay(String(PassChain::kGizmoRole), gizmo.get(), desc,
                            [gizmo](RenderPass& hud, int width, int height, double ratio) {
                                gizmo->setPixelRatio(ratio);
                                gizmo->onSurfaceResized(width, height);
                                hud.setViewport(gizmo->boxViewport());
                            });
    }

    // A frame-rate readout (bottom-right corner). It measures the actual
    // render-loop rate and needs no source camera, so it is opt-in through
    // FpsOverlayOptions::enabled.
    if (options.fps.enabled) {
        auto fps = make_intrusive<FpsOverlay>();
        fps->setPixelRatio(options.fps.pixel_ratio);
        fps->setSize(options.fps.width_px, options.fps.height_px);

        PassDesc desc;
        desc.name          = u8"fps_overlay";
        desc.tag           = u8"Hud";
        desc.stage         = PassStage::Overlay;
        desc.order         = overlay_base + options.fps.order;
        desc.camera        = intrusive_ptr<Camera>(fps->camera());
        desc.content       = intrusive_ptr<Scene>(fps->content());
        desc.viewport      = fps->boxViewport();
        desc.clear_enabled = false;
        desc.depth_mode    = DepthMode::Disabled;
        // Measure the frame rate and repaint the digits (see FpsOverlay::update).
        desc.before_draw   = [fps] { fps->update(); };
        pipeline.addOverlay(String(PassChain::kStatsRole), fps.get(), desc,
                            [fps](RenderPass& hud, int width, int height, double ratio) {
                                fps->setPixelRatio(ratio);
                                fps->onSurfaceResized(width, height);
                                hud.setViewport(fps->boxViewport());
                            });
    }
    return true;
}

void ChainBuilder::reportRequestedShadows() const
{
    // A shadow is asked for by the LIGHT (Light::isCastingShadow + ShadowSettings) —
    // that is where the request lives, and it outlives any one pipeline. What
    // this reports is the request the pipeline did NOT honour: a forward path
    // builds no shadow at all yet (S2b), and a deferred path asked to shade with
    // the host's own program builds none either (it said so where it decided).
    // A report that fires whenever a request EXISTS — what this did while the
    // deferred path already built the pass — tells the host its picture is
    // unshadowed while it is shadowed, which is worse than saying nothing.
    std::size_t requesting = 0;
    for (const Scene* scene : { content_.get(), transparent_.get() }) {
        if (scene == nullptr) {
            continue;
        }
        for (const auto& light : scene->lights()) {
            if (light != nullptr && light->isEnabled() && light->isCastingShadow()) {
                ++requesting;
            }
        }
    }
    if (requesting <= shadows_built_) {
        return;
    }
    const std::size_t unbuilt = requesting - shadows_built_;
    // The integer goes in as ASCII digits, like every other message in the SDK.
    const std::string   digits = std::to_string(unbuilt);
    const std::u8string count_text(digits.begin(), digits.end());
    engine_->reportEngineProblem(vn::graphics::DiagnosticSeverity::Warning,
                                 vn::graphics::DiagnosticCategory::UnsupportedRequest,
                                 String(count_text) +
                                     String(u8" shadow-casting light(s) in this pipeline's content have no shadow "
                                            u8"pass (its path builds none, or its lighting program is the host's; "
                                            u8"see .ai/design/graphics-pipeline.md §9): those lights cast nothing"));
}

namespace
{

/**
 * @brief Finds the light a content scene asks a shadow for.
 *
 * Single directional light per content, as the shadow design fixes it (graphics-pipeline.md §9): the
 * FIRST enabled, shadow-casting directional light is the one the pass is built for, and the rest
 * are reported as unbuilt by reportRequestedShadows().
 *
 * @param content Content scene to scan (may be null).
 * @return A strong handle to the light that casts the shadow, or null when the content asks for none:
 *         the shadow map states whose it is (RenderTarget::setShadowOf) and therefore holds it.
 */
intrusive_ptr<const Light> requestedShadowLight(raw_ptr<const Scene> content)
{
    if (content == nullptr) {
        return nullptr;
    }
    // Single directional light per content, as the shadow design fixes it (graphics-pipeline.md §9).
    for (const auto& light : content->lights()) {
        if (light != nullptr && light->isEnabled() && light->isCastingShadow() &&
            light->type() == LightType::Directional) {
            return light;   // a strong handle: the map holds its light (RenderTarget::setShadowOf)
        }
    }
    return nullptr;
}

}  // namespace

intrusive_ptr<RenderTarget> ChainBuilder::buildShadowPass(PassChain& pipeline,
                                                                  const intrusive_ptr<const Light>& shadow_light_owner)
{
    // The map keeps the light it belongs to alive (RenderTarget::setShadowOf): a retained target can
    // outlive the scene that built it, and a consumer reads the light through the map.
    const Light& shadow_light = *shadow_light_owner;
    const int resolution = static_cast<int>(shadow_light.shadowSettings().resolution);
    auto      shadow_map = make_intrusive<RenderTarget>();
    shadow_map->setName(u8"shadow_map");
    shadow_map->setSize(resolution > 0 ? resolution : 1024, resolution > 0 ? resolution : 1024);
    shadow_map->attachDepth(RenderTarget::DepthFormat::D24);
    // The shading samples it, so its depth must end in SHADER_READ_ONLY (a pass of it that
    // PRESERVED depth would revoke that, which is why the shadow pass clears instead).
    shadow_map->setDepthPromotion(true);

    auto light_camera = make_intrusive<Camera>();
    // ONE derivation of the light camera: the pass renders through this camera and the target
    // STATES its view-projection, so the shading reads the same matrix instead of fitting a
    // second ortho box of its own (see .ai/design/graphics-pipeline.md §9).
    // The map states what it IS, not who reads it: a consumer finds the shadow by asking the declared
    // targets whose shadow they are (RenderTarget::setShadowOf), so nothing is inferred from declaration
    // order and RenderPass stays a generic stage.
    shadow_map->setShadowOf(shadow_light_owner);
    shadow_map->setProducerViewProjection(directionalShadowMatrix(shadow_light, content_->boundingBox(), *light_camera));

    PassDesc desc;
    desc.name          = u8"shadow";
    desc.tag           = u8"Shadow";
    desc.stage         = PassStage::Depth;
    desc.camera        = light_camera;
    desc.content       = content_;
    // The recipe's content selection, on every pass that walks this content (see PipelineOptions::layer_mask):
    // what the picture does not show does not cast either, which is the whole point of drawing one content
    // through several passes.
    desc.layer_mask    = content_layers_;
    desc.writes        = shadow_map;
    // Its only writer owns the hand-off: the pass that samples it declares this target as an input,
    // and the engine answers that from the passes that DREW into it.
    desc.output_target = shadow_map;
    pipeline.addPass(desc);
    // It is built: reportRequestedShadows() is about what this pipeline did NOT build.
    ++shadows_built_;
    return shadow_map;
}

bool ChainBuilder::buildForwardPath(PassChain& pipeline)
{
    if (engine_ == nullptr || camera_ == nullptr) {
        return false;
    }
    // A shadow-casting light in the content is honoured on this path too, through the SAME pass the
    // deferred path builds (one implementation, one light camera). The content pass then STATES the
    // shadow it shades - the map and the light it was cast by (RenderPass::ShadowSource) - and that
    // declaration is the whole hand-off: the engine resolves it (RenderEngine::resolvePassInputs) and
    // the backend binds it into the content set's shadow binding, where the forward program shades
    // with it (ShadowAbi / ShaderAbi.hpp).
    intrusive_ptr<RenderTarget> shadow_map;
    if (intrusive_ptr<const Light> shadow_light = requestedShadowLight(content_.get()); shadow_light != nullptr) {
        shadow_map = buildShadowPass(pipeline, shadow_light);
    }
    // The forward content pass IS the lit result, so it lives at the shading
    // stage; there is no separate geometry pass to place.
    PassDesc main_desc;
    main_desc.name    = u8"main";
    main_desc.tag     = u8"Opaque";
    main_desc.stage   = PassStage::Shading;
    main_desc.camera  = intrusive_ptr<Camera>(camera_);
    main_desc.content = content_;
    main_desc.layer_mask = content_layers_;
    if (shadow_map != nullptr) {
        main_desc.input_targets = { shadow_map };
    }
    const intrusive_ptr<RenderPass> pass = pipeline.addPass(main_desc);
    // Optional transparent content: a depth-on pass stacked right after the
    // main content in the SAME window render pass, so it occludes against the
    // opaque depth the main pass just wrote (see setTransparentContent).
    if (transparent_ != nullptr) {
        PassDesc transparent_desc;
        transparent_desc.name    = u8"forward_transparent";
        transparent_desc.tag     = u8"Transparent";
        transparent_desc.stage   = PassStage::Transparent;
        transparent_desc.camera  = camera_;
        transparent_desc.content = transparent_;
        transparent_desc.layer_mask = content_layers_;
        // Translucent / overlay content: depth-TESTS against the opaque depth
        // the main content pass just wrote, but does NOT write depth (standard
        // alpha-blend rule) and does not clear.
        transparent_desc.clear_enabled = false;
        transparent_desc.depth_mode    = DepthMode::TestOnly;
        pipeline.addPass(transparent_desc);
    }
    pipeline.setWindowPass(pass);
    return true;
}

bool ChainBuilder::buildDeferredPath(PassChain& pipeline, const ChainDesc& options)
{
    if (engine_ == nullptr || camera_ == nullptr || content_ == nullptr) {
        return false;
    }
    // The light this pipeline must cast a shadow for, if any: the request lives on the LIGHT
    // (Light::isCastingShadow, with its own resolution/bias in ShadowSettings), because a light outlives
    // any one pipeline and a scene may be drawn by several. Single directional light per content,
    // as the shadow design fixes it (graphics-shadow.md §8).
    intrusive_ptr<const Light> shadow_light = requestedShadowLight(content_.get());

    // Programs default to the built-in temporary shaders so the preset works
    // out of the box; explicit programs in the options override them.
    intrusive_ptr<ShaderProgram> gbuf_program = options.gbuffer_program;
    intrusive_ptr<ShaderProgram> light_program = options.lighting_program;
    if (gbuf_program == nullptr) {
        gbuf_program = defaultGbufferGeometryProgram();
    }
    if (light_program == nullptr) {
        // A shadow changes the SHADING, so it changes which program the lighting pass draws with
        // (the variant declares the map and its block; see BuiltinShaders::deferredLightProgram).
        light_program = shadow_light != nullptr ? shadowedDeferredLightProgram() : deferredLightProgram();
    }

    // G-buffer at the requested / current surface / default size.
    int width  = options.offscreen_width;
    int height = options.offscreen_height;
    if (width <= 0 || height <= 0) {
        width  = engine_->surfaceWidth();
        height = engine_->surfaceHeight();
    }
    if (width <= 0 || height <= 0) {
        width  = 640;
        height = 360;
    }

    // A host that supplied its own lighting program gets NO shadow pass: the shading is theirs, so
    // a shadow they do not shade would be a pass nobody reads. That gap is REPORTED — once, by
    // reportRequestedShadows() at the end of build(), which is also what covers the forward path:
    // reporting it here as well would say the same thing twice for one problem.
    intrusive_ptr<RenderTarget> shadow_map;
    if (shadow_light != nullptr && options.lighting_program != nullptr) {
        shadow_light = nullptr;
    }
    if (shadow_light != nullptr) {
        shadow_map = buildShadowPass(pipeline, shadow_light);
    }

    // Canonical G-buffer (shared factory): albedo (0), view normal +
    // shininess (1), specular (2), view position (3) and depth. The geometry
    // program must write exactly these outputs.
    auto gbuffer = defaultGbufferTarget(width, height);

    // G-buffer geometry pass (order < 0), publishing the target as "GBuffer".
    PassDesc gbuf_desc;
    gbuf_desc.name          = u8"gbuffer";
    gbuf_desc.tag           = u8"GBuffer";
    gbuf_desc.stage         = PassStage::Geometry;
    gbuf_desc.camera        = camera_;
    gbuf_desc.content       = content_;
    gbuf_desc.layer_mask    = content_layers_;
    gbuf_desc.writes        = gbuffer;
    // The program is a SCENE pass' override (`PassDesc::program_override`): `PassDesc::program` is the
    // full-screen field a Screen pass draws through, so naming it here left the G-buffer pass with no program at
    // all - every body then drew with its own (or the engine's forward) program, which writes ONE colour output:
    // the geometry landed in albedo and the normal / specular / position attachments kept their transparent-black
    // clear, so the lighting read nothing and a PiP of either of those three was black.
    gbuf_desc.program_override = std::move(gbuf_program);
    gbuf_desc.output_name   = u8"GBuffer";
    // The same wire, declared at object level: this pass is the G-buffer's ONLY writer, so it owns the
    // hand-off and promises the whole target (one line instead of one per colour attachment — the
    // promise is shape-agnostic). The name above still drives the per-frame registry (design §14).
    gbuf_desc.output_target = gbuffer;
    pipeline.addPass(gbuf_desc);

    // Fullscreen deferred lighting. Without transparent content it runs at
    // order 0 as the WINDOW pass that presents the view camera (so
    // RenderControl / SceneView add no forward pass). With transparent content
    // the lit result must first be composited with the forward content
    // off-screen, so the same program shades into a composite target instead.
    if (transparent_ == nullptr) {
        PassDesc light_desc;
        light_desc.name    = u8"deferred_lighting";
        light_desc.tag     = u8"Lighting";
        light_desc.stage   = PassStage::Shading;
        light_desc.kind    = PassKind::Screen;
        light_desc.camera  = intrusive_ptr<Camera>(camera_);
        light_desc.content = content_;
        light_desc.input_names = { u8"GBuffer" };
        // It reads the WHOLE G-buffer: a fullscreen program receives every colour attachment of its
        // source (plus its depth while that one is sampleable) and picks by binding, so the unit it
        // consumes is the target — one declaration instead of "name + attachment index".
        light_desc.input_targets = { gbuffer };
        if (shadow_map != nullptr) {
            // The pass states the shadow it shades (see RenderPass::ShadowSource): the map, and the
            // light it was cast by - which also puts the map among the pass' inputs, with the same
            // ordering and lifetime guarantees. The shadow ABI puts its two bindings right after the
            // source's own (see BuiltinShaders::deferredLightProgram).
            light_desc.input_targets.push_back(shadow_map);
        }
        light_desc.program = std::move(light_program);
        const intrusive_ptr<RenderPass> light = pipeline.addPass(light_desc);

        pipeline.setOffscreenTarget(std::move(gbuffer));
        pipeline.setWindowPass(light);
        return true;
    }

    // ---- Deferred + forward composite (transparent content present) ----
    // One off-screen composite target whose single render pass stacks the
    // passes in record order: fullscreen lighting (0) -> forward (+1). It does
    // NOT own a depth buffer: its passes draw against the G-buffer's depth (a
    // DepthView edge, stated by each pass that draws into it), so the opaque
    // scene is rasterised once and the forward content tests against that same
    // depth. The G-buffer's depth is therefore kept as an attachment (not
    // promoted to a sampled texture) here.
    gbuffer->setDepthPromotion(false);
    auto composite = make_intrusive<RenderTarget>();
    composite->setName(u8"composite");
    composite->setSize(width, height);
    composite->attachColor(RenderTarget::ColorFormat::RGBA8);

    // Fullscreen deferred lighting (order 0) INTO the composite: a depth-off
    // program that overwrites every pixel with the lit opaque result.
    PassDesc light_desc;
    light_desc.name    = u8"deferred_lighting";
    light_desc.tag     = u8"Lighting";
    light_desc.stage   = PassStage::Shading;
    light_desc.kind    = PassKind::Screen;
    light_desc.camera  = intrusive_ptr<Camera>(camera_);
    light_desc.content = content_;
    light_desc.writes  = composite;
    light_desc.input_names   = { u8"GBuffer" };
    light_desc.input_targets = { gbuffer };   // reads the whole G-buffer (see the comment on the program path)
    if (shadow_map != nullptr) {
        light_desc.input_targets.push_back(shadow_map);
    }
    light_desc.program = std::move(light_program);
    // The edge, stated by the pass that draws against it (see DepthView): the composite has no depth of its
    // own, so what it tests against is the G-buffer's.
    light_desc.depth_view = depthViewOf(gbuffer);
    pipeline.addPass(light_desc);

    // Forward transparent / overlay content (order +1): depth-TESTS against
    // the G-buffer depth the composite loaded (TestOnly — it never writes
    // depth, the standard translucent rule) and blends per material over the
    // lit opaque result. It is the composite's last writer, so it publishes
    // the target as "Composite".
    PassDesc transparent_desc;
    transparent_desc.name    = u8"forward_transparent";
    transparent_desc.tag     = u8"Transparent";
    transparent_desc.stage   = PassStage::Transparent;
    transparent_desc.camera  = intrusive_ptr<Camera>(camera_);
    transparent_desc.content = transparent_;
    transparent_desc.layer_mask = content_layers_;
    transparent_desc.writes  = composite;
    transparent_desc.clear_enabled = false;
    transparent_desc.depth_mode    = DepthMode::TestOnly;
    transparent_desc.output_name   = u8"Composite";
    transparent_desc.depth_view    = depthViewOf(gbuffer);
    // The composite's LAST writer owns the hand-off (the lighting pass above also draws into the same
    // target but promises nothing): promising on both would be a collision report, and the collision
    // report is exactly about two passes both claiming to be the source of one image.
    transparent_desc.output_target = composite;
    pipeline.addPass(transparent_desc);

    // Present the baked composite to the window (order 2). Carrying the view
    // camera with a null render target is what makes it a window pass; a host
    // that built this pipeline declares it as the surface's presentation
    // (RenderSurface::present), which is what keeps a default forward pass from
    // being added beside it. The HUD overlays (orders 10 / 30) stack above it.
    PassDesc present_desc;
    present_desc.name   = u8"present";
    present_desc.tag    = u8"Present";
    present_desc.stage  = PassStage::Present;
    present_desc.kind   = PassKind::Screen;
    present_desc.camera = intrusive_ptr<Camera>(camera_);
    // The recipe names the copy program: the SDK's own passes have no implicit shading either, so
    // "present the baked target" is an SDK program (BuiltinShaders::screenCopyProgram) rather than a
    // backend-side default (see ScreenPass::setProgram).
    present_desc.program = screenCopyProgram();
    present_desc.input_names   = { u8"Composite" };
    present_desc.input_targets = { composite };   // it presents the whole baked target
    const intrusive_ptr<RenderPass> present = pipeline.addPass(present_desc);

    pipeline.setOffscreenTarget(std::move(gbuffer));
    pipeline.setCompositeTarget(std::move(composite));
    pipeline.setWindowPass(present);
    return true;
}

}  // namespace detail

ShadingPath PassChain::effectivePath(const ChainDesc& options) noexcept
{
    // A non-Lit mode is a geometry-stage override: it needs the pass that turns geometry into the picture, and a
    // deferred recipe has no such pass in the picture's path (its window pass is a fullscreen lighting program
    // reading the G-buffer). So the chain is built forward while such a mode is active - and the REQUEST is left
    // untouched, so switching back to Lit builds deferred again.
    if (options.mode != ShadingMode::Lit && options.path == ShadingPath::Deferred) {
        return ShadingPath::Forward;
    }
    return options.path;
}

intrusive_ptr<PassChain> PassChain::build(raw_ptr<RenderEngine> engine, const ChainDesc& desc)
{
    return detail::ChainBuilder(engine).build(desc);
}

VN_GRAPHICS_NS_END
