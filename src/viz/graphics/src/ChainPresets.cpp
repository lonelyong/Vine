#include <vine/graphics/ChainPresets.hpp>

#include <algorithm>
#include <limits>

#include <vine/graphics/BuiltinShaders.hpp>
#include <vine/graphics/Camera.hpp>
#include <vine/graphics/Light.hpp>
#include <vine/graphics/PassDesc.hpp>
#include <vine/graphics/RenderEngine.hpp>
#include <vine/graphics/RenderPass.hpp>
#include <vine/graphics/Scene.hpp>
#include <vine/graphics/ScreenPass.hpp>
#include <vine/graphics/ShaderProgram.hpp>

VN_GRAPHICS_NS_BEGIN

intrusive_ptr<ShaderProgram> defaultGbufferGeometryProgram()
{
    return gbufferGeometryProgram();
}

intrusive_ptr<ShaderProgram> defaultDeferredLightProgram()
{
    return deferredLightProgram();
}

intrusive_ptr<RenderTarget> defaultGbufferTarget(int width, int height)
{
    if (width <= 0) {
        width = 640;
    }
    if (height <= 0) {
        height = 360;
    }
    auto gbuffer = make_intrusive<RenderTarget>();
    gbuffer->setSize(width, height);
    // att 0: albedo - HALF FLOAT, not 8 bit (D5, wired 2026-09-25). What the extra bits buy was measured END
    // TO END, because the display is 8 bit too: with a DIM light (x0.5, the demo's regime) the output is
    // identical either way - the output's own quantisation is the limit (both: 1 LSB of error, 9 levels over a
    // 0..0.06 ramp) - while with a BRIGHT one (x4) the 8-bit albedo's step reaches the eye multiplied: 2 LSB of
    // error and 16 levels against 1 LSB and 62 levels for the half-float store (see ContentPassTest's
    // `MeasureWhatAHalfFloatAlbedoWouldBuyEndToEnd`). So this is future-proofing for hosts that light with an
    // intensity above 1, at +8.3 MB per 1080p G-buffer and one recompile of the offscreen pipelines (the shape
    // is part of their key). Trade-off worth stating: the colour read-back packs RGBA8 only, so this attachment
    // can no longer be read back as a target attachment - the previews copy it through a program, which is how
    // the demo shows it.
    gbuffer->attachColor(RenderTarget::ColorFormat::RGBA16F);
    gbuffer->attachColor(RenderTarget::ColorFormat::RGBA16F); // att 1: view normal (+ shininess)
    gbuffer->attachColor(RenderTarget::ColorFormat::RGBA8);   // att 2: specular
    gbuffer->attachColor(RenderTarget::ColorFormat::RGBA16F); // att 3: view position
    gbuffer->attachDepth(RenderTarget::DepthFormat::D24);
    gbuffer->setName(u8"gbuffer");
    return gbuffer;
}

Mat4d directionalShadowMatrix(const Light& light, const vn::math::Aabbd& bounds, Camera& camera)
{
    using vn::math::Vec3d;

    const bool valid = bounds.max().x >= bounds.min().x && bounds.max().y >= bounds.min().y &&
                       bounds.max().z >= bounds.min().z;
    const Vec3d extent = valid ? Vec3d(bounds.max().x - bounds.min().x, bounds.max().y - bounds.min().y,
                                       bounds.max().z - bounds.min().z)
                               : Vec3d(2.0, 2.0, 2.0);
    const Vec3d centre = valid ? Vec3d(bounds.center().x, bounds.center().y, bounds.center().z) : Vec3d(0.0, 0.0, 0.0);
    // Every orientation of the box fits in this sphere, so the window does not shrink when the sun
    // moves; the margin keeps a caster standing exactly on the border out of the depth clamp.
    const double radius  = extent.length() * 0.5;
    const double depth   = radius * 4.0 + 1.0;
    const Vec3d  forward = light.direction().length() > 1e-9 ? light.direction().normalized() : Vec3d(0.0, 0.0, -1.0);
    // Any up vector that is not parallel to the direction: a light is a direction, not a roll.
    const Vec3d up  = std::abs(forward.z) > 0.9 ? Vec3d(0.0, 1.0, 0.0) : Vec3d(0.0, 0.0, 1.0);
    const Vec3d eye = centre - forward * (radius * 2.0 + 1.0);

    camera.lookAt(eye, eye + forward, up);
    camera.setOrthographic(-radius, radius, -radius, radius, 0.0, depth);
    // Orthographic: the window above carries the shape, and the matrix does not read the camera's aspect at all.
    // The shadow map is square anyway (the window is -radius..radius on both axes).
    return camera.projectionMatrix() * camera.viewMatrix();
}

