#include <vine/graphics/Camera.hpp>
#include <vine/graphics/RenderEngine.hpp>
#include <vine/graphics/RenderPass.hpp>
#include <vine/graphics/RenderPipelineBuilder.hpp>
#include <vine/graphics/RenderSurface.hpp>
#include <vine/graphics/SceneView.hpp>

#include <gtest/gtest.h>

#include <vector>

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
intrusive_ptr<Pipeline> buildFor(const intrusive_ptr<SceneView>& view, RenderEngine* engine,
                                 const PipelineOptions& options = {})
{
    RenderPipelineBuilder builder(engine);
    builder.setCamera(view->camera());
    builder.setContent(view->scene());
    intrusive_ptr<Pipeline> pipeline = builder.build(options);
    if (pipeline != nullptr) {
        view->setPipeline(pipeline);
    }
    return pipeline;
}

/** @brief A view bound to @p engine and drawing @p options (the engine outlives the view in these tests). */
intrusive_ptr<SceneView> viewOn(RenderEngine* engine, const PipelineOptions& options = {})
{
    auto view = intrusive_ptr<SceneView>(new SceneView());
    view->setEngine(engine);
    buildFor(view, engine, options);
    return view;
}

}  // namespace

/**
 * @brief A surface's presentation table: what draws into it, in which order, and who decides.
 *
 * "Who presents this surface" used to be inferred - a view asked the engine whether ANY registered pass carried
 * its camera with a null target (the removed RenderEngine::hasWindowPass) and stood down when one did, and this
 * table used to build the view's default window pass on demand. These tests pin the replacement: the SURFACE owns
 * the answer, a HOST declares it, and a declaration resolves to a pass (a view's declaration points at the pipeline
 * the view adopted; a host with its own pass declares that one) - the table never builds anything.
 */
