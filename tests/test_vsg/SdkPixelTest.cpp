#include "SdkPixelFixture.hpp"

#include <gtest/gtest.h>

#include <cstddef>
#include <cstdint>
#include <cstdio>

#include <vine/graphics/BuiltinShaders.hpp>
#include <vine/graphics/RenderEngine.hpp>
#include <vine/graphics/RenderPass.hpp>
#include <vine/graphics/SceneView.hpp>

using vn::graphics::RenderPass;
using vn::graphics::SceneView;
using vn::graphics::ShadingPath;
using vn::graphics::backend::PixelProbe;
using vn::graphics::backend::Rgba8;

namespace
{
/// @brief Whether a read-back pixel is one the content covered (see the fixture's note on the colours).
bool covered(const Rgba8& pixel)
{
    return pixel.r != 0U || pixel.g != 0U || pixel.b != 0U;
}

/// @brief The pixels of @p probe the content covered.
std::size_t coveredPixels(const PixelProbe& probe)
{
    std::size_t count = 0;
    for (int y = 0; y < probe.height(); ++y) {
        for (int x = 0; x < probe.width(); ++x) {
            if (covered(probe.pixel(x, y))) {
                ++count;
            }
        }
    }
    return count;
}

/// @brief Whether @p probe covers (x, y) or any of its eight neighbours.
///
/// THE TWO RASTERISERS DO NOT AGREE ON THE BOUNDARY, by design: the fill covers the pixels whose CENTRE is
/// inside the triangle, while a one-pixel-wide line covers the pixels its segment touches, and at the exact
/// edge those are different pixels. Measured on a 36x36 quad: filled = 1296 pixels (the whole block), edges =
/// 180 (the 140-pixel silhouette plus the 36-pixel diagonal, both drawn one step OUTSIDE the block's centre
/// rule). One pixel of slack is therefore what "the same geometry" means between the two - a picture of
/// another shape, or the same shape shifted by two pixels, still fails this.
bool coveredWithinOnePixel(const PixelProbe& probe, int x, int y)
{
    for (int dy = -1; dy <= 1; ++dy) {
        for (int dx = -1; dx <= 1; ++dx) {
            const int px = x + dx;
            const int py = y + dy;
            if (px < 0 || py < 0 || px >= probe.width() || py >= probe.height()) {
                continue;
            }
            if (covered(probe.pixel(px, py))) {
                return true;
            }
        }
    }
    return false;
}

/// @brief Whether (x, y) is on the silhouette of what @p probe covers (covered, with an uncovered neighbour).
bool onSilhouette(const PixelProbe& probe, int x, int y)
{
    if (!covered(probe.pixel(x, y))) {
        return false;
    }
    return !coveredWithinOnePixel(probe, x, y) || !covered(probe.pixel(x + 1 < probe.width() ? x + 1 : x, y)) ||
           !covered(probe.pixel(x, y + 1 < probe.height() ? y + 1 : y)) ||
           !covered(probe.pixel(x > 0 ? x - 1 : x, y)) || !covered(probe.pixel(x, y > 0 ? y - 1 : y));
}

/// @brief Skips out of a case when this machine cannot serve the fixture.
///
/// The gate is stated once, for both cases: a device that satisfies the backend's floor, a session that comes
/// up, and a backend that serves off-screen pixels at all. Everything after it is a failure, not a skip.
#define VINE_SKIP_WITHOUT_SDK_DEVICE(fixture)                                                                    \
    if (!SdkPixelFixture::available()) {                                                                         \
        GTEST_SKIP() << "no physical device satisfies the render backend's floor";                               \
    }                                                                                                            \
    SdkPixelFixture fixture;                                                                                     \
    if (!fixture.up()) {                                                                                         \
        GTEST_SKIP() << "the SDK session cannot come up on this machine (see the diagnostics above)";            \
    }                                                                                                            \
    if (!fixture.backend->supportsRenderTargets()) {                                                             \
        GTEST_SKIP() << "this device cannot serve off-screen pixels";                                            \
    }
}  // namespace

