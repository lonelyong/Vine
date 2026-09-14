#pragma once

#include <vine/appfw/appfw_global.hpp>
#include <vine/graphics/Scene.hpp>
#include <vine/intrusive_ptr.hpp>

V_APPFW_NS_BEGIN

namespace gui {
class RenderControl;
}

/**
 * @brief The default demo the app installs at start-up: content, overlays, diagnostic passes and pipeline.
 *
 * The demo is the app's content vocabulary rather than part of the shell: the shell owns the
 * window, the dock layout and the Ribbon, this class owns what the central 3D view draws.
 *
 * ONE CONTENT VOCABULARY, TWO COMPOSITIONS. The forward and Deferred examples draw the same content
 * wherever both paths can draw it identically, and each adds the slices only it can use (buildScene is
 * the one place that states how a path composes them):
 *  - opaque base: ground slab + lit box + cube-mapped box, in both paths;
 *  - G-buffer variety: the extra opaque blocks the Deferred geometry pass shades in one go, Deferred
 *    only - the forward path's variety is its own showcase;
 *  - forward showcase: wireframe / culling / custom-program / point-cloud / nested-transform slices,
 *    forward only, because the G-buffer geometry pass replaces each drawable's program with its own;
 *  - blended pair: the translucent box and the rainbow star cloud, which need a destination to blend
 *    against - drawn in the forward scene, or in the Deferred overlay scene the pipeline composites
 *    after the deferred-lit result (depth test on, depth write off);
 *  - sky box: in an overlay for both paths, drawn last so it fills only the background.
 *
 * So a single Scene OBJECT cannot serve both paths, while the vocabulary can and does: what differs is
 * which slices a path may draw, and buildScene decides that in one place.
 */
class AppShellDemo
{
  public:
    /**
     * @brief Binds the demo to the render control whose view it populates.
     *
     * @param control Render control owning the engine, view, camera and scene the demo builds into.
     */
    explicit AppShellDemo(gui::RenderControl* control);

    /**
     * @brief Builds the demo: scene content, overlay scene, light rig, diagnostic passes and the pipeline.
     *
     * Must be called before the control is initialized (before the backend attaches), because the
     * pipeline is built from the passes registered here.
     */
    void install();

  private:
    /**
     * @brief Composes the scene content for one path and builds the overlay scene for it.
     *
     * @param deferred true for the Deferred path (opaque base + G-buffer variety, overlay with the
     *                 blended pair and a light rig), false for the forward path (opaque base + forward
     *                 showcase + blended pair, sky-only overlay).
     * @return The overlay scene the pipeline draws after the path's own result (never null).
     */
    vine::intrusive_ptr<vine::graphics::Scene> buildScene(bool deferred);

    /// Render control the demo builds into (owned by the shell's dock layout).
    gui::RenderControl* control_;
};

V_APPFW_NS_END
