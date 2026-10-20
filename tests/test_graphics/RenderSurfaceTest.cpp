#include <vine/graphics/Camera.hpp>
#include <vine/graphics/RenderEngine.hpp>
#include <vine/graphics/RenderPass.hpp>
#include <vine/graphics/RenderPipelineBuilder.hpp>
#include <vine/graphics/RenderSurface.hpp>
#include <vine/graphics/SceneView.hpp>

#include <gtest/gtest.h>

using namespace vn::graphics;
using vn::intrusive_ptr;
using vn::Color;

namespace
{

/**
 * @brief Builds a pipeline for @p view from @p options and adopts it: the two steps a host does for a view.
 *
 * A view no longer carries a recipe - a pipeline IS the recipe (see RenderPipelineBuilder and
 * SceneView::setPipeline) - so a test that wants "this view, drawn this way" states the options here and hands the
 * result over. The builder is given the view's own camera and scene, which is what every caller in the app does.
 *
 * @param view    View the pipeline is built for; it adopts the result.
 * @param engine  Engine the passes are registered on.
 * @param options The build options (chain, mode, overlays; see PipelineOptions).
 * @return The pipeline that was built and adopted; null when the builder refused @p options.
 */
intrusive_ptr<RenderPipeline> buildFor(const intrusive_ptr<SceneView>& view, RenderEngine* engine,
                                 const RenderPipelineOptions& options = {})
{
    RenderPipelineBuilder builder(engine);
    builder.setCamera(view->camera());
    builder.setContent(view->scene());
    intrusive_ptr<RenderPipeline> pipeline = builder.build(options);
    if (pipeline != nullptr) {
        view->setPipeline(pipeline);
    }
    return pipeline;
}

/** @brief A view bound to @p engine and drawing @p options (the engine outlives the view in these tests). */
intrusive_ptr<SceneView> viewOn(RenderEngine* engine, const RenderPipelineOptions& options = {})
{
    auto view = intrusive_ptr<SceneView>(new SceneView());
    view->setEngine(engine);
    buildFor(view, engine, options);
    return view;
}

}  // namespace

/**
 * @brief One render area: the ONE pass that paints this window, and the size it is painted at.
 *
 * "Who presents this surface" used to be inferred - a view asked the engine whether ANY registered pass carried its
 * camera with a null target (the removed RenderEngine::hasWindowPass) and stood down when one did, and a table used
 * to build the view's default window pass on demand. These tests pin the replacement: the AREA owns the answer, a
 * HOST declares it, a declaration resolves to a pass (a view's declaration points at the pipeline the view adopted;
 * a host with its own pass declares that one), and the area declares nothing else - no panes, no rectangles, no
 * focus, and no clear policy of its own. One area, one picture.
 */
TEST(RenderSurfaceTest, PresentingAViewDeclaresWhatItDraws)
{
    // The declaration a host makes when it has no main pass of its own: this area is presented by the pass of the
    // pipeline the VIEW adopted. The area resolves the pass back through the view (it builds nothing), so the host
    // order is: build, adopt, declare.
    auto engine  = intrusive_ptr<RenderEngine>(new RenderEngine());
    auto view    = intrusive_ptr<SceneView>(new SceneView());
    view->setEngine(engine.get());
    auto surface = engine->createSurface();

    EXPECT_FALSE(surface->hasPresentation());
    EXPECT_EQ(surface->presentingPass(), nullptr);
    EXPECT_EQ(surface->presentedView(), nullptr);
    auto pipeline = buildFor(view, engine.get());
    ASSERT_NE(pipeline, nullptr);
    EXPECT_TRUE(surface->present(*view));
    EXPECT_TRUE(surface->hasPresentation());
    EXPECT_EQ(engine->passCount(), 1u) << "one pass: the window pass of the pipeline that was adopted";
    EXPECT_EQ(surface->presentingPass(), pipeline->windowPass());
    EXPECT_EQ(surface->presentedView(), view.get());

    // Declaring the same view twice is ONE presentation.
    EXPECT_FALSE(surface->present(*view));
    EXPECT_EQ(surface->presentingPass(), pipeline->windowPass());

    // Naming the very pass that presents the area is the same picture again - not a second presentation.
    EXPECT_FALSE(surface->present(pipeline->windowPass()));
    EXPECT_EQ(surface->presentedView(), view.get())
        << "declaring the pass again must not turn the view's presentation into a host pass";

    // Dismissing it empties the area and leaves the view alone: the pipeline is the view's, not the surface's.
    EXPECT_TRUE(surface->dismiss(view.get()));
    EXPECT_FALSE(surface->hasPresentation());
    EXPECT_FALSE(surface->dismiss(view.get()));
    EXPECT_NE(view->pipeline(), nullptr);
    EXPECT_EQ(engine->passCount(), 1u);
}

