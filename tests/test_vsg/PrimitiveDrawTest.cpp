#include "SdkPixelFixture.hpp"

#include <gtest/gtest.h>

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <vector>

#include <vine/Buffer.hpp>
#include <vine/Color.hpp>
#include <vine/graphics/BuiltinShaders.hpp>
#include <vine/graphics/DepthMode.hpp>
#include <vine/graphics/FrameStats.hpp>
#include <vine/graphics/Geometry.hpp>
#include <vine/graphics/Material.hpp>
#include <vine/graphics/MatrixTransform.hpp>
#include <vine/graphics/Primitive.hpp>
#include <vine/graphics/RenderPass.hpp>
#include <vine/graphics/Scene.hpp>
#include <vine/graphics/StateNode.hpp>
#include <vine/graphics/Topology.hpp>
#include <vine/math/Matrix4x4.hpp>
#include <vine/vsg/internal/BackendContent.hpp>
#include <vine/vsg/internal/ContentAssembly.hpp>
#include <vine/vsg/internal/StreamUploads.hpp>

using vn::graphics::DepthMode;
using vn::graphics::Geometry;
using vn::graphics::Material;
using vn::graphics::MatrixTransform;
using vn::graphics::Primitive;
using vn::graphics::RenderPass;
using vn::graphics::Scene;
using vn::graphics::StateNode;
using vn::graphics::Topology;
using vn::graphics::backend::PixelProbe;
using vn::graphics::backend::Rgba8;

