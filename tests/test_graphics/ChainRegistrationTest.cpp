#include <vine/graphics/Camera.hpp>
#include <vine/graphics/DiagnosticsRoute.hpp>
#include <vine/graphics/FpsOverlay.hpp>
#include <vine/graphics/OutputRegistry.hpp>
#include <vine/graphics/PassChain.hpp>
#include <vine/graphics/PassDesc.hpp>
#include <vine/graphics/PassRegistry.hpp>
#include <vine/graphics/RenderEngine.hpp>
#include <vine/graphics/RenderPass.hpp>
#include <vine/graphics/Scene.hpp>

#include <vine/String.hpp>
#include <vine/intrusive_ptr.hpp>

#include <gtest/gtest.h>

using namespace vn::graphics;
using vn::intrusive_ptr;
using vn::raw_ptr;
using vn::String;

namespace
{

/*
 * @brief A declaration that asks nothing of the device: a scene pass in the HUD stage with no content and no clear.
 *
 * Registration is what these tests are about, so the declaration stays at the minimum the engine accepts - what it
 * becomes is the same node a hand-built pass would be (see RenderEngine::addPassOwnedBy).
 */
PassDesc overlayDeclaration(const char8_t* name)
{
    PassDesc declaration;
    declaration.name          = name;
    declaration.tag           = u8"Test";
    declaration.stage         = PassStage::Overlay;
    declaration.kind          = PassKind::Scene;
    declaration.clear_enabled = false;
    return declaration;
}

} // namespace

/*
 * @brief The chain is its own registration scope: what it registered it takes back, and only that.
 *
 * Passes used to live in two places - the engine's slots and a vector on the chain that existed only to be walked
 * again in a destructor - and then in a separate move-only token. The chain now writes itself on the slots it
 * registered: the engine holds each pass once, a named pass is found by its role, and the chain going away IS the
 * unregistration.
 */
TEST(ChainRegistrationTest, AChainTakesBackExactlyThePassesItRegistered)
{
    auto engine = intrusive_ptr<RenderEngine>(new RenderEngine());
    EXPECT_EQ(engine->passCount(), 0u);

    auto chain = intrusive_ptr<PassChain>(new PassChain(engine->passes(), engine->diagnostics()));

    const raw_ptr<RenderPass> hud = chain->addOverlay(String(u8"hud"), nullptr, overlayDeclaration(u8"hud_pass"));
    ASSERT_NE(hud, nullptr);
    EXPECT_NE(chain->addOverlay(String(u8"second"), nullptr, overlayDeclaration(u8"second_pass")), nullptr);
    EXPECT_EQ(engine->passCount(), 2u) << "the engine holds each pass once, not copied into a second list";
    EXPECT_EQ(chain->overlayPass(String(u8"hud")), hud) << "a named pass comes back by its role";
    EXPECT_EQ(chain->overlayPass(String(u8"nothing")), nullptr);

    // A pass the host registered itself is not the chain's business, and both live in the one table.
    EXPECT_NE(engine->addPass(overlayDeclaration(u8"outside")), nullptr);
    EXPECT_EQ(engine->passCount(), 3u);

    chain = nullptr;
    EXPECT_EQ(engine->passCount(), 1u) << "the chain went, its passes went with it, the outsider stayed";
}

/*
 * @brief A chain can outlive the engine it registered on: it then answers "nothing" instead of reaching into freed
 * memory, which is the whole reason the engine is held through a lease and not a raw pointer.
 */
TEST(ChainRegistrationTest, AChainOutlivingItsEngineAnswersNothing)
{
    intrusive_ptr<PassChain> survivor;
    {
        auto engine = intrusive_ptr<RenderEngine>(new RenderEngine());
        survivor    = intrusive_ptr<PassChain>(new PassChain(engine->passes(), engine->diagnostics()));
        ASSERT_NE(survivor->addOverlay(String(u8"doomed"), nullptr, overlayDeclaration(u8"doomed")), nullptr);
        EXPECT_EQ(engine->passCount(), 1u);
    }

    EXPECT_FALSE(survivor->isValid()) << "the engine it registered on is gone";
    EXPECT_EQ(survivor->overlayPass(String(u8"doomed")), nullptr);
    EXPECT_EQ(survivor->addOverlay(String(u8"late"), nullptr, overlayDeclaration(u8"late")), nullptr) << "there is nobody left to register it on";
}

