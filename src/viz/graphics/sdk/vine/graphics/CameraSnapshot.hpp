#pragma once
#include "graphics_global.hpp"

#include <vine/math/Matrix4x4.hpp>
#include <vine/math/Vector3.hpp>

VN_GRAPHICS_NS_BEGIN

using vn::math::Mat4d;
using vn::math::Vec3d;

class Camera;

/**
 * @brief A camera AS A VALUE: the numbers a frame is built from, and no object to keep alive.
 *
 * @details
 * - WHY IT IS A VALUE. A frame names a view, and a view is data: the projection, the world-to-view matrix, the eye,
 *   the target and the up vector. Keeping the `Camera` object instead would make the frame's validity depend on the
 *   authoring object's lifetime and on the thread that owns it - and a frame that crosses to a recording side has
 *   exactly those two problems (see `.ai/design/graphics-threading.md`).
 * - WHAT IT IS FOR. The collection context (`CollectView`), the view block (`buildViewBlock`) and the CPU light
 *   transform all read these numbers; a pass takes the snapshot once, when it describes itself, so the culling, the
 *   projection and the shading of one frame cannot see a camera that moved in between.
 * - `present` is a FACT, not an optional: "this drawing call named no camera" is a real answer (the call issues
 *   nothing), and it must not be confused with default-constructed matrices that happen to be zero.
 *
 * @note Carries no shape policy of its own: the projection `Camera::projectionMatrix()` already applied the
 *       camera's aspect, so a consumer cannot derive a second, disagreeing projection.
 */
struct VN_GRAPHICS_API CameraSnapshot {
    bool  present{ false }; ///< Whether a camera was announced at all.
    Mat4d view{};           ///< World -> view matrix.
    Mat4d projection{};     ///< Clip-space projection (see Camera).
    Vec3d eye{};            ///< Eye position (world space).
    Vec3d target{};         ///< Look-at target (world space).
    Vec3d up{};             ///< Up vector (world space).

    /**
     * @brief Takes the view @p camera states right now.
     *
     * @param camera Camera to read; the numbers are copied here, so a later edit of it is not seen by this value.
     * @return The snapshot, with @ref present set.
     */
    [[nodiscard]] static CameraSnapshot of(const Camera& camera);
};

VN_GRAPHICS_NS_END
