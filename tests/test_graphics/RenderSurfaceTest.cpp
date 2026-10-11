#include <vine/graphics/Camera.hpp>
#include <vine/graphics/RenderEngine.hpp>
#include <vine/graphics/RenderPass.hpp>
#include <vine/graphics/RenderSurface.hpp>
#include <vine/graphics/SceneView.hpp>

#include <gtest/gtest.h>

#include <vector>

using namespace vn::graphics;
using vn::intrusive_ptr;
using vn::Color;

namespace
{

/** @brief A view bound to @p engine, ready to present (the engine outlives the view in these tests). */
intrusive_ptr<SceneView> viewOn(RenderEngine* engine)
{
    auto view = intrusive_ptr<SceneView>(new SceneView());
    view->setEngine(engine);
    return view;
}

}  // namespace

/**
 * @brief A surface's presentation table: what draws into it, in which order, and who decides.
 *
 * "Who presents this surface" used to be inferred - a view asked the engine whether ANY registered pass carried
 * its camera with a null target (the removed RenderEngine::hasWindowPass) and stood down when one did. These tests
 * pin the replacement: the SURFACE owns the answer, a host declares it, and the view's default window pass is only
 * built when nothing was declared.
 */
TEST(RenderSurfaceTest, PresentingAViewBuildsItsDefaultWindowPass)
{
    auto engine  = intrusive_ptr<RenderEngine>(new RenderEngine());
    auto view    = intrusive_ptr<SceneView>(new SceneView());
    view->setEngine(engine.get());
    auto surface = intrusive_ptr<RenderSurface>(new RenderSurface(engine.get()));

    EXPECT_FALSE(surface->hasPresentation());
    EXPECT_TRUE(surface->present(*view));
    EXPECT_TRUE(surface->hasPresentation());
    EXPECT_EQ(engine->passCount(), 1u);
    ASSERT_NE(view->pipeline(), nullptr);

    const std::vector<RenderSurface::Presentation> table = surface->presentations();
    ASSERT_EQ(table.size(), 1u);
    EXPECT_EQ(table[0].pass, view->pipeline()->windowPass());
    EXPECT_EQ(table[0].view, view.get());

    // Declaring the same view twice is ONE presentation.
    EXPECT_FALSE(surface->present(*view));
    EXPECT_EQ(surface->presentations().size(), 1u);

    // Dismissing it empties the table and leaves the view alone: the pipeline is the view's, not the surface's.
    EXPECT_TRUE(surface->dismiss(view.get()));
    EXPECT_FALSE(surface->hasPresentation());
    EXPECT_FALSE(surface->dismiss(view.get()));
    EXPECT_NE(view->pipeline(), nullptr);
    EXPECT_EQ(engine->passCount(), 1u);
}

TEST(RenderSurfaceTest, AHostPassRegisteredOnTheEngineDoesNotSuppressTheDefaultWindowPass)
{
    // The fallback the control runs (see SurfaceWindow::init): with nothing DECLARED, the control's own view
    // presents the surface - even when a pass carrying that very camera is registered on the engine. Merely
    // existing is not a presentation (that inference is what M2a removed).
    auto engine  = intrusive_ptr<RenderEngine>(new RenderEngine());
    auto view    = intrusive_ptr<SceneView>(new SceneView());
    view->setEngine(engine.get());
    auto surface = intrusive_ptr<RenderSurface>(new RenderSurface(engine.get()));

    auto host_pass = intrusive_ptr<RenderPass>(new RenderPass());
    host_pass->setCamera(intrusive_ptr<Camera>(view->camera()));
    engine->addPass(host_pass, 0);

    ASSERT_FALSE(surface->hasPresentation());
    EXPECT_TRUE(surface->present(*view));
    ASSERT_NE(view->pipeline(), nullptr);
    EXPECT_EQ(engine->passCount(), 2u) << "the host's pass, plus the view's default window pass";
}

