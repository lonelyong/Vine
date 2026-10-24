/**
 * @brief 内置阴影端到端的一张图：一盏投射方向光、一个挡在它和地面之间的 caster，地面上必须出现它的影子。
 *
 * WHY IT EXISTS. 阴影的每一段都有自己的断言——`ShadowBlockTest` 测块的打包（用一张手写的 producer 矩阵），
 * `ContentPassTest` 测绑定，`GraphicsTest` 测 chain 建出了哪条 pass——但"引擎建出来的那条 pipeline 画出的图里
 * 到底有没有影子"一直没有人读像素。缺这一条的代价在 2026-10-24 量到：demo 的地面上一片影子都没有，而每一段
 * 结构断言都是绿的（光相机、发布出去的矩阵、光空间的 v 轴、reverse-Z 的深度方向，任何一处接错都只表现为
 * "没有影子"）。同一天的第二件事也在这张图上：影子**存在**但落在相机背后时同样看不见，所以这个用例量的是
 * "影子落在它该落的地方"，而不是一个数字。
 *
 * 场景是刻意可算的：地面一块朝 +z 的四边形，挡板一块平行的四边形悬在它上面（z = 0.5），光沿 +x 斜射下来，
 * 所以影子落在挡板的 +x 一侧，落点由几何直接算出（见用例里的注释）。
 */

#include <gtest/gtest.h>

#include <algorithm>
#include <cstdio>
#include <vector>

#include <vine/graphics/Camera.hpp>
#include <vine/graphics/ChainPresets.hpp>
#include <vine/graphics/FramePlan.hpp>
#include <vine/graphics/Geometry.hpp>
#include <vine/graphics/Group.hpp>
#include <vine/graphics/Light.hpp>
#include <vine/graphics/Material.hpp>
#include <vine/graphics/MatrixTransform.hpp>
#include <vine/graphics/PassChain.hpp>
#include <vine/graphics/RenderPass.hpp>
#include <vine/graphics/Scene.hpp>

#include "SdkPixelFixture.hpp"
#include "TestGeometryBuilders.hpp"

