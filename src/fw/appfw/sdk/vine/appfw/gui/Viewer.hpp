#pragma once
#include <vine/appfw/appfw_global.hpp>

#include <vine/intrusive_ptr.hpp>
#include <vine/raw_ptr.hpp>

#include <vine/appfw/gui/ViewerOptions.hpp>
#include <vine/graphics/ShadingMode.hpp>

namespace vn::graphics
{
class RenderEngine;
class RenderSurface;
class SceneView;
}  // namespace vn::graphics

VN_APPFWGUI_NS_BEGIN

class RenderControl;

/**
 * @brief The REQUEST half of one render area: what the picture that draws it is built from, and the build itself.
 *
 * One render area draws one picture (see RenderSurface), and this is the object that turns "how the host wants it
 * drawn" into the chain that draws it: the request is a VALUE (ViewerOptions - path, shading mode, which overlays,
 * off-screen size), and every structural change is one build plus one adoption on the area's view
 * (SceneView::setChain). What is left over in the area - who draws it, how big it is, which window it is - stays
 * where it was: on the surface (RenderSurface), which this class READS and never copies.
 *
 * WHAT IT IS NOT. It is not an owner of the area's presentation: isActive() READS it (a pass a host declared there, a view
 * released elsewhere), so this class never holds a second answer; view() is the view it draws with, which is the host's
 * question about the PICTURE rather than about the area. It is not a layout engine (no split, no weight, no per-view widget: multi-pane was dropped, one area
 * draws one view), not a base class for "kinds of viewers" (the difference between viewers is the VALUE of their
 * requests), and not a second configuration surface beside ViewerOptions (one request, one build, one mapping).
 *
 * WHAT IT BORROWS. The engine (chains are built on it), the area (its presentation is declared into, its ratio
 * fills the overlays' context) and the view (the picture's camera and content). All three must outlive this
 * object, which is why a Viewer is owned by whoever owns the area (RenderControl hands one out through viewer()).
 *
 * THE TAKEOVER. An area is presented by this viewer's view unless a host declared a main pass of its OWN on the
 * area (RenderSurface::present(RenderPass)), which is how a host that brings a picture of its own keeps the default
 * chain from being built beside it: such a pass makes isActive() answer false, and takeArea() leaves the area
 * alone. A view whose drawing a host takes over is given back with release().
 */
class VN_APPFW_API Viewer
{
  public:
    /**
     * @brief Creates a viewer for one render area, drawing the area with @p view.
     *
     * The view is registered here with the default request and neither a chain nor a declaration: the first attach
     * builds its chain and declares it (takeArea), while a host that configures it before the attach builds it
     * through setViewerOptions. Nothing is built until there is an area to draw into.
     *
     * @param engine  Engine the chain is built on and registered with (borrowed).
     * @param surface The area whose picture this viewer builds (borrowed).
     * @param view    The view that draws the area; null leaves the viewer with no view (nothing to build for and
     *                nothing to declare).
     */
    Viewer(raw_ptr<vn::graphics::RenderEngine> engine, raw_ptr<vn::graphics::RenderSurface> surface,
           intrusive_ptr<vn::graphics::SceneView> view);

    /**
     * @brief Destroys the viewer.
     *
     * The chain stays adopted on its view and the area keeps whatever was declared: a viewer owns a request, not a
     * declaration.
     */
    ~Viewer();

  public:
    /**
     * @brief Gets how the area draws: the request the current chain was last built from.
     *
     * @return The request (the defaults before one was ever stated).
     */
    [[nodiscard]] ViewerOptions viewerOptions() const;

    /**
     * @brief Re-states how the area draws: rebuilds the chain from @p options, adopts it, and declares the picture.
     *
     * The structural call: a chain, an overlay's existence and the mode the chain must be able to express are build
     * decisions, so this costs one chain build. The rebuild CARRIES the runtime choices of the live chain (an
     * overlay the user hid stays hidden - see PassChain::setGizmoVisible), because the request cannot state them:
     * they are switches on the built chain, not recipe fields. A declaration the area already has is untouched (it
     * resolves the view's pass afresh, so a rebuilt chain is followed), and a view that had none is declared - the
     * area's view becomes the picture this way, which is what the first attach does.
     *
     * @param options How the area draws from now on (see ViewerOptions).
     * @return true when the chain was built and adopted; false when the builder refused @p options (the previous
     *         chain - and the previous request - then stay in place).
     */
    bool setViewerOptions(const ViewerOptions& options);

    /**
     * @brief Switches the shading mode of the area.
     *
     * The cheap call: a mode the built chain can express is one override on the live chain (no rebuild, no device
     * work). When it cannot express @p mode (a deferred chain asked for a non-Lit mode), the area is rebuilt from
     * its current request carrying @p mode - see ViewerOptions and PassChain::effectivePath.
     *
     * @param mode The viewing mode to switch to.
     * @return true when the area now shades in @p mode; false when there is no view, or the rebuild failed (the
     *         previous chain then stays in place).
     */
    bool setShadingMode(vn::graphics::ShadingMode mode);

    /**
     * @brief Gives the view back to the host: the area's picture leaves this viewer.
     *
     * The declaration this viewer made is withdrawn (RenderSurface::dismiss), the view is forgotten and the
     * reference this viewer held is dropped - the view is the host's again, and a host that keeps it may destroy
     * it. The chain this viewer built stays ADOPTED on the view: a chain belongs to the view it draws, so a host
     * that takes the picture over replaces it (SceneView::setChain) rather than starting from nothing.
     *
     * @param view View to give back (the one this viewer draws with).
     * @return true when @p view was this viewer's view.
     */
    bool release(raw_ptr<vn::graphics::SceneView> view);

    /**
     * @brief Gets the view that draws this area, or null when the viewer has none.
     *
     * The view this viewer builds for and declares - what a host asks before touching the picture. Whether it
     * actually presents the area right now is isActive()'s answer (a host pass can have taken the area over, in
     * which case this is still the view the viewer would draw with).
     *
     * @return The view, or null (released, or the viewer was created with none).
     */
    [[nodiscard]] raw_ptr<vn::graphics::SceneView> view() const;

    /**
     * @brief Whether this viewer is the area's presenter: the area shows its view and no pass a host declared.
     *
     * The question a host asks to know whether the default picture is still this viewer's: a pass declared on the
     * area (RenderSurface::present(RenderPass)) means the host took the area over, and a view released or never
     * declared is not presenting anything. The answer is read from the area, so it moves the moment the area does.
     *
     * @return true when this viewer's view presents the area and no host-declared pass does.
     */
    [[nodiscard]] bool isActive() const;

    /**
     * @brief Claims the area for this viewer's view, if no pass a host declared presents it.
     *
     * The step the area's owner runs where the area's presentation is decided (the first attach, and every
     * re-attach): a view with no chain gets one built from the request (the defaults, when the host never
     * configured it) and is then declared on the area. A pass a host declared means the host took the area over -
     * nothing is built and nothing is declared (that is what keeps a host's own main pass from being doubled by a
     * default chain beside it). Idempotent: declaring the same view twice is one presentation, and a re-attach
     * re-runs it to no effect.
     */
    void takeArea();

  private:
    struct Impl;
    Impl* const d;
};

VN_APPFWGUI_NS_END
