#include <vine/graphics/RenderPipelineBuilder.hpp>

#include <vine/graphics/AxisGizmo.hpp>
#include <vine/graphics/BuiltinShaders.hpp>
#include <vine/graphics/Camera.hpp>
#include <vine/graphics/FpsOverlay.hpp>
#include <vine/graphics/Light.hpp>
#include <vine/math/Point3.hpp>
#include <vine/math/Rect3.hpp>
#include <vine/graphics/RenderDiagnostic.hpp>
#include <vine/graphics/RenderEngine.hpp>
#include <vine/graphics/Scene.hpp>
#include <vine/graphics/ScreenPass.hpp>
#include <vine/graphics/ShaderProgram.hpp>

VN_GRAPHICS_NS_BEGIN

/** @brief Builds the default G-buffer geometry program (scene -> MRT).
 *
 * The canonical four outputs the Deferred preset's G-buffer declares: albedo (0),
 * view-space normal + shininess (1), specular (2) and view-space position (3). It
 * matches the backend's per-material block and view-space light ABI.
 *
 * The program — and the GLSL it is built from — is owned by the SDK (see
 * BuiltinShaders.hpp); this is RenderPipelineBuilder's alias for it. Callers may
 * still override it through PipelineOptions::gbuffer_program.
 *
 * @return The geometry program.
 */
intrusive_ptr<ShaderProgram> RenderPipelineBuilder::defaultGbufferGeometryProgram()
{
    return gbufferGeometryProgram();
}

/** @brief Builds the default deferred-lighting fragment program (fullscreen).
 *
 * Samples the G-buffer's albedo / normal / specular / view-position
 * attachments (binding 0..3) and shades ambient + up to three directional
 * lights whose parameters arrive in the backend's view-space push block.
 *
 * The program — and the GLSL it is built from — is owned by the SDK (see
 * BuiltinShaders.hpp); this is RenderPipelineBuilder's alias for it. Callers may
 * still override it through PipelineOptions::lighting_program.
 *
 * @return The lighting program (fragment stage only).
 */
intrusive_ptr<ShaderProgram> RenderPipelineBuilder::defaultDeferredLightProgram()
{
    return deferredLightProgram();
}

Mat4d RenderPipelineBuilder::directionalShadowMatrix(const Light& light, const vn::math::Aabbd& bounds,
                                                        Camera& camera)
{
    using vn::math::Vec3d;

    const bool   valid   = bounds.max().x >= bounds.min().x && bounds.max().y >= bounds.min().y &&
                         bounds.max().z >= bounds.min().z;
    const Vec3d  extent  = valid ? Vec3d(bounds.max().x - bounds.min().x, bounds.max().y - bounds.min().y,
                                        bounds.max().z - bounds.min().z)
                                 : Vec3d(2.0, 2.0, 2.0);
    const Vec3d  centre  = valid ? Vec3d(bounds.center().x, bounds.center().y, bounds.center().z) : Vec3d(0.0, 0.0, 0.0);
    // Every orientation of the box fits in this sphere, so the window does not shrink when the sun
    // moves; the margin keeps a caster standing exactly on the border out of the depth clamp.
    const double radius  = extent.length() * 0.5;
    const double depth   = radius * 4.0 + 1.0;
    const Vec3d  forward = light.direction().length() > 1e-9 ? light.direction().normalized() : Vec3d(0.0, 0.0, -1.0);
    // Any up vector that is not parallel to the direction: a light is a direction, not a roll.
    const Vec3d up  = std::abs(forward.z) > 0.9 ? Vec3d(0.0, 1.0, 0.0) : Vec3d(0.0, 0.0, 1.0);
    const Vec3d eye = centre - forward * (radius * 2.0 + 1.0);

    camera.setViewMatrixAsLookAt(eye, eye + forward, up);
    camera.setProjectionMatrixAsOrtho(-radius, radius, -radius, radius, 0.0, depth);
    return camera.projectionMatrix() * camera.viewMatrix();
}