namespace
{

using vn::graphics::ChainDesc;
using vn::graphics::Geometry;
using vn::graphics::Group;
using vn::graphics::Light;
using vn::graphics::Material;
using vn::graphics::MatrixTransform;
using vn::graphics::PassChain;
using vn::graphics::Scene;
using vn::graphics::ShadingPath;
using vn::test::setPrimitiveIndices;

/// @brief 造一块朝向 +z 的四边形，放在 @p z 处，半边长为 @p half。
vn::intrusive_ptr<Geometry> makeGroundQuad(double half, double z)
{
    const vn::intrusive_ptr<Geometry> geometry(new Geometry());
    const float                       h  = static_cast<float>(half);
    const float                       fz = static_cast<float>(z);
    geometry->setPositions(vn::intrusive_ptr<vn::Buffer<float>>(
        new vn::Buffer<float>(std::vector<float>{ -h, -h, fz, h, -h, fz, h, h, fz, -h, h, fz })));
    geometry->setNormals(vn::intrusive_ptr<vn::Buffer<float>>(
        new vn::Buffer<float>(std::vector<float>{ 0.0F, 0.0F, 1.0F, 0.0F, 0.0F, 1.0F, 0.0F, 0.0F, 1.0F, 0.0F, 0.0F,
                                                  1.0F })));
    setPrimitiveIndices(*geometry, 0U, vn::intrusive_ptr<vn::Buffer<std::uint32_t>>(
        new vn::Buffer<std::uint32_t>(std::vector<std::uint32_t>{ 0U, 1U, 2U, 0U, 2U, 3U })));
    geometry->setRevision(1U);

    const vn::intrusive_ptr<Material> material(new Material());
    material->setBaseColor(vn::Colorf::kWhite);
    material->setRoughness(1.0F);   // 无高光：影子区只剩环境项，"太阳这一项有没有"就是整张图的对比
    geometry->setMaterial(material);
    return geometry;
}

/// @brief 把一块几何挂到 @p root 下。
void addQuad(Group* root, const vn::intrusive_ptr<Geometry>& geometry)
{
    const vn::intrusive_ptr<MatrixTransform> transform(new MatrixTransform());
    transform->addChild(geometry);
    root->addChild(transform);
}

/// @brief 把这张图打印成粗粒度的 ASCII 亮度图（只在失败时看，不做断言）。
void printAscii(const vn::graphics::backend::PixelProbe& probe)
{
    const int side = probe.width();
    for (int y = 0; y < side; y += 2) {
        std::printf("[shadow-pixels] ");
        for (int x = 0; x < side; x += 2) {
            const auto pixel = probe.pixel(x, y);
            const int  value = static_cast<int>((pixel.r + pixel.g + pixel.b) / 3);
            std::printf("%c", value > 220 ? '#' : (value > 150 ? '+' : (value > 80 ? '.' : (value > 20 ? '-' : ' '))));
        }
        std::printf("\n");
    }
}

/// @brief 一辆 [0,1] 的照度：像素三通道的平均。
int brightnessOf(const vn::graphics::backend::Rgba8& pixel)
{
    return (static_cast<int>(pixel.r) + static_cast<int>(pixel.g) + static_cast<int>(pixel.b)) / 3;
}

/// @brief 世界 (x, y, 0) 在这张正交画面里的像素（相机是 [-1,1]^2、y 朝上，读回的行 0 是屏幕顶部）。
vn::graphics::backend::Rgba8 worldPixel(const vn::graphics::backend::PixelProbe& probe, double x, double y)
{
    const int side = probe.width();
    const int col  = static_cast<int>((x + 1.0) * 0.5 * static_cast<double>(side));
    const int row  = static_cast<int>((1.0 - y) * 0.5 * static_cast<double>(side));
    return probe.pixel(std::clamp(col, 0, side - 1), std::clamp(row, 0, side - 1));
}

/// @brief 地面 + 悬空挡板 + 一盏投射太阳，就是两个用例共用的场景。
vn::intrusive_ptr<Scene> buildCastingScene()
{
    const vn::intrusive_ptr<Group> root(new Group());
    addQuad(root.get(), makeGroundQuad(0.95, 0.0));
    addQuad(root.get(), makeGroundQuad(0.25, 0.5));   // 挡板：半边 0.25，悬在地面上方 0.5

    const vn::intrusive_ptr<Scene> content(new Scene());
    content->setRoot(root);

    auto ambient = Light::createAmbient();
    ambient->setIntensity(0.10F);
    content->addLight(ambient);

    // 太阳：光沿 (0.5, 0, -0.866) 行进 —— 从 -x 上方斜射下来，所以影子落在挡板的 +x 一侧。
    auto sun = Light::createDirectional(vn::math::Vec3d(0.5, 0.0, -0.866));
    sun->setIntensity(1.0F);
    sun->setCastShadow(true);
    sun->setShadowResolution(512);
    sun->setShadowBias(0.002F);
    content->addLight(sun);
    return content;
}

/**
 * @brief 断言这一帧里"亮区亮、影子区暗"。
 *
 * 亮区取挡板 -x 一侧的远处地面；影子区取 +x 一侧：挡板高 0.5、光的方向是 (0.5, 0, -0.866)，所以影子沿 +x
 * 偏移 0.5 * 0.5 / 0.866 ≈ 0.29，挡板的影子覆盖 x ∈ [0.04, 0.54]，取 x = 0.40。
 */
void expectTheShadowFalls(const vn::graphics::backend::PixelProbe& probe)
{
    ASSERT_TRUE(probe.valid()) << "这一帧必须读得回来";
    const int lit      = brightnessOf(worldPixel(probe, -0.70, 0.0));
    const int shadowed = brightnessOf(worldPixel(probe, 0.40, 0.0));
    std::printf("[shadow-pixels] lit=%d shadowed=%d\n", lit, shadowed);
    EXPECT_GT(lit, 150) << "亮区必须是太阳照到的地面";
    EXPECT_LT(shadowed, lit - 40) << "挡板的影子区必须比亮区暗：这就是'阴影图里到底有没有影子'的那张图";
}

}  // namespace