TEST(RenderSurfaceTest, AHostDeclaredPassIsTheSurfacesPresentation)
{
    // The other direction: a host that says which pass presents the surface gets no default window pass built
    // beside it (the demo declares its own main pass exactly like this - see DemoScene::addDemoPipeline).
    auto engine  = intrusive_ptr<RenderEngine>(new RenderEngine());
    auto view    = intrusive_ptr<SceneView>(new SceneView());
    view->setEngine(engine.get());
    auto surface = intrusive_ptr<RenderSurface>(new RenderSurface(engine.get()));

    auto host_pass = intrusive_ptr<RenderPass>(new RenderPass());
    host_pass->setCamera(intrusive_ptr<Camera>(view->camera()));
    engine->addPass(host_pass, 0);
    EXPECT_TRUE(surface->present(host_pass.get()));

    // The control's guard, verbatim: nothing to declare means nothing to build.
    if (!surface->hasPresentation()) {
        surface->present(*view);
    }
    EXPECT_EQ(view->pipeline(), nullptr) << "a declared pass keeps the view's default pipeline from being built";
    EXPECT_EQ(engine->passCount(), 1u);

    const std::vector<RenderSurface::Presentation> table = surface->presentations();
    ASSERT_EQ(table.size(), 1u);
    EXPECT_EQ(table[0].pass, host_pass.get());
    EXPECT_EQ(table[0].view, nullptr) << "a pass a host declared belongs to no view";
}

TEST(RenderSurfaceTest, PresentationsComeBackInEngineOrder)
{
    // The table answers in the order the engine runs the passes, not in the order they were declared: the first
    // entry is the pass that draws FIRST on this surface (the one a clear lands on), and ties keep declaration
    // order.
    auto engine  = intrusive_ptr<RenderEngine>(new RenderEngine());
    auto view    = intrusive_ptr<SceneView>(new SceneView());
    view->setEngine(engine.get());
    auto surface = intrusive_ptr<RenderSurface>(new RenderSurface(engine.get()));

    auto overlay = intrusive_ptr<RenderPass>(new RenderPass());
    overlay->setCamera(intrusive_ptr<Camera>(view->camera()));
    engine->addPass(overlay, 30);          // declared FIRST, runs LAST
    EXPECT_TRUE(surface->present(overlay.get()));
    EXPECT_TRUE(surface->present(*view));  // declared second, runs first (the window pass sits at the shading stage)

    const std::vector<RenderSurface::Presentation> table = surface->presentations();
    ASSERT_EQ(table.size(), 2u);
    EXPECT_EQ(table[0].view, view.get());
    EXPECT_EQ(table[1].pass, overlay.get());

    // A pass the engine no longer has draws nothing, so it leaves the answer - the declaration is not forgotten,
    // it just is not in this frame.
    engine->removePass(overlay.get());
    EXPECT_EQ(surface->presentations().size(), 1u);
    EXPECT_TRUE(surface->dismiss(overlay.get()));
    EXPECT_FALSE(surface->dismiss(overlay.get()));
}

TEST(RenderSurfaceTest, TheTableFollowsARebuiltPipeline)
{
    auto engine  = intrusive_ptr<RenderEngine>(new RenderEngine());
    auto view    = intrusive_ptr<SceneView>(new SceneView());
    view->setEngine(engine.get());
    auto surface = intrusive_ptr<RenderSurface>(new RenderSurface(engine.get()));

    EXPECT_TRUE(surface->present(*view));
    ASSERT_NE(view->pipeline(), nullptr);
    EXPECT_EQ(surface->presentations()[0].pass, view->pipeline()->windowPass());

    // A recipe change rebuilds the view's pipeline, and the window pass of the new one is a NEW pass: the entry
    // names the VIEW, and the pass is resolved when the table is read, so the answer moves with the rebuild.
    view->setOptions({ .gizmo = true });
    ASSERT_NE(view->pipeline(), nullptr);
    EXPECT_EQ(engine->passCount(), 2u) << "one rebuild: window pass + gizmo, no stale registration";

    const std::vector<RenderSurface::Presentation> table = surface->presentations();
    ASSERT_EQ(table.size(), 1u) << "the entry follows the rebuild instead of holding the pass that went away";
    EXPECT_EQ(table[0].pass, view->pipeline()->windowPass());
    EXPECT_EQ(table[0].view, view.get());
}

