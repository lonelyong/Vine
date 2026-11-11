#pragma once
#include "graphics_global.hpp"

#include <vine/math/Matrix4x4.hpp>
#include <vine/math/Vector2.hpp>
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

    /**
     * @brief Answers the world-space ray THIS PICTURE sees through a point of itself.
     *
     * @details
     * - The value's own question, and the one a pointer needs: "what is under this pixel of the picture I am looking
     *   at". It is asked of the SNAPSHOT rather than of the live camera because those are different pictures when a
     *   frame is answered somewhere else: the camera may have moved since, and a pick that used the new pose would miss
     *   what the user pointed at (design graphics-threading.md N17 - the one place where "the camera is ahead of the
     *   picture" is a semantic error, not a lag).
     * - Both ends of the pixel are unprojected through this snapshot's own matrices, so it answers the same way for a
     *   perspective and an orthographic camera and for an off-centre window: the projection carries which clip depth is
     *   NEAR (this tree renders Reverse-Z), and the ray is read as the segment between the two, oriented away from the
     *   eye. A perspective's ray starts AT the eye; an orthographic one has no eye on it, so it starts where the pixel
     *   crosses the near plane.
     *
     * @param screen01  The point in NORMALISED picture coordinates - 0..1 across it, x right and y DOWN, which is the
     *                  convention a window pointer arrives in (not OpenGL's y-up clip space).
     * @param origin    Receives the ray's origin in world space.
     * @param direction Receives the ray's direction in world space, normalised.
     * @return false when this snapshot holds no camera (see @ref present), or when its projection collapses the
     *         picture to a point (a degenerate shape: there is no ray to give).
     */
    [[nodiscard]] bool screenRay(const vn::math::Vec2d& screen01, Vec3d& origin, Vec3d& direction) const;
};

VN_GRAPHICS_NS_END