TEST(ShadowPixelTest, ACastingSunDarkensTheGroundWhereItsShadowFalls)
{
    VINE_SKIP_WITHOUT_SDK_DEVICE(fixture);

    fixture.scene = buildCastingScene();

    ChainDesc options;
    options.path    = ShadingPath::Deferred;
    options.camera  = fixture.camera.get();
    options.content = fixture.scene;
    vn::intrusive_ptr<PassChain> pipeline = PassChain::build(fixture.engine.get(), options);
    ASSERT_NE(pipeline, nullptr);

    vn::graphics::RenderPass* const picture = pipeline->presentingPass();
    ASSERT_NE(picture, nullptr);
    picture->setRenderTarget(fixture.target);
    picture->setClearEnabled(true);
    picture->setClearColor(vn::Colorf(0.0F, 0.0F, 0.0F, 1.0F));
    picture->setShouldClearDepth(true);

    // 计划里必须真的有一条 shadow pass：没有它，"地面很亮"和"地面亮且被影子切过"是同一张图。
    const vn::graphics::FramePlan plan = fixture.engine->framePlan();
    bool                         has_shadow_pass = false;
    for (const vn::graphics::PassPlan& row : plan.passes) {
        has_shadow_pass = has_shadow_pass || (row.name == vn::String(u8"shadow"));
    }
    ASSERT_TRUE(has_shadow_pass) << "一盏 setCastShadow(true) 的方向灯必须让 Deferred 预设建出 shadow pass";

    const vn::graphics::backend::PixelProbe read_back = fixture.read();
    printAscii(read_back);
    expectTheShadowFalls(read_back);
}

/**
 * @brief 有透明 OVERLAY 内容时（demo 的天空盒 / 混合对走的就是这一支），影子同样必须落下来。
 *
 * WHY IT IS A SECOND CASE. `ChainBuilder` 在这条路径上把光照 pass 改成画进一张离屏 composite（而不是窗口），
 * 那份 pass 的输入列表是另外拼的——阴影图漏在这一次拼接里，影子就会**只**在这条路径上消失，而上面的用例
 * 仍然是绿的。demo 的默认路径正是这一条。
 */
TEST(ShadowPixelTest, TheSameShadowFallsWhenTheFrameCompositesAnOverlay)
{
    VINE_SKIP_WITHOUT_SDK_DEVICE(fixture);

    fixture.scene = buildCastingScene();

    // 空的 overlay 场景就够：声明了 `transparent` 内容，构图就换到 composite 那一支。
    const vn::intrusive_ptr<Scene> overlay(new Scene());
    overlay->setRoot(vn::intrusive_ptr<Group>(new Group()));

    ChainDesc options;
    options.path        = ShadingPath::Deferred;
    options.camera      = fixture.camera.get();
    options.content     = fixture.scene;
    options.transparent = overlay;
    vn::intrusive_ptr<PassChain> pipeline = PassChain::build(fixture.engine.get(), options);
    ASSERT_NE(pipeline, nullptr);

    vn::graphics::RenderPass* const picture = pipeline->presentingPass();
    ASSERT_NE(picture, nullptr);
    picture->setRenderTarget(fixture.target);
    picture->setClearEnabled(true);
    picture->setClearColor(vn::Colorf(0.0F, 0.0F, 0.0F, 1.0F));
    picture->setShouldClearDepth(true);

    const vn::graphics::backend::PixelProbe read_back = fixture.read();
    expectTheShadowFalls(read_back);
}

/**
 * @brief 同一条内容走 FORWARD 路径时，影子同样要落在该落的地方。
 *
 * WHY IT IS A THIRD CASE. 前向路径的接线是另一套：阴影图是**主 pass 的输入**，而不是一条全屏光照 pass 的
 * 输入；着色项由内容程序（`forwardProgram()`，无条件带阴影 ABI）在逐 drawable 的块里拿开关。这两处任一
 * 接错，deferred 的两个用例全绿而 forward 没有影子。
 */
TEST(ShadowPixelTest, TheSameShadowFallsOnTheForwardPath)
{
    VINE_SKIP_WITHOUT_SDK_DEVICE(fixture);

    fixture.scene = buildCastingScene();

    ChainDesc options;
    options.path    = ShadingPath::Forward;
    options.camera  = fixture.camera.get();
    options.content = fixture.scene;
    vn::intrusive_ptr<PassChain> pipeline = PassChain::build(fixture.engine.get(), options);
    ASSERT_NE(pipeline, nullptr);

    vn::graphics::RenderPass* const picture = pipeline->presentingPass();
    ASSERT_NE(picture, nullptr);
    picture->setRenderTarget(fixture.target);
    picture->setClearEnabled(true);
    picture->setClearColor(vn::Colorf(0.0F, 0.0F, 0.0F, 1.0F));
    picture->setShouldClearDepth(true);

    const vn::graphics::backend::PixelProbe read_back = fixture.read();
    printAscii(read_back);
    expectTheShadowFalls(read_back);
}