TEST(RenderSurfaceTest, AViewOnAnotherEngineIsRefusedOutLoud)
{
    // A view bound to another engine registers its passes on THAT frame: it could present this surface never. The
    // refusal says so instead of leaving a host looking at an empty area.
    auto engine  = intrusive_ptr<RenderEngine>(new RenderEngine());
    auto other   = intrusive_ptr<RenderEngine>(new RenderEngine());
    auto view    = intrusive_ptr<SceneView>(new SceneView());
    view->setEngine(other.get());
    auto surface = intrusive_ptr<RenderSurface>(new RenderSurface(engine.get()));

    EXPECT_FALSE(surface->present(*view));
    EXPECT_FALSE(surface->hasPresentation());
    EXPECT_EQ(view->pipeline(), nullptr) << "the refusal comes before anything is built";
    EXPECT_EQ(other->passCount(), 0u);
    EXPECT_EQ(engine->diagnosticCount(DiagnosticCategory::PassProtocolViolation), 1u);
}

/**
 * @brief The ONE clear of a surface: the first presentation announces it, and the surface does not own a colour.
 *
 * The backend applies the first clear announcement per attachment and silently drops the rest, so two clearing
 * presentations used to mean "the second pass' clearColor() is not what the picture shows", decided by pass
 * registration order and told to nobody. The surface decides WHO clears; the values stay on the pass that asked.
 */
TEST(RenderSurfaceTest, OnlyTheFirstPresentationClearsTheSurface)
{
    auto engine  = intrusive_ptr<RenderEngine>(new RenderEngine());
    auto left    = viewOn(engine.get());
    auto right   = viewOn(engine.get());
    auto surface = intrusive_ptr<RenderSurface>(new RenderSurface(engine.get()));

    surface->present(*left);
    surface->present(*right);

    RenderPass* const first  = left->pipeline()->windowPass();
    RenderPass* const second = right->pipeline()->windowPass();
    EXPECT_TRUE(first->isClearEnabled());
    EXPECT_FALSE(second->isClearEnabled())
        << "the second presentation stops claiming a clear the backend would have dropped";

    // ... and the surface invented nothing: the colour is still the one the pass asked for (one copy of it).
    EXPECT_EQ(first->clearColor(), Color(51, 51, 51, 255));
    EXPECT_TRUE(first->shouldClearDepth());
    EXPECT_TRUE(second->clearColor() == Color(51, 51, 51, 255) || !second->isClearEnabled())
        << "a presentation that does not clear does not need its colour touched either";
}

TEST(RenderSurfaceTest, TheClearMovesToThePresentationThatIsFirstNext)
{
    auto engine  = intrusive_ptr<RenderEngine>(new RenderEngine());
    auto left    = viewOn(engine.get());
    auto right   = viewOn(engine.get());
    auto surface = intrusive_ptr<RenderSurface>(new RenderSurface(engine.get()));

    surface->present(*left);
    surface->present(*right);
    RenderPass* const second = right->pipeline()->windowPass();
    ASSERT_FALSE(second->isClearEnabled());

    // The presentation that was drawing first leaves: the one behind it is now what draws first, and it gets the
    // clear it asked for back (it was told to stand down while it was second).
    EXPECT_TRUE(surface->dismiss(left.get()));
    EXPECT_TRUE(second->isClearEnabled());
}

