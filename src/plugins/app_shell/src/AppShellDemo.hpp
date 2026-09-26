#pragma once

#include <array>
#include <memory>

#include <vine/appfw/appfw_global.hpp>
#include <vine/graphics/Group.hpp>
#include <vine/graphics/Scene.hpp>
#include <vine/graphics/Texture.hpp>
#include <vine/imaging/Image.hpp>
#include <vine/intrusive_ptr.hpp>

#include <vine/async/Task.hpp>

VN_APPFW_NS_BEGIN

namespace gui {
class RenderControl;
}

class AppShellDemo;

/**
 * @brief The demo's cube-map faces, read and decoded but NOT yet a CubeMap.
 *
 * This is the boundary the demo's start-up is split along. Reading a shipped 2048^2 face, decoding it and
 * box-filtering it down is the whole cost of the demo's start-up (measured 2026-09-26: 1765 ms of the
 * 1768 ms install()), and none of it touches a window, a widget or a scene - so it happens off the
 * application thread (see loadDemoCubeImagesAsync()). Turning the images into a CubeMap and hanging it on
 * the scene is a few microseconds and stays on the application thread, where the graphics objects belong
 * (see AppShellDemo::installContent()).
 *
 * The arrays are parallel to the face tables (kBoxCubeFaces / kSkyCubeFaces): slot i is face i's image.
 * A face that could not be read is null, and the group is only used when all six are there.
 */
struct DemoCubeImages {
    /// The BOX map's faces (256^2): what `env_box` samples.
    std::array<vn::intrusive_ptr<vn::imaging::Image>, 6> box;
    /// The SKY map's faces (512^2): what `sky_box` samples.
    std::array<vn::intrusive_ptr<vn::imaging::Image>, 6> sky;
    /// Whether all six faces of each map were read (a group with a missing face is skipped, already reported).
    bool box_complete = false;
    bool sky_complete = false;
};

/**
 * @brief Reads, decodes and filters the demo's two cube maps, off the application thread.
 *
 * Pure data: file reads, JPEG decodes and an integer box filter, in twelve pool tasks run concurrently
 * (vn::async::run + whenAll, one face each), so the wall-clock cost is close to ONE face rather than
 * twelve - on the machine the numbers above were taken on, that is the difference between 1.8 s and a
 * fraction of a second. Nothing here creates a graphics object or touches a widget, which is what makes
 * it safe to run anywhere.
 *
 * Assets that are not there are reported - once per map - and the map is skipped: a cube map with a white
 * face would read as a shading bug (see the file's asset directory note).
 *
 * @return The staged faces; await it, then hand the result to AppShellDemo::installContent() on the
 *         application thread.
 */
vn::async::Task<DemoCubeImages> loadDemoCubeImagesAsync();

/**
 * @brief Finishes the demo's start-up in the background: reads the assets off the application thread, then has the
 * application thread hang them on the skeleton.
 *
 * The shell calls this once the skeleton is built (AppShellDemo::install() + the control's init(), both synchronous and
 * on the application thread) and then returns: the interface is up, and this brings the content in.
 *
 * The shape - and why it is the plugin's business rather than the framework's:
 *  1. `co_await loadDemoCubeImagesAsync()` reads, decodes and filters twelve 2048^2 faces CONCURRENTLY on the pool
 *     (~1.8 s of the app's start-up becomes a fraction of a second), leaving the application thread free: the startup
 *     frame keeps repainting, progress keeps being reported, nothing freezes;
 *  2. `MainThreadDispatcher::invokeOnMainThread()` hands the WHOLE install - the two CubeMaps and the scene nodes, which
 *     are graphics objects and belong to the application thread - over in one call, and **waits for it**: the task keeps
 *     its own thread and merely lends the application thread the piece that must run there. There is nothing between
 *     the two that could run anywhere else, so one delegation is enough.
 *
 * The work is fire-and-forget on purpose: the boot does not wait for it (the framework does not own a plugin's threads),
 * so the main window may come up a moment before the content does. It is owned by this coroutine's own frame (a
 * DetachedTask that captured the demo), so nothing else has to keep the demo alive.
 *
 * @param demo The demo whose skeleton is waiting for its content.
 */
void assembleDemoContentLater(std::shared_ptr<AppShellDemo> demo);

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
     * @brief Builds the demo's SKELETON: content and overlay scene roots, camera, diagnostic passes and the
     * pipeline - everything that does not need an asset.
     *
     * Must be called before the control is initialized (before the backend attaches), because the pipeline
     * is built from the passes registered here.
     *
     * It deliberately reads NO asset: the two cube maps are the expensive half of the demo's start-up and
     * arrive later (loadDemoCubeImagesAsync() + installContent()), which is what takes that half off the
     * application thread. Both scenes are left with the slot the content goes into - the overlay's sky slot
     * is a group created here, in the position the sky has to keep (see buildScene).
     */
    void install();

    /**
     * @brief Hangs the staged cube maps on the skeleton: `env_box` in the content scene, `sky_box` in the
     * overlay's sky slot.
     *
     * APPLICATION THREAD ONLY: it creates graphics objects (the two CubeMaps) and adds them to scenes the
     * render control is already drawing. The heavy half - reading, decoding, filtering - is behind it by
     * then (loadDemoCubeImagesAsync()).
     *
     * A group that is incomplete is skipped (the reader reported it), so a demo without its assets is the
     * scene the skeleton built and nothing else.
     *
     * @param images Staged faces for both maps.
     */
    void installContent(const DemoCubeImages& images);

    /**
     * @brief Reports whether the render session is still being attached.
     *
     * The attach reads the scene graph (pipelines from the registered passes, plus one warm-up frame), so the demo's
     * content has to be installed after it - see the wait in assembleDemoContentLater().
     *
     * @return true while the attach is in flight.
     */
    [[nodiscard]] bool sessionAttaching() const noexcept;

  private:
    /**
     * @brief Composes the scene content for one path and builds the overlay scene for it.
     *
     * @param deferred true for the Deferred path (opaque base + G-buffer variety, overlay with the
     *                 blended pair and a light rig), false for the forward path (opaque base + forward
     *                 showcase + blended pair, sky-only overlay).
     * @return The overlay scene the pipeline draws after the path's own result (never null).
     */
    vn::intrusive_ptr<vn::graphics::Scene> buildScene(bool deferred);

    /// Render control the demo builds into (owned by the shell's dock layout).
    gui::RenderControl* control_;

    /// Content scene root the cube-mapped box is hung on later (see installContent()); owned by the scene.
    vn::intrusive_ptr<vn::graphics::Group> content_root_;

    /// The overlay's sky slot: created by install() - empty, but FIRST in the overlay root, because the sky
    /// has to fill the background before the blended pair composites over it (see buildScene).
    vn::intrusive_ptr<vn::graphics::Group> sky_group_;
};

VN_APPFW_NS_END
