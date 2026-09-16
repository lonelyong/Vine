/**
 * @brief A DATA edit of an already-drawn geometry is served by re-pointing the changed stream.
 *
 * The incremental path (P6) exists so that "the model changed one channel" costs one stream's
 * upload instead of the whole data node: same vertex count, new bytes. From OUTSIDE the two paths
 * are indistinguishable -- a re-pointed stream and a rebuilt node draw the same picture -- so this
 * phase asserts the picture AND the counter pair the backend exposes for it:
 *
 *   1. a quad drawn from its own positions: the first frame BUILDS the data node
 *      (dataNodesBuilt() + 1, streamsRefreshed() unchanged). The quad sits far outside the view,
 *      so the centre pixel is the clear colour;
 *   2. the SAME geometry object gets new positions that bring the quad over the centre -- an
 *      edit, not a new geometry;
 *   3. the next frame is served from the incremental path (streamsRefreshed() + 1) WITHOUT
 *      building the node again (dataNodesBuilt() unchanged), and the centre pixel is now the
 *      quad's colour. Later frames stay quiet: the refreshed stream is the item's new identity,
 *      so nothing refreshes twice.
 *
 * Both halves are load-bearing: the pixel proves the new bytes reached the GPU, the counters
 * prove they arrived by the cheap path. A mutation that makes the fast path refuse turns this
 * phase red while the picture stays correct -- which is exactly why pixels alone cannot cover it.
 *
 * Nothing else in the self-test edits a geometry it has already drawn, so this is the only
 * end-to-end cover for the incremental family (P5/P6/P7/P9/P10); see
 * .ai/memory/graphics-perf-backlog.md (V6 measurement, V7 gap).
 */

#include "selftest_support.hpp"

#include <cstdint>
#include <cstdio>
#include <vector>

