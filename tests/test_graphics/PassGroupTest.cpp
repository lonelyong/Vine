#include <vine/graphics/Camera.hpp>
#include <vine/graphics/FpsOverlay.hpp>
#include <vine/graphics/PassDesc.hpp>
#include <vine/graphics/PassGroup.hpp>
#include <vine/graphics/RenderEngine.hpp>
#include <vine/graphics/RenderPass.hpp>
#include <vine/graphics/RenderPipeline.hpp>
#include <vine/graphics/RenderPipelineBuilder.hpp>
#include <vine/graphics/Scene.hpp>

#include <vine/String.hpp>
#include <vine/intrusive_ptr.hpp>

#include <gtest/gtest.h>

using namespace vn::graphics;
using vn::intrusive_ptr;
using vn::raw_ptr;
using vn::String;

namespace {

/*
 * @brief A declaration that asks nothing of the device: a scene pass in the HUD stage with no content and no clear.
 *
 * Registration is what these tests are about, so the declaration stays at the minimum the engine accepts - what it
 * becomes is the same node a hand-built pass would be (see RenderEngine::addPassToGroup).
 */
PassDesc overlayDeclaration(const char8_t* name)
{
    PassDesc declaration;
    declaration.name          = name;
    declaration.tag           = u8"Test";
    declaration.stage         = PipelineStage::Overlay;
    declaration.kind          = PassKind::Scene;
    declaration.clear_enabled = false;
    return declaration;
}

}  // namespace

/*
 * @brief A registration group is ONE token over the engine's own table, so what it registered it takes back.
 *
 * Passes used to live in two places - the engine's slots and a vector on the pipeline that existed only to be walked
 * again in a destructor - which made "registered but not remembered" a reachable state. A group writes its id on the
 * slots it registered instead: the engine holds each pass once, a named pass is found by its role, and dropping the
 * token IS the unregistration.
 */
TEST(PassGroupTest, AGroupTakesBackExactlyThePassesItRegistered)
{
    auto engine = intrusive_ptr<RenderEngine>(new RenderEngine());
    EXPECT_EQ(engine->passCount(), 0u);

    PassGroup group = engine->createPassGroup();
    ASSERT_TRUE(group.isValid()) << "an engine that is there mints a usable token";

    const intrusive_ptr<RenderPass> hud = group.add(overlayDeclaration(u8"hud_pass"), String(u8"hud"));
    ASSERT_NE(hud, nullptr);
    EXPECT_NE(group.add(overlayDeclaration(u8"second_pass")), nullptr);
    ASSERT_EQ(group.passes().size(), 2u);
    EXPECT_EQ(group.pass(String(u8"hud")), hud.get()) << "a named pass comes back by its role";
    EXPECT_EQ(group.pass(String(u8"nothing")), nullptr);

    // A pass the host registered itself is not the group's business, and both live in the one table.
    EXPECT_NE(engine->addPass(overlayDeclaration(u8"outside")), nullptr);
    EXPECT_EQ(engine->passCount(), 3u) << "a grouped pass is registered once, not copied into a second list";
    EXPECT_EQ(group.passes().size(), 2u) << "and the group speaks only for its own passes";

    group = PassGroup();
    EXPECT_EQ(engine->passCount(), 1u) << "the token went, its passes went with it, the outsider stayed";
    EXPECT_FALSE(group.isValid());
}

/*
 * @brief A group can outlive the engine it registered on: it then answers "nothing" instead of reaching into freed
 * memory, which is the whole reason the engine is held through a lease and not a raw pointer.
 */
TEST(PassGroupTest, AGroupOutlivingItsEngineAnswersNothing)
{
    PassGroup survivor;
    {
        auto engine = intrusive_ptr<RenderEngine>(new RenderEngine());
        survivor    = engine->createPassGroup();
        ASSERT_NE(survivor.add(overlayDeclaration(u8"doomed")), nullptr);
        EXPECT_EQ(engine->passCount(), 1u);
    }

    EXPECT_FALSE(survivor.isValid()) << "the engine it registered on is gone";
    EXPECT_TRUE(survivor.passes().empty());
    EXPECT_EQ(survivor.pass(String(u8"doomed")), nullptr);
    EXPECT_EQ(survivor.add(overlayDeclaration(u8"late")), nullptr) << "there is nobody left to register it on";
}

/*
 * @brief The HUD passes are reached by ROLE, and the pipeline's passes leave with the pipeline's token.
 *
 * The recipe hands over the overlay's content and the label its pass carries, so the pipeline needs no field per
 * overlay pass - two roles, two passes, one lookup each - and nothing it registered survives it.
 */
