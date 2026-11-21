// The compiled copy of the module guide's three complete examples (`docs/guides/graphics.md` §3.3, §3.4,
// §3.5) - the two-pass offscreen composite, the hand-built deferred pipeline, and the builder that hides
// both.
//
// WHY, same as `GuideExample.cpp`: a snippet nothing compiles is a snippet that rots. These three are the
// blocks a host copies when it stops using the default chain, and two of them had already drifted - §3.3
// named `make_intrusive` unqualified (it lives in `vn`, not in `vn::graphics`), and §3.5 set the default
// content program on the ENGINE (`engine.setDefaultContentProgram`) although that setting belongs to the
// device's frame half, and registered its overlay on a `chain` that the same block never declared. The
// builder block also passed `&engine` where the parameter is already a `raw_ptr`.
//
// WHAT RUNS AND WHAT ONLY COMPILES: nothing here is executed - each function needs a device, a window and
// a chain, which is a host's business. They are compiled so the guide's three answers to "how do I build my
// own picture?" are type-checked, and `scripts/check_doc_symbols.py` requires every statement the guide's
// blocks carry to appear here.

#include <vine/graphics/AxisGizmo.hpp>
#include <vine/graphics/BuiltinShaders.hpp>
#include <vine/graphics/Camera.hpp>
#include <vine/graphics/ChainPresets.hpp>
#include <vine/graphics/Geometry.hpp>
#include <vine/graphics/Material.hpp>
#include <vine/graphics/PassChain.hpp>
#include <vine/graphics/RenderArea.hpp>
#include <vine/graphics/RenderBackendRegistry.hpp>
#include <vine/graphics/RenderDevice.hpp>
#include <vine/graphics/RenderEngine.hpp>
#include <vine/graphics/RenderFrameExecutor.hpp>
#include <vine/graphics/RenderTarget.hpp>
#include <vine/graphics/Scene.hpp>
#include <vine/graphics/ShaderProgram.hpp>

#include <vine/Color.hpp>
#include <vine/String.hpp>
#include <vine/intrusive_ptr.hpp>

