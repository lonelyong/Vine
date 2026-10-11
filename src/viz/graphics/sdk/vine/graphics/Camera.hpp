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
 * @brief Camera: the VIEWING (eye / target / up) and the PROJECTION PARAMETERS (fov, planes, ortho window).
 *
 * Camera does NOT hold a projection matrix, and it does not hold an aspect ratio: the projection of a picture
 * depends on how wide that picture is, and that is a fact about the RECTANGLE the picture is drawn into - the
 * window, a pane of a split surface, a shadow map - not about the camera. Asking for the matrix is therefore a
 * question with an argument (projectionMatrix(aspect)), and the argument is derived from the rectangle at render
 * time in ONE place - RenderBackend::viewAspect(), read both by the view block that builds the projection and by
 * the culling that decides what is worth projecting. The same camera may then be drawn into rectangles of
 * different shapes in the same frame: a mirror view, a picture-in-picture of the same eye, an editor's two panes
 * of one scene. A camera that held a size would have to be copied (or nudged) per picture - which is what Unity's
 * Camera.rect forces and what this class exists to avoid.
 *
 * The projection PARAMETERS are all state here: fov / near / far and the orthographic WINDOW (kept whole - see
 * setProjectionMatrixAsOrtho). The perspective projection is rebuilt per call (a handful of multiplies, see
 * projectionMatrix); an orthographic one ignores the aspect argument, because its window already carries a shape.
 *
 * WHAT projectionMatrix() IS FOR. The matrix this class builds is the CPU-side one: a right-handed view looking
 * down -Z, and a CLIP SPACE WITH z IN [-1, 1] (the classic OpenGL range). A backend may render with a different
 * convention — the vsg backend renders Vulkan-style reverse-Z (near maps to 1, far to 0, the Y axis flipped) —
 * and it is expected to: which clip space a GPU wants is a property of the API, not of the camera. So this matrix
 * is the right thing for the engine's own uses (frustum culling, view-dependent size, CPU-side math) and for a
 * consumer that knows the backend it is talking to, but it does NOT predict the picture's clip space.
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

    /** @brief Sets the projection parameters of a perspective camera.
     *
     * NO aspect: how wide the picture is belongs to the rectangle it is drawn into, not to the camera (see the
     * class note). Supply it where the projection is needed - projectionMatrix(aspect), or the pass' rectangle a
     * backend derives it from.
     *
     * @param fovy    Vertical field of view in degrees.
     * @param zNear   Near clipping plane distance (> 0).
     * @param zFar    Far clipping plane distance.
     */
    void setProjectionMatrixAsPerspective(double fovy, double zNear, double zFar);

    /** @brief Sets the projection parameters of an orthographic camera.
     *
     * NO aspect: an orthographic window is a rectangle in view space, not a frustum fitted to a picture, so its
     * shape is already stated by the window itself (projectionMatrix ignores the aspect it is handed).
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

    /** @brief Gets the projection matrix of a picture of @p aspect.
     *
     * The engine's CPU-side projection (clip space with z in [-1, 1]); a backend may render with another
     * convention, so this is the matrix the ENGINE culls and measures with, not a prediction of the picture's
     * clip space (see the class note).
     *
     * Rebuilt per call from the parameters above - there is no cache to invalidate and no state to keep in step
     * with a rectangle, which is what makes one camera usable for several pictures of different shapes.
     *
     * @param aspect Width / height of the picture (ignored by an orthographic camera, whose window carries its
     *               own shape - see setProjectionMatrixAsOrtho).
     * @return The view -> clip matrix.
     */
    Mat4d projectionMatrix(double aspect) const;

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
     * @param aspect    Width / height of the picture the coordinates are in - the same rectangle the
     *                  projection uses (see the class note): picking with another one would aim at what
     *                  a differently shaped picture would have shown.
     * @return Ray in world space.
     */
    Ray screenToWorldRay(const Vec2d& screenPos, double aspect) const;

  private:
    String         name_;
    Mat4d          view_{ Mat4d() };
    ProjectionType projection_type_ = ProjectionType::Perspective;
    // Copy of the last look-at parameters, used for ray generation.
    Vec3d eye_{ 0.0, 0.0, 5.0 };
    Vec3d center_{ 0.0, 0.0, 0.0 };
    Vec3d up_{ 0.0, 1.0, 0.0 };
    // The projection PARAMETERS (no matrix and no aspect: both are per-picture, see the class note).
    double near_plane_   = 0.1;
    double far_plane_    = 1000.0;
    double fov_          = 45.0;
    // The orthographic window, kept WHOLE: a backend that rebuilds the projection from parameters (and
    // the picking ray) needs all four bounds, and the height alone cannot express an off-centre one.
    double ortho_left_   = -5.0;
    double ortho_right_  = 5.0;
    double ortho_bottom_ = -5.0;
    double ortho_top_    = 5.0;
};

using CameraPtr = intrusive_ptr<Camera>;

VN_GRAPHICS_NS_END