/*
 * @brief The HUD passes are reached by ROLE, and everything the chain registered leaves with it.
 *
 * The recipe hands over the overlay's content and the label its pass carries, so the chain needs no field per
 * overlay pass - two roles, two passes, one lookup each - and nothing it registered survives it.
 */
TEST(ChainRegistrationTest, TheChainFindsItsHudPassesByRoleAndHandsThemAllBack)
{
    auto engine  = intrusive_ptr<RenderEngine>(new RenderEngine());
    auto content = intrusive_ptr<Scene>(new Scene());
    auto cam     = intrusive_ptr<Camera>(new Camera());

    ChainDesc options;
    options.camera              = cam.get();
    options.content             = content;
    options.gizmo.source_camera = cam.get();
    options.fps.enabled         = true;
    auto chain                  = PassChain::build(engine.get(), options);
    ASSERT_NE(chain, nullptr);

    EXPECT_NE(chain->gizmoPass(), nullptr) << "the gizmo pass is registered under its role";
    EXPECT_NE(chain->fpsOverlayPass(), nullptr) << "so is the readout";
    EXPECT_NE(chain->gizmoPass(), chain->fpsOverlayPass()) << "the two roles are two passes";

    // The switch reaches the pass the role names, and only that one.
    EXPECT_TRUE(chain->setGizmoVisible(false));
    EXPECT_FALSE(chain->isGizmoVisible());
    EXPECT_TRUE(chain->isStatsVisible()) << "hiding one overlay leaves the other alone";

    chain = nullptr;
    EXPECT_EQ(engine->passCount(), 0u) << "dropping the chain removes every pass it registered";
}

/*
 * @brief A THIRD overlay is a CALL, not a change to the class: the recipe's two go through the same public entry.
 *
 * An overlay is a value - a role, the content that owns its own box, and the layout that re-anchors it - so a host
 * that wants another HUD layer registers one and the chain needs to know nothing about it. The alternative (a field
 * and a resize() branch per overlay) is exactly what a second overlay used to cost.
 */
TEST(ChainRegistrationTest, AHostRegistersAThirdOverlayWithoutTouchingTheChain)
{
    auto engine  = intrusive_ptr<RenderEngine>(new RenderEngine());
    auto content = intrusive_ptr<Scene>(new Scene());
    auto cam     = intrusive_ptr<Camera>(new Camera());

    ChainDesc options;
    options.camera      = cam.get();
    options.content     = content;
    options.fps.enabled = true; // one overlay the recipe knows; the host's is a second readout
    auto chain          = PassChain::build(engine.get(), options);
    ASSERT_NE(chain, nullptr);
    const std::size_t recipe_passes = engine->passCount();

    auto corner = intrusive_ptr<FpsOverlay>(new FpsOverlay());
    corner->setSize(80, 24);

    PassDesc hud;
    hud.name          = u8"corner_readout";
    hud.tag           = u8"Hud";
    hud.stage         = PassStage::Overlay;
    hud.order         = passStageOrder(PassStage::Overlay) + 50;
    hud.kind          = PassKind::Scene;
    hud.camera        = intrusive_ptr<Camera>(corner->camera());
    hud.content       = intrusive_ptr<Scene>(corner->content());
    hud.viewport      = corner->boxViewport();
    hud.clear_enabled = false;

    int                       laid_out_width  = 0;
    int                       laid_out_height = 0;
    double                    laid_out_ratio  = 0.0;
    const raw_ptr<RenderPass> hud_pass =
        chain->addOverlay(String(u8"corner"),
                          corner.get(),
                          hud,
                          [corner, &laid_out_width, &laid_out_height, &laid_out_ratio](RenderPass& pass, int width, int height, double ratio) {
                              laid_out_width  = width;
                              laid_out_height = height;
                              laid_out_ratio  = ratio;
                              corner->setPixelRatio(ratio);
                              corner->onSurfaceResized(width, height);
                              pass.setViewport(corner->boxViewport());
                          });
    ASSERT_NE(hud_pass, nullptr) << "a host-owned overlay registers through the same entry the recipe uses";
    EXPECT_EQ(engine->passCount(), recipe_passes + 1) << "one more pass in the SAME table, under its own role";
    EXPECT_EQ(chain->overlayContent(String(u8"corner")), corner.get()) << "the content comes back untyped";
    EXPECT_EQ(chain->overlayPass(String(u8"corner")), hud_pass);

    // The generic switch works on a role the class never heard of, and leaves the recipe's overlay alone.
    EXPECT_TRUE(chain->isOverlayVisible(String(u8"corner")));
    EXPECT_TRUE(chain->setOverlayVisible(String(u8"corner"), false));
    EXPECT_FALSE(chain->isOverlayVisible(String(u8"corner")));
    EXPECT_TRUE(chain->isStatsVisible());
    EXPECT_FALSE(chain->setOverlayVisible(String(u8"no_such_role"), true))
        << "a switch over a role nobody registered says so instead of silently doing nothing";

    // resize() runs every overlay's own layout: the third one is laid out without being named in the chain.
    chain->resize(800, 600, 2.0);
    EXPECT_EQ(laid_out_width, 800);
    EXPECT_EQ(laid_out_height, 600);
    EXPECT_DOUBLE_EQ(laid_out_ratio, 2.0);

    chain = nullptr;
    EXPECT_EQ(engine->passCount(), 0u) << "the host's overlay left with the same chain";
}

