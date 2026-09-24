#pragma once
#include "graphics_global.hpp"

#include <vine/raw_ptr.hpp>

VN_GRAPHICS_NS_BEGIN

class Camera;

/** @brief How a camera follows another camera's view. */
enum class MirrorMode {
    /// Camera is fully independent (e.g. a minimap or a 2D screen HUD).
    None = 0,
    /// Copy only the source orientation; keep the target's own framing.
    Orientation,
    /// Adopt the source view (eye / target / up) completely.
    FullView,
};

/** @brief Applies a mirror mode onto a target camera from a source camera.
 *
 * Mirroring is the independent camera-follow component behind HUD content
 * such as an axis gizmo: a small view keeps facing the same way as the
 * interactive main camera while it rotates. It operates on plain cameras only
 * and knows nothing about passes or the render engine, so it can run anywhere
 * a frame is driven (e.g. inside a pass' execute()).
 *
 * Orientation keeps the target camera's own framing distance and rebuilds an
 * un-rolled up basis perpendicular to the source view direction; the target
 * content is expected to be centred on its local origin. FullView adopts the
 * source eye / target / up completely.
 *
 * @param dst  Target camera whose view matrix is rewritten.
 * @param src  Source camera to follow.
 * @param mode Mirror mode (None leaves @p dst untouched).
 */
VN_GRAPHICS_API void applyCameraMirror(raw_ptr<Camera> dst, raw_ptr<Camera> src, MirrorMode mode);

VN_GRAPHICS_NS_END
