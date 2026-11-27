#include "ChainBuilder.hpp"

#include <span>
#include <vector>

#include <vine/geom/spatial/Aabb.hpp>
#include <vine/geom/spatial/BoxSolid.hpp>
#include <vine/geom/spatial/SolidTessellation.hpp>
#include <vine/graphics/AxisGizmo.hpp>
#include <vine/graphics/BuiltinShaders.hpp>
#include <vine/graphics/Camera.hpp>
#include <vine/graphics/ChainPresets.hpp>
#include <vine/graphics/FpsOverlay.hpp>
#include <vine/graphics/FullscreenPass.hpp>
#include <vine/graphics/Geometry.hpp>
#include <vine/graphics/Light.hpp>
#include <vine/graphics/Material.hpp>
#include <vine/graphics/PassDesc.hpp>
#include <vine/graphics/RenderDiagnostic.hpp>
#include <vine/graphics/RenderEngine.hpp>
#include <vine/graphics/Scene.hpp>
#include <vine/graphics/ShaderProgram.hpp>
#include <vine/graphics/StateNode.hpp>
#include <vine/graphics/Texture.hpp>
#include <vine/math/Point3.hpp>
#include <vine/math/Vector3.hpp>

#include "EngineDiagnostics.hpp"

VN_GRAPHICS_NS_BEGIN