/*
 * @brief The three narrow handles are leases: they may outlive the engine and then answer "nothing".
 *
 * A recipe holds the registry and the diagnostic route instead of the engine, so both have to survive the engine
 * being dropped without reaching into freed memory - the same rule the chain itself follows.
 */
TEST(ChainRegistrationTest, TheHandlesOutliveTheirEngineAndThenAnswerNothing)
{
    PassRegistry     registry;
    OutputRegistry   outputs;
    DiagnosticsRoute diagnostics;
    {
        auto engine = intrusive_ptr<RenderEngine>(new RenderEngine());
        registry    = engine->passes();
        outputs     = engine->outputs();
        diagnostics = engine->diagnostics();

        EXPECT_TRUE(registry.isValid());
        EXPECT_TRUE(outputs.isValid());
        EXPECT_TRUE(diagnostics.isValid());
        EXPECT_NE(registry.add(overlayDeclaration(u8"outside")), nullptr) << "the same table the engine's own methods read";
        EXPECT_EQ(registry.count(), engine->passCount());
    }

    EXPECT_FALSE(registry.isValid()) << "the engine it read is gone";
    EXPECT_FALSE(outputs.isValid());
    EXPECT_FALSE(diagnostics.isValid());
    EXPECT_EQ(registry.count(), 0u);
    EXPECT_EQ(registry.add(overlayDeclaration(u8"late")), nullptr) << "there is nobody left to register it on";
    EXPECT_EQ(outputs.resolve(String(u8"nothing")), nullptr);
    EXPECT_EQ(outputs.transientTarget(String(u8"nothing")), nullptr);
    EXPECT_EQ(outputs.transientCount(), 0u);
    EXPECT_EQ(diagnostics.count(), 0u);
    EXPECT_TRUE(diagnostics.isClean());
    diagnostics.report(DiagnosticSeverity::Warning, DiagnosticCategory::UnsupportedRequest, String(u8"dropped with the engine"));
    EXPECT_EQ(diagnostics.count(), 0u) << "a report with no route is a no-op, not a crash";
}

/*
 * @brief A chain registers through the handle it was given - the engine's own count is that table's count.
 */
TEST(ChainRegistrationTest, AChainRegistersThroughTheHandleItWasGiven)
{
    auto engine = intrusive_ptr<RenderEngine>(new RenderEngine());
    auto chain  = intrusive_ptr<PassChain>(new PassChain(engine->passes(), engine->diagnostics()));

    const raw_ptr<RenderPass> hud = chain->addOverlay(String(u8"hud"), nullptr, overlayDeclaration(u8"hud_pass"));
    ASSERT_NE(hud, nullptr);
    EXPECT_EQ(engine->passes().count(), 1u) << "one table: the handle and the engine answer the same number";
    EXPECT_EQ(chain->overlayPass(String(u8"hud")), hud);

    chain = nullptr;
    EXPECT_EQ(engine->passes().count(), 0u);
    EXPECT_TRUE(engine->passes().isValid()) << "the engine is still there; only the chain went";
}
