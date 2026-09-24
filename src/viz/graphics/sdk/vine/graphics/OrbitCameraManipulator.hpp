#pragma once

#include <vine/raw_ptr.hpp>

#include "CameraManipulator.hpp"

VN_GRAPHICS_NS_BEGIN

class Scene;

/**
 * @brief Orbit-style camera manipulator with pivot picking and projection
 * agnostic controls.
 *
 * Implements a pivot-anchored orbit / pan / zoom camera with a pickable anchor
 * point:
 *
 * - Left-drag rotates the whole camera rig rigidly about the press anchor (the
 *   grabbed model point, or the scene centre on empty space), so that point
 *   stays pinned under the cursor while the model turns about it. The camera
 *   up is carried rigidly by the rotation, which gives the view the roll
 *   freedom it needs to sweep over the top to a true top-down / bottom-up view
 *   and beyond, without gimbal shake or a sudden 180 deg flip.
 * - Middle/right-drag pans.
 * - The wheel dollies along the view: it dives along the ray to the anchor
 *   (the point under the cursor stays pinned to it) and, once the camera
 *   reaches the surface, keeps moving forward so it can travel through the
 *   scene instead of stopping at a fixed point.
 *
 * On every button press / scroll the manipulator ray-picks the scene under the
 * cursor. When a model is hit, the hit point becomes the anchor (the rotate
 * pivot and the pan / zoom depth reference); when nothing is hit the anchor
 * falls back to the scene bounding-box centre, or to the world origin when
 * there is no scene. In FirstPerson mode WASD / arrow keys drive movement
 * instead.
 *
 * A rotate press never applies the pivot (no jump). Away from a drag, pan /
 * zoom and the keyboard orbit are expressed as a level spherical orbit
 * (yaw / pitch / roll about the camera target) so programmatic motion stays
 * stable and level.
 *
 * Both projection types are supported through a single distance parameter: the
 * camera keeps an eye-to-centre distance, which drives the perspective dolly
 * step and the orthographic frustum height.
 */
class VN_GRAPHICS_API OrbitCameraManipulator : public CameraManipulator {
  public:
    /** @brief Constructs an orbit manipulator bound to a camera.
     *
     * The orbit centre and distance are initialized from the camera's current
     * view (centre = camera target, distance = distance from eye to target).
     *
     * @param camera Camera to manipulate. Must outlive the manipulator.
     * @param scene  Optional scene used for ray picking, anchor fallback and
     *               fitToScreen(). May be null (no picking / scene centre).
     */
    explicit OrbitCameraManipulator(raw_ptr<Camera> camera, raw_ptr<Scene> scene = nullptr);

    /** @brief Destroys the manipulator. */
    ~OrbitCameraManipulator() override;

  public:
    // ---- Orbit / pan / zoom ----

    /** @brief Orbits the camera around the current centre (yaw/pitch).
     *
     * @param deltaYaw   Yaw angle change in radians.
     * @param deltaPitch Pitch angle change in radians.
     */
    void orbit(double deltaYaw, double deltaPitch);

    /** @brief Sets the orbit centre (target point). */
    void setOrbitCenter(const Vec3d& center);

    /** @brief Gets the orbit centre (target point). */
    Vec3d orbitCenter() const;

    /** @brief Gets the orbit radius (eye-to-centre distance). */
    double orbitRadius() const;

    /** @brief Sets the orbit radius (eye-to-centre distance).
     *
     * @param radius Distance from eye to orbit centre.
     */
    void setOrbitRadius(double radius);

    /** @brief Pans the camera based on screen-space deltas.
     *
     * The pan is scaled at the current anchor depth, so the grabbed world
     * point stays under the cursor.
     *
     * @param screenDx Horizontal screen delta in pixels.
     * @param screenDy Vertical screen delta in pixels.
     */
    void pan(double screenDx, double screenDy);

    /** @brief Zooms by a multiplicative factor.
     *
     * Keeps the current anchor fixed on screen when it lies on the cursor ray
     * (see zoomToCursor()); otherwise scales the eye-to-centre distance.
     * The distance is multiplied by the factor, so a factor below 1 moves the
     * camera closer (zoom in) and a factor above 1 moves it away (zoom out).
     *
     * @param factor Zoom factor; <1 zooms in, >1 zooms out.
     */
    void zoom(double factor);

    /** @brief Moves the camera forward/backward in first-person mode.
     *
     * @param distance Positive moves forward.
     */
    void moveForward(double distance);

    /** @brief Moves the camera right/left in first-person mode.
     *
     * @param distance Positive moves right.
     */
    void moveRight(double distance);

    /** @brief Moves the camera up/down in first-person mode.
     *
     * @param distance Positive moves up.
     */
    void moveUp(double distance);

    /** @brief Frames the whole scene into the view.
     *
     * Positions the orbit centre at the scene bounding-box centre and picks an
     * eye-to-centre distance that fits the scene into the vertical/horizontal
     * field of view (perspective) or the frustum height (orthographic).
     *
     * @return true when a scene with valid bounds was fitted.
     */
    bool fitToScreen() override;

    /** @brief Restores the view captured when the manipulator was constructed. */
    void home() override;

    // ---- Properties ----

    /** @brief Sets whether wheel zoom keeps the cursor anchor fixed on screen.
     *
     * When enabled, zooming towards a picked point keeps that world point under
     * the cursor; the orbit centre slides towards the anchor so successive
     * rotations orbit around the zoomed region.
     *
     * @param enabled True to zoom towards the cursor anchor.
     */
    void setZoomToCursor(bool enabled);

    /** @brief Returns whether wheel zoom anchors to the cursor. */
    bool zoomToCursor() const;