TEST(RenderSurfaceTest, APassRegisteredOnTheEngineIsNotAPresentation)
{
    // Merely existing is not a presentation (that inference is what S7 removed): a pass carrying this very camera and
    // drawing the default framebuffer presents nothing until somebody says so. A HUD pass is the case that matters -
    // a null-target pass that is not the picture.
    auto engine  = intrusive_ptr<RenderEngine>(new RenderEngine());
    auto surface = engine->createSurface();
    auto hud     = intrusive_ptr<RenderPass>(new RenderPass());
    hud->setName(vn::String(u8"fps_overlay"));
    engine->addPass(hud, 30);

    EXPECT_EQ(engine->passCount(), 1u);
    EXPECT_FALSE(surface->hasPresentation()) << "a pass is not a presentation by existing";
    EXPECT_EQ(surface->presentingPass(), nullptr);
}

TEST(RenderSurfaceTest, AHostDeclaredPassIsTheSurfacesPresentation)
{
    // The declaration a host makes when it built its own main pass (the demo's deferred pipeline does exactly this):
    // the pass presents the area, and there is no view to name.
    auto engine  = intrusive_ptr<RenderEngine>(new RenderEngine());
    auto surface = engine->createSurface();
    auto main    = intrusive_ptr<RenderPass>(new RenderPass());
    main->setName(vn::String(u8"deferred_lighting"));
    engine->addPass(main, 0);

    EXPECT_TRUE(surface->present(main.get()));
    EXPECT_TRUE(surface->hasPresentation());
    EXPECT_EQ(surface->presentingPass(), main.get());
    EXPECT_EQ(surface->presentedView(), nullptr) << "a host's own pass has no view";
    EXPECT_FALSE(surface->present(main.get())) << "the same pass twice is one presentation";
    EXPECT_TRUE(surface->dismiss(main.get()));
    EXPECT_FALSE(surface->hasPresentation());
}

TEST(RenderSurfaceTest, ThePassFollowsARebuiltPipeline)
{
    // The pass is not remembered as a pointer: it is resolved through the view every time the area is read, so a
    // recipe change (which rebuilds the pipeline and creates a NEW window pass) is followed instead of leaving the
    // area naming a pass that left the frame.
    auto engine  = intrusive_ptr<RenderEngine>(new RenderEngine());
    auto view    = viewOn(engine.get());
    auto surface = engine->createSurface();
    ASSERT_TRUE(surface->present(*view));
    RenderPass* const first = surface->presentingPass();
    ASSERT_NE(first, nullptr);

    RenderPipelineOptions with_gizmo;
    with_gizmo.gizmo.source_camera = view->camera();
    ASSERT_NE(buildFor(view, engine.get(), with_gizmo), nullptr);
    ASSERT_NE(view->pipeline(), nullptr);
    EXPECT_NE(surface->presentingPass(), first);
    EXPECT_EQ(surface->presentingPass(), view->pipeline()->windowPass());
    EXPECT_EQ(surface->presentedView(), view.get());
}