namespace detail
{

namespace
{

/**
 * @brief Builds the scene a declared background is drawn from (the definition below states the whole contract).
 *
 * @param options      The declaration (see BackgroundOptions).
 * @param material_out Receives the material holding the map, so the chain can swap the map on a live picture.
 * @return The scene the background pass draws.
 */
intrusive_ptr<Scene> makeBackgroundScene(const BackgroundOptions& options, intrusive_ptr<Material>& material_out);

} // namespace

ChainBuilder::ChainBuilder(raw_ptr<RenderEngine> engine, const void* owner)
  : engine_(engine)
  , owner_(owner)
{}

intrusive_ptr<PassChain> ChainBuilder::build(const ChainDesc& options)
{
    if (engine_ == nullptr) {
        return nullptr;
    }
    // The request is a value; the builder copies what it needs for THIS build once, so the recipes below read a
    // snapshot instead of a host's object that could change half way through.
    camera_                = options.camera;
    content_               = options.content;
    transparent_           = options.transparent;
    background_            = options.background;
    shadows_built_         = 0;
    content_layers_        = options.layer_mask;
    // The chain IS its registration: each pass is filed with it as owner, so a build that stops half way (or a host
    // that drops the handle) takes its passes back out of the frame. The handles are leases; the engine may die
    // first and the chain then answers "gone" instead of reaching into freed memory.
    auto pipeline          = make_intrusive<PassChain>(engine_->passes(), engine_->diagnostics());
    // THE SCOPE EXISTS BEFORE ANY APPLY: an overlay added during the build commits a plan of its own, so the chain must
    // know whose rows they are from its first statement (see PassChain::build(engine, desc, owner)).
    pipeline->scope_       = owner_ != nullptr ? owner_ : pipeline.get();
    pipeline->scope_owned_ = owner_ == nullptr;
    // THE BACKGROUND'S CONTENT IS BUILT BEFORE THE PLAN: the pass it is drawn by references it and the live map switch
    // writes into its material, so the chain takes both in hand here - once, rather than from inside a recipe (see
    // ChainDesc::background). It is a SCENE OF ITS OWN on purpose: content would be re-programmed by the G-buffer pass,
    // framed by the shadow fit and measured by the content bounding box (graphics-rendering.md §2.15).
    if (background_.has_value()) {
        intrusive_ptr<Material> background_material;
        background_scene_ = makeBackgroundScene(*background_, background_material);
        pipeline->setBackgroundContent(background_scene_, std::move(background_material));
    }
    // The chain the REQUEST assembles is not always the chain it names: a mode that needs the geometry pass
    // decides it (see effectivePath). The chain records what it was built as, because "can this chain
    // express that mode" is what a later setShadingMode() is answered from.
    const ShadingPath path = PassChain::effectivePath(options);
    const bool        ok   = (path == ShadingPath::Forward) ? buildForwardPath(*pipeline) : buildDeferredPath(*pipeline, options);
    if (!ok) {
        return nullptr;
    }
    pipeline->path_ = path;
    reportRequestedShadows();
    if (!applyOverlays(*pipeline, options)) {
        return nullptr;
    }
    // The build's mode is applied AFTER the commit (see PassChain::build): a mode is an override ON the picture pass,
    // so it needs the node, not the declaration.
    return pipeline;
}

namespace
{

/**
 * @brief Builds the scene a declared background is drawn from: the SDK's direction box, its program, its state.
 *
 * @details
 * - THE GEOMETRY IS A UNIT DIRECTION BOX: every vertex carries its own centre-to-vertex VECTOR as the texcoord the
 *   fragment stage samples the cube map with (three components - see Geometry::setTexcoords3), which is what makes
 *   the sampler a `samplerCube` and the interpolated value the direction of the ray leaving through that fragment.
 *   The box is a UNIT cube because the vertex stage centres it on the eye and scales it to the window: whoever draws
 *   it, its size is not part of the picture (see builtin_skybox.vert).
 * - The DIRECTION is re-expressed in the map's authored frame when the declaration states one (a map authored Y-up
 *   shown in a Z-up world). Applying the rotation here, to the channel, and not to a transform, is what keeps it a
 *   rotation of the ENVIRONMENT rather than of the box.
 * - The program and the state are the SDK's own pair (`skyboxProgram` / `skyboxRenderState`): a background drawn with
 *   one of them and not the other is the defect those two exist to make impossible (see BuiltinShaders.hpp).
 *
 * @param options      The declaration (see BackgroundOptions).
 * @param material_out Receives the material holding the map, so the chain can swap the map on a live picture.
 * @return The scene the background pass draws.
 */
intrusive_ptr<Scene> makeBackgroundScene(const BackgroundOptions& options, intrusive_ptr<Material>& material_out)
{
    const vn::geom::spatial::Triangulationf box = vn::geom::spatial::tessellateSolid(vn::geom::spatial::BoxSolidf{ 1.0F, 1.0F, 1.0F });

    std::vector<vn::math::Vec3f> directions;
    directions.reserve(box.positions.size());
    for (const vn::math::Vec3f& corner : box.positions) {
        if (options.world_to_map.has_value()) {
            const vn::math::Mat3d& to_map = *options.world_to_map;
            const auto             row    = [&corner, &to_map](std::size_t index) {
                return static_cast<float>(to_map.element(index, 0) * corner.x + to_map.element(index, 1) * corner.y + to_map.element(index, 2) * corner.z);
            };
            directions.emplace_back(row(0), row(1), row(2));
        }
        else {
            directions.emplace_back(corner.x, corner.y, corner.z);
        }
    }

    auto geometry = make_intrusive<Geometry>();
    geometry->setName(u8"skybox");
    geometry->setPositions(packAttribute(box.positions));
    geometry->setNormals(packAttribute(box.normals));
    geometry->setTexcoords3(packAttribute(directions));
    Primitive draw;
    draw.indices = Primitive::IndexSpan::whole(packIndices(box.indices));
    // setPositions states a non-indexed default draw; the indexed one REPLACES it - appending would keep the default
    // and draw the vertices as triangles across face boundaries, which smears the map across the box.
    geometry->setPrimitive(0U, std::move(draw));

    auto material = make_intrusive<Material>();
    // The sky program samples the map and multiplies nothing, so the map IS the colour; white is stated so the slot
    // says what it means rather than relying on a material default nobody reads.
    material->setBaseColor(vn::Colorf::kWhite);
    material->setTexture(TextureSlot::BaseColor, options.map);
    geometry->setMaterial(material);
    geometry->setProgram(skyboxProgram());

    // THE STATE IS CONTENT's here, not the pass': `PassRenderState::depth` is a POLICY for content that authored none
    // (see StateResolution.hpp), so a sky that let the pass decide would inherit the pass' test/write/compare and
    // write the far plane over the whole rectangle.
    auto root = make_intrusive<StateNode>();
    root->setName(u8"skybox_root");
    const RenderState state = skyboxRenderState();
    // The two items skyboxRenderState() is made of; an item added there has to be applied here too (the pair is the
    // contract, and this is the one other place that has both halves in hand).
    if (state.depth.has_value()) {
        root->setDepth(*state.depth);
    }
    if (state.cullMode.has_value()) {
        root->setCullMode(*state.cullMode);
    }
    root->addChild(geometry);

    auto scene = make_intrusive<Scene>();
    scene->setName(u8"skybox");
    scene->setRoot(root);
    material_out = std::move(material);
    return scene;
}

} // namespace

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
        if (options.gizmo.box_size > 0) {
            gizmo->setBoxSize(options.gizmo.box_size);
        }
        // One style, one rebuild: the two scalars the options carry are the marker's length and its widest radius.
        AxesStyle style;
        style.length       = options.gizmo.axis_length;
        style.tip_radius   = options.gizmo.thickness;
        style.shaft_radius = style.tip_radius * kAxesShaftToTipRatio;
        gizmo->setStyle(style);