namespace
{
/// @brief The world plane every part of the scene lies in (see the fixture's camera).
constexpr float kPlaneZ = 0.5F;

/// @brief The filled block's left and right edges: the left third of the frame.
constexpr float kFillLeft  = -0.9F;
constexpr float kFillRight = -0.2F;

/// @brief The top and bottom of every part (the fixture's camera window is [-1, 1]).
constexpr float kPartTop    = 0.9F;
constexpr float kPartBottom = -0.9F;

/// @brief The vertical line's column: between the fill and the points.
constexpr float kLineX = 0.2F;

/// @brief The points' column: the right third of the frame, clear of the line.
constexpr float kPointsX = 0.7F;

/// @brief Whether a pixel is (dominantly) red / green / blue.
///
/// The three parts are drawn with saturated colours and no antialiasing, so a pixel is one part's or the
/// clear colour's - the slack is there to keep a driver's rounding out of the judgement, not to blend.
bool reddish(const Rgba8& pixel)
{
    return pixel.r > 128U && pixel.g < 64U && pixel.b < 64U;
}

bool greenish(const Rgba8& pixel)
{
    return pixel.g > 128U && pixel.r < 64U && pixel.b < 64U;
}

bool bluish(const Rgba8& pixel)
{
    return pixel.b > 128U && pixel.r < 64U && pixel.g < 64U;
}

/// @brief How many pixels each of the three parts owns.
struct PartPixels
{
    std::size_t red{0};
    std::size_t green{0};
    std::size_t blue{0};
};

PartPixels countParts(const PixelProbe& probe)
{
    PartPixels counts;
    for (int y = 0; y < probe.height(); ++y) {
        for (int x = 0; x < probe.width(); ++x) {
            const Rgba8& pixel = probe.pixel(x, y);
            if (reddish(pixel)) {
                ++counts.red;
            } else if (greenish(pixel)) {
                ++counts.green;
            } else if (bluish(pixel)) {
                ++counts.blue;
            }
        }
    }
    return counts;
}

/// @brief Packs @p count vertices of an xyz run into the scalar buffer a channel reads.
vn::intrusive_ptr<vn::Buffer<float>> packVec3(std::vector<float> scalars)
{
    return vn::intrusive_ptr<vn::Buffer<float>>(new vn::Buffer<float>(std::move(scalars)));
}

/**
 * @brief The scene every case draws: ONE geometry whose data is three parts, at three places in the frame.
 *
 * WHY ONE GEOMETRY AND THREE PLACES. The claim under test is "one geometry, N primitives, one upload, N
 * draws", and a picture can only prove the second half if the parts cannot be confused with each other: so
 * the triangles fill the left third, the line runs down the middle, and the points sit in the right third.
 * Three colours at three places cannot come from one draw, and none of them can come from a stray pixel of
 * another part.
 *
 * WHY THE STATE NODE'S MATERIAL IS WHITE. It is the trap: if the per-primitive material were dropped on the
 * way to the backend, every part would inherit white and the three colour counts would read zero - which is
 * exactly what this case has to catch.
 *
 * WHY ONE INDEX ARENA. All three spans are slices of ONE index buffer, which is the sharing the API is built
 * around (the backend aliases the whole buffer and the draw states the span). The upload counts below read
 * the consequence: three uploads (positions, normals, the arena) for a frame that records nine stream binds.
 *
 * @return The scene the case draws.
 */
vn::intrusive_ptr<Scene> makeThreePartScene()
{
    const vn::intrusive_ptr<Geometry> geometry(new Geometry());
    geometry->setPositions(packVec3({ // 0..3 - the filled block
                                     kFillLeft, kPartBottom, kPlaneZ, kFillRight, kPartBottom, kPlaneZ,
                                     kFillRight, kPartTop, kPlaneZ, kFillLeft, kPartTop, kPlaneZ,
                                     // 4..5 - the line's two ends
                                     kLineX, kPartBottom, kPlaneZ, kLineX, kPartTop, kPlaneZ,
                                     // 6..9 - the four points
                                     kPointsX, -0.6F, kPlaneZ, kPointsX, -0.2F, kPlaneZ, kPointsX, 0.2F,
                                     kPlaneZ, kPointsX, 0.6F, kPlaneZ }));
    geometry->setNormals(packVec3({ 0.0F, 0.0F, 1.0F, 0.0F, 0.0F, 1.0F, 0.0F, 0.0F, 1.0F, 0.0F,
                                    0.0F, 1.0F, 0.0F, 0.0F, 1.0F, 0.0F, 0.0F, 1.0F, 0.0F, 0.0F,
                                    1.0F, 0.0F, 0.0F, 1.0F, 0.0F, 0.0F, 1.0F, 0.0F, 0.0F, 1.0F }));

    const vn::intrusive_ptr<vn::Buffer<std::uint32_t>> arena(new vn::Buffer<std::uint32_t>(
        std::vector<std::uint32_t>{ 0U, 1U, 2U, 0U, 2U, 3U, // 三角形：两块拼成左边那块面
                                    4U, 5U,                // 线：一条竖线段
                                    6U, 7U, 8U, 9U }));    // 点：四个顶点

    // 数据即绘制给了这条几何体一条默认图元（三角形、非索引）；这一条要的是拼出来的三条，从空开始。
    geometry->clearPrimitives();

    const vn::intrusive_ptr<Material> fill(new Material());
    fill->setDiffuse(vn::Colorf(1.0F, 0.0F, 0.0F, 1.0F));

    const vn::intrusive_ptr<Material> outline(new Material());
    outline->setDiffuse(vn::Colorf(0.0F, 1.0F, 0.0F, 1.0F));

    const vn::intrusive_ptr<Material> markers(new Material());
    markers->setDiffuse(vn::Colorf(0.0F, 0.0F, 1.0F, 1.0F));

    Primitive triangles;
    triangles.topology = Topology::Triangles;
    triangles.indices  = Primitive::IndexSpan::slice(arena, 0U, 6U);
    triangles.material = fill;
    geometry->addPrimitive(triangles);

    Primitive lines;
    lines.topology = Topology::Lines;
    lines.indices  = Primitive::IndexSpan::slice(arena, 6U, 2U);
    lines.material = outline;
    geometry->addPrimitive(lines);

    Primitive points;
    points.topology = Topology::Points;
    points.indices  = Primitive::IndexSpan::slice(arena, 8U, 4U);
    points.material = markers;
    geometry->addPrimitive(points);

    // 通道与图元是分批改完的，最后公告一次（见 Geometry::bumpRevision）。
    geometry->bumpRevision();

    const vn::intrusive_ptr<Material> inherited(new Material());
    inherited->setDiffuse(vn::Colorf(1.0F, 1.0F, 1.0F, 1.0F));

    const vn::intrusive_ptr<StateNode> state(new StateNode());
    state->setMaterial(inherited);
    state->addChild(geometry);

    const vn::intrusive_ptr<MatrixTransform> transform(new MatrixTransform());
    transform->setMatrix(vn::math::Mat4d());
    transform->addChild(state);

    const vn::intrusive_ptr<Scene> scene(new Scene());
    scene->setRoot(transform);
    return scene;
}

} // namespace

