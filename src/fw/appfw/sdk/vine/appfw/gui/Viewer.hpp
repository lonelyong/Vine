#pragma once

#include <vector>

#include <vine/intrusive_ptr.hpp>
#include <vine/raw_ptr.hpp>

#include <vine/graphics/ShadingMode.hpp>
#include <vine/graphics/Viewport.hpp>

#include <vine/appfw/appfw_global.hpp>

#include "ViewerOptions.hpp"

namespace vn::graphics
{
class RenderEngine;
class RenderSurface;
class SceneView;
}

VN_APPFWGUI_NS_BEGIN

/**
 * @brief The panes of ONE render area: which views draw it, and how each of them draws (a request per pane).
 *
 * WHAT IT IS. A render area (RenderSurface: one native window, one swapchain, one frame) is usually divided
 * into panes - a single 3D view, or a four-pane comparison where each pane has its own camera, its own content
 * and its own picture settings. The SET of views and their RECTANGLES are facts about the area and live in its
 * presentation table (RenderSurface::presentations, stated with present / setViewport); what is left over is the
 * per-pane REQUEST - chain, shading mode, which overlays exist, off-screen size (ViewerOptions) - and the build
 * that turns a request into a chain. This class is that half: one request per pane, one build/adopt per request
 * change, and the declaration of each pane on the area's table.
 *
 * WHAT IT IS NOT. It is not a second owner of the layout: views() reads the presentation table, the rectangle a
 * pane draws into stays there and is stated through setPaneRect, and nothing here caches either. It is not a
 * layout engine (no split, no weight, no per-pane widget: a host that computes rectangles states them), not a
 * base class for "kinds of viewers" (the difference between viewers is the VALUE of their requests), and not a
 * second configuration surface beside ViewerOptions (one request, one build, one mapping).
 *
 * WHAT IT BORROWS. The engine (chains are built on it) and the surface (its table is declared into, its ratio
 * fills the overlays' context). Both must outlive this object, which is why a Viewer is owned by whoever owns
 * the area (RenderControl hands one out through viewer()).
 *
 * THE TAKEOVER. An area is presented by this viewer's panes unless a host declared a main pass of its OWN on the
 * table (RenderSurface::present(RenderPass)), which is how a host that brings a picture of its own keeps the
 * default chain from being built beside it: such a pass makes isActive() answer false, and takeArea() leaves the
 * table alone. A pane whose drawing a host takes over pane by pane is given back with release().
 *
 * THE TABLE IS THE ANSWER, ONCE MORE. isActive() and views() are reads of the presentation table, never of a
 * remembered copy: a presentation dismissed anywhere (a view dropped, a pass dismissed) is gone from both on the
 * next call, whether this viewer or somebody else dismissed it.
 */
class VN_APPFW_API Viewer
{
  public:
    /**
     * @brief Creates a viewer for one render area, with the area's own view as its first pane.
     *
     * The area's own view (the control's: the view the area falls back to) is registered here with the default
     * request and neither a chain nor a declaration: the first attach builds its chain and declares it
     * (takeArea), while a host that configures it before the attach builds it through setViewerOptions.
     *
     * @param engine  Engine the panes' chains are built on and registered with (borrowed).
     * @param surface The area's presentation table (borrowed).
     * @param view    The area's own view; null leaves the viewer with no pane until one is added.
     */
    Viewer(raw_ptr<vn::graphics::RenderEngine> engine, raw_ptr<vn::graphics::RenderSurface> surface,
           intrusive_ptr<vn::graphics::SceneView> view);

    /**
     * @brief Destroys the viewer.
     *
     * The panes' chains stay adopted on their views and the area's table keeps whatever was declared: a viewer
     * owns requests, not declarations.
     */
    ~Viewer();

  public:
    /**
     * @brief Adds a pane: builds @p view's chain from @p options, adopts it and declares the pane on the area.
     *
     * The whole order a pane needs in one call (build -> adopt -> declare): the pipeline is built for @p view
     * (its camera and content are the builder's inputs), adopted (SceneView::setPipeline, which lays the pane's
     * geometry out) and declared into @p rect of the area's table (RenderSurface::present), so input routing and
     * focus follow from the same statement (RenderSurface::viewAt). The request is remembered with the pane:
     * a later rebuild (setViewerOptions, setShadingMode) starts from it.
     *
     * @param view    View to add; must be bound to this viewer's engine. Null is refused.
     * @param rect    The area it draws into (normalized); an invalid rectangle means the whole area.
     * @param options How this pane draws from now on (see ViewerOptions).
     * @return true when the pane is in the area and draws the chain built from @p options; false when the view
     *         is bound to another engine, the builder refused @p options (see RenderPipelineBuilder::build), or
     *         the table refused the declaration (it reports why - e.g. a view that presents another surface).
     */
    bool addView(intrusive_ptr<vn::graphics::SceneView> view, const vn::graphics::NormalizedViewport& rect,
                 const ViewerOptions& options);

    /**
     * @brief Gives @p view back to the host: the pane leaves this viewer.
     *
     * The declaration this viewer made for @p view is withdrawn (RenderSurface::dismiss), its request is
     * forgotten and the reference this viewer held is dropped - the view is the host's again, and a host that
     * keeps it may destroy it. The chain this viewer built stays ADOPTED on the view: a chain belongs to the
     * view it draws, so a host that takes the pane's drawing over replaces it (SceneView::setPipeline) rather
     * than starting from nothing.
     *
     * @param view View to give back.
     * @return true when @p view was one of this viewer's panes.
     */
    bool removeView(raw_ptr<vn::graphics::SceneView> view);