TEST(PassGroupTest, ThePipelineFindsItsHudPassesByRoleAndHandsThemAllBack)
{
    auto engine  = intrusive_ptr<RenderEngine>(new RenderEngine());
    auto content = intrusive_ptr<Scene>(new Scene());
    auto cam     = intrusive_ptr<Camera>(new Camera());

    RenderPipelineBuilder builder(engine.get());
    builder.setCamera(cam.get());
    builder.setContent(content);

    RenderPipelineOptions options;
    options.gizmo.source_camera = cam.get();
    options.fps.enabled         = true;
    auto pipeline               = builder.build(options);
    ASSERT_NE(pipeline, nullptr);

    EXPECT_NE(pipeline->gizmoPass(), nullptr) << "the gizmo pass is registered under its role";
    EXPECT_NE(pipeline->fpsOverlayPass(), nullptr) << "so is the readout";
    EXPECT_NE(pipeline->gizmoPass(), pipeline->fpsOverlayPass()) << "the two roles are two passes";

    // The switch reaches the pass the role names, and only that one.
    EXPECT_TRUE(pipeline->setGizmoVisible(false));
    EXPECT_FALSE(pipeline->isGizmoVisible());
    EXPECT_TRUE(pipeline->isStatsVisible()) << "hiding one overlay leaves the other alone";

    pipeline = nullptr;
    EXPECT_EQ(engine->passCount(), 0u) << "dropping the pipeline drops the token that owned its passes";
}

/*
 * @brief A THIRD overlay is a CALL, not a change to the class: the recipe's two go through the same public entry.
 *
 * An overlay is a value - a role, the content that owns its own box, and the layout that re-anchors it - so a host
 * that wants another HUD layer registers one and the pipeline needs to know nothing about it. The alternative (a
 * field and a resize() branch per overlay) is exactly what a second overlay used to cost.
 */
TEST(PassGroupTest, AHostRegistersAThirdOverlayWithoutTouchingThePipeline)
{
    auto engine  = intrusive_ptr<RenderEngine>(new RenderEngine());
    auto content = intrusive_ptr<Scene>(new Scene());
    auto cam     = intrusive_ptr<Camera>(new Camera());

    RenderPipelineBuilder builder(engine.get());
    builder.setCamera(cam.get());
    builder.setContent(content);

    RenderPipelineOptions options;
    options.fps.enabled         = true;  // one overlay the recipe knows; the host's is a second readout
    auto pipeline               = builder.build(options);
    ASSERT_NE(pipeline, nullptr);
    const std::size_t recipe_passes = engine->passCount();

    auto corner = intrusive_ptr<FpsOverlay>(new FpsOverlay());
    corner->setSize(80, 24);

    PassDesc hud;
    hud.name          = u8"corner_readout";
    hud.tag           = u8"Hud";
    hud.stage         = PipelineStage::Overlay;
    hud.order         = pipelineStageOrder(PipelineStage::Overlay) + 50;
    hud.kind          = PassKind::Scene;
    hud.camera        = intrusive_ptr<Camera>(corner->camera());
    hud.content       = intrusive_ptr<Scene>(corner->content());
    hud.viewport      = corner->boxViewport();
    hud.clear_enabled = false;

    int    laid_out_width  = 0;
    int    laid_out_height = 0;
    double laid_out_ratio  = 0.0;
    const raw_ptr<RenderPass> hud_pass =
        pipeline->addOverlay(String(u8"corner"), corner.get(), hud,
                             [corner, &laid_out_width, &laid_out_height, &laid_out_ratio](RenderPass& pass,
                                                                                         int width,
                                                                                         int height,
                                                                                         double ratio) {
                                 laid_out_width  = width;
                                 laid_out_height = height;
                                 laid_out_ratio  = ratio;
                                 corner->setPixelRatio(ratio);
                                 corner->onSurfaceResized(width, height);
                                 pass.setViewport(corner->boxViewport());
                             });
    ASSERT_NE(hud_pass, nullptr) << "a host-owned overlay registers through the same entry the recipe uses";
    EXPECT_EQ(engine->passCount(), recipe_passes + 1) << "one more pass in the SAME table, under its own role";
    EXPECT_EQ(pipeline->overlayContent(String(u8"corner")), corner.get()) << "the content comes back untyped";
    EXPECT_EQ(pipeline->overlayPass(String(u8"corner")), hud_pass);

    // The generic switch works on a role the class never heard of, and leaves the recipe's overlay alone.
    EXPECT_TRUE(pipeline->isOverlayVisible(String(u8"corner")));
    EXPECT_TRUE(pipeline->setOverlayVisible(String(u8"corner"), false));
    EXPECT_FALSE(pipeline->isOverlayVisible(String(u8"corner")));
    EXPECT_TRUE(pipeline->isStatsVisible());
    EXPECT_FALSE(pipeline->setOverlayVisible(String(u8"no_such_role"), true))
        << "a switch over a role nobody registered says so instead of silently doing nothing";

    // resize() runs every overlay's own layout: the third one is laid out without being named in the pipeline.
    pipeline->resize(800, 600, 2.0);
    EXPECT_EQ(laid_out_width, 800);
    EXPECT_EQ(laid_out_height, 600);
    EXPECT_DOUBLE_EQ(laid_out_ratio, 2.0);

    pipeline = nullptr;
    EXPECT_EQ(engine->passCount(), 0u) << "the host's overlay left with the same token";
}
