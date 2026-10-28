#include "SdkPixelFixture.hpp"

#include <gtest/gtest.h>

#include <cstddef>
#include <cstdint>
#include <cstdio>

#include <array>

#include <vine/Buffer.hpp>
#include <vine/graphics/BuiltinShaders.hpp>
#include <vine/graphics/DepthMode.hpp>
#include <vine/graphics/Geometry.hpp>

#include <vine/graphics/Group.hpp>
#include <vine/graphics/ImageRef.hpp>
#include <vine/graphics/Material.hpp>
#include <vine/graphics/MatrixTransform.hpp>
#include <vine/graphics/PassChain.hpp>
#include <vine/graphics/RenderEngine.hpp>
#include <vine/graphics/RenderPass.hpp>
#include <vine/graphics/RenderTarget.hpp>
#include <vine/graphics/SceneView.hpp>
#include <vine/graphics/ScreenPass.hpp>
#include <vine/graphics/StateNode.hpp>

#include "TestGeometryBuilders.hpp"

using vn::graphics::ChainDesc;
using vn::graphics::PassChain;
using vn::graphics::RenderPass;
using vn::graphics::RenderTarget;
using vn::graphics::SceneView;
using vn::graphics::ScreenPass;
using vn::graphics::ShadingMode;
using vn::graphics::ShadingPath;
using vn::graphics::backend::PixelProbe;
using vn::graphics::backend::Rgba8;
using vn::test::setPrimitiveIndices;

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
           !covered(probe.pixel(x, y + 1 < probe.height() ? y + 1 : y)) || !covered(probe.pixel(x > 0 ? x - 1 : x, y)) ||
           !covered(probe.pixel(x, y > 0 ? y - 1 : y));
}

} // namespace

// 线框不是"少画一点"，是**同一批几何按边画**：同一个视图、同一台相机、同一条 pass、同一张目标，只把光栅化模式换掉
// （M1 把四档做成了 pass 的属性，换档不重建），两版画面对照之下边真的存在、且边落在实体版覆盖的那块轮廓上。这一条
// 把 M1 从"该画的 pass 在画"补到"画出来的像素对"：配方是**延迟**的、档是非 Lit 的，于是生效的链条按前向建，画面就是
// 那条几何 pass 产出的 —— 正是这一条要探的那张画面。
TEST(SdkPixelTest, AWireframeModeDrawsTheEdgesOfTheSameGeometry)
{
    VN_SKIP_WITHOUT_SDK_DEVICE(fixture);

    vn::intrusive_ptr<SceneView> view(new SceneView());
    view->setEngine(fixture.engine.get());
    view->setScene(fixture.scene);
    view->resize(SdkPixelFixture::kSide, SdkPixelFixture::kSide, 1.0);

    // Built once, with the options that are in force for the first picture: the chain forward, so the picture
    // pass is the scene pass itself.
    ChainDesc options;
    options.path    = ShadingPath::Deferred; // 延迟的**请求**……
    options.mode    = ShadingMode::Unlit;    // ……配上非 Lit 档 ⇒ 建成的链是前向的（effectivePath）
    options.camera  = view->camera();
    options.content = view->scene();
    view->setChain(PassChain::build(fixture.engine.get(), options));
    ASSERT_NE(view->chain(), nullptr);

    // The picture pass draws into the fixture's off-screen target instead of the window: the SDK's own pass
    // object, with the SDK's own mode applied to it, so the ONE thing this case turns on is the polygon mode.
    RenderPass* const picture = view->chain()->presentingPass();
    ASSERT_NE(picture, nullptr);
    picture->setRenderTarget(fixture.target);
    picture->setClearEnabled(true);
    picture->setClearColor(vn::Colorf(0.0F, 0.0F, 0.0F, 1.0F));
    picture->setShouldClearDepth(true);
    ASSERT_TRUE(view->fitToScreen()) << "默认操纵器按内容把相机摆好";

    const PixelProbe filled = fixture.read();
    ASSERT_TRUE(filled.valid()) << "画出来的这一帧必须读得回来";
    ASSERT_EQ(filled.width(), SdkPixelFixture::kSide);

    // 同一帧的另一版：只换档。链条没变 ⇒ 不该重建 ⇒ 还是那条 pass（目标没被换掉），相机当然也没动。
    // 这里**不判**"pass 上多了个 PolygonMode::Line 覆盖"：那是 M1 在 test_graphics 的结构判据；这一条判的是像素，
    // 覆盖没到光栅器就该由画面来说（否则结构断言会先红，抢掉像素判据的那一刀）。
    EXPECT_TRUE(view->chain()->setShadingMode(ShadingMode::Wireframe));
    ASSERT_EQ(view->chain()->presentingPass(), picture) << "链条内换档不重建（M1 的另一半）";

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
                filled_covered,
                line_covered,
                filled.pixelCount(),
                edges_inside,
                silhouette,
                traced);

    // 包含关系：每一个边上的像素都**贴着实体版覆盖的那块**。这比"两边数量不同"强得多 —— 它排除了"线框版画的是
    // 另一批东西"（错的目标、上一帧的残留、叠加的 HUD）。
    EXPECT_EQ(edges_inside, line_covered) << "边必须落在实体版覆盖的那块轮廓上（差集就是「线框版画了别的东西」）";
    // 反过来也判：实体版的轮廓**大部分都有边走过**，否则"边"可能只是几条不相干的线段。
    EXPECT_GT(silhouette, 0U);
    EXPECT_GT(traced * 2U, silhouette) << "实体版轮廓的一半以上必须有边走过";
}