TEST(RenderSurfaceTest, AViewWithNoPipelineIsRefusedOutLoud)
{
    // A declaration without a chain would leave a host believing a window is being drawn into while nothing draws.
    auto engine  = intrusive_ptr<RenderEngine>(new RenderEngine());
    auto view    = intrusive_ptr<SceneView>(new SceneView());
    view->setEngine(engine.get());
    auto surface = engine->createSurface();

    EXPECT_EQ(view->pipeline(), nullptr);
    EXPECT_FALSE(surface->present(*view));
    EXPECT_FALSE(surface->hasPresentation());
    EXPECT_EQ(engine->diagnosticCount(DiagnosticCategory::PassProtocolViolation), 1u);
}

TEST(RenderSurfaceTest, AViewOnAnotherEngineIsRefusedOutLoud)
{
    // A view bound to another engine registers its passes on THAT frame: it could present this surface never. The
    // refusal says so instead of leaving a host looking at an empty area.
    auto engine  = intrusive_ptr<RenderEngine>(new RenderEngine());
    auto other   = intrusive_ptr<RenderEngine>(new RenderEngine());
    auto view    = viewOn(other.get());
    auto surface = engine->createSurface();

    ASSERT_NE(view->pipeline(), nullptr);
    EXPECT_FALSE(surface->present(*view));
    EXPECT_FALSE(surface->hasPresentation());
    EXPECT_EQ(engine->passCount(), 0u) << "nothing of that view was ever registered on this surface's engine";
    EXPECT_EQ(engine->diagnosticCount(DiagnosticCategory::PassProtocolViolation), 1u);
}

TEST(RenderSurfaceTest, ASecondAreaIsAConstructionFailureNotASecondObject)
{
    // ONE engine drives ONE area, and the ENGINE OWNS it (RenderEngine::createSurface): two areas would both declare
    // a picture for one swapchain (they draw over each other, and only one's target is the frame's default
    // framebuffer) while the other's size quietly drove the passes. What used to be a refusal at registration is now
    // impossible to construct: the second call answers null, ONCE, and says why. A host that wants a second window
    // builds a second engine (a device may serve several).
    auto engine = intrusive_ptr<RenderEngine>(new RenderEngine());
    auto first  = engine->createSurface();
    ASSERT_NE(first, nullptr);

    auto second = engine->createSurface();
    auto third  = engine->createSurface();
    EXPECT_EQ(second, nullptr) << "there is no second area to get";
    EXPECT_EQ(third, nullptr);
    EXPECT_EQ(engine->diagnosticCount(DiagnosticCategory::UnsupportedRequest), 1u)
        << "one condition, one report - not one per attempt";
    EXPECT_EQ(engine->diagnosticCount(), 1u);

    // The area the engine drives is untouched by the refusals: it still decides what this engine presents.
    auto view = viewOn(engine.get());
    ASSERT_NE(view->pipeline(), nullptr);
    EXPECT_TRUE(first->present(*view));
    EXPECT_TRUE(first->hasPresentation());
}

TEST(RenderSurfaceTest, ARefusedSecondAreaCannotReshapeTheFrame)
{
    // The frame context is what the passes lay themselves out on, so exactly one thing may feed it. There is only one
    // area to resize, which is the point of the ownership: the "another window's size shaped my frame" failure has no
    // object to come from any more. What is worth pinning is that the refusal leaves the standing area in charge.
    auto engine = intrusive_ptr<RenderEngine>(new RenderEngine());
    auto first  = engine->createSurface();
    ASSERT_NE(first, nullptr);
    EXPECT_EQ(engine->createSurface(), nullptr);

    first->resize(800, 600);
    EXPECT_EQ(engine->surfaceWidth(), 800);
    EXPECT_EQ(engine->surfaceHeight(), 600);

    first->resize(1024, 768);
    EXPECT_EQ(engine->surfaceWidth(), 1024) << "the engine's own area is still the one it hears";
    EXPECT_EQ(engine->surfaceHeight(), 768);
}

