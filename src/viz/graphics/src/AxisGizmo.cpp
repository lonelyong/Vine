#include <vine/graphics/AxisGizmo.hpp>

#include <utility>

#include <vine/graphics/Camera.hpp>
#include <vine/graphics/CameraMirror.hpp>
#include <vine/graphics/Geometry.hpp>
#include <vine/graphics/Group.hpp>
#include <vine/graphics/Light.hpp>
#include <vine/graphics/Scene.hpp>

#include "PixelGeometry.hpp"

VN_GRAPHICS_NS_BEGIN

VN_OBJECT_META_IMPL(AxisGizmo, Object);

namespace
{

/// Ambient floor of the marker's own rig: an arrow's colour stays readable where the key light does not reach.
constexpr float kAmbientIntensity = 0.45f;
/// Key light of the marker's own rig: enough shading to read a shaft as a cylinder and a tip as a cone.
constexpr float kKeyIntensity = 0.75f;

} // namespace

AxisGizmo::AxisGizmo()
{
    // Framing camera: square perspective that fits the unit marker at the
    // mirror distance. The view is overwritten every frame by Orientation
    // mirroring; only the framing distance and projection are kept. Owned by
    // this gizmo so the pass' raw camera pointer never dangles.
    camera_ = make_intrusive<Camera>();
    // Framing distance chosen so the unit marker fills most of the square
    // sub-viewport (half-height = 3.3 * tan(22.5) ~ 1.37 world units maps a
    // length-1 arrow to ~73% of the half box).
    camera_->lookAt(vn::math::Vec3d(0.0, 0.0, 3.3), vn::math::Vec3d(0.0, 0.0, 0.0), vn::math::Vec3d(0.0, 1.0, 0.0));
    camera_->setPerspective(45.0, 0.05, 20.0);
    // The picture's shape is the camera's (see Camera), and the box this camera frames is a SQUARE
    // (see onSurfaceResized) - so it is 1:1, stated here where the camera is configured rather than left to
    // whatever the rectangle it ends up drawing into happens to be.
    camera_->setAspect(1.0);

    // The marker carries its OWN light rig: a HUD overlay is drawn in its own scene, whose only other source of light
    // would be the backends' flat ambient fallback - which leaves the arrows as colour silhouettes, with no shading
    // to read a shaft as a cylinder or a tip as a cone. The key light follows the camera (see applyMirror), so the
    // face the camera sees is the lit one from every orbit.
    ambient_   = Light::createAmbient();
    ambient_->setIntensity(kAmbientIntensity);
    key_light_ = Light::createDirectional(vn::math::Vec3d(0.0, 0.0, -1.0));
    key_light_->setIntensity(kKeyIntensity);

    rebuild();
}

AxisGizmo::~AxisGizmo() = default;

raw_ptr<Camera> AxisGizmo::camera() const
{
    return camera_.get();
}

Viewport AxisGizmo::boxViewport() const
{
    return box_;
}

void AxisGizmo::setAxisLength(double length)
{
    if (length > 0.0 && length != style_.length) {
        style_.length = length;
        rebuild();
    }
}

void AxisGizmo::setThickness(double thickness)
{
    if (thickness > 0.0 && thickness != style_.tip_radius) {
        // An arrow's widest radius is its cone's; the shaft keeps the share a default marker gives it, so a thicker
        // marker stays an arrow rather than becoming a spike on a hairline.
        style_.tip_radius   = thickness;
        style_.shaft_radius = thickness * kAxesShaftToTipRatio;
        rebuild();
    }
}

void AxisGizmo::setStyle(const AxesStyle& style)
{
    if (style != style_) {
        style_ = style;
        rebuild();
    }
}

const AxesStyle& AxisGizmo::axesStyle() const
{
    return style_;
}

void AxisGizmo::setBoxSize(int size)
{
    if (size > 0 && size != size_px_) {
        size_px_ = size;
        onSurfaceResized(surface_w_, surface_h_);
    }
}

void AxisGizmo::setPixelRatio(double ratio)
{
    if (ratio > 0.0 && ratio != pixel_ratio_) {
        pixel_ratio_ = ratio;
        onSurfaceResized(surface_w_, surface_h_);
    }
}

