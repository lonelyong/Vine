#include <vine/graphics/AxisGizmo.hpp>

#include <cstdint>
#include <utility>

#include <vine/Colorf.hpp>
#include <vine/graphics/Camera.hpp>
#include <vine/graphics/Geometry.hpp>
#include <vine/graphics/Group.hpp>
#include <vine/graphics/Material.hpp>
#include <vine/graphics/Node.hpp>
#include <vine/graphics/RenderBackend.hpp>
#include <vine/graphics/Scene.hpp>
#include <vine/geometry/Array.hpp>

V_GRAPHICS_NS_BEGIN

V_OBJECT_META_IMPL(AxisGizmo, RenderPass);

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
void appendBox(vine::geometry::Vec3fArray& positions, vine::geometry::Vec3fArray& normals,
               vine::geometry::UInt32Array& indices, const vine::math::Vec3f& mn,
               const vine::math::Vec3f& mx)
{
    using vine::math::Vec3f;

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
    camera_->setViewMatrixAsLookAt(vine::math::Vec3d(0.0, 0.0, 3.3), vine::math::Vec3d(0.0, 0.0, 0.0),
                                   vine::math::Vec3d(0.0, 1.0, 0.0));
    camera_->setProjectionMatrixAsPerspective(45.0, 1.0, 0.05, 20.0);
    setCamera(camera_.get());

    // A HUD pass draws over the main content each frame: never clear and no
    // depth occlusion (always on top).
    setClearEnabled(false);
    setOcclusionEnabled(false);
    rebuild();
}

AxisGizmo::~AxisGizmo() = default;

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
    // Convert the Qt logical surface size into device pixels, which is the
    // space the backend's native surface uses for viewports.
    const int dev_w = static_cast<int>(surface_w_ * pixel_ratio_);
    const int dev_h = static_cast<int>(surface_h_ * pixel_ratio_);
    if (dev_w <= 0 || dev_h <= 0) {
        return;
    }
    // Bottom-left corner in top-left-origin device coordinates.
    const int side = size_px_ > dev_h - 2 * margin_px_ ? dev_h - 2 * margin_px_ : size_px_;
    setViewport(margin_px_, dev_h - margin_px_ - side, side, side);
}

void AxisGizmo::execute(raw_ptr<Scene> /*scene*/, raw_ptr<RenderBackend> backend)
{
    applyMirror();
    RenderPass::execute(content_.get(), backend);
}

void AxisGizmo::applyMirror()
{
    applyCameraMirror(camera_.get(), source_camera_, MirrorMode::Orientation);
}

void AxisGizmo::rebuild()
{
    using vine::math::Vec3f;
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
        vine::geometry::Vec3fArray positions;
        vine::geometry::Vec3fArray normals;
        vine::geometry::UInt32Array indices;
        appendBox(positions, normals, indices, stick.mn, stick.mx);

        auto geometry = make_intrusive<Geometry>();
        geometry->setPositions(positions);
        geometry->setNormals(normals);
        geometry->setIndices(indices);

        auto material = make_intrusive<Material>();
        material->setDiffuse(stick.color);
        // Flat unshaded sticks: with the gizmo lit by a pure ambient light,
        // phong's ambientColor = diffuse * ambient * ambient.a, so a WHITE
        // ambient material makes ambientColor == diffuse == the stick colour
        // regardless of viewing direction (a directional headlight made the
        // sticks go dark/black when the camera looked along a diagonal).
        material->setAmbient(Colorf(1.0f, 1.0f, 1.0f, 1.0f));
        // Black specular avoids any white highlight.
        material->setSpecular(Colorf(0.0f, 0.0f, 0.0f, 1.0f));
        geometry->setMaterial(material);

        auto group = make_intrusive<Group>();
        group->addChild(geometry);
        root->addChild(group);
    }
    scene->setRoot(root);
    content_ = std::move(scene);
}

V_GRAPHICS_NS_END