// 线框不是"少画一点"，是**同一批几何按边画**：同一个视图、同一台相机、同一条 pass、同一张目标，只把光栅化模式换掉
// （M1 把四档做成了 pass 的属性，换档不重建），两版画面对照之下边真的存在、且边落在实体版覆盖的那块轮廓上。这一条
// 把 M1 从"该画的 pass 在画"补到"画出来的像素对"：配方是**延迟**的、档是非 Lit 的，于是生效的链条按前向建，画面就是
// 那条几何 pass 产出的 —— 正是这一条要探的那张画面。
TEST(SdkPixelTest, AWireframeModeDrawsTheEdgesOfTheSameGeometry)
{
    VINE_SKIP_WITHOUT_SDK_DEVICE(fixture);

    vn::intrusive_ptr<SceneView> view(new SceneView());
    view->setEngine(fixture.engine.get());
    view->setScene(fixture.scene);
    view->onSurfaceResized(SdkPixelFixture::kSide, SdkPixelFixture::kSide, 1.0);

    // Built once, with the options that are in force for the first picture: the chain forward, so the picture
    // pass is the scene pass itself.
    view->setOptions({ .path = ShadingPath::Deferred, .mode = SceneView::Options::Mode::Unlit });
    view->ensureWindowPass();
    ASSERT_NE(view->pipeline(), nullptr);

    // The picture pass draws into the fixture's off-screen target instead of the window: the SDK's own pass
    // object, with the SDK's own mode applied to it, so the ONE thing this case turns on is the polygon mode.
    RenderPass* const picture = view->pipeline()->windowPass();
    ASSERT_NE(picture, nullptr);
    picture->setRenderTarget(fixture.target);
    picture->setClearEnabled(true);
    picture->setClearColor(vn::Color(0, 0, 0));
    picture->setShouldClearDepth(true);
    ASSERT_TRUE(view->fitToScreen()) << "默认操纵器按内容把相机摆好";

    const PixelProbe filled = fixture.read();
    ASSERT_TRUE(filled.valid()) << "画出来的这一帧必须读得回来";
    ASSERT_EQ(filled.width(), SdkPixelFixture::kSide);

    // 同一帧的另一版：只换档。链条没变 ⇒ 不该重建 ⇒ 还是那条 pass（目标没被换掉），相机当然也没动。
    // 这里**不判**"pass 上多了个 PolygonMode::Line 覆盖"：那是 M1 在 test_graphics 的结构判据；这一条判的是像素，
    // 覆盖没到光栅器就该由画面来说（否则结构断言会先红，抢掉像素判据的那一刀）。
    view->setOptions({ .path = ShadingPath::Deferred, .mode = SceneView::Options::Mode::Wireframe });
    ASSERT_EQ(view->pipeline()->windowPass(), picture) << "链条内换档不重建（M1 的另一半）";

    const PixelProbe lines = fixture.read();
    ASSERT_TRUE(lines.valid());
    ASSERT_EQ(lines.width(), SdkPixelFixture::kSide);

    const std::size_t filled_covered = coveredPixels(filled);
    const std::size_t line_covered   = coveredPixels(lines);

    // 判据自己先立住：实体版真的盖住了一大片（否则下面的包含关系是在比较噪声），线框版真的画出了东西（"空画面"
    // 正是这一条要抓的失败），而且边严格少于它所围的面。
    EXPECT_GT(filled_covered, filled.pixelCount() / 4U) << "相机取景之后这个四边形必须占住一块真画面";
    EXPECT_GT(line_covered, 0U) << "线框版是空画面 = 光栅化模式没到达光栅器";
    EXPECT_LT(line_covered, filled_covered) << "边覆盖得比面少：它是同一批几何的另一种画法";

    std::size_t edges_inside = 0;
    for (int y = 0; y < lines.height(); ++y) {
        for (int x = 0; x < lines.width(); ++x) {
            if (covered(lines.pixel(x, y)) && coveredWithinOnePixel(filled, x, y)) {
                ++edges_inside;
            }
        }
    }

    std::size_t silhouette = 0;
    std::size_t traced     = 0;
    for (int y = 0; y < filled.height(); ++y) {
        for (int x = 0; x < filled.width(); ++x) {
            if (!onSilhouette(filled, x, y)) {
                continue;
            }
            ++silhouette;
            if (coveredWithinOnePixel(lines, x, y)) {
                ++traced;
            }
        }
    }

    std::printf("[sdk-pixels] filled=%zu lines=%zu of %zu; edges on the covered block=%zu; silhouette=%zu "
                "traced=%zu\n",
                filled_covered, line_covered, filled.pixelCount(), edges_inside, silhouette, traced);

    // 包含关系：每一个边上的像素都**贴着实体版覆盖的那块**。这比"两边数量不同"强得多 —— 它排除了"线框版画的是
    // 另一批东西"（错的目标、上一帧的残留、叠加的 HUD）。
    EXPECT_EQ(edges_inside, line_covered)
        << "边必须落在实体版覆盖的那块轮廓上（差集就是「线框版画了别的东西」）";
    // 反过来也判：实体版的轮廓**大部分都有边走过**，否则"边"可能只是几条不相干的线段。
    EXPECT_GT(silhouette, 0U);
    EXPECT_GT(traced * 2U, silhouette) << "实体版轮廓的一半以上必须有边走过";
}