    /** @brief Sets the minimum eye-to-centre distance.
     *
     * Distance 0 (the default) derives the minimum from the camera near plane.
     *
     * @param distance Minimum distance; <= 0 means auto.
     */
    void setMinDistance(double distance);

    /** @brief Gets the configured minimum distance (0 = auto). */
    double minDistance() const;

    /** @brief Sets the scene used for picking / anchor / fitToScreen.
     *
     * @param scene Scene, or null to disable scene features.
     */
    void setScene(raw_ptr<Scene> scene);

    /** @brief Gets the bound scene (may be null). */
    raw_ptr<Scene> scene() const;

    /** @brief Ray-picks the scene under a screen point.
     *
     * @param screenX  Screen x in device pixels.
     * @param screenY  Screen y in device pixels.
     * @param outPoint Receives the world-space hit point on success.
     * @return true when a model was hit.
     */
    bool pickAt(double screenX, double screenY, Vec3d& outPoint) const;

    /** @brief Sets the orbit centre to the point under the cursor.
     *
     * Ray-picks the scene; on a hit the centre snaps to the hit point, on a
     * miss it snaps to the scene bounding-box centre (or the world origin when
     * there is no scene). The eye is left in place, so the view re-aims rather
     * than teleports.
     *
     * @param screenX Screen x in device pixels.
     * @param screenY Screen y in device pixels.
     * @return true when a model was hit.
     */
    bool setCenterFromScreen(double screenX, double screenY);

    /** @brief Commits pending changes to the bound camera. */
    void apply() override;

  public:
    // ---- Window input callbacks ----

    /** @brief Handles a mouse button press (see CameraManipulator). */
    void onMousePress(const vn::window::MouseEvent& event) override;

    /** @brief Handles mouse motion, applying the active drag interaction. */
    void onMouseMove(const vn::window::MouseEvent& event) override;

    /** @brief Handles a mouse button release, ending any drag interaction. */
    void onMouseRelease(const vn::window::MouseEvent& event) override;

    /** @brief Handles mouse wheel scroll, zooming the camera. */
    void onScroll(const vn::window::ScrollEvent& event) override;

    /** @brief Handles a key press (first-person movement / Home = fit). */
    void onKeyDown(const vn::window::KeyEvent& event) override;

    /** @brief Handles a key release. */
    void onKeyUp(const vn::window::KeyEvent& event) override;

    /** @brief Updates viewport size and the camera projection on a resize. */
    void onResize(const vn::window::ResizeEvent& event) override;

  private:
    /** @brief Ray from the camera through a screen pixel. */
    void makeScreenRay(double screenX, double screenY, Vec3d& origin, Vec3d& direction) const;

    /** @brief Computes the anchor (pick point / scene centre / origin). */
    Vec3d resolveAnchor(double screenX, double screenY, bool& onRay) const;

    /** @brief Re-aims the view at a point without moving the eye. */
    void aimCenterAt(const Vec3d& point);

    /** @brief Rotates the whole camera rig about the press anchor.
     *
     * Yaws about the world-up axis through the pivot, then pitches about the
     * resulting screen-right axis, carrying the camera up rigidly so the view
     * may roll and sweep over the top (true 90 deg elevation and beyond). The
     * rigid rotation keeps the pivot's screen position exactly fixed.
     *
     * @param screenDx Horizontal screen delta in pixels.
     * @param screenDy Vertical screen delta in pixels.
     */
    void pivotRotate(double screenDx, double screenDy);

    /** @brief Derives centre/distance/yaw/pitch/roll from the camera state. */
    void syncFromCamera();

    /** @brief Keeps the camera eye outside the scene bounds (orthographic). */
    void clampEyeOutsideBounds();

    /** @brief Clamps distance to the allowed minimum. */
    double clampDistance(double distance) const;

    /** @brief Updates the orthographic projection from distance and aspect. */
    void updateOrthoProjection();

  private:
    // ---- State ----

    raw_ptr<Scene> scene_ = nullptr;

    Vec3d center_{ 0.0, 0.0, 0.0 };
    double distance_ = 10.0;
    double yaw_ = 0.0;
    double pitch_ = 0.0;
    double roll_ = 0.0;  // signed camera roll about its forward axis (-pi, pi].

    // Snapshot for home().
    Vec3d home_center_{ 0.0, 0.0, 0.0 };
    double home_distance_ = 10.0;
    double home_yaw_ = 0.0;
    double home_pitch_ = 0.0;
    double home_roll_ = 0.0;

    bool zoom_to_cursor_ = true;
    double min_distance_ = 0.0;  // 0 => derive from the near plane.

    // Current interaction anchor (pick point, scene centre or origin): the
    // rotate pivot and the pan/zoom depth reference / zoom-to-cursor anchor.
    Vec3d anchor_{ 0.0, 0.0, 0.0 };
    bool anchor_on_ray_ = false;  // anchor was a geometry hit on the cursor ray.

    CameraManipulator::DragAction drag_ = CameraManipulator::DragAction::None;
    double last_x_ = 0.0;
    double last_y_ = 0.0;
    bool pointer_known_ = false;

    // Viewport size in device pixels (fed by onResize).
    int viewport_w_ = 800;
    int viewport_h_ = 600;

    // Sensitivity of drag-based interactions (radians / pixels).
    double drag_sensitivity_ = 0.01;
    // Zoom factor per wheel notch.
    double wheel_factor_ = 1.15;
    // First-person movement step per key event (world units).
    double move_step_ = 0.5;
    // First-person yaw/pitch rotation per key event (radians).
    double rotate_step_ = 0.05;
};

VN_GRAPHICS_NS_END
