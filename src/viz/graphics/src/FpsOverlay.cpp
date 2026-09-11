#include <vine/graphics/FpsOverlay.hpp>

#include <cstdint>
#include <utility>

#include <vine/Colorf.hpp>
#include <vine/graphics/Camera.hpp>
#include <vine/graphics/Geometry.hpp>
#include <vine/graphics/Group.hpp>
#include <vine/graphics/Node.hpp>
#include <vine/graphics/RenderBackend.hpp>
#include <vine/graphics/Scene.hpp>
#include <vine/geometry/Array.hpp>

V_GRAPHICS_NS_BEGIN

V_OBJECT_META_IMPL(FpsOverlay, RenderPass);

namespace
{

/**
 * @brief Appends an axis-aligned box to the mesh arrays.
 *
 * Six flat-shaded faces with outward, counter-clockwise (seen from the
 * outside) winding so the box renders from either side. Mirrors the box
 * builder used by AxisGizmo so the bars share its geometry conventions.
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
        const int  a = f / 2; // face normal axis
        const bool neg  = (f % 2) == 1;
        const int  nxt  = (a + 1) % 3;
        const int  nxt2 = (a + 2) % 3;
        const int  ua   = neg ? nxt2 : nxt;
        const int  va   = neg ? nxt : nxt2;
        const Vec3f n   = neg ? -axis(a) : axis(a);
        const Vec3f u   = axis(ua);
        const Vec3f v   = axis(va);

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

/** @brief One seven-segment bar: centre offset and half extents in-plane. */
struct BarSpec
{
    double dx, dy, hx, hy;
};

}  // namespace

FpsOverlay::FpsOverlay()
{
    // Framing camera: the digit row lies in the z == 0 plane; the camera
    // looks down -z from a distance that fits the row, with the projection
    // aspect updated to match the (wide) readout box on every resize.
    camera_ = make_intrusive<Camera>();
    camera_->setViewMatrixAsLookAt(vine::math::Vec3d(0.0, 0.0, 1.6), vine::math::Vec3d(0.0, 0.0, 0.0),
                                   vine::math::Vec3d(0.0, 1.0, 0.0));
    camera_->setProjectionMatrixAsPerspective(45.0, 1.0, 0.05, 20.0);
    setCamera(camera_.get());

    // A HUD pass draws over the main content each frame: never clear and no
    // depth occlusion (always on top).
    setClearEnabled(false);
    setOcclusionEnabled(false);
    rebuild();
}

FpsOverlay::~FpsOverlay() = default;

void FpsOverlay::setPixelRatio(double ratio)
{
    if (ratio > 0.0 && ratio != pixel_ratio_) {
        pixel_ratio_ = ratio;
        onSurfaceResized(surface_w_, surface_h_);
    }
}

void FpsOverlay::setSize(int width, int height)
{
    if (width > 0 && height > 0 && (width != box_width_px_ || height != box_height_px_)) {
        box_width_px_  = width;
        box_height_px_ = height;
        onSurfaceResized(surface_w_, surface_h_);
    }
}

raw_ptr<Scene> FpsOverlay::content() const
{
    return content_.get();
}

void FpsOverlay::onSurfaceResized(int width, int height)
{
    surface_w_ = width;
    surface_h_ = height;
    if (surface_w_ <= 0 || surface_h_ <= 0) {
        return;
    }
    // Convert the (Qt logical) surface size into device pixels, the space the
    // backend's native surface uses for viewports.
    const int dev_w = static_cast<int>(surface_w_ * pixel_ratio_);
    const int dev_h = static_cast<int>(surface_h_ * pixel_ratio_);
    if (dev_w <= 0 || dev_h <= 0) {
        return;
    }

    // Fit the readout box into the surface (minus the margin), preserving its
    // aspect so the digits are never stretched or clipped.
    int w = box_width_px_;
    int h = box_height_px_;
    if (w > dev_w - 2 * margin_px_) {
        const double scale = static_cast<double>(dev_w - 2 * margin_px_) / w;
        w = static_cast<int>(w * scale);
        h = static_cast<int>(h * scale);
    }
    if (h > dev_h - 2 * margin_px_) {
        const double scale = static_cast<double>(dev_h - 2 * margin_px_) / h;
        w = static_cast<int>(w * scale);
        h = static_cast<int>(h * scale);
    }
    if (w <= 0 || h <= 0) {
        return;
    }

    // Bottom-right corner in top-left-origin device coordinates.
    setViewport(dev_w - margin_px_ - w, dev_h - margin_px_ - h, w, h);
    // Match the framing projection to the box aspect so the digit row fills
    // the box without distortion.
    camera_->setProjectionMatrixAsPerspective(45.0, static_cast<double>(w) / static_cast<double>(h), 0.05, 20.0);
}

void FpsOverlay::execute(raw_ptr<Scene> /*scene*/, raw_ptr<RenderBackend> backend)
{
    // Measure the actual render-loop frame rate from the wall clock between
    // executes and (throttled) refresh the readout before drawing.
    const auto now = std::chrono::steady_clock::now();
    if (last_tick_.time_since_epoch().count() != 0) {
        const double dt = std::chrono::duration<double>(now - last_tick_).count();
        updateReadout(dt);
    }
    last_tick_ = now;

    RenderPass::execute(content_.get(), backend);
}

