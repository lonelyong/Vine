#pragma once
#include "graphics_global.hpp"

#include "FrameContext.hpp"

VN_GRAPHICS_NS_BEGIN

/**
 * @brief An object that does its own per-frame work before a frame describes it.
 *
 * @details
 * - THE OBJECT OWNS ITS UPDATE. What a HUD overlay repaints, or what a gizmo mirrors, is the object's behaviour,
 *   not a field of a pass declaration: a `std::function` inside `PassDesc` made "the plan is a value" true for only
 *   part of the value, and left the frame to run work the frame did not own (see E-05).
 * - WHO CALLS IT. Whoever drives the frame calls it first: `PassChain::update(context)` walks the chain's overlays in
 *   registration order, and `SceneView::update(context)` forwards to the chain it adopted. `SurfaceWindow::renderFrame`
 *   does both once per frame (update, then publish, then frame), so a host on the standard path gets the old
 *   behaviour without writing anything.
 * - WHAT IT IS GIVEN. One `FrameContext` per frame (see `FrameContext`), composed by the engine and handed to every
 *   object in that frame: the frame's id, what the previous frame cost, and the surface size in logical pixels with
 *   the ratio. It replaced "each object reads the clock itself" - one clock, and two overlays cannot disagree about
 *   which frame they are in (`BC-25`).
 * - WHAT IT MAY TOUCH. Content and its own state, never a backend: it runs on the thread that builds the frame's
 *   content half, before any pass is described, and a publication it posts is what that frame draws (see
 *   `Scene::publish`).
 *
 * @note
 * - A DECLARATION CANNOT CARRY ONE: this is an object interface, so an overlay is identified by identity (the
 *   content object a chain registered), not by a copyable value - which is what lets "which overlays update" be
 *   answered without copying a closure per pass per frame.
 * - A host that drives the engine from its own loop calls `SceneView::update(engine->frameContext())` (or the
 *   chain's) before `RenderEngine::frame()`; skipping it means the overlays simply do not update that frame (BC-08).
 */
class VN_GRAPHICS_API FrameUpdate {
  public:
    FrameUpdate()          = default;
    virtual ~FrameUpdate();

    FrameUpdate(const FrameUpdate&)            = delete;
    FrameUpdate& operator=(const FrameUpdate&) = delete;

    /**
     * @brief Does this object's per-frame work for @p context.
     *
     * @details
     * - Called once per frame, before the passes that draw the object are described, with the SAME context every other
     *   object of that frame gets. A disabled overlay's pass does not stop this call: visibility is the pass' flag,
     *   and the chain still updates the object it was told about (an overlay that is switched back on must show the
     *   current state, not a stale one).
     *
     * @param context This frame's context (see FrameContext); borrowed for the call.
     */
    virtual void onFrame(const FrameContext& context) = 0;
};

VN_GRAPHICS_NS_END