namespace selftest
{

namespace
{
/** @brief Positions of an axis-aligned quad centred at (@p x, 0, 0).
 *
 * @param x    Centre along X; far outside the view frustum when @p x is large.
 * @param half Half extent of the quad.
 * @return Four vertices, counter-clockwise from the bottom left.
 */
vine::geometry::Vec3fArray quadPositions(float x, float half = 0.4f)
{
    vine::geometry::Vec3fArray positions;
    positions.push_back(vine::math::Vec3f(x - half, -half, 0.0f));
    positions.push_back(vine::math::Vec3f(x + half, -half, 0.0f));
    positions.push_back(vine::math::Vec3f(x + half, half, 0.0f));
    positions.push_back(vine::math::Vec3f(x - half, half, 0.0f));
    return positions;
}
} // namespace

bool runDataRefreshPhase(vine::vsg::VsgRenderer& renderer, const CameraPtr& camera, int frames)
{
    bool ok = true;

    auto target = RenderTargetPtr(new RenderTarget());
    target->setSize(64, 36);
    target->attachColor(RenderTarget::ColorFormat::RGBA8);
    target->attachDepth(RenderTarget::DepthFormat::D24);

    const vine::Color clear_color(10, 20, 30, 255);
    // Positions only: no normals, no UVs, no colour, so every optional channel is BUILT -- and a
    // position edit invalidates the derived normals too, which is one of the streams the refresh
    // has to serve.
    auto geometry = GeometryPtr(new Geometry());
    geometry->setPositions(vine::graphics::packAttribute(quadPositions(6.0f)));
    geometry->setIndices(
        vine::graphics::packIndices(vine::geometry::UInt32Array{ 0u, 1u, 2u, 0u, 2u, 3u }));

    auto material = MaterialPtr(new Material());
    material->setDiffuse(vine::Colorf(0.9f, 0.15f, 0.05f, 1.0f)); // distinctly red
    RenderCommand command(geometry, material, Mat4d());
    auto          pass = RenderPassPtr(new RenderPass());

    constexpr std::uint32_t kWidth  = 64u;
    constexpr std::uint32_t kHeight = 36u;
    const std::size_t       centre  = static_cast<std::size_t>(kHeight / 2u) * kWidth + kWidth / 2u;

    /// Renders one frame of the single-command scene.
    const auto draw_once = [&]() {
        FrameScope frame(renderer);
        PassScope  pass_scope(renderer, pass.get(), 0, target.get(), clear_color, true,
                              vine::graphics::DepthMode::TestAndWrite);
        renderer.render(std::vector<RenderCommand>{ command }, camera.get());
    };

    /// Reads attachment 0 back into @p pixels.
    const auto read_back = [&](std::vector<std::uint8_t>& pixels) {
        return renderer.readColorBuffer(target.get(), 0, pixels) && pixels.size() >= (centre + 1u) * 4u;
    };

    const auto release = [&]() {
        renderer.releasePass(pass.get());
        renderer.releaseRenderTarget(target.get());
    };

    const std::size_t built_before     = renderer.dataNodesBuilt();
    const std::size_t refreshed_before = renderer.streamsRefreshed();

    draw_once();
    const std::size_t built_after_first = renderer.dataNodesBuilt();
    std::vector<std::uint8_t> pixels;
    if (!read_back(pixels)) {
        std::fprintf(stderr, "[selftest] FAIL: the data-refresh phase could not read its target back\n");
        release();
        return false;
    }
    if (built_after_first != built_before + 1u) {
        std::fprintf(stderr,
                     "[selftest] FAIL: drawing a geometry for the first time built %zu data node(s), expected 1\n",
                     built_after_first - built_before);
        ok = false;
    }
    const bool centre_is_clear = pixels[centre * 4u + 0u] == 10u && pixels[centre * 4u + 1u] == 20u &&
                                 pixels[centre * 4u + 2u] == 30u;
    if (!centre_is_clear) {
        std::fprintf(stderr,
                     "[selftest] FAIL: before the edit the centre holds (%u,%u,%u), expected the clear colour"
                     " (10,20,30) — the quad must start outside the view\n",
                     pixels[centre * 4u + 0u], pixels[centre * 4u + 1u], pixels[centre * 4u + 2u]);
        ok = false;
    }

    // The edit: the SAME geometry, new positions -- and the ANNOUNCEMENT, which is the caller's job
    // (the per-channel setters deliberately leave the revision alone: see Geometry::setPositions).
    // Forgetting it is exactly how this phase first failed: nothing ran at all (0 refreshes AND 0
    // rebuilds) because the backend had no reason to look at the geometry again.
    geometry->setPositions(vine::graphics::packAttribute(quadPositions(0.0f)));
    geometry->setRevision(geometry->revision() + 1u);

    for (int i = 0; i < (frames > 0 ? frames : 1); ++i) {
        draw_once();
    }
    const std::size_t built_after_edit     = renderer.dataNodesBuilt();
    const std::size_t refreshed_after_edit = renderer.streamsRefreshed();
    if (!read_back(pixels)) {
        std::fprintf(stderr, "[selftest] FAIL: the data-refresh phase could not read its target back after the edit\n");
        release();
        return false;
    }
    if (refreshed_after_edit != refreshed_before + 1u) {
        std::fprintf(stderr,
                     "[selftest] FAIL: the position edit was served %zu time(s) by the incremental path,"
                     " expected exactly 1 (%zu frame(s) drawn)\n",
                     refreshed_after_edit - refreshed_before, static_cast<std::size_t>(frames));
        ok = false;
    }
    if (built_after_edit != built_after_first) {
        std::fprintf(stderr,
                     "[selftest] FAIL: the position edit BUILT the data node again (%zu build(s) after the"
                     " first frame, %zu after the edit) — the incremental path was skipped\n",
                     built_after_first - built_before, built_after_edit - built_before);
        ok = false;
    }
    // Red-dominant, not "bright": the geometry authors no normals, so the backend DERIVES them and the
    // default lights hit a grazing angle -- the quad reads a dim but unmistakably red (34,6,2) against
    // the clear colour's blue-dominant (10,20,30). Hue is what discriminates here, and it is what a
    // refresh that never reached the GPU cannot produce.
    const std::uint8_t red = pixels[centre * 4u + 0u];
    const bool centre_is_the_quad = red > pixels[centre * 4u + 1u] && red > pixels[centre * 4u + 2u];
    if (!centre_is_the_quad) {
        std::fprintf(stderr,
                     "[selftest] FAIL: after the edit the centre holds (%u,%u,%u), expected the quad's red —"
                     " the refreshed stream did not reach the GPU\n",
                     pixels[centre * 4u + 0u], pixels[centre * 4u + 1u], pixels[centre * 4u + 2u]);
        ok = false;
    }

    if (ok) {
        std::fprintf(stderr,
                     "[data-refresh] the position edit was served in place: %zu stream refresh(es) and %zu data"
                     " node build(s) over %d frame(s), with the centre going from the clear (10,20,30) to the"
                     " quad's red (%u,%u,%u)\n",
                     refreshed_after_edit - refreshed_before, built_after_edit - built_before, frames, red,
                     pixels[centre * 4u + 1u], pixels[centre * 4u + 2u]);
    }
    release();
    return ok;
}

} // namespace selftest