TEST(RenderSurfaceTest, APassThatIsNotInTheFrameCannotPresentTheArea)
{
    // "Declared, but nothing is drawn" is the silence this area exists to end: a pass the engine does not run draws
    // nothing here, so declaring it is refused and reported once - the demo and every host register their pass first
    // (RenderEngine::addPass, which a pipeline does itself when it is built).
    auto engine  = intrusive_ptr<RenderEngine>(new RenderEngine());
    auto surface = engine->createSurface();
    auto pass    = intrusive_ptr<RenderPass>(new RenderPass());
    pass->setName(vn::String(u8"deferred_lighting"));

    EXPECT_FALSE(surface->present(pass.get())) << "a pass the engine never registered draws nothing here";
    EXPECT_FALSE(surface->hasPresentation());
    EXPECT_EQ(engine->diagnosticCount(DiagnosticCategory::PassProtocolViolation), 1u);
    EXPECT_EQ(engine->diagnosticCount(), 1u);
    EXPECT_FALSE(surface->present(pass.get())) << "and trying again is not a second report";
    EXPECT_EQ(engine->diagnosticCount(), 1u);

    engine->addPass(pass, 0);
    EXPECT_TRUE(surface->present(pass.get())) << "registered first, declared second - the host order";
    EXPECT_TRUE(surface->hasPresentation());
    EXPECT_EQ(surface->presentingPass(), pass.get());
}

TEST(RenderSurfaceTest, APresentationWhosePassLeavesTheFrameStopsPresenting)
{
    // The other half of the same rule: a declared pass can leave the frame (RenderEngine::removePass, a pipeline that
    // was thrown away), and then the area presents NOTHING - "declared" is not "drawn". Answering otherwise would keep
    // a default path from building its own picture (Viewer::takeArea) while the window stayed empty.
    auto engine  = intrusive_ptr<RenderEngine>(new RenderEngine());
    auto surface = engine->createSurface();
    auto pass    = intrusive_ptr<RenderPass>(new RenderPass());
    engine->addPass(pass, 0);
    ASSERT_TRUE(surface->present(pass.get()));
    EXPECT_TRUE(surface->hasPresentation());

    engine->removePass(pass.get());
    EXPECT_FALSE(surface->hasPresentation()) << "a pass that is not in the frame presents nothing";
    EXPECT_EQ(surface->presentingPass(), nullptr);
    EXPECT_TRUE(surface->dismiss(pass.get())) << "and the declaration can still be taken back";
}

TEST(RenderSurfaceTest, OneAreaDrawsOnePicture)
{
    // The rule that replaced the multi-pane table: a second presentation (a view or a host pass) is refused, and the
    // refusal is REPORTED once per episode instead of silently drawing two passes into one window.
    auto engine  = intrusive_ptr<RenderEngine>(new RenderEngine());
    auto first   = viewOn(engine.get());
    auto second  = viewOn(engine.get());
    auto surface = engine->createSurface();
    ASSERT_TRUE(surface->present(*first));

    EXPECT_FALSE(surface->present(*second)) << "a second view is not a split - it is a second picture";
    EXPECT_EQ(surface->presentedView(), first.get());
    EXPECT_EQ(surface->presentingPass(), first->pipeline()->windowPass());

    auto host_pass = intrusive_ptr<RenderPass>(new RenderPass());
    engine->addPass(host_pass, 40);
    EXPECT_FALSE(surface->present(host_pass.get()));
    EXPECT_EQ(surface->presentedView(), first.get());
    EXPECT_EQ(engine->diagnosticCount(DiagnosticCategory::PassProtocolViolation), 1u)
        << "one condition, one report - not one per attempt";

    // Taking the first picture away arms the next report: a host that fixes it hears about a NEW conflict.
    ASSERT_TRUE(surface->dismiss(first.get()));
    ASSERT_TRUE(surface->present(host_pass.get()));
    EXPECT_FALSE(surface->present(*second));
    EXPECT_EQ(engine->diagnosticCount(DiagnosticCategory::PassProtocolViolation), 2u);
}

