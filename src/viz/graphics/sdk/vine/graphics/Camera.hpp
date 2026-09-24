#pragma once
#include "graphics_global.hpp"

#include <vine/Object.hpp>
#include <vine/RefCounted.hpp>
#include <vine/String.hpp>
#include <vine/intrusive_ptr.hpp>
#include <vine/math/Matrix4x4.hpp>
#include <vine/math/Vector2.hpp>
#include <vine/math/Vector3.hpp>

#include "Ray.hpp"

VN_GRAPHICS_NS_BEGIN

using vn::math::Mat4d;
using vn::math::Vec2d;
using vn::math::Vec3d;

/**
 * @brief Camera class managing view and projection matrices.
 *
 * Camera stores complete view and projection matrices. The view matrix is set
 * at once with setViewMatrixAsLookAt(); the projection matrix is set at once
 * with setProjectionMatrixAsPerspective() or setProjectionMatrixAsOrtho().
 * This avoids intermediate inconsistent states and mirrors the OSG camera API.
 *
 * WHAT projectionMatrix() IS FOR. The matrices this class builds are the CPU-side
 * ones: a right-handed view looking down -Z, and a CLIP SPACE WITH z IN [-1, 1]
 * (the classic OpenGL range). A backend may render with a different convention —
 * the vsg backend renders Vulkan-style reverse-Z (near maps to 1, far to 0, the Y
 * axis flipped) — and it is expected to: which clip space a GPU wants is a
 * property of the API, not of the camera. So this matrix is the right thing for
 * the engine's own uses (frustum culling, view-dependent size, CPU-side math) and
 * for a consumer that knows the backend it is talking to, but it does NOT predict
 * the picture's clip space.
 *
 * The orthographic WINDOW is kept whole (left/right/bottom/top), not just its
 * height: a backend that rebuilds the projection from these parameters has to use
 * all four or it renders a different frustum than the engine culled with (see
 * orthographicLeft()).
 */
class VN_GRAPHICS_API Camera : public Object, public RefCounted<Camera> {
    VN_OBJECT_META_DECL;

  public:
    enum class ProjectionType
    {
        Perspective,  ///< Perspective projection.
        Orthographic, ///< Orthographic projection.
    };

  public:
    Camera();
    ~Camera();

  public:
    /** @brief Gets the camera name. */
    String name() const;

    /** @brief Sets the camera name. */
    void setName(const String& name);

    /** @brief Gets the current projection type. */
    ProjectionType projectionType() const;

    /** @brief Sets the view matrix from a look-at specification.
     *
     * @param eye    Camera eye position (world space).
     * @param center Point the camera looks at (world space).
     * @param up     Up vector (does not need to be orthogonal).
     */
    void setViewMatrixAsLookAt(const Vec3d& eye, const Vec3d& center, const Vec3d& up);

    /** @brief Sets the projection matrix from perspective parameters.
     *
     * @param fovy    Vertical field of view in degrees.
     * @param aspect  Aspect ratio (width / height).
     * @param zNear   Near clipping plane distance (> 0).
     * @param zFar    Far clipping plane distance.
     */
    void setProjectionMatrixAsPerspective(double fovy, double aspect, double zNear, double zFar);

    /** @brief Sets the projection matrix from orthographic parameters.
     *
     * All four window bounds are kept (they are what the projection uses and what a backend that
     * rebuilds it from parameters must take, see the class note), so an off-centre window — a tiled
     * view, one eye of a stereo pair, a shifted shadow frustum — survives the round trip instead of
     * being silently centred.
     *
     * @param left   Left clipping plane.
     * @param right  Right clipping plane.
     * @param bottom Bottom clipping plane.
     * @param top    Top clipping plane.
     * @param zNear  Near clipping plane distance.
     * @param zFar   Far clipping plane distance.
     */
    void setProjectionMatrixAsOrtho(double left, double right, double bottom, double top, double zNear, double zFar);

    /** @brief Gets the camera eye position (last set via setViewMatrixAsLookAt). */
    Vec3d eye() const;

    /** @brief Gets the camera target/center point (last set via setViewMatrixAsLookAt). */
    Vec3d target() const;

    /** @brief Gets the up vector (last set via setViewMatrixAsLookAt). */
    Vec3d up() const;

    /** @brief Gets the near clipping plane distance. */
    double nearPlane() const;

    /** @brief Gets the far clipping plane distance. */
    double farPlane() const;

    /** @brief Gets the field of view in degrees (perspective only). */
    double fieldOfView() const;

    /** @brief Gets the aspect ratio (width / height). */
    double aspectRatio() const;

    /** @brief Gets the orthographic view height (top - bottom). */
    double orthographicHeight() const;

    /** @brief Gets the orthographic window's left bound.
     *
     * The window is the whole orthographic frustum in view space; together with the three accessors
     * below it is what a backend must use to build the matching projection (see the class note). Value
     * of the last setProjectionMatrixAsOrtho() call.
     *
     * @return Left bound (view space).
     */
    double orthographicLeft() const;

    /** @brief Gets the orthographic window's right bound.
     *
     * @return Right bound (view space).
     */
    double orthographicRight() const;

    /** @brief Gets the orthographic window's bottom bound.
     *
     * @return Bottom bound (view space).
     */
    double orthographicBottom() const;

    /** @brief Gets the orthographic window's top bound.
     *
     * @return Top bound (view space).
     */
    double orthographicTop() const;

    /** @brief Gets the view matrix. */
    Mat4d viewMatrix() const;

    /** @brief Gets the projection matrix.
     *
     * The engine's CPU-side projection (clip space with z in [-1, 1]); a backend may render with
     * another convention, so this is the matrix the ENGINE culls and measures with, not a prediction
     * of the picture's clip space (see the class note).
     *
     * @return The view -> clip matrix last set through one of the setters.
     */
    Mat4d projectionMatrix() const;

    /** @brief Converts screen coordinates to a world-space picking ray.
     *
     * The ray is built from the VIEW MATRIX's own basis, not from a second look-at computation, so a
     * ray always aims where the rendered picture is — including when the given up vector was parallel
     * to the view direction, the degenerate case lookAt() resolves internally and a hand-rolled cross
     * product cannot.
     *
     * For an orthographic camera the origin travels across the projection WINDOW (all four bounds, see
     * setProjectionMatrixAsOrtho) and the direction is the view direction.
     *
     * @param screenPos Normalized screen coordinates in [0, 1]
     *                  (pixel / viewport dimension).
     * @return Ray in world space.
     */
    Ray screenToWorldRay(const Vec2d& screenPos) const;

  private:
    String         name_;
    Mat4d          view_{ Mat4d() };
    Mat4d          projection_{ Mat4d() };
    ProjectionType projection_type_ = ProjectionType::Perspective;
    // Copy of the last look-at parameters, used for ray generation.
    Vec3d eye_{ 0.0, 0.0, 5.0 };
    Vec3d center_{ 0.0, 0.0, 0.0 };
    Vec3d up_{ 0.0, 1.0, 0.0 };
    // Copy of the last projection parameters, used for queries.
    double near_plane_   = 0.1;
    double far_plane_    = 1000.0;
    double fov_          = 45.0;
    double aspect_ratio_ = 1.0;
    // The orthographic window, kept WHOLE: a backend that rebuilds the projection from parameters (and
    // the picking ray) needs all four bounds, and the height alone cannot express an off-centre one.
    double ortho_left_   = -5.0;
    double ortho_right_  = 5.0;
    double ortho_bottom_ = -5.0;
    double ortho_top_    = 5.0;
};

using CameraPtr = intrusive_ptr<Camera>;

VN_GRAPHICS_NS_END
