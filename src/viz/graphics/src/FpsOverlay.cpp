#include <vine/graphics/FpsOverlay.hpp>

#include <cstdint>
#include <utility>

#include <vine/Colorf.hpp>
#include <vine/graphics/Camera.hpp>
#include <vine/graphics/Geometry.hpp>
#include <vine/graphics/Group.hpp>
#include <vine/graphics/Material.hpp>
#include <vine/graphics/RenderBackend.hpp>
#include <vine/graphics/Scene.hpp>
#include <vine/geometry/Array.hpp>

VN_GRAPHICS_NS_BEGIN

VN_OBJECT_META_IMPL(FpsOverlay, RenderPass);

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

/// Three digits, seven segments each: the row's bars, in the order the pattern bits use.
constexpr std::size_t kSegmentCount = 3u * 7u;
/// Vertices one bar contributes (six flat faces of four corners).
constexpr std::size_t kVerticesPerBar = 24u;
/// Indices one bar contributes (six quads = twelve triangles).
constexpr std::size_t kIndicesPerBar = 36u;

/// The colour a lit bar shows. Ambient-only lighting makes the material's diffuse the colour that
/// reaches the frame, so this IS the readout's on-screen colour: a BRIGHT near-white green, because the
/// readout is drawn over whatever the scene shows -- a dim or saturated green gets lost against a bright
/// sky or a mid-tone surface.
constexpr Colorf kLit{ 0.70f, 1.00f, 0.75f, 1.0f };

}  // namespace

