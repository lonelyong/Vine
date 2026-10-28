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
 * @brief Camera: the VIEWING (eye / target / up), the PROJECTION PARAMETERS (fov, planes, ortho window) and the
 *        SHAPE of the picture it is projected for (aspect).
 *
 * @details
 * - ONE CAMERA, ONE PICTURE SHAPE: the aspect lives here because the projection is what needs it, and every
 *   reader of the projection then reads the SAME number from the same place - the view block that builds
 *   it, the culling that decides what is worth projecting, and the picking ray. A host that wants the same
 *   eye at two shapes uses two cameras.
 * - A camera whose shape differs from the picture a pass draws it into projects a STRETCHED picture; the
 *   engine REPORTS that while compiling the frame (see RenderEngine's frame-plan problems) instead of
 *   letting the picture be quietly wrong. What this replaced derived the aspect from the pass' rectangle at
 *   render time - a shape only the BACKEND could answer, depending on what had been announced before it
 *   (see `.ai/design/graphics-architecture.md`).
 * - The projection PARAMETERS are all state here: fov / near / far, the aspect, and the orthographic
 *   WINDOW (kept whole - see setOrthographic). The perspective projection is rebuilt per call;
 *   an orthographic one ignores the aspect, because its window already carries a shape.
 *
 * @note
 * - projectionMatrix() is the CPU-side matrix: a right-handed view looking down -Z with clip space z in
 *   [-1, 1] (the classic OpenGL range). A backend may render with another convention - the vsg backend
 *   renders Vulkan-style reverse-Z (near maps to 1, far to 0, Y flipped) - and that is expected: which clip
 *   space a GPU wants is a property of the API, not of the camera. So this matrix serves the engine's own
 *   uses (frustum culling, view-dependent size, CPU-side math) and does NOT predict the picture's clip
 *   space.
 */
class VN_GRAPHICS_API Camera : public Object, public RefCounted<Camera> {
    VN_OBJECT_META_DECL;

  public:
    enum class ProjectionType
    {
        Perspective,  // Perspective projection.
        Orthographic, // Orthographic projection.
    };

  public:
    /** @brief Constructs a camera with a default view and a 1.0 aspect. */
    Camera();
    ~Camera();

  public:
    /** @brief Gets the camera name.
     *
     * @return The name (empty when never set).
     */
    String name() const;

    /** @brief Sets the camera name.
     *
     * @param name New camera name.
     */
    void setName(const String& name);

    /** @brief Gets the current projection type.
     *
     * @return Perspective or Orthographic.
     */
    ProjectionType projectionType() const;

    /** @brief Points the camera at @p center from @p eye, keeping @p up as its up direction.
     *
     * @details
     * - This states the VIEW, not a matrix: eye / target / up are kept, so a later lookAt or a read back of them
     *   answers the same three values, and the view matrix is rebuilt from them on demand (see viewMatrix).
     *
     * @param eye    Camera eye position (world space).
     * @param center Point the camera looks at (world space).
     * @param up     Up vector (does not need to be orthogonal).
     */
    void lookAt(const Vec3d& eye, const Vec3d& center, const Vec3d& up);

    /** @brief Makes this a perspective camera with the given frustum parameters.
     *
     * @details
     * - The picture's SHAPE is not among them: it is the camera's own state (see setAspect), because it is
     *   what the projection is built with and the same number must be read by everything that projects or
     *   culls.
     *
     * @param fovy    Vertical field of view in degrees.
     * @param zNear   Near clipping plane distance (> 0).
     * @param zFar    Far clipping plane distance.
     */
    void setPerspective(double fovy, double zNear, double zFar);

    /** @brief Gets the shape of the picture this camera is projected for (width / height).
     *
     * @return The aspect ratio; 1.0 until a host states one (see setAspect).
     */
    double aspect() const;

    /** @brief States the shape of the picture this camera is projected for.
     *
     * @details
     * - The ONE number the projection, the culling and the picking ray are built from: setting it is how a
     *   view says "this is the rectangle I am being drawn into" (SceneView does it from its own rectangle on
     *   every geometry change). Read it back rather than keeping a second copy: a camera whose aspect
     *   disagrees with the picture it is drawn into projects a stretched picture, and the engine reports
     *   that while compiling the frame.
     *
     * @param aspect Width / height of the picture; a non-positive value is ignored (1.0 is the state a
     *               camera is born with, which is right for the square pictures a headless test draws).
     */
    void setAspect(double aspect) noexcept;

    /** @brief Makes this an orthographic camera with the given window.
     *
     * @details
     * - NO aspect: an orthographic window is a rectangle in view space, not a frustum fitted to a picture, so
     *   its shape is already stated by the window itself (projectionMatrix ignores the aspect it is handed).
     * - All four window bounds are kept (what the projection uses and what a backend that rebuilds it from
     *   parameters must take, see the class note), so an off-centre window - a tiled view, one eye of a
     *   stereo pair, a shifted shadow frustum - survives the round trip instead of being silently centred.
     *
     * @param left   Left clipping plane.
     * @param right  Right clipping plane.
     * @param bottom Bottom clipping plane.
     * @param top    Top clipping plane.
     * @param zNear  Near clipping plane distance.
     * @param zFar   Far clipping plane distance.
     */
    void setOrthographic(double left, double right, double bottom, double top, double zNear, double zFar);

    /** @brief Gets the camera eye position (last set via lookAt).
     *
     * @return The eye point (world space).
     */
    Vec3d eye() const;

    /** @brief Gets the camera target/center point (last set via lookAt).
     *
     * @return The target point (world space).
     */
    Vec3d target() const;

    /** @brief Gets the up vector (last set via lookAt).
     *
     * @return The up vector.
     */
    Vec3d up() const;

    /** @brief Gets the near clipping plane distance.
     *
     * @return The near plane distance.
     */
    double nearPlane() const;

    /** @brief Gets the far clipping plane distance.
     *
     * @return The far plane distance.
     */
    double farPlane() const;

    /** @brief Gets the field of view in degrees (perspective only).
     *
     * @return The vertical field of view.
     */
    double fieldOfView() const;

    /** @brief Gets the orthographic view height (top - bottom).
     *
     * @return The window height in view space.
     */
    double orthographicHeight() const;

    /** @brief Gets the orthographic window's left bound.
     *
     * The window is the whole orthographic frustum in view space; together with the three accessors
     * below it is what a backend must use to build the matching projection (see the class note). Value
     * of the last setOrthographic() call.
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

    /** @brief Gets the projection matrix for the shape this camera states (see setAspect).
     *
     * The engine's CPU-side projection (clip space with z in [-1, 1]); a backend may render with another
     * convention, so this is the matrix the ENGINE culls and measures with, not a prediction of the picture's
     * clip space (see the class note).
     *
     * Rebuilt per call from the parameters above - there is no cache to invalidate. An orthographic camera
     * ignores the aspect, whose window carries its own shape (see setOrthographic).
     *
     * @return The view -> clip matrix.
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
     * setOrthographic) and the direction is the view direction.
     *
     * @param screenPos Normalized screen coordinates in [0, 1]
     *                  (pixel / viewport dimension).
     * @return Ray in world space.
     */
    Ray screenToWorldRay(const Vec2d& screenPos) const;

  private:
    String         name_;
    Mat4d          view_{ Mat4d() };
    ProjectionType projection_type_ = ProjectionType::Perspective;
    // Copy of the last look-at parameters, used for ray generation.
    Vec3d eye_{ 0.0, 0.0, 5.0 };
    Vec3d center_{ 0.0, 0.0, 0.0 };
    Vec3d up_{ 0.0, 1.0, 0.0 };
    // The projection PARAMETERS (no matrix: it is rebuilt per call). The aspect is here because the projection
    // cannot be built without it and every reader must read the same number (see the class note).
    double near_plane_   = 0.1;
    double far_plane_    = 1000.0;
    double fov_          = 45.0;
    double aspect_       = 1.0;
    // The orthographic window, kept WHOLE: a backend that rebuilds the projection from parameters (and
    // the picking ray) needs all four bounds, and the height alone cannot express an off-centre one.
    double ortho_left_   = -5.0;
    double ortho_right_  = 5.0;
    double ortho_bottom_ = -5.0;
    double ortho_top_    = 5.0;
};

using CameraPtr = intrusive_ptr<Camera>;

VN_GRAPHICS_NS_END