// 一条几何体、三条图元、一份索引 arena：**画面上是三段颜色，计数器上是三次上传**。
//
// WHY IT IS A DEVICE CASE. Everything about the primitives model that can be asserted without a device is
// asserted without one (the fact rows, the plan's (primitive, topology) pair, the collector's one command per
// primitive). What none of those can answer is the claim the model exists for: an asset's parts share one
// vertex set and still reach the rasteriser as separate draws - and that is a picture plus a binding count.
TEST(PrimitiveDrawTest, OneGeometryWithThreePrimitivesDrawsThreePartsFromOneUpload)
{
    VINE_SKIP_WITHOUT_SDK_DEVICE(fixture);

    const vn::intrusive_ptr<Scene> scene = makeThreePartScene();
    const vn::intrusive_ptr<RenderPass> pass(new RenderPass());
    pass->setName(vn::String(u8"three_parts"));
    pass->setCamera(fixture.camera);
    pass->setRenderTarget(fixture.target);
    pass->setClearEnabled(true);
    pass->setClearColor(vn::Colorf(0.0F, 0.0F, 0.0F, 1.0F));
    pass->setShouldClearDepth(true);
    // 三段共面：深度 LESS 会让后画的两段在自己的深度上被拒，而这一条量的是"三条都到了光栅器"，
    // 不是遮挡关系 —— 关掉深度，两个变量只剩拓扑与颜色。
    pass->setDepthMode(DepthMode::Disabled);
    // 无光照档：没有灯的环境里跑默认前向程序会得到一张被算黑的画面，而这一条判的是"哪一段画的"。
    pass->setProgramOverride(vn::graphics::unlitForwardProgram());
    fixture.engine->addPass(pass, scene, 0);

    const PixelProbe picture = fixture.read();
    ASSERT_TRUE(picture.valid()) << "画出来的这一帧必须读得回来";
    ASSERT_EQ(picture.width(), SdkPixelFixture::kSide);

    const PartPixels drawn = countParts(picture);
    std::printf("[primitives] one geometry, three primitives: red(fill)=%zu green(line)=%zu blue(points)=%zu\n",
                drawn.red, drawn.green, drawn.blue);

    // 三个下界各自钉一条图元：面盖住左边一大块、线是一列像素、点是四个 1 像素的点。
    EXPECT_GT(drawn.red, 800U) << "三角形图元没画出左边的面（或它的材质没被用上）";
    EXPECT_GT(drawn.green, 30U) << "线图元没画出那条竖线（拓扑没到达管线就是这条失败）";
    EXPECT_GT(drawn.blue, 2U) << "点图元没画出那四个点（Points 管线没被选中就是这条失败）";

    // 数量关系把"三段是三种拓扑"再钉一次：面 > 线 > 点，是同一条线上的三个几何量。
    EXPECT_LT(drawn.green, drawn.red) << "线覆盖得比面少：同一份顶点，不同的图元";
    EXPECT_LT(drawn.blue, drawn.green) << "点覆盖得比线少：四个 1 像素的点";

    // SDK 层的账：一个几何体是**一次绘制调用**，三条图元没有把它拆成三次。
    const vn::graphics::FrameReport report = fixture.engine->frameReport();
    ASSERT_TRUE(report.counters.has_value()) << "这个后端必须报帧计数，否则下面的数是猜的";
    EXPECT_EQ(report.counters->passes, 1U);
    EXPECT_EQ(report.counters->draws, 1U) << "一个几何体一次绘制调用：图元是这次调用里的三条命令";

    // 后端的账：位置、法线、索引 arena 各上传一次，其余六次取流命中已建的绑定。
    auto* const assembly = vn::vsg::detail::BackendContentAccess::assembly(*fixture.backend);
    ASSERT_NE(assembly, nullptr) << "会话起来之后内容世界必须存在";
    const std::uint64_t uploads_after_first = assembly->uploads().uploads();
    const std::uint64_t aliases_after_first = assembly->uploads().aliases();
    std::printf("[primitives] streams: uploads=%llu aliases=%llu live=%zu\n",
                static_cast<unsigned long long>(uploads_after_first),
                static_cast<unsigned long long>(aliases_after_first), assembly->uploads().live());

    EXPECT_EQ(uploads_after_first, 3U) << "位置 + 法线 + 索引 arena：三条图元共用 ⇒ 三次上传，不是九次";
    EXPECT_EQ(aliases_after_first, 6U) << "九次取流减三次上传：每条图元绑的是同一批流";
    EXPECT_EQ(assembly->uploads().live(), 3U);

    // 第二帧什么都不改：九次取流全部命中，一个字节都不该重传（首帧的九次里有三次是上传，
    // 所以两帧的 aliases 之差正好是九）。
    const PixelProbe again = fixture.read();
    ASSERT_TRUE(again.valid()) << "第二帧也必须读得回来";
    const std::uint64_t aliases_after_second = assembly->uploads().aliases();
    EXPECT_EQ(assembly->uploads().uploads(), uploads_after_first) << "稳态帧重传了流";
    EXPECT_EQ(aliases_after_second - aliases_after_first, 9U) << "稳态帧的九次取流必须全部命中已建的绑定";
}