void FpsOverlay::updateReadout(double dt)
{
    // EMA smoothing of the instantaneous frame rate.
    const double inst = (dt > 1e-6) ? (1.0 / dt) : 0.0;
    fps_smoothed_ = (fps_smoothed_ <= 0.0) ? inst : 0.2 * inst + 0.8 * fps_smoothed_;

    // Throttle the digit flips (~ every 0.15 s); flipping only on change keeps
    // the shared-material hot path quiet in steady state.
    readout_elapsed_ += dt;
    if (readout_elapsed_ < 0.15) {
        return;
    }
    readout_elapsed_ = 0.0;

    int value = static_cast<int>(fps_smoothed_ + 0.5);
    if (value > 999) {
        value = 999;
    }
    if (value == shown_value_) {
        return;
    }
    shown_value_ = value;

    // abcdefg bitmaps, bit 0 = a.
    static const std::uint8_t kDigitSegments[10] = { 0x3F, 0x06, 0x5B, 0x4F, 0x66,
                                                     0x6D, 0x7D, 0x07, 0x7F, 0x6F };
    static const Colorf kLit(0.25f, 1.00f, 0.40f, 1.0f);
    static const Colorf kDim(0.08f, 0.08f, 0.09f, 1.0f);

    const int digit_values[3] = { (value / 100) % 10, (value / 10) % 10, value % 10 };
    for (int d = 0; d < 3; ++d) {
        const std::uint8_t segs = kDigitSegments[digit_values[d]];
        for (int s = 0; s < 7; ++s) {
            const std::size_t idx = static_cast<std::size_t>(d * 7 + s);
            if (idx < segment_materials_.size()) {
                segment_materials_[idx]->setDiffuse((segs & (1u << s)) ? kLit : kDim);
            }
        }
    }
}

void FpsOverlay::rebuild()
{
    using vine::math::Vec3f;

    // Seven-segment geometry in the z == 0 plane. Order: a b c d e f g =
    // top, upper-right, lower-right, bottom, lower-left, upper-left, middle.
    static constexpr double kCellW = 0.60;
    static constexpr double kCellH = 1.00;
    static constexpr double kBarT  = 0.12; // bar thickness in-plane
    static constexpr double kBarD  = 0.06; // bar depth out of plane
    static constexpr double kGap   = 0.14;
    static const BarSpec kSegments[7] = {
        { kCellW / 2.0, kCellH,       (kCellW - kBarT) / 2.0, kBarT / 2.0 },                 // a
        { kCellW,       kCellH * 0.75, kBarT / 2.0, (kCellH / 2.0 - kBarT) / 2.0 },         // b
        { kCellW,       kCellH * 0.25, kBarT / 2.0, (kCellH / 2.0 - kBarT) / 2.0 },         // c
        { kCellW / 2.0, 0.0,          (kCellW - kBarT) / 2.0, kBarT / 2.0 },                 // d
        { 0.0,          kCellH * 0.25, kBarT / 2.0, (kCellH / 2.0 - kBarT) / 2.0 },         // e
        { 0.0,          kCellH * 0.75, kBarT / 2.0, (kCellH / 2.0 - kBarT) / 2.0 },         // f
        { kCellW / 2.0, kCellH / 2.0, (kCellW - kBarT) / 2.0, kBarT / 2.0 },                 // g
    };
    // Centre the 3-digit row on the origin.
    const double row_width = 3.0 * kCellW + 2.0 * kGap;
    const double x0        = -row_width / 2.0;
    const double y0        = -kCellH / 2.0;

    static const Colorf kDim(0.08f, 0.08f, 0.09f, 1.0f);

    auto scene = make_intrusive<Scene>();
    auto root  = make_intrusive<Group>();
    segment_materials_.clear();
    segment_materials_.reserve(3u * 7u);

    for (int d = 0; d < 3; ++d) {
        const double ox = x0 + static_cast<double>(d) * (kCellW + kGap);
        for (int s = 0; s < 7; ++s) {
            const auto& bar = kSegments[s];
            const Vec3f centre(static_cast<float>(ox + bar.dx), static_cast<float>(y0 + bar.dy), 0.0f);
            const Vec3f half(static_cast<float>(bar.hx), static_cast<float>(bar.hy), static_cast<float>(kBarD / 2.0));

            vine::geometry::Vec3fArray positions;
            vine::geometry::Vec3fArray normals;
            vine::geometry::UInt32Array indices;
            appendBox(positions, normals, indices, centre - half, centre + half);

            auto geometry = make_intrusive<Geometry>();
            geometry->setPositions(positions);
            geometry->setNormals(normals);
            geometry->setIndices(indices);

            auto material = make_intrusive<Material>();
            material->setDiffuse(kDim);
            // On-top HUD content is lit by a pure ambient light: a WHITE
            // ambient material makes ambientColor == diffuse == the segment
            // colour, so flipping setDiffuse lights/dims the segment; black
            // specular avoids highlights.
            material->setAmbient(Colorf(1.0f, 1.0f, 1.0f, 1.0f));
            material->setSpecular(Colorf(0.0f, 0.0f, 0.0f, 1.0f));
            geometry->setMaterial(material);
            segment_materials_.push_back(material);

            auto group = make_intrusive<Group>();
            group->addChild(geometry);
            root->addChild(group);
        }
    }
    scene->setRoot(root);
    content_ = std::move(scene);
}

V_GRAPHICS_NS_END
