#pragma once
#include "graphics_global.hpp"

V_GRAPHICS_NS_BEGIN

/**
 * @brief Per-frame shared context (skeleton).
 *
 * Carries values that many passes / overlays need during one frame, updated
 * by RenderEngine each frame: elapsed time and the current surface size.
 * Later extensions: previous-frame view-projection matrices (for temporal
 * effects such as motion vectors / TAA) and the active light list.
 */
struct V_GRAPHICS_API FrameContext {
    double dt = 0.0;  ///< Seconds elapsed since the previous frame.
    /// Surface width as the HOST announced it (the Qt host announces logical pixels - what its widget
    /// reports - so a consumer that needs device pixels scales by that host's ratio; see Pipeline::resize
    /// and SceneView::addSurfaceLayout). 0 until the first resize.
    int surface_width = 0;
    /// Surface height as the host announced it (see surface_width).
    int surface_height = 0;
};

V_GRAPHICS_NS_END