// 夹具本身的最小用例：**一个视图都没有**，一个手搭的 pass 把同一批内容画进同一张目标。上一条量的是 SDK 的默认
// 视图，如果夹具只能通过视图工作，那它量到的就是自己的脚手架。
TEST(SdkPixelTest, TheFixtureAlsoDrawsContentWithNoViewAtAll)
{
    VINE_SKIP_WITHOUT_SDK_DEVICE(fixture);

    vn::intrusive_ptr<RenderPass> pass(new RenderPass());
    pass->setName(vn::String(u8"hand_wired_picture"));
    pass->setCamera(fixture.camera);
    pass->setRenderTarget(fixture.target);
    pass->setClearEnabled(true);
    pass->setClearColor(vn::Color(0, 0, 0));
    pass->setShouldClearDepth(true);
    // 无光照档的程序：没有光的环境里跑默认的前向程序，得到的是"被算黑了的画面"，而这一条要判的是覆盖，不是光照。
    pass->setProgramOverride(vn::graphics::unlitForwardProgram());
    fixture.engine->addPass(pass, fixture.scene, 0);

    const PixelProbe picture = fixture.read();
    ASSERT_TRUE(picture.valid());
    ASSERT_EQ(picture.width(), SdkPixelFixture::kSide);

    const std::size_t covered_count = coveredPixels(picture);
    std::printf("[sdk-pixels] no-view picture: covered=%zu of %zu\n", covered_count, picture.pixelCount());

    // 四边形在 [-0.9, 0.9] 里、相机是 [-1, 1] 的正交窗 ⇒ 覆盖住中间那一大块（81%），四角仍是清屏色。两个方向都
    // 判：只判"画了东西"分不出"清屏之后没画"和"整张都被涂满"，而这两种都是这条用例要排除的。
    EXPECT_GT(covered_count, picture.pixelCount() / 2U) << "内容必须真的画进了目标";
    EXPECT_LT(covered_count, picture.pixelCount() * 9U / 10U) << "四角必须还是清屏色，否则画的是整张目标";
    EXPECT_FALSE(covered(picture.pixel(0, 0))) << "左上角在四边形之外";
    EXPECT_TRUE(covered(picture.pixel(SdkPixelFixture::kSide / 2, SdkPixelFixture::kSide / 2)))
        << "画面正中在四边形之内";
}
