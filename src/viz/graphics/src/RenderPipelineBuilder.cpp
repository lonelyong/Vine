#include <vine/graphics/RenderPipelineBuilder.hpp>

#include <vine/graphics/AxisGizmo.hpp>
#include <vine/graphics/BuiltinShaders.hpp>
#include <vine/graphics/Camera.hpp>
#include <vine/graphics/FpsOverlay.hpp>
#include <vine/graphics/RenderEngine.hpp>
#include <vine/graphics/Scene.hpp>
#include <vine/graphics/ScreenPass.hpp>
#include <vine/graphics/ShaderProgram.hpp>

V_GRAPHICS_NS_BEGIN

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

intrusive_ptr<Pipeline> RenderPipelineBuilder::build(PipelinePreset preset,
                                                     const PipelineOptions& options)
{
    if (engine_ == nullptr) {
        return nullptr;
    }
    // The shadowed variants are placeholders until the shadow slice lands
    // (an order < 0 depth-only pass plus shadowed lighting).
    PipelinePreset base = preset;
    if (base == PipelinePreset::ForwardShadowed) {
        base = PipelinePreset::Forward;
    } else if (base == PipelinePreset::DeferredShadowed) {
        base = PipelinePreset::Deferred;
    }

    auto pipeline = make_intrusive<Pipeline>();
    const bool ok = (base == PipelinePreset::Forward)
        ? buildForward(*pipeline)
        : buildDeferred(*pipeline, options);
    if (!ok) {
        return nullptr;
    }

    // Optional HUD overlay: an axis gizmo (mirrors the source camera) stacked
    // above the window pass. It is re-anchored through Pipeline::resize.
    if (options.gizmo.source_camera != nullptr) {
        auto gizmo = make_intrusive<AxisGizmo>();
        gizmo->setSourceCamera(options.gizmo.source_camera);
        gizmo->setPixelRatio(options.gizmo.pixel_ratio);
        gizmo->setBoxSize(options.gizmo.box_size);
        gizmo->setAxisLength(options.gizmo.axis_length);
        gizmo->setThickness(options.gizmo.thickness);
        engine_->addPass(gizmo, options.gizmo.order);
        pipeline->retainPass(gizmo);
        pipeline->setGizmo(std::move(gizmo));
    }

    // Optional HUD overlay: a frame-rate readout (bottom-right corner). It
    // measures the actual render-loop rate and needs no source camera, so it
    // is enabled by default (see FpsOverlayOptions::enabled). Re-anchored
    // through Pipeline::resize like the gizmo.
    if (options.fps.enabled) {
        auto fps = make_intrusive<FpsOverlay>();
        fps->setPixelRatio(options.fps.pixel_ratio);
        fps->setSize(options.fps.width_px, options.fps.height_px);
        engine_->addPass(fps, options.fps.order);
        pipeline->retainPass(fps);
        pipeline->setFpsOverlay(std::move(fps));
    }
    return pipeline;
}

bool RenderPipelineBuilder::buildForward(Pipeline& pipeline)
{
    if (engine_ == nullptr || camera_ == nullptr) {
        return false;
    }
    auto pass = make_intrusive<RenderPass>();
    pass->setName(u8"main");
    pass->setCamera(camera_);
    if (content_ != nullptr) {
        engine_->addPass(pass, content_, 0);
    } else {
        engine_->addPass(pass, 0);
    }
    pipeline.retainPass(pass);
    // Optional transparent content: a depth-on pass stacked right after the
    // main content in the SAME window render pass, so it occludes against the
    // opaque depth the main pass just wrote (see setTransparentContent).
    if (transparent_ != nullptr) {
        auto transparent = make_intrusive<RenderPass>();
        transparent->setName(u8"forward_transparent");
        transparent->setCamera(camera_);
        // Translucent / overlay content: depth-TESTS against the opaque depth
        // the main content pass just wrote, but does NOT write depth (standard
        // alpha-blend rule) and does not clear.
        transparent->setClearEnabled(false);
        transparent->setDepthMode(DepthMode::TestOnly);
        engine_->addPass(transparent, transparent_, 1);
        pipeline.retainPass(transparent);
    }
    pipeline.setWindowPass(std::move(pass));
    return true;
}