TEST(RenderSurfaceTest, PresentingAViewDeclaresWhatItDraws)
{
    // The declaration a host makes when it has no main pass of its own: this surface is presented by the pass of the
    // pipeline the VIEW adopted. The table resolves the pass back through the view (it builds nothing), so the host
    // order is: build, adopt, declare.
    auto engine  = intrusive_ptr<RenderEngine>(new RenderEngine());
    auto view    = intrusive_ptr<SceneView>(new SceneView());
    view->setEngine(engine.get());
    auto surface = intrusive_ptr<RenderSurface>(new RenderSurface(engine.get()));

    EXPECT_FALSE(surface->hasPresentation());
    auto pipeline = buildFor(view, engine.get());
    ASSERT_NE(pipeline, nullptr);
    EXPECT_TRUE(surface->present(*view));
    EXPECT_TRUE(surface->hasPresentation());
    EXPECT_EQ(engine->passCount(), 1u) << "one pass: the window pass of the pipeline that was adopted";

    const std::vector<RenderSurface::Presentation> table = surface->presentations();
    ASSERT_EQ(table.size(), 1u);
    EXPECT_EQ(table[0].pass, pipeline->windowPass());
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

TEST(RenderSurfaceTest, AHostPassRegisteredOnTheEngineIsNotAPresentation)
{
    // Merely existing is not a presentation (that inference is what M2a removed): a pass carrying this very camera
    // can sit in the frame while the surface knows of nothing. The view draws whatever pipeline it adopted, and the
    // table answers only what the HOST declared - so the two are independent facts.
    auto engine  = intrusive_ptr<RenderEngine>(new RenderEngine());
    auto view    = intrusive_ptr<SceneView>(new SceneView());
    view->setEngine(engine.get());
    auto surface = intrusive_ptr<RenderSurface>(new RenderSurface(engine.get()));

    auto host_pass = intrusive_ptr<RenderPass>(new RenderPass());
    host_pass->setCamera(intrusive_ptr<Camera>(view->camera()));
    engine->addPass(host_pass, 0);

    EXPECT_FALSE(surface->hasPresentation());
    auto pipeline = buildFor(view, engine.get());
    ASSERT_NE(pipeline, nullptr);
    EXPECT_EQ(engine->passCount(), 2u) << "the host's pass, beside the window pass the view adopted";
    EXPECT_FALSE(surface->hasPresentation()) << "the host's pass draws this surface only if the host says so";

    EXPECT_TRUE(surface->present(*view));
    EXPECT_EQ(surface->presentations()[0].pass, pipeline->windowPass());
}

TEST(RenderSurfaceTest, AHostDeclaredPassIsTheSurfacesPresentation)
{
    // The other direction: what a host DECLARES is what presents the surface (the demo declares its own main pass
    // exactly like this - see DemoScene::addDemoPipeline). The table never builds one, so a host that brings its own
    // is the whole answer: nothing is created behind it, and the entry belongs to no view.
    auto engine  = intrusive_ptr<RenderEngine>(new RenderEngine());
    auto view    = intrusive_ptr<SceneView>(new SceneView());
    view->setEngine(engine.get());
    auto surface = intrusive_ptr<RenderSurface>(new RenderSurface(engine.get()));

    auto host_pass = intrusive_ptr<RenderPass>(new RenderPass());
    host_pass->setCamera(intrusive_ptr<Camera>(view->camera()));
    engine->addPass(host_pass, 0);
    EXPECT_TRUE(surface->present(host_pass.get()));

    EXPECT_EQ(engine->passCount(), 1u) << "the host's pass is the presentation; nothing was built beside it";
    const std::vector<RenderSurface::Presentation> table = surface->presentations();
    ASSERT_EQ(table.size(), 1u);
    EXPECT_EQ(table[0].pass, host_pass.get());
    EXPECT_EQ(table[0].view, nullptr) << "a pass a host declared belongs to no view";

    // The view adopted nothing, so it has nothing to declare: its declaration needs a pass to point at.
    EXPECT_FALSE(surface->present(*view));
    EXPECT_EQ(surface->presentations().size(), 1u);
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
    ASSERT_NE(buildFor(view, engine.get()), nullptr);
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

    ASSERT_NE(buildFor(view, engine.get()), nullptr);
    EXPECT_TRUE(surface->present(*view));
    ASSERT_NE(view->pipeline(), nullptr);
    EXPECT_EQ(surface->presentations()[0].pass, view->pipeline()->windowPass());

    // A structural change rebuilds the pipeline, and the window pass of the new one is a NEW pass: the entry names
    // the VIEW, and the pass is resolved when the table is read, so the answer moves with the rebuild.
    PipelineOptions with_gizmo;
    with_gizmo.gizmo.source_camera = view->camera();
    ASSERT_NE(buildFor(view, engine.get(), with_gizmo), nullptr);
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
    auto view    = viewOn(other.get());
    auto surface = intrusive_ptr<RenderSurface>(new RenderSurface(engine.get()));

    ASSERT_NE(view->pipeline(), nullptr);
    EXPECT_FALSE(surface->present(*view));
    EXPECT_FALSE(surface->hasPresentation());
    EXPECT_EQ(engine->passCount(), 0u) << "nothing of that view was ever registered on this surface's engine";
    EXPECT_EQ(engine->diagnosticCount(DiagnosticCategory::PassProtocolViolation), 1u);
}

TEST(RenderSurfaceTest, TheSurfaceOwnsTheRectangleAndTheViewReadsIt)
{
    // The split belongs HERE, with the clear and the order: a host states it once, in normalized terms, and the
    // view draws into what the table says instead of holding a second copy that could disagree (see
    // SceneView::viewport). Both spellings write the same rectangle - the view's forwards to the table.
    auto engine  = intrusive_ptr<RenderEngine>(new RenderEngine());
    auto view    = viewOn(engine.get());
    auto surface = intrusive_ptr<RenderSurface>(new RenderSurface(engine.get()));

    // A view that was already drawing into part of the surface keeps that part when it is presented.
    view->setViewport(NormalizedViewport{ 0.0, 0.0, 0.5, 1.0 });
    EXPECT_TRUE(surface->present(*view));
    ASSERT_EQ(surface->presentations().size(), 1u);
    ASSERT_TRUE(surface->presentations()[0].rect.has_value());
    EXPECT_DOUBLE_EQ(surface->presentations()[0].rect->width, 0.5);
    EXPECT_DOUBLE_EQ(view->viewport().width, 0.5) << "the view reads its rectangle from the table";

    view->setViewport(NormalizedViewport{ 0.5, 0.0, 0.5, 0.25 });
    ASSERT_TRUE(surface->presentations()[0].rect.has_value());
    EXPECT_DOUBLE_EQ(surface->presentations()[0].rect->x, 0.5) << "the view's spelling writes the table";
    EXPECT_DOUBLE_EQ(surface->presentations()[0].rect->height, 0.25);

    EXPECT_TRUE(surface->setViewport(view.get(), NormalizedViewport{ 0.0, 0.0, 0.25, 1.0 }));
    EXPECT_DOUBLE_EQ(view->viewport().width, 0.25) << "the surface's spelling writes the view's rectangle";

    // An invalid rectangle means the whole surface, whichever spelling asked (see SceneView::setViewport).
    EXPECT_TRUE(surface->setViewport(view.get(), NormalizedViewport{ 0.0, 0.0, 0.0, 0.0 }));
    EXPECT_TRUE(view->viewport().isFull());

    // A view that presents nothing here has no rectangle to state: the surface says so rather than inventing one.
    auto stranger = viewOn(engine.get());
    EXPECT_FALSE(surface->setViewport(stranger.get(), NormalizedViewport{ 0.0, 0.0, 0.5, 1.0 }));

    // Dismissing hands the rectangle BACK to the view: it keeps drawing into the same area, now as its own
    // statement rather than this surface's. The area is stated by the SURFACE here (0.7 - a value this view's own
    // field never held), so the assertion below can only pass if the surface really handed it over.
    EXPECT_TRUE(surface->setViewport(view.get(), NormalizedViewport{ 0.0, 0.0, 0.7, 1.0 }));
    EXPECT_TRUE(surface->dismiss(view.get()));
    EXPECT_DOUBLE_EQ(view->viewport().width, 0.7) << "the view keeps the rectangle it was drawn into";
    EXPECT_TRUE(surface->viewAt(0.25, 0.5) == nullptr) << "and the table no longer describes any split";
}

TEST(RenderSurfaceTest, PresentingAViewWithARectangleStatesTheSplitInOneCall)
{
    auto engine  = intrusive_ptr<RenderEngine>(new RenderEngine());
    auto view    = viewOn(engine.get());
    auto surface = intrusive_ptr<RenderSurface>(new RenderSurface(engine.get()));

    EXPECT_TRUE(surface->present(*view, NormalizedViewport{ 0.5, 0.0, 0.5, 1.0 }));
    EXPECT_DOUBLE_EQ(view->viewport().x, 0.5) << "the view draws into the stated half";

    // Declaring the same view twice is still ONE presentation, and the rectangle that stands is the one already
    // stated (a host that wants to move it calls setViewport - the geometry call).
    EXPECT_FALSE(surface->present(*view, NormalizedViewport{ 0.0, 0.0, 1.0, 1.0 }));
    EXPECT_EQ(surface->presentations().size(), 1u);
    EXPECT_DOUBLE_EQ(view->viewport().x, 0.5);

    // An invalid rectangle is the whole surface, and a presentation with no rectangle at all is the whole surface
    // too: that is the shape the default path had before splits existed.
    auto plain = viewOn(engine.get());
    EXPECT_TRUE(surface->present(*plain));
    ASSERT_TRUE(surface->presentations()[1].rect.has_value());
    EXPECT_TRUE(surface->presentations()[1].rect->isFull());
}

TEST(RenderSurfaceTest, AViewIsFoundByThePointItsRectangleHolds)
{
    // The default answer to "which view is the cursor over": the presentation whose rectangle contains the point.
    // A pass a host declared is not a view, so it is never the answer (it states its own viewport in device
    // pixels - see Presentation::rect).
    auto engine  = intrusive_ptr<RenderEngine>(new RenderEngine());
    auto left    = viewOn(engine.get());
    auto right   = viewOn(engine.get());
    auto surface = intrusive_ptr<RenderSurface>(new RenderSurface(engine.get()));

    surface->present(*left, NormalizedViewport{ 0.0, 0.0, 0.5, 1.0 });
    surface->present(*right, NormalizedViewport{ 0.5, 0.0, 0.5, 1.0 });

    EXPECT_EQ(surface->viewAt(0.0, 0.0), left.get());
    EXPECT_EQ(surface->viewAt(0.25, 0.5), left.get());
    EXPECT_EQ(surface->viewAt(0.75, 0.5), right.get());
    // The shared edge belongs to the east neighbour (NormalizedViewport::contains), so neighbouring areas never
    // both claim a point - and the lower/right edge of the surface belongs to nobody.
    EXPECT_EQ(surface->viewAt(0.5, 0.5), right.get());
    EXPECT_EQ(surface->viewAt(1.0, 1.0), nullptr);

    auto host_pass = intrusive_ptr<RenderPass>(new RenderPass());
    host_pass->setCamera(intrusive_ptr<Camera>(left->camera()));
    engine->addPass(host_pass, 5);
    auto other = intrusive_ptr<RenderSurface>(new RenderSurface(engine.get()));
    ASSERT_TRUE(other->present(host_pass.get()));
    EXPECT_EQ(other->viewAt(0.5, 0.5), nullptr) << "a host's pass is not a view";
}

TEST(RenderSurfaceTest, ThePointGoesToThePresentationDrawnLast)
{
    // Overlapping presentations (a preview in a corner of a full-surface view): the point belongs to the picture
    // that is actually on it, which is the one drawn last.
    auto engine  = intrusive_ptr<RenderEngine>(new RenderEngine());
    auto under   = viewOn(engine.get());
    auto over    = viewOn(engine.get());
    auto surface = intrusive_ptr<RenderSurface>(new RenderSurface(engine.get()));

    surface->present(*under);                                            // the whole surface
    surface->present(*over, NormalizedViewport{ 0.0, 0.0, 0.25, 0.25 });  // the corner preview

    EXPECT_EQ(surface->viewAt(0.1, 0.1), over.get());
    EXPECT_EQ(surface->viewAt(0.5, 0.5), under.get()) << "outside the preview the full-surface view is the one";
}

TEST(RenderSurfaceTest, AViewThatAlreadyPresentsAnotherSurfaceIsRefused)
{
    // One view draws ONE picture, and its rectangle is read from the surface it presents: a second presentation
    // would leave it with two rectangles and no way to tell which one it draws into. Refused out loud, like the
    // other-engine case.
    auto engine = intrusive_ptr<RenderEngine>(new RenderEngine());
    auto view   = viewOn(engine.get());
    auto first  = intrusive_ptr<RenderSurface>(new RenderSurface(engine.get()));
    auto second = intrusive_ptr<RenderSurface>(new RenderSurface(engine.get()));

    ASSERT_TRUE(first->present(*view, NormalizedViewport{ 0.0, 0.0, 0.5, 1.0 }));
    EXPECT_FALSE(second->present(*view));
    EXPECT_FALSE(second->hasPresentation());
    EXPECT_EQ(engine->diagnosticCount(DiagnosticCategory::PassProtocolViolation), 1u);

    // Nothing moved: the first surface still states the rectangle the view draws into...
    ASSERT_TRUE(first->presentations()[0].rect.has_value());
    EXPECT_DOUBLE_EQ(first->presentations()[0].rect->width, 0.5);
    EXPECT_DOUBLE_EQ(view->viewport().width, 0.5);

    // ... and dismissing it there frees the view for the other surface (with the rectangle it was drawing into).
    EXPECT_TRUE(first->dismiss(view.get()));
    EXPECT_TRUE(second->present(*view));
    ASSERT_TRUE(second->presentations()[0].rect.has_value());
    EXPECT_DOUBLE_EQ(second->presentations()[0].rect->width, 0.5);
}

TEST(RenderSurfaceTest, TheFocusIsThePaneTheInputWentTo)
{
    // Which pane the user is working in is a fact about this surface, like the rectangles and the clear: the input
    // path says which view it routed to, and the table answers "which view does a key / a command mean" - a
    // question with no position in it, so it cannot be answered by routing again.
    auto engine  = intrusive_ptr<RenderEngine>(new RenderEngine());
    auto left    = viewOn(engine.get());
    auto right   = viewOn(engine.get());
    auto surface = intrusive_ptr<RenderSurface>(new RenderSurface(engine.get()));

    surface->present(*left, NormalizedViewport{ 0.0, 0.0, 0.5, 1.0 });
    surface->present(*right, NormalizedViewport{ 0.5, 0.0, 0.5, 1.0 });

    // Nothing has been pressed yet: the FIRST presentation is the answer, so a surface always names a view.
    EXPECT_EQ(surface->focusedView(), left.get());

    EXPECT_TRUE(surface->focus(right.get()));
    EXPECT_EQ(surface->focusedView(), right.get());

    // A view that presents nothing here is not a pane of this surface (its rectangle could not be read), so it
    // cannot take the focus - and the focus does not move.
    auto stranger = viewOn(engine.get());
    EXPECT_FALSE(surface->focus(stranger.get()));
    EXPECT_EQ(surface->focusedView(), right.get());

    // Clearing the explicit focus brings the default back.
    EXPECT_TRUE(surface->focus(nullptr));
    EXPECT_EQ(surface->focusedView(), left.get());

    // The focused pane going away leaves no dangling answer.
    EXPECT_TRUE(surface->focus(right.get()));
    EXPECT_TRUE(surface->dismiss(right.get()));
    EXPECT_EQ(surface->focusedView(), left.get()) << "the dismissed pane's focus goes with it";

    // ... and neither does a presentation whose pass left the frame (the pipeline that built it was dropped): it
    // is still declared here, but it draws nothing, so it cannot be what "the current view" means.
    EXPECT_TRUE(surface->focus(left.get()));
    engine->removePass(left->pipeline()->windowPass());
    EXPECT_EQ(surface->focusedView(), nullptr) << "no live presentation has a view left";
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

    // A structural change rebuilds the pipeline, and its window pass is a NEW pass, created enabled: the surface has
    // to put its decision back on it (adopting a pipeline tells the surface - see SceneView::setPipeline).
    PipelineOptions with_gizmo;
    with_gizmo.gizmo.source_camera = right->camera();
    ASSERT_NE(buildFor(right, engine.get(), with_gizmo), nullptr);
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
    PipelineOptions deferred;
    deferred.path = ShadingPath::Deferred;
    auto view     = viewOn(engine.get(), deferred);
    auto surface  = intrusive_ptr<RenderSurface>(new RenderSurface(engine.get()));

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