/// The guide's three pass-building examples, statement for statement.
namespace guide_example
{
using namespace vn::graphics;

/**
 * @brief §3.3: an offscreen scene pass and a fullscreen composite that presents it.
 *
 * @details
 * - Two declarations in one plan: the first names its colour output, the second reads that name and also
 *   carries the target as an object - the two forms meet at the same address.
 * - The composite pass leaves `target` default, which is what makes it the window pass.
 *
 * @param engine     Engine the plan is applied to (borrowed).
 * @param camera     Camera both passes are taken with.
 * @param scene      Content the offscreen pass draws.
 * @param surface_w  Surface width in device pixels.
 * @param surface_h  Surface height in device pixels.
 * @param owner      Registration scope (see RenderEngine::applyPlan(plan, owner)).
 * @return The plan's answer, so a caller can report a refusal.
 */
PlanResult twoPassOffscreenToWindow(vn::raw_ptr<RenderEngine> engine,
                                    CameraPtr             camera,
                                    ScenePtr              scene,
                                    int                   surface_w,
                                    int                   surface_h,
                                    const void*           owner)
{
    auto scene_rt = vn::make_intrusive<RenderTarget>();
    scene_rt->setName(u8"scene_rt");
    scene_rt->attachColor(RenderTarget::ColorFormat::RGBA8);
    scene_rt->attachDepth(RenderTarget::DepthFormat::D24);
    scene_rt->setSize(surface_w, surface_h);

    PassDesc scene_pass;
    scene_pass.name          = u8"scene_into_rt";
    scene_pass.kind          = PassKind::Scene;
    scene_pass.order         = -1;
    scene_pass.camera        = camera;
    scene_pass.content       = scene;
    scene_pass.target        = Target::explicitTarget(scene_rt);
    scene_pass.clear_enabled = true;
    scene_pass.clear_color   = vn::Color(26, 26, 31).toLinear();
    scene_pass.clear_depth   = true;
    scene_pass.depth_mode    = DepthMode::TestAndWrite;
    scene_pass.output.name   = u8"SceneColor";
    scene_pass.output.whole_target = true;

    PassDesc compose_pass;
    compose_pass.name          = u8"compose_to_window";
    compose_pass.order         = 0;
    compose_pass.kind          = PassKind::Fullscreen;
    compose_pass.camera        = camera;
    compose_pass.program       = screenCopyProgram();
    compose_pass.input_names   = { vn::String(u8"SceneColor") };
    compose_pass.input_targets = { scene_rt };

    const PlanResult applied = engine->applyPlan(Plan{ { scene_pass, compose_pass } }, owner);
    return applied;
}

/**
 * @brief §3.4: the hand-built deferred pipeline - G-buffer, lighting, forward transparency, present.
 *
 * @details
 * - The four declarations are the ones `ChainDesc{ .path = ShadingPath::Deferred }` builds internally, so
 *   the hand-built chain and the preset are the same picture.
 * - The composite borrows the G-buffer's depth, which is what lets the forward pass be occluded by the
 *   opaque pass without rasterising the scene twice.
 *
 * @param engine            Engine the plan is applied to (borrowed).
 * @param camera            Camera every pass is taken with.
 * @param scene             Opaque content (the G-buffer pass draws it).
 * @param transparent_scene Translucent content drawn over the lit image.
 * @param width             Target width in device pixels.
 * @param height            Target height in device pixels.
 * @param owner             Registration scope.
 * @return The plan's answer.
 */
PlanResult deferredFourPass(vn::raw_ptr<RenderEngine> engine,
                            CameraPtr             camera,
                            ScenePtr              scene,
                            ScenePtr              transparent_scene,
                            int                   width,
                            int                   height,
                            const void*           owner)
{
    auto gbuffer = defaultGbufferTarget(width, height);

    auto gbuffer_program = defaultGbufferGeometryProgram();
    PassDesc gbuffer_pass;
    gbuffer_pass.name            = u8"gbuffer";
    gbuffer_pass.order           = -3;
    gbuffer_pass.kind            = PassKind::Scene;
    gbuffer_pass.camera          = camera;
    gbuffer_pass.content         = scene;
    gbuffer_pass.target          = Target::explicitTarget(gbuffer);
    gbuffer_pass.program_override = gbuffer_program;
    gbuffer_pass.output.name     = u8"GBuffer";
    gbuffer_pass.output.whole_target = true;

    gbuffer->setDepthPromotion(false);
    auto composite = vn::make_intrusive<RenderTarget>();
    composite->setName(u8"composite");
    composite->setSize(width, height);
    composite->attachColor(RenderTarget::ColorFormat::RGBA8);
    composite->shareDepth(depthViewOf(gbuffer));

    PassDesc light_pass;
    light_pass.name          = u8"deferred_lighting";
    light_pass.order         = 0;
    light_pass.kind          = PassKind::Fullscreen;
    light_pass.camera        = camera;
    light_pass.target        = Target::explicitTarget(composite);
    light_pass.input_names   = { vn::String(u8"GBuffer") };
    light_pass.input_targets = { gbuffer };
    light_pass.program       = defaultDeferredLightProgram();

    PassDesc forward_pass;
    forward_pass.name          = u8"forward_transparent";
    forward_pass.order         = 1;
    forward_pass.kind          = PassKind::Scene;
    forward_pass.camera        = camera;
    forward_pass.content       = transparent_scene;
    forward_pass.target        = Target::explicitTarget(composite);
    forward_pass.clear_enabled = false;
    forward_pass.depth_mode    = DepthMode::TestOnly;
    forward_pass.output.name   = u8"Composite";
    forward_pass.output.whole_target = true;

    PassDesc present_pass;
    present_pass.name          = u8"present";
    present_pass.order         = 2;
    present_pass.kind          = PassKind::Fullscreen;
    present_pass.camera        = camera;
    present_pass.program       = screenCopyProgram();
    present_pass.input_names   = { vn::String(u8"Composite") };
    present_pass.input_targets = { composite };

    const PlanResult applied = engine->applyPlan(Plan{ { gbuffer_pass, light_pass, forward_pass, present_pass } }, owner);
    return applied;
}

/**
 * @brief §3.5: the same two pictures through the builder, plus a hand-registered HUD overlay.
 *
 * @details
 * - `PassChain::build` is handed the engine itself; the chain is what holds the passes, and the overlay is
 *   registered ON THE CHAIN (`addOverlay`), which is the object the block builds.
 * - The default content program is a setting of the DEVICE's frame half, not of the engine: a device may
 *   drive more than one engine, and the program is the device's.
 *
 * @param engine              Engine the chain is built on (borrowed).
 * @param camera              Camera the picture is taken with.
 * @param scene               Content the chain draws.
 * @param surface_w           Surface width in logical pixels.
 * @param surface_h           Surface height in logical pixels.
 * @param device_pixel_ratio  Device pixels per logical pixel.
 * @param viewport_x          PiP rectangle origin, device pixels.
 * @param viewport_y          PiP rectangle origin, device pixels.
 * @return The chain, or null when the recipe was refused.
 */
vn::intrusive_ptr<PassChain> builderChain(vn::raw_ptr<RenderEngine> engine,
                                      CameraPtr             camera,
                                      ScenePtr              scene,
                                      int                   surface_w,
                                      int                   surface_h,
                                      double                device_pixel_ratio,
                                      int                   viewport_x,
                                      int                   viewport_y)
{
    ChainDesc options;
    options.content = scene;
    options.camera  = camera.get();

    options.path = ShadingPath::Deferred;
    options.offscreen_width  = 0;
    options.offscreen_height = 0;
    options.gbuffer_program  = nullptr;
    options.lighting_program = nullptr;

    auto pipeline = PassChain::build(engine, options);
    pipeline->resize(surface_w, surface_h, device_pixel_ratio);

    engine->device()->executor().setDefaultContentProgram(flatForwardProgram());
    engine->device()->executor().setDefaultContentProgram(forwardProgram());

    auto preview_pass = addOffscreenToScreen(engine, camera.get(), scene, u8"preview", 512, 288,
                                             RenderTarget::ColorFormat::RGBA8,
                                             RenderTarget::DepthFormat::D24,
                                             Viewport{ viewport_x, viewport_y, 320, 180 });
    if (preview_pass == nullptr) { return nullptr; }

    auto gizmo = vn::make_intrusive<AxisGizmo>();
    gizmo->setSourceCamera(camera.get());
    gizmo->onSurfaceResized(surface_w, surface_h);
    PassDesc hud;
    hud.name = u8"axis_gizmo"; hud.stage = PassStage::Overlay; hud.order = 10;
    hud.camera = vn::intrusive_ptr<Camera>(gizmo->camera());
    hud.content = vn::intrusive_ptr<Scene>(gizmo->content());
    hud.viewport = gizmo->boxViewport();
    hud.clear_enabled = false;                  // keep the picture it is drawn over
    hud.clear_depth   = true;                   // ...and own the depth inside the box
    hud.depth_mode    = DepthMode::TestAndWrite; // so the marker's arrows occlude each other
    pipeline->addOverlay(u8"gizmo", gizmo.get(), hud);

    return pipeline;
}

} // namespace guide_example