bool RenderPipelineBuilder::buildDeferred(Pipeline& pipeline,
                                          const PipelineOptions& options)
{
    if (engine_ == nullptr || camera_ == nullptr || content_ == nullptr) {
        return false;
    }
    // Programs default to the built-in temporary shaders so the preset works
    // out of the box; explicit programs in the options override them.
    intrusive_ptr<ShaderProgram> gbuf_program = options.gbuffer_program;
    intrusive_ptr<ShaderProgram> light_program = options.lighting_program;
    if (gbuf_program == nullptr) {
        gbuf_program = defaultGbufferGeometryProgram();
    }
    if (light_program == nullptr) {
        light_program = defaultDeferredLightProgram();
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

    // Canonical G-buffer (shared factory): albedo (0), view normal +
    // shininess (1), specular (2), view position (3) and depth. The geometry
    // program must write exactly these outputs.
    auto gbuffer = defaultGbufferTarget(width, height);

    // G-buffer geometry pass (order < 0), publishing the target as "GBuffer".
    auto gbuf_pass = make_intrusive<RenderPass>();
    gbuf_pass->setName(u8"gbuffer");
    gbuf_pass->setCamera(camera_);
    gbuf_pass->setRenderTarget(gbuffer);
    gbuf_pass->setProgramOverride(std::move(gbuf_program));
    gbuf_pass->setOutputName(u8"GBuffer");
    // The same wire, declared at object level: this pass is the G-buffer's ONLY writer, so it owns the
    // hand-off and promises the whole target (one line instead of one per colour attachment — the
    // promise is shape-agnostic). The name above still drives the per-frame registry (design §14).
    gbuf_pass->setOutputTarget(gbuffer);
    engine_->addPass(gbuf_pass, content_, -3);
    pipeline.retainPass(gbuf_pass);

    // Fullscreen deferred lighting. Without transparent content it runs at
    // order 0 as the WINDOW pass that presents the view camera (so
    // RenderControl / SceneView add no forward pass). With transparent content
    // the lit result must first be composited with the forward content
    // off-screen, so the same program shades into a composite target instead.
    if (transparent_ == nullptr) {
        auto light = make_intrusive<ScreenPass>();
        light->setName(u8"deferred_light");
        light->setCamera(camera_);
        light->addInputName(u8"GBuffer");
        // It reads the WHOLE G-buffer: a fullscreen program receives every colour attachment of its
        // source (plus its depth while that one is sampleable) and picks by binding, so the unit it
        // consumes is the target — one declaration instead of "name + attachment index".
        light->addInputTarget(gbuffer);
        light->setProgram(std::move(light_program));
        engine_->addPass(light, content_, 0);
        pipeline.retainPass(light);

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
    light->setName(u8"deferred_light");
    light->setCamera(camera_);
    light->setRenderTarget(composite);
    light->addInputName(u8"GBuffer");
    light->addInputTarget(gbuffer);   // reads the whole G-buffer (see the comment on the program path)
    light->setProgram(std::move(light_program));
    engine_->addPass(light, content_, 0);
    pipeline.retainPass(light);

    // Forward transparent / overlay content (order +1): depth-TESTS against
    // the G-buffer depth the composite loaded (TestOnly — it never writes
    // depth, the standard translucent rule) and blends per material over the
    // lit opaque result. It is the composite's last writer, so it publishes
    // the target as "Composite".
    auto transparent = make_intrusive<RenderPass>();
    transparent->setName(u8"forward_transparent");
    transparent->setCamera(camera_);
    transparent->setRenderTarget(composite);
    transparent->setClearEnabled(false);
    transparent->setDepthMode(DepthMode::TestOnly);
    transparent->setOutputName(u8"Composite");
    // The composite's LAST writer owns the hand-off (the lighting pass above also draws into the same
    // target but promises nothing): promising on both would be a collision report, and the collision
    // report is exactly about two passes both claiming to be the source of one image.
    transparent->setOutputTarget(composite);
    engine_->addPass(transparent, transparent_, 1);
    pipeline.retainPass(transparent);

    // Present the baked composite to the window (order 2). Carrying the view
    // camera with a null render target keeps hasWindowPass(camera) true, so
    // RenderControl / SceneView add no default forward pass; the HUD overlays
    // (orders 10 / 30) stack above it.
    auto present = make_intrusive<ScreenPass>();
    present->setName(u8"present");
    present->setCamera(camera_);
    present->addInputName(u8"Composite");
    present->addInputTarget(composite);   // it presents the whole baked target
    engine_->addPass(present, 2);
    pipeline.retainPass(present);

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
                                                                int pip_x, int pip_y, int pip_w, int pip_h)
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
    offscreen->setCamera(camera);
    offscreen->setRenderTarget(target);
    offscreen->setOutputName(output_slot);
    offscreen->setOutputTarget(target);   // its only writer: it owns the hand-off
    if (content_ != nullptr) {
        engine_->addPass(offscreen, content_, -2);
    } else {
        engine_->addPass(offscreen, -2);
    }
    passes_.push_back(offscreen);

    // An order > 0 ScreenPass sampling the slot into the PiP sub-viewport.
    auto screen = make_intrusive<ScreenPass>();
    screen->setName(output_slot);
    screen->addInputName(output_slot);
    screen->addInputTarget(target);   // samples the whole published target
    screen->setViewport(pip_x, pip_y, pip_w, pip_h);
    engine_->addPass(screen, 100);
    passes_.push_back(screen);

    return screen.get();
}

void RenderPipelineBuilder::addPass(intrusive_ptr<RenderPass> pass, int order)
{
    if (engine_ == nullptr || pass == nullptr) {
        return;
    }
    engine_->addPass(std::move(pass), order);
}

V_GRAPHICS_NS_END