    /**
     * @brief The takeover spelling of removeView: the host takes this pane's drawing over.
     *
     * Same act under the name the takeover reads as ("the host releases this pane from the viewer"): the
     * declaration is withdrawn and the pane is forgotten, so a panel the application now draws itself does not
     * stay declared through this viewer (isActive() answers from the table and sees the release at once).
     *
     * @param view View to give back.
     * @return true when @p view was one of this viewer's panes.
     */
    bool release(raw_ptr<vn::graphics::SceneView> view);

    /**
     * @brief The views of this area, in the order the engine draws them: a read of the presentation table.
     *
     * The answer is what the area presents NOW, not what was once declared: a pane whose view was dropped, a
     * view whose declaration was dismissed anywhere (here or by another owner of the table) is left out, and a
     * view a host declared on the table itself is included - the set of views an area is made of belongs to the
     * area, and this is a read of it (never a copy this class keeps in step).
     *
     * @return The views that present this area, first-drawn first (empty when no view does).
     */
    [[nodiscard]] std::vector<raw_ptr<vn::graphics::SceneView>> views() const;

    /**
     * @brief States the area @p view draws into (normalized) - forwarded to the area's table.
     *
     * Geometry, not recipe: nothing is rebuilt, and the pane's rectangle stays where the area states it
     * (RenderSurface::setViewport). The view need not be one of this viewer's panes: the rectangle is a fact
     * about the area, and any view presenting it can have one.
     *
     * @param view View whose area to state.
     * @param rect The area it draws into (normalized); an invalid rectangle means the whole area.
     * @return true when @p view presents this area (its rectangle was stated); false when it does not.
     */
    bool setPaneRect(raw_ptr<vn::graphics::SceneView> view, const vn::graphics::NormalizedViewport& rect);

    /**
     * @brief Gets how @p view draws: the request this pane was last built from.
     *
     * @param view View to read.
     * @return The request of that pane; the defaults when @p view is not one of this viewer's panes.
     */
    ViewerOptions viewerOptions(raw_ptr<vn::graphics::SceneView> view) const;

    /**
     * @brief Re-states how @p view draws: rebuilds its chain from @p options and adopts it.
     *
     * The structural call: a chain, an overlay's existence and the mode the chain must be able to express are
     * build decisions, so this costs one pipeline build. The rebuild CARRIES the runtime choices of the live
     * chain (an overlay the user hid stays hidden - see Pipeline::setGizmoVisible), because the request cannot
     * state them: they are switches on the built chain, not recipe fields. A declaration this pane already has
     * is untouched (the table resolves the view's pass afresh, so a rebuilt chain is followed), and a view that
     * had no declaration yet is registered - the area's own view becomes a pane this way.
     *
     * @param view    View to rebuild for; bound to this viewer's engine. Null is refused.
     * @param options How this pane draws from now on.
     * @return true when the chain was built and adopted; false when the builder refused @p options (the previous
     *         chain - and the previous request - then stay in place).
     */
    bool setViewerOptions(raw_ptr<vn::graphics::SceneView> view, const ViewerOptions& options);

    /**
     * @brief Switches the shading mode of one pane.
     *
     * The cheap call: a mode the built chain can express is one override on the live pipeline (no rebuild, no
     * device work). When it cannot express @p mode (a deferred chain asked for a non-Lit mode), the pane is
     * rebuilt from its current request carrying @p mode - see ViewerOptions and
     * RenderPipelineBuilder::effectivePath.
     *
     * @param view View whose pane to switch.
     * @param mode The viewing mode to switch to.
     * @return true when the pane now shades in @p mode; false when @p view is not one of this viewer's panes, or
     *         the rebuild failed (the previous chain then stays in place).
     */
    bool setShadingMode(raw_ptr<vn::graphics::SceneView> view, vn::graphics::ShadingMode mode);

    /**
     * @brief Whether this viewer is the area's presenter: the area shows its panes and no pass a host declared.
     *
     * The question a host asks to know whether the default picture is still this viewer's: a pass declared on
     * the area's table (RenderSurface::present(RenderPass)) means the host took the area over, and a viewer whose
     * panes are not declared (a pane released, nothing attached yet) is not presenting anything. The answer is
     * read from the table, so it moves the moment the table does.
     *
     * @return true when at least one pane of this viewer presents the area and no host-declared pass does.
     */
    [[nodiscard]] bool isActive() const;

    /**
     * @brief Claims the area for this viewer's panes, if no pass a host declared presents it.
     *
     * The step the area's owner runs where the area's presentation is decided (the first attach, and every
     * re-attach): each pane with no chain gets one built from its request (the area's own view, when the host
     * never configured it), and every pane then declares itself on the table. A pass a host declared means the
     * host took the area over - nothing is built and nothing is declared (that is what keeps a host's own main
     * pass from being doubled by a default chain beside it). Idempotent: declaring a pane twice is one
     * presentation, and a re-attach re-runs it to no effect.
     */
    void takeArea();

  private:
    /// One pane: the view, and the request its chain is built from. The rectangle is NOT here - it belongs to
    /// the area's table (see views / setPaneRect).
    struct Impl;
    Impl* const d;
};

VN_APPFWGUI_NS_END