TEST(RenderSurfaceTest, TheAreaOwnsTheSizeAndTheViewReadsIt)
{
    // The size of the thing the picture ends up in is a fact about the AREA: a host announces it once, and the view
    // reads it from there (its camera shape, its off-screen targets, its HUD boxes and its layout steps all follow
    // from it). A view presented LATER gets the remembered size at once - the host does not have to feed it.
    auto engine  = intrusive_ptr<RenderEngine>(new RenderEngine());
    auto view    = viewOn(engine.get());
    auto surface = engine->createSurface();
    ASSERT_TRUE(surface->present(*view));

    surface->resize(800, 600, 2.0);
    EXPECT_EQ(surface->pixelRatio(), 2.0);
    EXPECT_EQ(view->surfaceWidth(), 800);
    EXPECT_EQ(view->surfaceHeight(), 600);
    EXPECT_EQ(view->surfacePixelRatio(), 2.0);
    EXPECT_NEAR(view->camera()->aspect(), 800.0 / 600.0, 1e-9) << "the camera is shaped for the area";
    EXPECT_FALSE(surface->presentingPass()->hasViewport())
        << "a full-area picture is what a pass without a viewport already means";

    // State the size before anything is presented: the view that comes later is laid out with it at once. A second
    // WINDOW is a second engine (see RenderEngine::createSurface), which is what this half uses.
    auto other_engine = intrusive_ptr<RenderEngine>(new RenderEngine());
    auto other        = other_engine->createSurface();
    auto later        = viewOn(other_engine.get());
    other->resize(400, 400, 1.0);
    ASSERT_TRUE(other->present(*later));
    EXPECT_EQ(later->surfaceWidth(), 400);
    EXPECT_EQ(later->surfaceHeight(), 400);
    EXPECT_NEAR(later->camera()->aspect(), 1.0, 1e-9);
}

TEST(RenderSurfaceTest, TheAreaDoesNotTouchTheClearPolicyOfItsPicture)
{
    // What the area owns is WHO paints it - not whether that pass clears. The pass' own ask stands (a HUD-style pass
    // that draws over what is there keeps asking for no clear), and the backend's rule (the first clear announced per
    // attachment wins) is exactly the swapchain's own: one area, one picture, one clear.
    auto engine  = intrusive_ptr<RenderEngine>(new RenderEngine());
    auto view    = viewOn(engine.get());
    auto surface = engine->createSurface();
    RenderPass* const window = view->pipeline()->windowPass();
    ASSERT_NE(window, nullptr);
    EXPECT_TRUE(window->isClearEnabled());

    ASSERT_TRUE(surface->present(*view));
    EXPECT_TRUE(window->isClearEnabled()) << "the presentation's own ask is untouched";
    EXPECT_EQ(window->clearColor(), Color(51, 51, 51, 255));

    window->setClearEnabled(false);
    RenderPipelineOptions with_gizmo;
    with_gizmo.gizmo.source_camera = view->camera();
    ASSERT_NE(buildFor(view, engine.get(), with_gizmo), nullptr);
    EXPECT_NE(surface->presentingPass(), window);
    EXPECT_TRUE(surface->presentingPass()->isClearEnabled())
        << "a rebuilt chain's window pass is a new pass with its own (default) ask - the area invents nothing";

    // Deferred: the picture is a fullscreen pass over the whole area, whose program is a ScreenPass and therefore asks
    // for no clear at all - being the area's picture does not make it want one.
    auto deferred_engine = intrusive_ptr<RenderEngine>(new RenderEngine());
    RenderPipelineOptions deferred;
    deferred.path = ShadingPath::Deferred;
    auto deferred_view = viewOn(deferred_engine.get(), deferred);
    auto deferred_area = deferred_engine->createSurface();
    ASSERT_TRUE(deferred_area->present(*deferred_view));
    ASSERT_NE(deferred_view->pipeline()->windowPass(), nullptr);
    EXPECT_FALSE(deferred_view->pipeline()->windowPass()->isClearEnabled());
}

TEST(RenderSurfaceTest, APresentedViewThatDiesLeavesTheSurface)
{
    // The area borrows its view (it is the host's), so a host that drops it must not leave the area pointing at it:
    // the view leaves the area itself, in its own destructor, while the area is still there to be told.
    auto engine  = intrusive_ptr<RenderEngine>(new RenderEngine());
    auto view    = viewOn(engine.get());
    auto surface = engine->createSurface();
    ASSERT_TRUE(surface->present(*view));
    RenderPass* const window = surface->presentingPass();

    view = nullptr;
    EXPECT_FALSE(surface->hasPresentation()) << "a view that is gone cannot stay presented";
    EXPECT_EQ(surface->presentedView(), nullptr);
    EXPECT_EQ(surface->presentingPass(), nullptr);
    EXPECT_NE(window, nullptr) << "the pass itself is the pipeline's, and the pipeline went with the view";
}