intrusive_ptr<RenderTarget> RenderPipelineBuilder::defaultGbufferTarget(int width, int height)
{
    if (width <= 0) {
        width = 640;
    }
    if (height <= 0) {
        height = 360;
    }
    auto gbuffer = make_intrusive<RenderTarget>();
    gbuffer->setSize(width, height);
    gbuffer->attachColor(RenderTarget::ColorFormat::RGBA8);   // att 0: albedo
    gbuffer->attachColor(RenderTarget::ColorFormat::RGBA16F); // att 1: view normal (+ shininess)
    gbuffer->attachColor(RenderTarget::ColorFormat::RGBA8);   // att 2: specular
    gbuffer->attachColor(RenderTarget::ColorFormat::RGBA16F); // att 3: view position
    gbuffer->attachDepth(RenderTarget::DepthFormat::D24);
    gbuffer->setName(u8"gbuffer");
    return gbuffer;
}

RenderPipelineBuilder::RenderPipelineBuilder(raw_ptr<RenderEngine> engine)
  : engine_(engine)
{}

RenderPipelineBuilder& RenderPipelineBuilder::setContent(intrusive_ptr<Scene> content)
{
    content_ = std::move(content);
    return *this;
}

RenderPipelineBuilder& RenderPipelineBuilder::setTransparentContent(intrusive_ptr<Scene> transparent)
{
    transparent_ = std::move(transparent);
    return *this;
}

RenderPipelineBuilder& RenderPipelineBuilder::setCamera(raw_ptr<Camera> camera)
{
    camera_ = camera;
    return *this;
}

intrusive_ptr<Pipeline> RenderPipelineBuilder::build(const PipelineOptions& options)
{
    if (engine_ == nullptr) {
        return nullptr;
    }
    // The pipeline IS the plan: it registers each pass as it is planned and
    // remembers it, so a build that stops half way (or a host that drops the
    // handle) takes its passes back out of the frame instead of leaving them to
    // run with nobody holding them (see Pipeline).
    // The pipeline holds the engine STRONGLY (its passes live in that engine's
    // list): adopt the borrowed handle into an owning one.
    auto pipeline = make_intrusive<Pipeline>(intrusive_ptr<RenderEngine>(engine_));
    // A builder is reusable in principle, and the report is about THIS build.
    shadows_built_ = 0;
    const bool ok = (options.path == ShadingPath::Forward) ? buildForwardPath(*pipeline)
                                                           : buildDeferredPath(*pipeline, options);
    if (!ok) {
        return nullptr;
    }
    reportRequestedShadows();
    if (!applyOverlays(*pipeline, options)) {
        return nullptr;
    }
    return pipeline;
}

bool RenderPipelineBuilder::applyOverlays(Pipeline& pipeline, const PipelineOptions& options)
{
    // HUD overlays, stacked at one stage: what orders them against each other is
    // their own stacking offset inside it (see AxisGizmoOptions::order), not a
    // number that has to stay bigger than the passes below.
    const int overlay_base = pipelineStageOrder(PipelineStage::Overlay);

    // An axis gizmo, mirroring the source camera. Re-anchored through
    // Pipeline::resize.
    if (options.gizmo.source_camera != nullptr) {
        auto gizmo = make_intrusive<AxisGizmo>();
        gizmo->setSourceCamera(options.gizmo.source_camera);
        gizmo->setPixelRatio(options.gizmo.pixel_ratio);
        gizmo->setBoxSize(options.gizmo.box_size);
        gizmo->setAxisLength(options.gizmo.axis_length);
        gizmo->setThickness(options.gizmo.thickness);
        pipeline.addPass(gizmo, nullptr, overlay_base + options.gizmo.order);
        pipeline.setGizmo(std::move(gizmo));
    }

    // A frame-rate readout (bottom-right corner). It measures the actual
    // render-loop rate and needs no source camera, so it is opt-in through
    // FpsOverlayOptions::enabled.
    if (options.fps.enabled) {
        auto fps = make_intrusive<FpsOverlay>();
        fps->setPixelRatio(options.fps.pixel_ratio);
        fps->setSize(options.fps.width_px, options.fps.height_px);
        pipeline.addPass(fps, nullptr, overlay_base + options.fps.order);
        pipeline.setFpsOverlay(std::move(fps));
    }
    return true;
}