Mat4d spotShadowMatrix(const Light& light, const vn::math::Aabbd& bounds, Camera& camera)
{
    using vn::math::Vec3d;

    const bool valid = bounds.max().x >= bounds.min().x && bounds.max().y >= bounds.min().y &&
                       bounds.max().z >= bounds.min().z;
    const Vec3d  forward = light.direction().length() > 1e-9 ? light.direction().normalized() : Vec3d(0.0, 0.0, -1.0);
    const Vec3d  eye     = light.position();
    // Any up vector that is not parallel to the direction: a light is a direction, not a roll.
    const Vec3d  up      = std::abs(forward.z) > 0.9 ? Vec3d(0.0, 1.0, 0.0) : Vec3d(0.0, 0.0, 1.0);

    // How far the content is from the LIGHT, along its axis: a spot has a position, so a fitted sphere around the
    // whole scene would be the wrong question. An invalid box keeps a usable frustum instead of a degenerate one.
    double axis_min = 0.0;
    double axis_max = 100.0;
    if (valid)
    {
        axis_min = std::numeric_limits<double>::max();
        axis_max = std::numeric_limits<double>::lowest();
        for (int corner = 0; corner < 8; ++corner)
        {
            const Vec3d point((corner & 1) != 0 ? bounds.max().x : bounds.min().x,
                              (corner & 2) != 0 ? bounds.max().y : bounds.min().y,
                              (corner & 4) != 0 ? bounds.max().z : bounds.min().z);
            const double distance = (point - eye).dot(forward);
            axis_min              = std::min(axis_min, distance);
            axis_max              = std::max(axis_max, distance);
        }
        const double margin = std::max(1e-3, (axis_max - axis_min) * 0.01);
        axis_min -= margin;
        axis_max += margin;
    }
    // The eye may sit inside the bounds: the near plane then clamps to a small positive distance rather than to zero
    // (a zero near plane is a degenerate projection).
    const double near_plane = std::max(axis_min, 0.01);
    const double far_plane  = std::max(axis_max, near_plane + 1.0);

    // The cone the map covers is the spot's OUTER cone plus a small margin: the map edge then sits where the light is
    // already at zero strength (see vineEvaluateLight), so nothing fades at the map's border.
    const double half_angle = std::clamp(light.outerConeAngle() * 1.02, 0.5, 85.0);
    const double fov_y      = 2.0 * half_angle;

    camera.lookAt(eye, eye + forward, up);
    camera.setPerspective(fov_y, near_plane, far_plane);
    // The map is square and the shadow pass draws the whole target, so the aspect the camera derives from its pass
    // rectangle is 1 and the horizontal extent equals the vertical one.
    return camera.projectionMatrix() * camera.viewMatrix();
}

raw_ptr<ScreenPass> addOffscreenToScreen(raw_ptr<RenderEngine>  engine,
                                         raw_ptr<Camera>        camera,
                                         intrusive_ptr<Scene>   content,
                                         const String&          output_slot,
                                         int                    rt_width,
                                         int                    rt_height,
                                         RenderTarget::ColorFormat color_format,
                                         RenderTarget::DepthFormat depth_format,
                                         const Viewport&        pip)
{
    if (engine == nullptr || camera == nullptr) {
        // Scene-pass recipes need a view camera; a preview without one has nothing to draw through.
        return nullptr;
    }

    // Off-screen target + an order < 0 scene pass that renders into it and publishes the result under the slot name.
    auto target = make_intrusive<RenderTarget>();
    target->setName(output_slot);
    target->setSize(rt_width, rt_height);
    target->attachColor(color_format);
    target->attachDepth(depth_format);

    // Registered on the engine, NOT through a chain: the engine keeps this pass alive and no chain owns it, so it is
    // only addressable through clearPasses() (see the declaration).
    PassDesc preview_desc;
    preview_desc.name          = output_slot;
    preview_desc.tag           = u8"Preview";
    preview_desc.stage         = PassStage::Geometry;
    preview_desc.camera        = intrusive_ptr<Camera>(camera);
    preview_desc.content       = std::move(content);
    preview_desc.writes        = target;
    preview_desc.output_name   = output_slot;
    preview_desc.output_target = target;   // its only writer: it owns the hand-off
    engine->addPass(preview_desc);

    // The order > 0 ScreenPass sampling the slot into the PiP sub-viewport. Its picture is the SDK's copy program,
    // and it carries the view camera because a fullscreen program is drawn through the pass' view.
    PassDesc pip_desc;
    pip_desc.name   = output_slot;
    pip_desc.tag    = u8"Preview";
    pip_desc.stage  = PassStage::Preview;
    pip_desc.kind   = PassKind::Screen;
    pip_desc.camera = intrusive_ptr<Camera>(camera);
    // The recipe names the copy program: the SDK's own passes have no implicit shading either, so "present the baked
    // target" is an SDK program (BuiltinShaders::screenCopyProgram) rather than a backend-side default.
    pip_desc.program       = screenCopyProgram();
    pip_desc.input_names   = { output_slot };
    pip_desc.input_targets = { target };   // samples the whole published target
    pip_desc.viewport      = pip;
    // The node is the handle this function hands back, and a Screen declaration is a ScreenPass by construction.
    const intrusive_ptr<RenderPass> screen = engine->addPass(pip_desc);

    return static_cast<ScreenPass*>(screen.get());
}

VN_GRAPHICS_NS_END