void AxisGizmo::setSourceCamera(raw_ptr<Camera> camera)
{
    source_camera_ = camera;
}

raw_ptr<Camera> AxisGizmo::sourceCamera() const
{
    return source_camera_;
}

raw_ptr<Scene> AxisGizmo::content() const
{
    return content_.get();
}

void AxisGizmo::onSurfaceResized(int width, int height)
{
    surface_w_ = width;
    surface_h_ = height;
    if (surface_w_ <= 0 || surface_h_ <= 0) {
        return;
    }
    // Convert the host's logical surface size into device pixels, which is the space the backend's native
    // surface uses for viewports. The box and its margin are DEVICE pixels (the same unit the readout next door
    // states): a host on a high-DPI display that wants a larger gizmo states a larger size, so nothing is scaled
    // twice here.
    const detail::DeviceSize device = detail::toDeviceSize(detail::LogicalSize{ surface_w_, surface_h_ }, detail::PixelRatio{ pixel_ratio_ });
    if (!device.isValid()) {
        return;
    }
    // Bottom-left corner in top-left-origin device coordinates.
    //
    // A window mid-layout can be smaller than the box plus its margins (measured 100x30 on the demo's
    // first shown frame): then the box does not fit and the honest spelling of that is a ZERO-area
    // rectangle, which the backend's empty-rectangle rule skips. The naive `device.height - 2 * margin_px_`
    // went NEGATIVE there and announced a -2x-2 viewport - the same skip downstream, but as garbage
    // handed to the backend rather than a stated decision.
    const int fitted = device.height - 2 * margin_px_;
    const int side   = fitted > 0 ? (size_px_ > fitted ? fitted : size_px_) : 0;
    box_             = Viewport{ area_x_ + margin_px_, area_y_ + device.height - margin_px_ - side, side, side };
}

void AxisGizmo::setAreaOrigin(int x, int y)
{
    if (area_x_ == x && area_y_ == y) {
        return;
    }
    area_x_ = x;
    area_y_ = y;
    // Re-lay out inside the new area with the size that is already known, so the overlay is where it belongs
    // on the very next frame rather than on the next resize.
    onSurfaceResized(surface_w_, surface_h_);
}

void AxisGizmo::onFrame(const FrameContext&)
{
    applyMirror();
    // WHAT THIS HOOK WROTE IS WHAT THE FRAME DRAWS: a frame takes the scene's latest publication (see
    // detail::PublicationStore), so an update that moved content posts one - a hook that edited content and stayed
    // silent would leave the frame drawing the previous picture.
    if (content_ != nullptr) {
        (void)content_->publish();
    }
}

void AxisGizmo::applyMirror()
{
    applyCameraMirror(camera_.get(), source_camera_, MirrorMode::Orientation);

    // The key light travels from the eye towards what the framing camera looks at, so the side the camera sees is the
    // lit one at every orbit (a world-fixed light would leave an arrow flat exactly when the view arrives from its far
    // side). The framing camera always looks at the marker's origin - applyCameraMirror() builds it that way.
    if (key_light_ != nullptr) {
        const vn::math::Vec3d forward = camera_->target() - camera_->eye();
        if (forward.length() > 1.0e-9) {
            key_light_->setDirection(forward.normalized());
        }
    }
}

void AxisGizmo::rebuild()
{
    // ONE geometry, ONE draw: the marker's three arrows share a vertex stream and are told apart by their vertex
    // colour (see makeAxesMarker). What the gizmo adds to that geometry is its camera, its light rig and its box - the
    // arrows' streams are a prototype's, shared with every other marker built from the same style.
    auto axes = makeAxesMarker(style_);
    axes->setName(u8"axis_gizmo");

    auto root = make_intrusive<Group>();
    root->addChild(axes);

    auto scene = make_intrusive<Scene>();
    scene->setRoot(root);
    scene->addLight(ambient_);
    scene->addLight(key_light_);
    content_ = std::move(scene);
}

VN_GRAPHICS_NS_END