// 夹具本身的最小用例：**一个视图都没有**，一个手搭的 pass 把同一批内容画进同一张目标。上一条量的是 SDK 的默认
// 视图，如果夹具只能通过视图工作，那它量到的就是自己的脚手架。
TEST(SdkPixelTest, TheFixtureAlsoDrawsContentWithNoViewAtAll)
{
    VN_SKIP_WITHOUT_SDK_DEVICE(fixture);

    vn::intrusive_ptr<RenderPass> pass(new RenderPass());
    pass->setName(vn::String(u8"hand_wired_picture"));
    pass->setCamera(fixture.camera);
    pass->setRenderTarget(fixture.target);
    pass->setClearEnabled(true);
    pass->setClearColor(vn::Colorf(0.0F, 0.0F, 0.0F, 1.0F));
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
    EXPECT_TRUE(covered(picture.pixel(SdkPixelFixture::kSide / 2, SdkPixelFixture::kSide / 2))) << "画面正中在四边形之内";
}

// 延迟预设的 G-buffer：四张彩色附件都必须真的被几何 pass 写进去。
//
// WHY IT IS A PIXEL CASE. "The preset's G-buffer pass carries the G-buffer program" is a wiring fact, and it is
// asserted where wiring lives (test_graphics). What a host sees is one step further and this is where it is
// caught: the pass can carry the program and still leave three attachments at their clear, and the demo's
// default scene showed exactly that - the last three G-buffer previews were BLACK rectangles, because the
// geometry pass drew every body through a one-output program (base colour landed, normal / emission / position
// kept their transparent-black clear). So the copies below are the demo's own declarations - the SDK's
// screenCopyProgram(n) over an ImageRef bound to (target, n) - and the assertion is on what they land.
TEST(SdkPixelTest, TheDeferredPresetFillsEveryGbufferAttachment)
{
    VN_SKIP_WITHOUT_SDK_DEVICE(fixture);

    ChainDesc options;
    options.path                                        = ShadingPath::Deferred;
    options.camera                                      = fixture.camera.get();
    options.content                                     = fixture.scene;
    vn::intrusive_ptr<vn::graphics::PassChain> pipeline = PassChain::build(fixture.engine.get(), options);
    ASSERT_NE(pipeline, nullptr) << "延迟预设必须建得起来";

    // 夹具的"窗口"是后端自己开的，读不回来：把画面那条 pass 指到夹具的目标上，预览才有东西可叠。
    RenderPass* const picture = pipeline->presentingPass();
    ASSERT_NE(picture, nullptr);
    picture->setRenderTarget(fixture.target);
    picture->setClearEnabled(true);
    picture->setClearColor(vn::Colorf(0.0F, 0.0F, 0.0F, 1.0F));
    picture->setShouldClearDepth(true);

    RenderTarget* const gbuffer = pipeline->offscreenTarget();
    ASSERT_NE(gbuffer, nullptr);
    ASSERT_EQ(gbuffer->colorCount(), 4) << "延迟预设的 G-buffer 是四张彩色附件（见 defaultGbufferTarget）";

    // 四格预览 = demo 默认场景里那四条的写法：同一张源、每格一个换了下标的屏程序、一个 12x12 的视口。
    // 第三格用的是**拷 alpha**的那个变体（demo 同样如此）：attachment 2 的 rgb 是 emissive、本场景不发光，
    // 而它的 alpha 是遮挡因子 —— 写过的像素是 1、清屏处是 0，所以这一格是“这一张附件到底写了没有”的证据。
    constexpr int kSlot     = 2;
    constexpr int kSlotStep = 15;
    constexpr int kSlotSide = 12;
    for (int attachment = 0; attachment < 4; ++attachment) {
        vn::intrusive_ptr<ScreenPass> preview(new ScreenPass());
        preview->setName(vn::String(u8"gbuffer_preview"));
        preview->setCamera(vn::intrusive_ptr<vn::graphics::Camera>(fixture.camera));
        preview->setProgram(
            vn::graphics::screenCopyProgram(attachment, attachment == 2 ? vn::graphics::ScreenCopyChannels::Alpha : vn::graphics::ScreenCopyChannels::All));
        preview->addInputName(u8"GBuffer");
        vn::intrusive_ptr<vn::graphics::ImageRef> image(new vn::graphics::ImageRef(u8"GBuffer.preview"));
        image->bind(vn::intrusive_ptr<RenderTarget>(gbuffer), attachment);
        preview->addInput(image);
        preview->setRenderTarget(fixture.target);
        preview->setClearEnabled(false);
        preview->setShouldClearDepth(false);
        preview->setViewport(kSlot + attachment * kSlotStep, kSlot, kSlotSide, kSlotSide);
        fixture.engine->addPass(preview, 120 + attachment);
    }

    const PixelProbe read_back = fixture.read();
    ASSERT_TRUE(read_back.valid()) << "这一帧必须读得回来";

    // 每格取正中一个像素：四边形在夹具相机里盖住画面中央，所以这个点一定在几何上（否则下面的断言量的是清屏色）。
    std::array<Rgba8, 4> copies{};
    for (int attachment = 0; attachment < 4; ++attachment) {
        copies[static_cast<std::size_t>(attachment)] = read_back.pixel(kSlot + attachment * kSlotStep + kSlotSide / 2, kSlot + kSlotSide / 2);
        std::printf("[sdk-pixels] gbuffer attachment %d copy: r=%u g=%u b=%u a=%u\n",
                    attachment,
                    copies[static_cast<std::size_t>(attachment)].r,
                    copies[static_cast<std::size_t>(attachment)].g,
                    copies[static_cast<std::size_t>(attachment)].b,
                    copies[static_cast<std::size_t>(attachment)].a);
    }

    for (int attachment = 0; attachment < 4; ++attachment) {
        // 第 2 张附件拷的是 alpha（遮挡）：写过的像素是 1 → 白，清屏处是 0 → 黑，两种“被写过”的证据都在这里
        // 读得出来；其余三张写的就是颜色，透明黑 = 没写。
        const Rgba8& pixel   = copies[static_cast<std::size_t>(attachment)];
        const bool   written = attachment == 2 ? (pixel.r > 128U) : covered(pixel);
        EXPECT_TRUE(written) << "第 " << attachment << " 格是透明黑：那张附件留着清屏色 = 几何 pass 没写它（正是后三格全黑的那个失败）";
    }
    // 附件下标真的换了东西：四格里读到的不是同一张附件（绑定恒为 0 会让下面两条红），而且法线那张确实是法线
    // ——夹具的四边形法向 (0,0,1) 的八面体编码是 (0,0)，所以 rg 归零，蓝通道留的是材质的粗糙度。
    EXPECT_NE(copies[1], copies[0]) << "法线格与基础色格必须不是同一个像素值";
    EXPECT_GT(copies[1].b, copies[1].r) << "法线格以蓝为主（(0,0,1) 的八面体编码占 rg，粗糙度写在 b）";
    EXPECT_GT(copies[1].b, copies[1].g);
}