void RenderPipelineBuilder::reportRequestedShadows() const
{
    // A shadow is asked for by the LIGHT (Light::castShadow + ShadowSettings) —
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
            if (light != nullptr && light->isEnabled() && light->castShadow()) {
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
                                            u8"see .ai/design/render-pipeline.md §9): those lights cast nothing"));
}

namespace
{

/**
 * @brief Finds the light a content scene asks a shadow for.
 *
 * Single directional light per content, as the shadow design fixes it (render-pipeline.md §9): the
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
    // Single directional light per content, as the shadow design fixes it (render-pipeline.md §9).
    for (const auto& light : content->lights()) {
        if (light != nullptr && light->isEnabled() && light->castShadow() &&
            light->type() == LightType::Directional) {
            return light;   // a strong handle: the map holds its light (RenderTarget::setShadowOf)
        }
    }
    return nullptr;
}

}  // namespace

intrusive_ptr<RenderTarget> RenderPipelineBuilder::buildShadowPass(Pipeline& pipeline,
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
    // second ortho box of its own (see .ai/design/render-pipeline.md §9).
    // The map states what it IS, not who reads it: a consumer finds the shadow by asking the declared
    // targets whose shadow they are (RenderTarget::setShadowOf), so nothing is inferred from declaration
    // order and RenderPass stays a generic stage.
    shadow_map->setShadowOf(shadow_light_owner);
    shadow_map->setProducerViewProjection(directionalShadowMatrix(shadow_light, content_->boundingBox(), *light_camera));

    auto shadow_pass = make_intrusive<RenderPass>();
    shadow_pass->setName(u8"shadow");
    shadow_pass->setCamera(light_camera);
    shadow_pass->setRenderTarget(shadow_map);
    // Its only writer owns the hand-off: the pass that samples it declares this target as an input,
    // and the engine answers that from the passes that DREW into it.
    shadow_pass->setOutputTarget(shadow_map);
    pipeline.addPass(shadow_pass, content_, pipelineStageOrder(PipelineStage::Depth));
    // It is built: reportRequestedShadows() is about what this pipeline did NOT build.
    ++shadows_built_;
    return shadow_map;
}

bool RenderPipelineBuilder::buildForwardPath(Pipeline& pipeline)
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
    auto pass = make_intrusive<RenderPass>();
    pass->setName(u8"main");
    pass->setCamera(intrusive_ptr<Camera>(camera_));
    if (shadow_map != nullptr) {
        pass->addInputTarget(shadow_map);
    }
    pipeline.addPass(pass, content_, pipelineStageOrder(PipelineStage::Shading));
    // Optional transparent content: a depth-on pass stacked right after the
    // main content in the SAME window render pass, so it occludes against the
    // opaque depth the main pass just wrote (see setTransparentContent).
    if (transparent_ != nullptr) {
        auto transparent = make_intrusive<RenderPass>();
        transparent->setName(u8"forward_transparent");
        transparent->setCamera(intrusive_ptr<Camera>(camera_));
        // Translucent / overlay content: depth-TESTS against the opaque depth
        // the main content pass just wrote, but does NOT write depth (standard
        // alpha-blend rule) and does not clear.
        transparent->setClearEnabled(false);
        transparent->setDepthMode(DepthMode::TestOnly);
        pipeline.addPass(transparent, transparent_, pipelineStageOrder(PipelineStage::Transparent));
    }
    pipeline.setWindowPass(std::move(pass));
    return true;
}

bool RenderPipelineBuilder::buildDeferredPath(Pipeline& pipeline, const PipelineOptions& options)
{
    if (engine_ == nullptr || camera_ == nullptr || content_ == nullptr) {
        return false;
    }
    // The light this pipeline must cast a shadow for, if any: the request lives on the LIGHT
    // (Light::castShadow, with its own resolution/bias in ShadowSettings), because a light outlives
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
        width  = engine_->frameContext().surface_width;
        height = engine_->frameContext().surface_height;
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
    auto gbuf_pass = make_intrusive<RenderPass>();
    gbuf_pass->setName(u8"gbuffer");
    gbuf_pass->setCamera(intrusive_ptr<Camera>(camera_));
    gbuf_pass->setRenderTarget(gbuffer);
    gbuf_pass->setProgramOverride(std::move(gbuf_program));
    gbuf_pass->setOutputName(u8"GBuffer");
    // The same wire, declared at object level: this pass is the G-buffer's ONLY writer, so it owns the
    // hand-off and promises the whole target (one line instead of one per colour attachment — the
    // promise is shape-agnostic). The name above still drives the per-frame registry (design §14).
    gbuf_pass->setOutputTarget(gbuffer);
    pipeline.addPass(gbuf_pass, content_, pipelineStageOrder(PipelineStage::Geometry));

    // Fullscreen deferred lighting. Without transparent content it runs at
    // order 0 as the WINDOW pass that presents the view camera (so
    // RenderControl / SceneView add no forward pass). With transparent content
    // the lit result must first be composited with the forward content
    // off-screen, so the same program shades into a composite target instead.
    if (transparent_ == nullptr) {
        auto light = make_intrusive<ScreenPass>();
        light->setName(u8"deferred_lighting");
        light->setCamera(intrusive_ptr<Camera>(camera_));
        light->addInputName(u8"GBuffer");
        // It reads the WHOLE G-buffer: a fullscreen program receives every colour attachment of its
        // source (plus its depth while that one is sampleable) and picks by binding, so the unit it
        // consumes is the target — one declaration instead of "name + attachment index".
        light->addInputTarget(gbuffer);
        if (shadow_map != nullptr) {
            // The pass states the shadow it shades (see RenderPass::ShadowSource): the map, and the
            // light it was cast by - which also puts the map among the pass' inputs, with the same
            // ordering and lifetime guarantees. The shadow ABI puts its two bindings right after the
            // source's own (see BuiltinShaders::deferredLightProgram).
            light->addInputTarget(shadow_map);
        }
        light->setProgram(std::move(light_program));
        pipeline.addPass(light, content_, pipelineStageOrder(PipelineStage::Shading));

        pipeline.setOffscreenTarget(std::move(gbuffer));
        pipeline.setWindowPass(std::move(light));
        return true;
    }

    // ---- Deferred + forward composite (transparent content present) ----
    // One off-screen composite target whose single render pass stacks the
    // passes in record order: fullscreen lighting (0) -> forward (+1). It does
    // NOT own a depth buffer: it borrows the G-buffer's depth (shareDepth), so
    // the opaque scene is rasterised once and the forward content tests against
    // that same depth. The G-buffer's depth is therefore kept as an attachment
    // (not promoted to a sampled texture) here.
    gbuffer->setDepthPromotion(false);
    auto composite = make_intrusive<RenderTarget>();
    composite->setName(u8"composite");
    composite->setSize(width, height);
    composite->attachColor(RenderTarget::ColorFormat::RGBA8);
    composite->shareDepth(gbuffer);

    // Fullscreen deferred lighting (order 0) INTO the composite: a depth-off
    // program that overwrites every pixel with the lit opaque result.
    auto light = make_intrusive<ScreenPass>();
    light->setName(u8"deferred_lighting");
    light->setCamera(intrusive_ptr<Camera>(camera_));
    light->setRenderTarget(composite);
    light->addInputName(u8"GBuffer");
    light->addInputTarget(gbuffer);   // reads the whole G-buffer (see the comment on the program path)
    if (shadow_map != nullptr) {
        light->addInputTarget(shadow_map);
    }
    light->setProgram(std::move(light_program));
    pipeline.addPass(light, content_, pipelineStageOrder(PipelineStage::Shading));

    // Forward transparent / overlay content (order +1): depth-TESTS against
    // the G-buffer depth the composite loaded (TestOnly — it never writes
    // depth, the standard translucent rule) and blends per material over the
    // lit opaque result. It is the composite's last writer, so it publishes
    // the target as "Composite".
    auto transparent = make_intrusive<RenderPass>();
    transparent->setName(u8"forward_transparent");
    transparent->setCamera(intrusive_ptr<Camera>(camera_));
    transparent->setRenderTarget(composite);
    transparent->setClearEnabled(false);
    transparent->setDepthMode(DepthMode::TestOnly);
    transparent->setOutputName(u8"Composite");
    // The composite's LAST writer owns the hand-off (the lighting pass above also draws into the same
    // target but promises nothing): promising on both would be a collision report, and the collision
    // report is exactly about two passes both claiming to be the source of one image.
    transparent->setOutputTarget(composite);
    pipeline.addPass(transparent, transparent_, pipelineStageOrder(PipelineStage::Transparent));

    // Present the baked composite to the window (order 2). Carrying the view
    // camera with a null render target keeps hasWindowPass(camera) true, so
    // RenderControl / SceneView add no default forward pass; the HUD overlays
    // (orders 10 / 30) stack above it.
    auto present = make_intrusive<ScreenPass>();
    present->setName(u8"present");
    present->setCamera(intrusive_ptr<Camera>(camera_));
    // The recipe names the copy program: the SDK's own passes have no implicit shading either, so
    // "present the baked target" is an SDK program (BuiltinShaders::screenCopyProgram) rather than a
    // backend-side default (see ScreenPass::setProgram).
    present->setProgram(screenCopyProgram());
    present->addInputName(u8"Composite");
    present->addInputTarget(composite);   // it presents the whole baked target
    pipeline.addPass(present, nullptr, pipelineStageOrder(PipelineStage::Present));

    pipeline.setOffscreenTarget(std::move(gbuffer));
    pipeline.setCompositeTarget(std::move(composite));
    pipeline.setWindowPass(std::move(present));
    return true;
}

raw_ptr<ScreenPass> RenderPipelineBuilder::addOffscreenToScreen(const String& output_slot,
                                                                int rt_width,
                                                                int rt_height,
                                                                RenderTarget::ColorFormat color_format,
                                                                RenderTarget::DepthFormat depth_format,
                                                                const Viewport& pip)
{
    if (engine_ == nullptr) {
        return nullptr;
    }
    raw_ptr<Camera> camera = camera_;
    if (camera == nullptr) {
        // Scene-pass recipes need a view camera; none was bound via setCamera.
        return nullptr;
    }

    // Off-screen target + an order < 0 scene pass that renders into it and
    // publishes the result under the slot name.
    auto target = make_intrusive<RenderTarget>();
    target->setName(output_slot);
    target->setSize(rt_width, rt_height);
    target->attachColor(color_format);
    target->attachDepth(depth_format);

    auto offscreen = make_intrusive<RenderPass>();
    offscreen->setName(output_slot);
    offscreen->setCamera(intrusive_ptr<Camera>(camera));
    offscreen->setRenderTarget(target);
    offscreen->setOutputName(output_slot);
    offscreen->setOutputTarget(target);   // its only writer: it owns the hand-off
    // Registered on the engine, NOT through a Pipeline handle: the engine keeps
    // this pass alive and no Pipeline owns it, so it is only addressable through
    // clearPasses() — see addOffscreenToScreen's ownership note.
    engine_->addPass(offscreen, content_, pipelineStageOrder(PipelineStage::Geometry));

    // An order > 0 ScreenPass sampling the slot into the PiP sub-viewport. Its picture is the SDK's
    // copy program, and it carries the view camera because a fullscreen program is drawn through the
    // pass' view (see ScreenPass::setProgram).
    auto screen = make_intrusive<ScreenPass>();
    screen->setName(output_slot);
    screen->setCamera(intrusive_ptr<Camera>(camera_));
    screen->setProgram(screenCopyProgram());
    screen->addInputName(output_slot);
    screen->addInputTarget(target);   // samples the whole published target
    screen->setViewport(pip);
    engine_->addPass(screen, pipelineStageOrder(PipelineStage::Preview));

    return screen.get();
}

void RenderPipelineBuilder::addPass(intrusive_ptr<RenderPass> pass, int order)
{
    if (engine_ == nullptr || pass == nullptr) {
        return;
    }
    engine_->addPass(std::move(pass), order);
}

VN_GRAPHICS_NS_END
