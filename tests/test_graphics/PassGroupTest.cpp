#include <vine/graphics/Camera.hpp>
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
