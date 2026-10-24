#include <vine/graphics/AxisGizmo.hpp>

#include <cstdint>
#include <utility>

#include <vine/Colorf.hpp>
#include <vine/graphics/Camera.hpp>
#include <vine/graphics/CameraMirror.hpp>
#include <vine/graphics/Geometry.hpp>
#include <vine/graphics/Group.hpp>
#include <vine/graphics/Material.hpp>
#include <vine/graphics/Node.hpp>
#include <vine/graphics/Scene.hpp>
#include <vine/geometry/Array.hpp>

#include "PixelGeometry.hpp"

VN_GRAPHICS_NS_BEGIN

VN_OBJECT_META_IMPL(AxisGizmo, Object);

namespace
{

/**
 * @brief Appends an axis-aligned box to the mesh arrays.
 *
 * Builds six flat-shaded faces with outward, counter-clockwise (seen from the
 * outside) winding so the box is consistent and renders from either side.
 *
 * @param positions Box vertex positions (appended).
 * @param normals   Per-vertex face normals (appended).
 * @param indices   Triangle indices (appended).
 * @param mn        Minimum corner.
 * @param mx        Maximum corner.
 */
void appendBox(vn::geometry::Vec3fArray& positions, vn::geometry::Vec3fArray& normals,
               vn::geometry::UInt32Array& indices, const vn::math::Vec3f& mn,
               const vn::math::Vec3f& mx)
{
    using vn::math::Vec3f;

    const Vec3f c = (mn + mx) * 0.5f;
    const float h[3] = { (mx.x - mn.x) * 0.5f, (mx.y - mn.y) * 0.5f, (mx.z - mn.z) * 0.5f };
    const auto axis = [](int a) {
        return a == 0 ? Vec3f(1.0f, 0.0f, 0.0f) : (a == 1 ? Vec3f(0.0f, 1.0f, 0.0f)
                                                           : Vec3f(0.0f, 0.0f, 1.0f));
    };
    const float cu[4] = { -1.0f, 1.0f, 1.0f, -1.0f };
    const float cv[4] = { -1.0f, -1.0f, 1.0f, 1.0f };

    for (int f = 0; f < 6; ++f) {
        const int a = f / 2;        // face normal axis.
        const bool neg = (f % 2) == 1;
        const int nxt = (a + 1) % 3;
        const int nxt2 = (a + 2) % 3;
        const int ua = neg ? nxt2 : nxt;
        const int va = neg ? nxt : nxt2;
        const Vec3f n = neg ? -axis(a) : axis(a);
        const Vec3f u = axis(ua);
        const Vec3f v = axis(va);

        const std::uint32_t base = static_cast<std::uint32_t>(positions.size());
        for (int k = 0; k < 4; ++k) {
            positions.push_back(c + n * h[a] + u * (cu[k] * h[ua]) + v * (cv[k] * h[va]));
            normals.push_back(n);
        }
        indices.push_back(base + 0);
        indices.push_back(base + 1);
        indices.push_back(base + 2);
        indices.push_back(base + 0);
        indices.push_back(base + 2);
        indices.push_back(base + 3);
    }
}

}  // namespace

AxisGizmo::AxisGizmo()
{
    // Framing camera: square perspective that fits the unit sticks at the
    // mirror distance. The view is overwritten every frame by Orientation
    // mirroring; only the framing distance and projection are kept. Owned by
    // this gizmo so the pass' raw camera pointer never dangles.
    camera_ = make_intrusive<Camera>();
    // Framing distance chosen so the unit sticks fill most of the square
    // sub-viewport (half-height = 3.3 * tan(22.5) ~ 1.37 world units maps a
    // length-1 stick to ~73% of the half box).
    camera_->lookAt(vn::math::Vec3d(0.0, 0.0, 3.3), vn::math::Vec3d(0.0, 0.0, 0.0),
                                   vn::math::Vec3d(0.0, 1.0, 0.0));
    camera_->setPerspective(45.0, 0.05, 20.0);
    // The picture's shape is the camera's (see Camera), and the box this camera frames is a SQUARE
    // (see onSurfaceResized) - so it is 1:1, stated here where the camera is configured rather than left to
    // whatever the rectangle it ends up drawing into happens to be.
    camera_->setAspect(1.0);
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
    if (length > 0.0 && length != axis_length_) {
        axis_length_ = length;
        rebuild();
    }
}

void AxisGizmo::setThickness(double thickness)
{
    if (thickness > 0.0 && thickness != thickness_) {
        thickness_ = thickness;
        rebuild();
    }
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
    // surface uses for viewports.
    const detail::DeviceSize device =
        detail::toDeviceSize(detail::LogicalSize{ surface_w_, surface_h_ }, detail::PixelRatio{ pixel_ratio_ });
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
    box_ = Viewport{ area_x_ + margin_px_, area_y_ + device.height - margin_px_ - side, side, side };
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

void AxisGizmo::update()
{
    applyMirror();
}

void AxisGizmo::applyMirror()
{
    applyCameraMirror(camera_.get(), source_camera_, MirrorMode::Orientation);
}

void AxisGizmo::rebuild()
{
    using vn::math::Vec3f;
    const float l = static_cast<float>(axis_length_);
    const float t = static_cast<float>(thickness_);

    struct Stick {
        Vec3f mn;
        Vec3f mx;
        Colorf color;
    };
    const Stick sticks[3] = {
        { Vec3f(0.0f, -t, -t), Vec3f(l, t, t), Colorf(0.9f, 0.2f, 0.2f, 1.0f) },
        { Vec3f(-t, 0.0f, -t), Vec3f(t, l, t), Colorf(0.2f, 0.8f, 0.2f, 1.0f) },
        { Vec3f(-t, -t, 0.0f), Vec3f(t, t, l), Colorf(0.2f, 0.4f, 0.95f, 1.0f) },
    };

    auto scene = make_intrusive<Scene>();
    // Single root: one identity Group owns the three axis sticks.
    auto root = make_intrusive<Group>();
    for (const auto& stick : sticks) {
        vn::geometry::Vec3fArray positions;
        vn::geometry::Vec3fArray normals;
        vn::geometry::UInt32Array indices;
        appendBox(positions, normals, indices, stick.mn, stick.mx);

        auto geometry = make_intrusive<Geometry>();
        geometry->setPositions(packAttribute(positions));
        geometry->setNormals(packAttribute(normals));

        auto material = make_intrusive<Material>();
        // Flat unshaded sticks: the gizmo is lit by a pure ambient light, so the shading term is
        // base_colour * ambient and the stick's colour IS what lands on screen, whatever the view
        // direction (a directional headlight used to make the sticks go dark/black when the camera
        // looked along a diagonal).
        material->setBaseColor(stick.color);

        Primitive stick_draw;
        stick_draw.topology = Topology::Triangles;
        stick_draw.indices  = Primitive::IndexSpan::whole(packIndices(indices));
        stick_draw.material = material;
        geometry->setPrimitive(0U, std::move(stick_draw));

        auto group = make_intrusive<Group>();
        group->addChild(geometry);
        root->addChild(group);
    }
    scene->setRoot(root);
    content_ = std::move(scene);
}

VN_GRAPHICS_NS_END