namespace
{

/// @brief 造一张共面四边形（同一 z），带自己的材质颜色，作为 @p parent 的子节点。
///
/// 用的就是夹具那张 quad 的顶点（`[-0.9, 0.9]` 的方块、法向 +z、两个三角形），只是颜色归调用方给 —— 两张叠在
/// 一起的四边形是"谁在上面"这件事唯一能读出来的形状。
vn::intrusive_ptr<vn::graphics::MatrixTransform>
makeCoplanarQuad(const vn::intrusive_ptr<vn::graphics::Group>& parent, const vn::Colorf& colour, vn::graphics::AlphaMode mode = vn::graphics::AlphaMode::Opaque)
{
    const float                                     half = 0.9F;
    const float                                     z    = 0.0F;
    const vn::intrusive_ptr<vn::graphics::Geometry> geometry(new vn::graphics::Geometry());
    geometry->setPositions(
        vn::intrusive_ptr<vn::Buffer<float>>(new vn::Buffer<float>(std::vector<float>{ -half, -half, z, half, -half, z, half, half, z, -half, half, z })));
    geometry->setNormals(vn::intrusive_ptr<vn::Buffer<float>>(
        new vn::Buffer<float>(std::vector<float>{ 0.0F, 0.0F, 1.0F, 0.0F, 0.0F, 1.0F, 0.0F, 0.0F, 1.0F, 0.0F, 0.0F, 1.0F })));
    setPrimitiveIndices(*geometry,
                        0U,
                        vn::intrusive_ptr<vn::Buffer<std::uint32_t>>(new vn::Buffer<std::uint32_t>(std::vector<std::uint32_t>{ 0U, 1U, 2U, 0U, 2U, 3U })));
    geometry->setRevision(1U);

    const vn::intrusive_ptr<vn::graphics::Material> material(new vn::graphics::Material());
    material->setBaseColor(colour);
    material->setAlphaMode(mode);

    const vn::intrusive_ptr<vn::graphics::StateNode> state(new vn::graphics::StateNode());
    state->setMaterial(material);
    state->addChild(geometry);

    const vn::intrusive_ptr<vn::graphics::MatrixTransform> transform(new vn::graphics::MatrixTransform());
    transform->setMatrix(vn::math::Mat4d());
    transform->addChild(state);
    parent->addChild(transform);
    return transform;
}

/// @brief 这个像素是否以 @p channel 为主（无光照档下材质色直接落地，颜色空间由平台决定）。
bool dominantly(const vn::graphics::backend::Rgba8& pixel, char channel)
{
    const auto& r = pixel.r;
    const auto& g = pixel.g;
    const auto& b = pixel.b;
    if (channel == 'r') {
        return r > 128U && g < 64U && b < 64U;
    }
    return g > 128U && r < 64U && b < 64U;
}

} // namespace