FpsOverlay::FpsOverlay()
{
    // Framing camera: the digit row lies in the z == 0 plane; the camera
    // looks down -z from a distance that fits the row, with the projection
    // aspect updated to match the (wide) readout box on every resize.
    camera_ = make_intrusive<Camera>();
    camera_->setViewMatrixAsLookAt(vn::math::Vec3d(0.0, 0.0, 1.6), vn::math::Vec3d(0.0, 0.0, 0.0),
                                   vn::math::Vec3d(0.0, 1.0, 0.0));
    camera_->setProjectionMatrixAsPerspective(45.0, 1.0, 0.05, 20.0);
    setCamera(camera_);

    // A HUD pass draws over the main content each frame: never clear and no
    // depth occlusion (always on top).
    setClearEnabled(false);
    setDepthMode(DepthMode::Disabled);
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

FpsOverlay::Sampler& FpsOverlay::sampler() noexcept
{
    return sampler_;
}

const FpsOverlay::Sampler& FpsOverlay::sampler() const noexcept
{
    return sampler_;
}

bool FpsOverlay::Sampler::addFrame(double dt_seconds) noexcept
{
    if (!(dt_seconds > 0.0)) {
        return false;  // a clock that did not move is not a frame, and feeding it would only add time
    }
    ++frames_;
    elapsed_seconds_ += dt_seconds;
    if (elapsed_seconds_ < window_seconds) {
        return false;
    }

    // frames / elapsed IS what the window averaged, and this is what gets published: no 1/dt of one frame
    // in it, no blending that would print a value no interval ever had (see the class note).
    published_       = static_cast<double>(frames_) / elapsed_seconds_;
    frames_          = 0;
    elapsed_seconds_ = 0.0;
    return true;
}

double FpsOverlay::Sampler::fps() const noexcept
{
    return published_;
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
    // The window IS the readout's cadence (see Sampler): a figure is published when one closes, and on a
    // value that does not change nothing happens at all - a change rewrites ONE geometry's positions and
    // announces it (see writePattern), so a steady frame rate costs no data work.
    if (!sampler_.addFrame(dt)) {
        return;
    }

    int value = static_cast<int>(sampler_.fps() + 0.5);
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
    const int digit_values[3] = { (value / 100) % 10, (value / 10) % 10, value % 10 };
    std::uint32_t pattern = 0u;
    for (int d = 0; d < 3; ++d) {
        pattern |= static_cast<std::uint32_t>(kDigitSegments[digit_values[d]]) << (d * 7);
    }
    writePattern(pattern);
}

void FpsOverlay::writePattern(std::uint32_t pattern)
{
    if (readout_ == nullptr || row_positions_.empty()) {
        return;
    }
    // AN OFF BAR IS COLLAPSED ONTO ITS FIRST CORNER, not left where it is: a box whose corners all
    // coincide covers no pixels, so a segment the digit does not light is not drawn at all. That is the
    // picture the per-segment visibility gate used to produce (no dark "8" behind the number), reached
    // this way because the row is ONE geometry: what a segment can do individually is contribute its
    // vertices, and the honest way to contribute nothing is to contribute no area.
    std::vector<vn::math::Vec3f> positions = row_positions_;
    for (std::size_t seg = 0; seg < kSegmentCount; ++seg) {
        if ((pattern & (1u << seg)) != 0u) {
            continue;
        }
        const std::size_t       base      = seg * kVerticesPerBar;
        const vn::math::Vec3f collapsed = positions[base];
        for (std::size_t v = 0; v < kVerticesPerBar; ++v) {
            positions[base + v] = collapsed;
        }
    }

    // The vertex count is the one the geometry was built with, so this is a DATA edit of a drawn
    // geometry: the backend re-points the position stream instead of building the node again. Announcing
    // it is the caller's job (see Geometry::setRevision), and a forgotten announcement is silent -- the
    // renderer would keep drawing the digits it uploaded first.
    readout_->setPositions(packAttribute(positions));
    readout_->bumpRevision();
    // Nothing is drawn while the row is hidden, and the first measurement is what un-hides it (the row
    // starts blank rather than showing a dark three-eights behind the number).
    readout_->setVisible(true);
}

void FpsOverlay::rebuild()
{
    using vn::math::Vec3f;

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

    // The row is built with EVERY segment lit: this is the template writePattern() copies its lit bars
    // from, and it is what fixes the geometry's vertex and index count for the rest of its life -- what a
    // change edits is which of those bars carry area, never how many vertices exist.
    vn::geometry::Vec3fArray positions;
    vn::geometry::Vec3fArray normals;
    vn::geometry::UInt32Array indices;
    positions.reserve(kSegmentCount * kVerticesPerBar);
    normals.reserve(kSegmentCount * kVerticesPerBar);
    indices.reserve(kSegmentCount * kIndicesPerBar);

    for (int d = 0; d < 3; ++d) {
        const double ox = x0 + static_cast<double>(d) * (kCellW + kGap);
        for (int s = 0; s < 7; ++s) {
            const auto& bar = kSegments[s];
            const Vec3f centre(static_cast<float>(ox + bar.dx), static_cast<float>(y0 + bar.dy), 0.0f);
            const Vec3f half(static_cast<float>(bar.hx), static_cast<float>(bar.hy), static_cast<float>(kBarD / 2.0));
            appendBox(positions, normals, indices, centre - half, centre + half);
        }
    }

    auto geometry = make_intrusive<Geometry>();
    geometry->setPositions(packAttribute(positions));
    geometry->setNormals(packAttribute(normals));
    geometry->setIndices(packIndices(indices));

    // ONE material for the whole row: the bar colour lives here, not per segment, because the row is one
    // draw -- an off segment is expressed by GEOMETRY (no area), never by a colour.
    auto material = make_intrusive<Material>();
    material->setDiffuse(kLit);
    // On-top HUD content is lit by a pure ambient light: a WHITE ambient material makes
    // ambientColor == diffuse == the bar colour, so this IS the readout's on-screen colour; black
    // specular avoids highlights.
    material->setAmbient(Colorf(1.0f, 1.0f, 1.0f, 1.0f));
    material->setSpecular(Colorf(0.0f, 0.0f, 0.0f, 1.0f));
    geometry->setMaterial(material);
    // Hidden until the first measurement: the readout draws the digits' LIT segments only (see
    // writePattern), so it starts blank rather than showing a dark three-eights behind the number.
    geometry->setVisible(false);

    auto root = make_intrusive<Group>();
    root->addChild(geometry);
    auto scene = make_intrusive<Scene>();
    scene->setRoot(root);
    content_ = std::move(scene);

    readout_       = geometry;
    row_positions_ = std::move(positions);
    shown_value_   = -1;
}

VN_GRAPHICS_NS_END