TEST(RenderSurfaceTest, AViewThatOutlivesItsSurfaceDoesNotCallIntoIt)
{
    // The other order: the host drops the surface (or the whole control) while its view lives on. The view must stop
    // naming it - it is going away, and a view that outlived it must not reach back into it.
    auto engine  = intrusive_ptr<RenderEngine>(new RenderEngine());
    auto view    = viewOn(engine.get());
    auto surface = engine->createSurface();
    ASSERT_TRUE(surface->present(*view));
    surface->resize(320, 240, 1.0);
    ASSERT_EQ(view->surfaceWidth(), 320);

    surface = nullptr;
    // The view keeps the numbers it last derived from the area, and is its own again: the host sizes it directly.
    EXPECT_EQ(view->surfaceWidth(), 320);
    EXPECT_EQ(view->surfaceHeight(), 240);
    view->resize(640, 480);
    EXPECT_EQ(view->surfaceWidth(), 640);
    EXPECT_EQ(view->surfaceHeight(), 480);
    EXPECT_NE(view->pipeline(), nullptr) << "the pipeline is the view's, not the surface's";
}

TEST(RenderSurfaceTest, DismissingAViewStopsFeedingItTheSize)
{
    // A dismissed view is the host's again and draws nothing into the area, so the area's size announcements must not
    // reach it any more: what it derives from is its own last-known size, which the dismissal hands back to it.
    auto engine  = intrusive_ptr<RenderEngine>(new RenderEngine());
    auto view    = viewOn(engine.get());
    auto surface = engine->createSurface();
    ASSERT_TRUE(surface->present(*view));
    surface->resize(640, 480, 1.0);
    ASSERT_EQ(view->surfaceWidth(), 640);

    ASSERT_TRUE(surface->dismiss(view.get()));
    surface->resize(1280, 720, 1.0);
    EXPECT_EQ(view->surfaceWidth(), 640) << "the area no longer feeds a view it does not present";
    EXPECT_EQ(view->surfaceHeight(), 480);

    // Presented again, it follows the area's current size (one owner, no second copy to keep in step).
    ASSERT_TRUE(surface->present(*view));
    EXPECT_EQ(view->surfaceWidth(), 1280);
    EXPECT_EQ(view->surfaceHeight(), 720);
}

/**
 * @brief An area that outlives its engine is a DEAD area, not a dangling pointer.
 *
 * The area borrows its engine (it is 1:1 with it and registers itself on it), and "borrowed" used to mean a raw
 * pointer plus a comment: a host that kept the area past the engine's destruction called into freed memory from
 * ~RenderSurface, from resize() and from hasPresentation(). The borrow is a Lease now - the same one the view and
 * the pipeline hold - so every use asks whether the engine is still there.
 */
TEST(RenderSurfaceTest, AnAreaOutlivingItsEngineAnswersNothingInsteadOfCallingIntoIt)
{
    intrusive_ptr<RenderSurface> surface;
    {
        auto engine = intrusive_ptr<RenderEngine>(new RenderEngine());
        surface     = engine->createSurface();
        surface->resize(640, 480, 1.0);
        EXPECT_FALSE(surface->hasPresentation());
    }   // the engine goes first

    EXPECT_FALSE(surface->hasPresentation());
    EXPECT_EQ(surface->presentingPass(), nullptr);
    EXPECT_EQ(surface->presentedView(), nullptr);

    // A size announcement is recorded (the number is the area's own fact) but reaches no frame.
    surface->resize(800, 600, 1.0);
    EXPECT_EQ(surface->pixelRatio(), 1.0);

    // And no picture can be declared into a frame that is gone.
    auto view = intrusive_ptr<SceneView>(new SceneView());
    EXPECT_FALSE(surface->present(*view));
}