TEST(RenderSurfaceTest, APresentationThatRebuildsGetsTheSurfacesClearDecisionBack)
{
    auto engine  = intrusive_ptr<RenderEngine>(new RenderEngine());
    auto left    = viewOn(engine.get());
    auto right   = viewOn(engine.get());
    auto surface = intrusive_ptr<RenderSurface>(new RenderSurface(engine.get()));

    surface->present(*left);
    surface->present(*right);
    ASSERT_FALSE(right->pipeline()->windowPass()->isClearEnabled());

    // A recipe change rebuilds the view's pipeline, and its window pass is a NEW pass, created enabled: the surface
    // has to put its decision back on it (the view tells the surface - see SceneView::rebuildDefaultWindowPass).
    right->setOptions({ .gizmo = true });
    ASSERT_NE(right->pipeline(), nullptr);
    EXPECT_EQ(engine->passCount(), 3u) << "left's window pass, plus the rebuilt pair";
    EXPECT_FALSE(right->pipeline()->windowPass()->isClearEnabled())
        << "a rebuild must not quietly hand the surface a second clear";
}

TEST(RenderSurfaceTest, ARecipeWithNoClearOfItsOwnIsNotMadeToClear)
{
    // Deferred: the picture is a fullscreen pass over the whole surface, whose program is a ScreenPass and therefore
    // asks for no clear. The surface decides WHO clears, not WHETHER one is needed - being the first presentation
    // does not make a pass want a clear.
    auto engine  = intrusive_ptr<RenderEngine>(new RenderEngine());
    auto view    = viewOn(engine.get());
    view->setOptions({ .path = ShadingPath::Deferred });
    auto surface = intrusive_ptr<RenderSurface>(new RenderSurface(engine.get()));

    EXPECT_TRUE(surface->present(*view));
    ASSERT_NE(view->pipeline(), nullptr);
    ASSERT_NE(view->pipeline()->windowPass(), nullptr);
    EXPECT_FALSE(view->pipeline()->windowPass()->isClearEnabled());
}

TEST(RenderSurfaceTest, TheSamePassReachedTwiceStillClearsOnce)
{
    auto engine  = intrusive_ptr<RenderEngine>(new RenderEngine());
    auto view    = viewOn(engine.get());
    auto surface = intrusive_ptr<RenderSurface>(new RenderSurface(engine.get()));

    surface->present(*view);
    RenderPass* const window = view->pipeline()->windowPass();
    EXPECT_TRUE(surface->present(window));
    EXPECT_EQ(surface->presentations().size(), 2u) << "two declarations, both naming the same pass";
    EXPECT_TRUE(window->isClearEnabled()) << "one pass is one decision, not one decision per declaration";
}

TEST(RenderSurfaceTest, APresentedViewThatDiesLeavesTheSurface)
{
    // The surface borrows its views (they are the host's), so a host that drops one must not leave the table
    // pointing at it: the view leaves the table itself, and the clear moves to what remains - all in that order, in
    // one destructor.
    auto engine  = intrusive_ptr<RenderEngine>(new RenderEngine());
    auto left    = viewOn(engine.get());
    auto surface = intrusive_ptr<RenderSurface>(new RenderSurface(engine.get()));
    surface->present(*left);
    RenderPass* const left_window = left->pipeline()->windowPass();

    auto right = viewOn(engine.get());
    surface->present(*right);
    ASSERT_EQ(surface->presentations().size(), 2u);
    ASSERT_FALSE(right->pipeline()->windowPass()->isClearEnabled());

    right = nullptr;
    EXPECT_EQ(surface->presentations().size(), 1u) << "a view that is gone cannot stay presented";
    EXPECT_TRUE(left_window->isClearEnabled());
}

TEST(RenderSurfaceTest, AViewThatOutlivesItsSurfaceDoesNotCallIntoIt)
{
    // A hosted view can outlive the table (the host destroys the surface first, or drops the whole control): the
    // surface stops naming its views as it goes, so the view's destructor has nothing to call. A dangling call here
    // would be a use-after-free, not a wrong picture.
    auto engine = intrusive_ptr<RenderEngine>(new RenderEngine());
    auto view   = viewOn(engine.get());
    {
        auto surface = intrusive_ptr<RenderSurface>(new RenderSurface(engine.get()));
        surface->present(*view);
        ASSERT_EQ(engine->passCount(), 1u);
    }

    view = nullptr;
    EXPECT_EQ(engine->passCount(), 0u) << "the view's destructor ran to the end (its window pass is unregistered)";
}