// 同一 pass 内的画序**是像素**，不是列表次序：两张**共面**的四边形、颜色一红一绿、pass 关掉深度（共面时只有顺序
// 能决定谁留下），于是"后画的赢"这句话可以直接读出来。默认没有声明时赢的是树里靠后的那个（稳定排序），而一句
// `setDrawOrder(1)` 就把它翻过来 —— 描边 / 贴花 / 高亮那一类"必须压在已经画好的东西上面"的声明就靠这个。
TEST(SdkPixelTest, ADrawOrderDecidesWhichOfTwoCoplanarQuadsIsOnTop)
{
    VN_SKIP_WITHOUT_SDK_DEVICE(fixture);

    const vn::intrusive_ptr<vn::graphics::Group> root(new vn::graphics::Group());
    const auto                                   red   = makeCoplanarQuad(root, vn::Colorf(1.0F, 0.0F, 0.0F, 1.0F));
    const auto                                   green = makeCoplanarQuad(root, vn::Colorf(0.0F, 1.0F, 0.0F, 1.0F));

    const vn::intrusive_ptr<vn::graphics::Scene> scene(new vn::graphics::Scene());
    scene->setRoot(root);

    vn::intrusive_ptr<RenderPass> pass(new RenderPass());
    pass->setName(vn::String(u8"draw_order_picture"));
    pass->setCamera(fixture.camera);
    pass->setRenderTarget(fixture.target);
    pass->setClearEnabled(true);
    pass->setClearColor(vn::Colorf(0.0F, 0.0F, 0.0F, 1.0F));
    pass->setShouldClearDepth(true);
    // 共面 ⇒ 深度测试会让"后画的"被拒（同深度 LESS 不成立），而这条要量的正是**顺序**，所以把深度关掉：
    // pass 里只剩顺序这一个决定谁留下的因素。
    pass->setDepthMode(vn::graphics::DepthMode::Disabled);
    // 无光照档：没有灯的环境里跑默认前向程序会得到"被算黑"的画面，而这条判的是覆盖顺序，不是光照。
    pass->setProgramOverride(vn::graphics::unlitForwardProgram());
    fixture.engine->addPass(pass, scene, 0);

    const PixelProbe first = fixture.read();
    ASSERT_TRUE(first.valid());
    const auto& centre_before = first.pixel(SdkPixelFixture::kSide / 2, SdkPixelFixture::kSide / 2);
    std::printf("[sdk-pixels] draw order: default centre=(%u,%u,%u)\n", centre_before.r, centre_before.g, centre_before.b);
    EXPECT_TRUE(dominantly(centre_before, 'g')) << "没有声明时，树里**靠后**那张（绿）后画 ⇒ 它在上面（稳定排序保住了遍历序）";

    // 一句声明把它翻过来：红的那张画在最后。
    red->setDrawOrder(1);
    const PixelProbe second = fixture.read();
    ASSERT_TRUE(second.valid());
    const auto& centre_after = second.pixel(SdkPixelFixture::kSide / 2, SdkPixelFixture::kSide / 2);
    std::printf("[sdk-pixels] draw order: declared centre=(%u,%u,%u)\n", centre_after.r, centre_after.g, centre_after.b);
    EXPECT_TRUE(dominantly(centre_after, 'r')) << "红的那张声明了更高的画序 ⇒ 它后画、压在上面（这正是高亮 / 描边的做法）";
    EXPECT_NE(centre_before.r, centre_after.r) << "同一个像素真的换了主人，不是两次读到同一张残影";
}