        // The gizmo is CONTENT (its scene, its framing camera, its box); the pass that draws it is a
        // declaration like every other one - which is why the name and the HUD placement are stated here instead
        // of being inherited from a pass subclass (see PassDesc). The per-frame work is the object's own
        // (FrameUpdate: it mirrors the source camera when the chain updates its overlays).
        PassDesc desc;
        desc.name          = u8"axis_gizmo";
        desc.stage         = PassStage::Overlay;
        desc.order         = overlay_base + options.gizmo.order;
        desc.camera        = intrusive_ptr<Camera>(gizmo->camera());
        desc.content       = intrusive_ptr<Scene>(gizmo->content());
        desc.viewport      = gizmo->boxViewport();
        // A HUD OWNS THE DEPTH INSIDE ITS BOX: the pass keeps the colour it is drawn over and resets only the depth in
        // its rectangle, then tests and writes - so the marker's arrows occlude each other like the solids they are,
        // while the scene underneath is never occluded by them. The two halves are independent on purpose (see
        // AttachmentPolicy); stating only `depth_policy` would test the arrows against the SCENE's depth, which has nothing to
        // do with their own.
        desc.clear_enabled = false;
        desc.clear_depth   = true;
        desc.depth_policy    = DepthPolicy::TestAndWrite;
        // Registered through the same public entry a host uses (see PassChain::addOverlay): the recipe states the
        // overlay's CONTENT, the label its pass carries, and how it re-anchors itself - a third overlay is another
        // call, not a change to PassChain. The layout ANSWERS the rectangle the pass draws into.
        pipeline.addOverlay(String(PassChain::kGizmoRole), gizmo.get(), desc, [gizmo](int width, int height, double ratio) {
            gizmo->setPixelRatio(ratio);
            gizmo->onSurfaceResized(width, height);
            return gizmo->boxViewport();
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
        desc.stage         = PassStage::Overlay;
        desc.order         = overlay_base + options.fps.order;
        desc.camera        = intrusive_ptr<Camera>(fps->camera());
        desc.content       = intrusive_ptr<Scene>(fps->content());
        desc.viewport      = fps->boxViewport();
        desc.clear_enabled = false;
        desc.depth_policy    = DepthPolicy::Disabled;
        // The readout measures and repaints itself when the chain updates its overlays (FrameUpdate).
        pipeline.addOverlay(String(PassChain::kStatsRole), fps.get(), desc, [fps](int width, int height, double ratio) {
            fps->setPixelRatio(ratio);
            fps->onSurfaceResized(width, height);
            return fps->boxViewport();
        });
    }
    return true;
}

void ChainBuilder::reportRequestedShadows() const
{
    // A shadow is asked for by the LIGHT (Light::isCastingShadow + ShadowSettings) - that is where the request lives,
    // and it outlives any one pipeline. What this reports is the request the built-in chain did NOT honour, and it
    // has exactly THREE reasons - counted apart, because "N lights cast nothing" cannot be told from a mistyped
    // setting:
    //
    //   1. the chain builds ONE map, so a SECOND caster has none (by design, see graphics-rendering.md N8);
    //   2. a POINT light cannot be put in that map at all: it needs six faces of one (backlog S6);
    //   3. a path with no lit shading to shadow - the host's own lighting program, or a mode that does not shade -
    //      builds no pass even for the first caster.
    //
    // A report that fires whenever a request EXISTS - what this did while the deferred path already built the pass -
    // tells the host its picture is unshadowed while it is shadowed, which is worse than saying nothing.
    std::size_t buildable       = 0; // enabled casters of a kind the chain CAN build a 2D map for
    std::size_t unrepresentable = 0; // enabled casters no 2D map represents (a point light)
    for (const Scene* scene : { content_.get(), transparent_.get() }) {
        if (scene == nullptr) {
            continue;
        }
        for (const auto& light : scene->lights()) {
            if (light == nullptr || !light->isEnabled() || !light->isCastingShadow()) {
                continue;
            }
            if (light->hasDirection()) {
                ++buildable;
            }
            else {
                ++unrepresentable;
            }
        }
    }

    // The integer goes in as ASCII digits, like every other message in the SDK.
    const auto report = [this](const std::string& message) {
        engine_->reportEngineProblem(
            detail::engineProblem(u8"engine.shadow.not_built", vn::graphics::DiagnosticCategory::UnsupportedRequest, String::fromUtf8(message)));
    };

    if (buildable > shadows_built_) {
        const std::string unbuilt = std::to_string(buildable - shadows_built_);
        if (shadows_built_ == 0U) {
            report(unbuilt + " shadow-casting light(s) have no shadow pass: this chain's path builds none (its"
                             " lighting program is the host's, or its shading mode has nothing to shade from). See"
                             " .ai/design/graphics-rendering.md §2.7");
        }
        else {
            report(unbuilt + " shadow-casting light(s) are not built: this chain builds ONE shadow map, for the"
                             " first enabled directional or spot light of its content, so a second caster stays"
                             " unshadowed. See .ai/design/graphics-rendering.md §2.7 and §6 N8");
        }
    }
    if (unrepresentable > 0U) {
        const std::string points = std::to_string(unrepresentable);
        report(points + " shadow-casting point light(s) cast nothing in this chain: a point light's shadow is six faces"
                        " of one map, which this pipeline does not build (backlog S6). See"
                        " .ai/design/graphics-rendering.md §6 N8");
    }
}

namespace
{

/**
 * @brief Finds the light a content scene asks a shadow for.
 *
 * Single caster per content, as the shadow design fixes it (graphics-rendering.md §2.7 / §6 N8): the
 * FIRST enabled, shadow-casting light of a casting kind is the one the pass is built for, and the rest
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
    // The kinds that HAVE a pass today: a directional light gets an orthographic light camera, a spot a perspective
    // one taken from its cone. A POINT light has no pass yet (it needs six faces; see backlog S6) and is reported
    // rather than silently ignored.
    for (const auto& light : content->lights()) {
        if (light != nullptr && light->isEnabled() && light->isCastingShadow() && light->hasDirection()) {
            return light; // a strong handle: the map holds its light (RenderTarget::setShadowOf)
        }
    }
    return nullptr;
}

} // namespace

intrusive_ptr<RenderTarget> ChainBuilder::buildShadowPass(PassChain& pipeline, const intrusive_ptr<const Light>& shadow_light_owner)
{
    // The map keeps the light it belongs to alive (RenderTarget::setShadowOf): a retained target can
    // outlive the scene that built it, and a consumer reads the light through the map.
    const Light& shadow_light = *shadow_light_owner;
    const int    resolution   = static_cast<int>(shadow_light.shadowSettings().resolution);
    auto         shadow_map   = make_intrusive<RenderTarget>();
    shadow_map->setName(u8"shadow_map");
    shadow_map->setSize(resolution > 0 ? resolution : 1024, resolution > 0 ? resolution : 1024);
    shadow_map->attachDepth(RenderTarget::DepthFormat::D24);
    // The shading samples it, so its depth must end in SHADER_READ_ONLY (a pass of it that
    // PRESERVED depth would revoke that, which is why the shadow pass clears instead).
    shadow_map->setDepthPromotion(true);

    auto light_camera = make_intrusive<Camera>();
    // ONE derivation of the light camera: the pass renders through this camera and the target STATES its
    // view-projection, so the shading reads the same matrix instead of fitting a second one of its own (see
    // .ai/design/graphics-rendering.md §2.7). A directional light gets the orthographic fit, a spot the
    // perspective one taken from its cone - the shading does not care which, it maps through a matrix.
    // The map states what it IS, not who reads it: a consumer finds the shadow by asking the declared
    // targets whose shadow they are (RenderTarget::setShadowOf), so nothing is inferred from declaration
    // order and RenderPass stays a generic stage.
    shadow_map->setShadowOf(shadow_light_owner);
    const vn::geom::spatial::Aabbd shadow_bounds = content_->boundingBox();
    shadow_map->setProducerViewProjection(shadow_light.type() == LightType::Spot ? spotShadowMatrix(shadow_light, shadow_bounds, *light_camera)
                                                                                 : directionalShadowMatrix(shadow_light, shadow_bounds, *light_camera));

    PassDesc desc;
    desc.name          = u8"shadow";
    desc.stage         = PassStage::Depth;
    desc.camera        = light_camera;
    desc.content       = content_;
    // The recipe's content selection, on every pass that walks this content (see PipelineOptions::layer_mask):
    // what the picture does not show does not cast either, which is the whole point of drawing one content
    // through several passes.
    desc.layer_mask    = content_layers_;
    desc.target        = Target::explicitTarget(shadow_map);
    // WHAT MAY OCCLUDE: only the content the sort leaves in its opaque queue (see PassSelection). A map records a
    // DISTANCE, so a surface whose coverage is a fraction of its fragment cannot be written into it - the depth of a
    // blended quad would shadow everything under the parts its colour only tints. The criterion is the sort's own, so
    // the queue, the blend state and "does this cast" are one answer; a Mask material still casts, and its cut-out
    // reaches the map through the alpha threshold the depth stage applies (graphics-rendering.md §2.7).
    desc.selection           = PassSelection::Opaque;
    // A DEPTH MAP HAS NO COLOUR ATTACHMENT, so the pass draws with the depth-only program instead of whatever each
    // drawable's own program is: the content program writes location 0, and a fragment output nobody can see is a
    // validation warning plus per-fragment shading work no one reads. The override is the pass's statement "everything
    // here writes depth and nothing else" (see BuiltinShaders::depthOnlyProgram), and it leaves the drawables - their
    // geometry, their variants, their matrices - untouched.
    desc.program_override = depthOnlyProgram();
    // Its only writer owns the hand-off: the pass that samples it declares this target as an input,
    // and the engine answers that from the passes that DREW into it.
    desc.output.whole_target = true;
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
    // deferred path builds (one implementation, one light camera). The map STATES whose shadow it is and how
    // to read it (`RenderTarget::setShadowOf` + `setProducerViewProjection`), the backend resolves that from
    // the declared input and packs the block - the pass model itself has no shadow field (see
    // .ai/design/graphics-rendering.md §2.7).
    intrusive_ptr<RenderTarget> shadow_map;
    if (intrusive_ptr<const Light> shadow_light = requestedShadowLight(content_.get()); shadow_light != nullptr) {
        shadow_map = buildShadowPass(pipeline, shadow_light);
    }
    // The forward content pass IS the lit result, so it lives at the shading
    // stage; there is no separate geometry pass to place.
    PassDesc main_desc;
    main_desc.name       = u8"main";
    main_desc.stage      = PassStage::Shading;
    main_desc.camera     = intrusive_ptr<Camera>(camera_);
    main_desc.content    = content_;
    main_desc.layer_mask = content_layers_;
    if (shadow_map != nullptr) {
        main_desc.inputs = { ImageInput::byTarget(shadow_map) };
    }
    pipeline.addPass(main_desc);
    // THE BACKGROUND, when the request declares one: right after the shaded result and before any forward-only
    // content, so that content blends onto the sky instead of floating over an empty pixel (see backgroundPassDesc).
    // It draws into the window, against the depth the main pass just wrote.
    if (background_.has_value()) {
        pipeline.addPass(backgroundPassDesc(nullptr, nullptr), String(PassChain::kBackgroundRole));
    }
    // Optional transparent content: a depth-on pass stacked right after the
    // main content in the SAME window render pass, so it occludes against the
    // opaque depth the main pass just wrote (see setTransparentContent).
    if (transparent_ != nullptr) {
        PassDesc transparent_desc;
        transparent_desc.name          = u8"forward_transparent";
        transparent_desc.stage         = PassStage::Transparent;
        transparent_desc.camera        = camera_;
        transparent_desc.content       = transparent_;
        transparent_desc.layer_mask    = content_layers_;
        // Translucent / overlay content: depth-TESTS against the opaque depth
        // the main content pass just wrote, but does NOT write depth (standard
        // alpha-blend rule) and does not clear.
        transparent_desc.clear_enabled = false;
        transparent_desc.depth_policy    = DepthPolicy::TestOnly;
        pipeline.addPass(transparent_desc);
        // The chain owns the extra scene from here on (see PassChain::publishContent): the content the HOST owns is
        // the host's to publish, and this one has no other owner.
        pipeline.setTransparentContent(transparent_);
    }
    pipeline.setWindowPass(main_desc.name);
    return true;
}

PassDesc ChainBuilder::backgroundPassDesc(const intrusive_ptr<RenderTarget>& target, const intrusive_ptr<RenderTarget>& depth_owner) const
{
    PassDesc desc;
    desc.name          = u8"skybox";
    desc.stage         = PassStage::Transparent;
    // BEFORE every other forward-only pass: the content that blends over the picture has to blend over the SKY, so
    // the background is the first thing among them (the stage states the placement, the order the order inside it).
    desc.order         = passStageOrder(PassStage::Transparent) - 1;
    desc.camera        = intrusive_ptr<Camera>(camera_);
    desc.content       = background_scene_;
    // It keeps the picture the shading just wrote, and it writes no depth: the box's own state is where the test and
    // the missing write come from (see skyboxRenderState), and both clear flags stay false so neither attachment is
    // touched before it draws.
    desc.clear_enabled = false;
    desc.depth_policy  = DepthPolicy::TestOnly;
    // A background whose environment has not arrived draws nothing yet: the row and the scene stay, so the map can be
    // installed without re-registering the picture's passes (see PassChain::setBackgroundMap).
    desc.enabled       = background_.has_value() && background_->map != nullptr;
    if (target != nullptr) {
        desc.target     = Target::explicitTarget(target);
        // The composite has no depth of its own (see DepthView): what this pass tests against is the G-buffer's - the
        // same edge the lighting pass and the transparent pass state.
        desc.depth_view = depthViewOf(depth_owner);
    }
    return desc;
}

bool ChainBuilder::buildDeferredPath(PassChain& pipeline, const ChainDesc& options)
{
    if (engine_ == nullptr || camera_ == nullptr || content_ == nullptr) {
        return false;
    }
    // The light this pipeline must cast a shadow for, if any: the request lives on the LIGHT
    // (Light::isCastingShadow, with its own resolution/bias in ShadowSettings), because a light outlives
    // any one pipeline and a scene may be drawn by several. Single directional light per content,
    // as the shadow design fixes it (graphics-rendering.md §8).
    intrusive_ptr<const Light> shadow_light = requestedShadowLight(content_.get());

    // Programs default to the built-in temporary shaders so the preset works
    // out of the box; explicit programs in the options override them.
    intrusive_ptr<ShaderProgram> gbuf_program  = options.gbuffer_program;
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
        width  = engine_->areaWidth();
        height = engine_->areaHeight();
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

    // Canonical G-buffer (shared factory): base colour (0), the octahedral
    // shading normal with roughness / metallic beside it (1), emission with
    // occlusion in alpha (2), view position (3) and depth. The geometry
    // program must write exactly these outputs (shaders/vine_gbuffer.glsl
    // states the packing).
    auto gbuffer = defaultGbufferTarget(width, height);

    // G-buffer geometry pass (order < 0), publishing the target as "GBuffer".
    PassDesc gbuf_desc;
    gbuf_desc.name             = u8"gbuffer";
    gbuf_desc.stage            = PassStage::Geometry;
    gbuf_desc.camera           = camera_;
    gbuf_desc.content          = content_;
    gbuf_desc.layer_mask       = content_layers_;
    gbuf_desc.target           = Target::explicitTarget(gbuffer);
    // The program is a SCENE pass' override (`PassDesc::program_override`): `PassDesc::program` is the
    // full-screen field a full-screen pass draws through, so naming it here left the G-buffer pass with no program at
    // all - every body then drew with its own (or the engine's forward) program, which writes ONE colour output:
    // the geometry landed in albedo and the normal / specular / position attachments kept their transparent-black
    // clear, so the lighting read nothing and a PiP of either of those three was black.
    gbuf_desc.program_override = std::move(gbuf_program);
    gbuf_desc.output.name      = u8"GBuffer";
    // The same wire, declared at object level: this pass is the G-buffer's ONLY writer, so it owns the
    // hand-off and promises the whole target (one line instead of one per colour attachment — the
    // promise is shape-agnostic). The name above still drives the per-frame registry (design §14).
    gbuf_desc.output.whole_target    = true;
    pipeline.addPass(gbuf_desc);

    // Fullscreen deferred lighting. With NO forward-only content it runs at order 0 as the WINDOW pass that presents
    // the view camera (so RenderControl / SceneView add no forward pass). With forward-only content - a background, a
    // transparent scene, or both - the lit result has to be baked off-screen with it, because that content TESTS
    // DEPTH and the depth it has to test against is the G-buffer's: the same program shades into a composite target
    // instead.
    const bool has_forward_only = transparent_ != nullptr || background_.has_value();
    if (!has_forward_only) {
        PassDesc light_desc;
        light_desc.name          = u8"deferred_lighting";
        light_desc.stage         = PassStage::Shading;
        light_desc.kind          = DrawKind::Fullscreen;
        light_desc.camera        = intrusive_ptr<Camera>(camera_);
        light_desc.content       = content_;
        // It reads the WHOLE G-buffer: a fullscreen program receives every colour attachment of its
        // source (plus its depth while that one is sampleable) and picks by binding, so the unit it
        // consumes is the target - one declaration (byTarget), and the first produced input is the source.
        light_desc.inputs        = { ImageInput::byTarget(gbuffer) };
        if (shadow_map != nullptr) {
            // The map goes in as an ordinary INPUT, and its own statement is what makes it a shadow: the
            // target names its light and publishes how to read it (`RenderTarget::setShadowOf` /
            // `setProducerViewProjection`). The shadow ABI puts its two bindings right after the source's own
            // (see BuiltinShaders::deferredLightProgram), and the engine's plan has no shadow field to fill.
            light_desc.inputs.push_back(ImageInput::byTarget(shadow_map));
        }
        light_desc.program                    = std::move(light_program);
        pipeline.addPass(light_desc);

        pipeline.setOffscreenTarget(std::move(gbuffer));
        pipeline.setWindowPass(light_desc.name);
        return true;
    }

    // THE DEFERRED + FORWARD COMPOSITE: one off-screen composite target whose single render pass stacks the passes in
    // record order - fullscreen lighting (0) -> background (+1) -> forward transparent content (+2). It does NOT own a
    // depth buffer: its passes draw against the G-buffer's depth (a DepthView edge, stated by each pass that draws into
    // it), so the opaque scene is rasterised once and the forward-only content tests against that same depth. The
    // G-buffer's depth is therefore kept as an attachment (not promoted to a sampled texture) here.
    gbuffer->setDepthPromotion(false);
    auto composite = make_intrusive<RenderTarget>();
    composite->setName(u8"composite");
    composite->setSize(width, height);
    composite->attachColor(RenderTarget::ColorFormat::RGBA8);

    // Fullscreen deferred lighting (order 0) INTO the composite: a depth-off
    // program that overwrites every pixel with the lit opaque result.
    PassDesc light_desc;
    light_desc.name          = u8"deferred_lighting";
    light_desc.stage         = PassStage::Shading;
    light_desc.kind          = DrawKind::Fullscreen;
    light_desc.camera        = intrusive_ptr<Camera>(camera_);
    light_desc.content       = content_;
    light_desc.target        = Target::explicitTarget(composite);
    light_desc.inputs        = { ImageInput::byTarget(gbuffer) }; // reads the whole G-buffer (see the comment on the program path)
    if (shadow_map != nullptr) {
        light_desc.inputs.push_back(ImageInput::byTarget(shadow_map));
    }
    light_desc.program    = std::move(light_program);
    // The edge, stated by the pass that draws against it (see DepthView): the composite has no depth of its
    // own, so what it tests against is the G-buffer's.
    light_desc.depth_view = depthViewOf(gbuffer);

    // The forward-only passes that stack over the lit result, in the order they are drawn.
    std::vector<PassDesc> composite_writers;
    composite_writers.push_back(std::move(light_desc));
    // The background first among them: what the transparent content blends over is the sky, not an empty pixel.
    std::size_t background_writer = composite_writers.size();
    if (background_.has_value()) {
        background_writer = composite_writers.size();
        composite_writers.push_back(backgroundPassDesc(composite, gbuffer));
    }
    // Forward transparent / overlay content: depth-TESTS against the G-buffer depth the composite loaded (TestOnly -
    // it never writes depth, the standard translucent rule) and blends per material over the lit result.
    if (transparent_ != nullptr) {
        PassDesc transparent_desc;
        transparent_desc.name          = u8"forward_transparent";
        transparent_desc.stage         = PassStage::Transparent;
        transparent_desc.camera        = intrusive_ptr<Camera>(camera_);
        transparent_desc.content       = transparent_;
        transparent_desc.layer_mask    = content_layers_;
        transparent_desc.target        = Target::explicitTarget(composite);
        transparent_desc.clear_enabled = false;
        transparent_desc.depth_policy  = DepthPolicy::TestOnly;
        transparent_desc.depth_view    = depthViewOf(gbuffer);
        composite_writers.push_back(std::move(transparent_desc));
    }
    // The composite's LAST writer owns the hand-off (the passes above draw into the same target but promise nothing):
    // promising on two would be a collision report, and the collision report is exactly about two passes both claiming
    // to be the source of one image.
    composite_writers.back().output.name         = u8"Composite";
    composite_writers.back().output.whole_target = true;
    for (std::size_t index = 0; index < composite_writers.size(); ++index) {
        // The background is registered under its role, so a live map switch can find it (see setBackgroundMap).
        pipeline.addPass(composite_writers[index], index == background_writer ? String(PassChain::kBackgroundRole) : String());
    }
    if (transparent_ != nullptr) {
        pipeline.setTransparentContent(transparent_); // see PassChain::publishContent
    }

    // Present the baked composite to the window (order 2). Carrying the view
    // camera with a null render target is what makes it a window pass; a host
    // that built this pipeline declares it as the surface's presentation
    // (RenderArea::present), which is what keeps a default forward pass from
    // being added beside it. The HUD overlays (orders 10 / 30) stack above it.
    PassDesc present_desc;
    present_desc.name                       = u8"present";
    present_desc.stage                      = PassStage::Present;
    present_desc.kind                       = DrawKind::Fullscreen;
    present_desc.camera                     = intrusive_ptr<Camera>(camera_);
    // The recipe names the copy program: the SDK's own passes have no implicit shading either, so
    // "present the baked target" is an SDK program (BuiltinShaders::fullscreenCopyProgram) rather than a
    // backend-side default (see FullscreenPass::setProgram).
    present_desc.program                    = fullscreenCopyProgram();
    present_desc.inputs                     = { ImageInput::byTarget(composite) }; // it presents the whole baked target
    pipeline.addPass(present_desc);

    pipeline.setOffscreenTarget(std::move(gbuffer));
    pipeline.setCompositeTarget(std::move(composite));
    pipeline.setWindowPass(present_desc.name);
    return true;
}

} // namespace detail

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
    return build(engine, desc, nullptr);
}

intrusive_ptr<PassChain> PassChain::build(raw_ptr<RenderEngine> engine, const ChainDesc& desc, const void* owner)
{
    // THE RECIPE STATES, THE COMMIT APPLIES: the builder assembles declarations, one apply states them as the SCOPE's
    // rows (a whole plan, so a pass reading a name another pass of the chain publishes validates), and the plan's
    // refusals are the build's answer (reported once, see PassChain::apply).
    intrusive_ptr<PassChain> chain = detail::ChainBuilder(engine, owner).build(desc);
    if (chain == nullptr) {
        return nullptr;
    }
    if (!chain->apply()) {
        return nullptr;
    }
    // The build's mode lands as the same override a live switch applies (one place knows what a mode means), and it
    // always succeeds here: the chain was chosen to be able to express it.
    (void)chain->setShadingMode(desc.mode);
    return chain;
}

VN_GRAPHICS_NS_END