// 透明度的三个模式**是像素**（graphics-material.md 2.7 / F7 / T4）：同一张四边形、同一个无光照档程序，只换材质的
// 模式与自身的 alpha —— 于是"模式做了什么"可以直接读出来，而不用先相信着色器里的分支。
//
// 三个相位用的是**同一份材质**，在两次读之间改它的值：后端每帧重读命令里的材质（`ContentStore::updateMaterial`
// 读的是 `revision()`），所以改材质不需要重建场景 —— 这正是设计里"材质改了就下一帧生效"那条。
TEST(SdkPixelTest, TheAlphaModeDecidesWhatALeafCutsAndWhatItBlends)
{
    VN_SKIP_WITHOUT_SDK_DEVICE(fixture);

    const vn::intrusive_ptr<vn::graphics::Group> root(new vn::graphics::Group());
    // alpha = 0.25 的 Mask：低于默认 cutoff（0.5）⇒ 整片被 discard，画面留着清屏色。
    const auto quad = makeCoplanarQuad(root, vn::Colorf(1.0F, 0.0F, 0.0F, 0.25F), vn::graphics::AlphaMode::Mask);

    const vn::intrusive_ptr<vn::graphics::Scene> scene(new vn::graphics::Scene());
    scene->setRoot(root);

    vn::intrusive_ptr<RenderPass> pass(new RenderPass());
    pass->setName(vn::String(u8"alpha_mode_picture"));
    pass->setCamera(fixture.camera);
    pass->setRenderTarget(fixture.target);
    pass->setClearEnabled(true);
    pass->setClearColor(vn::Colorf(0.0F, 0.0F, 0.0F, 1.0F));
    pass->setShouldClearDepth(true);
    // 无光照档：这条量的是"模式"，不是光照算术。
    pass->setProgramOverride(vn::graphics::unlitForwardProgram());
    fixture.engine->addPass(pass, scene, 0);

    const auto centre = [&]() { return fixture.read().pixel(SdkPixelFixture::kSide / 2, SdkPixelFixture::kSide / 2); };

    // ① Mask + alpha 0.25：低于 cutoff，几何不写任何像素。
    const Rgba8 cut = centre();
    std::printf("[sdk-pixels] mask 0.25: (%u,%u,%u)\n", cut.r, cut.g, cut.b);
    EXPECT_EQ(cut.r, 0U) << "Mask 低于 cutoff 的片元必须整片消失（清屏色保留）";

    // ② 同一个材质把自身 alpha 抬到 0.75（越过 cutoff）：几何出现，且**完全不透明**（Mask 不淡化，它裁剪）。
    auto* const state_node = dynamic_cast<vn::graphics::StateNode*>(quad->children().front().get());
    ASSERT_NE(state_node, nullptr) << "夹具的四边形挂在 StateNode 下";
    auto* const leaf = dynamic_cast<vn::graphics::Material*>(state_node->material());
    ASSERT_NE(leaf, nullptr) << "夹具的四边形自带一份材质";
    leaf->setBaseColor(vn::Colorf(1.0F, 0.0F, 0.0F, 0.75F));
    const Rgba8 kept = centre();
    std::printf("[sdk-pixels] mask 0.75: (%u,%u,%u)\n", kept.r, kept.g, kept.b);
    EXPECT_GT(kept.r, 200U) << "存活下来的片元是底座色本身（Mask 存活即不透明：alpha 0.75 不该把它压暗）";

    // ③ Blend + alpha 0.5：模式让这条 drawable 进透明队列、打开混合，输出 α = opacity × 材质 α = 0.5，
    //    于是在全黑清屏上留下底色的一半。
    leaf->setAlphaMode(vn::graphics::AlphaMode::Blend);
    leaf->setBaseColor(vn::Colorf(1.0F, 0.0F, 0.0F, 0.5F));
    const Rgba8 blended = centre();
    std::printf("[sdk-pixels] blend 0.5: (%u,%u,%u)\n", blended.r, blended.g, blended.b);
    EXPECT_GT(blended.r, 100U) << "Blend 必须混合（半个底色）——0 意味着模式没让这条 drawable 打开混合";
    EXPECT_LT(blended.r, 200U) << "也不该是整块底色 ——那是不透明的那个值";
}
