#pragma once

#include <vine/raw_ptr.hpp>

#include "CameraManipulator.hpp"

VN_GRAPHICS_NS_BEGIN

class Scene;

/**
 * @brief Orbit-style camera manipulator with pivot picking and projection-agnostic controls.
 *
 * @details
 * - ONE STATE, ONE PICTURE. The manipulator owns the VIEWPOINT - an orbit centre, the eye-to-centre distance and the
 *   three angles that place the eye on that sphere - and the camera owns the PICTURE its projection makes of it. The
 *   two meet in `apply()`, which writes the camera's view (`lookAt`) and nothing else; in particular the ORTHOGRAPHIC
 *   WINDOW belongs to the camera, so an authored off-centre window survives every gesture and the eye-to-centre
 *   distance is never a picture scale (see `.ai/design/graphics-navigation.md` 2.1 / 2.2).
 * - Left-drag rotates the whole camera rig rigidly about the press anchor (the grabbed model point, or the scene
 *   centre on empty space), so that point stays pinned under the cursor while the model turns about it. The camera up
 *   is carried rigidly by the rotation, which gives the view the roll freedom it needs to sweep over the top to a true
 *   top-down / bottom-up view and beyond, without gimbal shake or a sudden 180 deg flip. In FirstPerson mode the same
 *   drag looks ABOUT THE EYE instead and leaves the eye where it is.
 * - Middle/right-drag pans, scaled by the picture's OWN extent: the anchor depth for a perspective picture, the
 *   window's height for a parallel one, so a pixel is a pixel in both projections.
 * - The wheel is a ZOOM and it is exactly `dolly()`'s operation: the picture's scale changes by one factor. Perspective
 *   multiplies the eye-to-centre distance (so the step is proportional to the current scale); orthographic scales the
 *   camera's window, because a parallel picture does not depend on where the eye sits along the view axis. With
 *   `setDollyToCursor(true)` (the default) the picked point under the cursor keeps its picture position: the orbit
 *   centre slides for a perspective picture, the window slides for a parallel one.
 * - TRAVEL IS A VERB OF ITS OWN: `moveForward` / `moveRight` / `moveUp` (WASD, and Q/E for up/down, in FirstPerson) move
 *   the rig without changing the picture's scale. The wheel does not fly through the scene - it stops at the surface it
 *   is aiming at, which is what lets the next rotation orbit the region just approached.
 * - A LOOK IS THE OTHER VERB, AND IT IS ONE VERB: `look` turns the view about the EYE and leaves the eye where it is.
 *   The left-drag in FirstPerson and the arrow keys both ask for it and neither implements it - a device only converts
 *   its own units into a `look()` step, so two devices in one mode cannot disagree about what looking means.
 * - On every button press / scroll the manipulator ray-picks the scene under the POINTER THE EVENT NAMES. A model hit
 *   becomes the anchor (the rotate pivot, the pan depth reference and the zoom target); nothing hit falls back to the
 *   scene bounding-box centre, or to the world origin when there is no scene.
 * - A rotate press never applies the pivot (no jump). Away from a drag, `orbit()` and pan / zoom are expressed as a
 *   level spherical orbit (yaw / pitch / roll about the orbit centre) so programmatic motion stays stable and level.
 *   The world is right-handed and Z-up (`kWorldUp`), and the manipulator's horizon is that axis; the world unit is the
 *   content's.
 */
class VN_GRAPHICS_API OrbitCameraManipulator : public CameraManipulator {
  public:
    /** @brief Constructs an orbit manipulator bound to a camera.
     *
     * @details
     * - The orbit centre and distance are initialized from the camera's current view (centre = camera
     *   target, distance = distance from eye to target).
     *
     * @param camera Camera to manipulate. Must outlive the manipulator.
     * @param scene  Optional scene used for ray picking, anchor fallback and fitToScreen(). May be null
     *               (no picking / scene centre).
     */
    explicit OrbitCameraManipulator(raw_ptr<Camera> camera, raw_ptr<Scene> scene = nullptr);

    /** @brief Destroys the manipulator. */
    ~OrbitCameraManipulator() override;

  public:
    /** @brief Orbits the camera around the current centre (yaw/pitch).
     *
     * @details
     * - This is the TURNTABLE's verb: the eye travels along the orbit sphere, so the picture swings about the centre.
     *   A first-person gaze is `look()`, which leaves the eye where it is; a caller that wants to turn the view about
     *   the eye must not reach for this one.
     *
     * @param deltaYaw   Yaw angle change of the EYE about the centre, in radians.
     * @param deltaPitch Pitch angle change of the eye about the centre, in radians.
     */
    void orbit(double deltaYaw, double deltaPitch);

    /** @brief Turns the view about the EYE: one look, whichever device asks for it, and the eye never moves.
     *
     * @details
     * - Yaws about the world-up axis through the eye, then pitches about the resulting picture-right axis, so the
     *   horizon stays level. The target follows the view at the same eye distance, which is what keeps the orbit state
     *   (centre / distance) meaningful after a look.
     * - The angles are the rotation APPLIED TO THE VIEW, not the eye's place on the orbit sphere: a positive yaw turns
     *   the view LEFT (counter-clockwise seen from the world up) and a positive pitch looks UP. `orbit()` states its
     *   deltas about the centre instead, because there the eye is what moves.
     * - This is the ONLY implementation of a look, and it is public because a host has the same need the built-in
     *   devices have: the left-drag in `FirstPerson` converts pixels with `drag_sensitivity_` and the arrow keys
     *   convert one key step with `rotate_step_`, and both call here. A host that wrote its own look would disagree
     *   with the drag - one mode cannot have two pictures of "look".
     * - Like the travel verbs it does not consult `mode()`: it states a motion of the viewpoint, and the EVENT HANDLER
     *   decides which verb a gesture means.
     * - A view pointing along the world up has no pitch axis, so such a step is refused WHOLE - nothing reaches the
     *   camera - rather than applied halfway.
     *
     * @param deltaYaw   Yaw change in radians; positive turns the view left, 0 leaves it.
     * @param deltaPitch Pitch change in radians; positive looks up, 0 leaves it.
     */
    void look(double deltaYaw, double deltaPitch);

    /** @brief Sets the orbit centre (target point). */
    void setOrbitCenter(const Vec3d& center);

    /** @brief Gets the orbit centre (target point). */
    Vec3d orbitCenter() const;

    /** @brief Gets the orbit distance (eye-to-centre).
     *
     * @return Distance from eye to orbit centre.
     */
    double orbitDistance() const;

    /** @brief Sets the orbit distance (eye-to-centre).
     *
     * @param distance Distance from eye to orbit centre.
     */
    void setOrbitDistance(double distance);

    /** @brief Pans the camera based on screen-space deltas.
     *
     * @details
     * - The pan is scaled by the picture's own extent at the grabbed point, so the world point under the cursor
     *   travels with it: a perspective picture reads the anchor depth and its field of view, a parallel picture reads
     *   the camera's window height, and the eye-to-centre distance is not consulted there - a parallel picture has one
     *   scale and it is the window.
     *
     * @param screenDx Horizontal screen delta in pixels.
     * @param screenDy Vertical screen delta in pixels.
     */
    void pan(double screenDx, double screenDy);

    /** @brief Zooms the picture by a multiplicative factor: the one operation the wheel also performs.
     *
     * @details
     * - A PERSPECTIVE camera zooms by multiplying the eye-to-centre distance, so the world step is proportional to the
     *   current scale and the same ratio is one notch at every zoom level. With `dollyToCursor()` on and a picked
     *   anchor on the cursor ray, the orbit centre slides so that point keeps its picture position, and the zoom stops
     *   rather than passing THROUGH it (the anchor stays in front of the near plane).
     * - An ORTHOGRAPHIC camera zooms by scaling its window - the picture's extent - and does not move the eye at all: a
     *   parallel picture does not depend on where the eye sits along the view axis. With `dollyToCursor()` on and a
     *   picked anchor, the window slides by the amount that keeps that point where it was on screen. The eye is still
     *   kept outside the content, because that is a CLIPPING placement, not a scale.
     *
     * @param factor Zoom factor; <1 zooms in (closer / narrower window), >1 zooms out.
     */
    void dolly(double factor);

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
     * @details
     * - Positions the orbit centre at the scene bounding-box centre and picks an eye-to-centre distance
     *   that fits the scene into the vertical/horizontal field of view (perspective) or the frustum
     *   height (orthographic).
     * - The camera's near/far planes are scaled with that distance (1% and 100x of it), because the camera's
     *   defaults stop at the unit scale: a millimetre-scale model would otherwise sit behind the far plane.
     *
     * @return true when a scene with valid bounds was fitted.
     */
    bool fitToScreen() override;

    /** @brief Restores the view captured when the manipulator was constructed. */
    void home() override;

    /** @brief Sets whether a zoom keeps the cursor anchor fixed on screen.
     *
     * @details
     * - When enabled, zooming towards a picked point keeps that world point under the cursor: a perspective picture
     *   slides the orbit centre towards it, a parallel one slides its window. The orbit centre / window then covers
     *   the region just approached, so the next rotation orbits what the user just zoomed at.
     * - When disabled the zoom changes the picture's scale only and the point under the cursor moves with it. Both
     *   readers of the switch (`dolly` and the wheel, which is the same operation) honour it, so a host gets one
     *   answer whichever route its input takes.
     *
     * @param enabled True to zoom towards the cursor anchor.
     */
    void setDollyToCursor(bool enabled);

    /** @brief Returns whether a wheel dolly anchors to the cursor.
     *
     * @return true when the picked anchor stays under the cursor (see setDollyToCursor).
     */
    bool dollyToCursor() const;

    /** @brief Sets the minimum eye-to-centre distance.
     *
     * Distance 0 (the default) derives the minimum from the camera near plane.
     *
     * @param distance Minimum orbit distance; <= 0 means auto.
     */
    void setMinOrbitDistance(double distance);

    /** @brief Gets the configured minimum orbit distance (0 = auto).
     *
     * @return Minimum distance; 0 means the near plane decides.
     */
    double minOrbitDistance() const;

    /** @brief Sets the scene used for picking / anchor / fitToScreen.
     *
     * @param scene Scene, or null to disable scene features.
     */
    void setScene(raw_ptr<Scene> scene);

    /** @brief Gets the bound scene (may be null). */
    raw_ptr<Scene> scene() const;

    /** @brief Ray-picks the scene under a screen point.
     *
     * @details
     * - The point is a PIXEL INDEX in the viewport's device pixels, and the ray is taken through that pixel's CENTRE
     *   (index + 0.5), because that is the sample the picture shows there. The picture's own centre is therefore
     *   `(width / 2 - 0.5, height / 2 - 0.5)` in this spelling.
     * - The query follows the PICTURE the host announced (see setPresentedCamera), not the live camera, when the two
     *   differ. It answers over the data's own topology and ignores the pass' view switches (clip planes, culling,
     *   a point/topology override) and a billboard's per-view rotation: a pick selects the CONTENT, not the current
     *   viewing of it (see RayIntersection and AutoTransform).
     *
     * @param screenX  Screen x in device pixels.
     * @param screenY  Screen y in device pixels.
     * @param outPoint Receives the world-space hit point on success.
     * @return true when a model was hit.
     */
    bool pickAt(double screenX, double screenY, Vec3d& outPoint) const;

    /** @brief Sets the orbit centre to the point under the cursor.
     *
     * @details
     * - Ray-picks the scene; on a hit the centre snaps to the hit point, on a miss it snaps to the
     *   scene bounding-box centre (or the world origin when there is no scene). The eye is left in
     *   place, so the view re-aims rather than teleports.
     *
     * @param screenX Screen x in device pixels.
     * @param screenY Screen y in device pixels.
     * @return true when a model was hit.
     */
    bool setCenterFromScreen(double screenX, double screenY);

    /** @brief Commits pending changes to the bound camera. */
    void apply() override;

  public:
    /** @brief Handles a mouse button press (see CameraManipulator). */
    void onMousePress(const vn::window::MouseEvent& event) override;

    /** @brief Handles mouse motion, applying the active drag interaction. */
    void onMouseMove(const vn::window::MouseEvent& event) override;

    /** @brief Handles a mouse button release, ending any drag interaction. */
    void onMouseRelease(const vn::window::MouseEvent& event) override;

    /** @brief Handles mouse wheel scroll, zooming the camera. */
    void onScroll(const vn::window::ScrollEvent& event) override;

    /** @brief Handles a key press: the keyboard's travel and look pairs in `FirstPerson`, and Home = fit in both modes.
     *
     * @details
     * - TRAVEL: W / S forward and back, A / D left and right, Q / E down and up, by one `move_step_` each.
     * - LOOK: Left / Right yaw and Up / Down pitch, by one `rotate_step_` each, through `look()` - the same verb the
     *   left-drag reaches. The arrows never travel; the two devices of one mode state one rule.
     * - Home fits the scene (or restores the construction view when there is nothing to fit) in EITHER mode. Outside
     *   `FirstPerson` the other keys do nothing: a turntable has no keyboard gaze and no travel.
     *
     * @param event Key event; only `code` and the press itself are read (hold repeats arrive as further presses).
     */
    void onKeyDown(const vn::window::KeyEvent& event) override;

    /** @brief Handles a key release. */
    void onKeyUp(const vn::window::KeyEvent& event) override;

    /** @brief Updates viewport size and, for a parallel picture, the window's shape.
     *
     * @details
     * - A resize keeps the orthographic window's HEIGHT - that is the picture's scale, and a resize must not change
     *   what the user zoomed to - and gives it the camera's own shape (see Camera). A perspective picture needs
     *   nothing here: its shape lives in the camera.
     */
    void onResize(const vn::window::ResizeEvent& event) override;

  private:
    /** @brief Ray from the camera through a screen pixel. */
    void makeScreenRay(double screenX, double screenY, Vec3d& origin, Vec3d& direction) const;

    /** @brief Computes the anchor (pick point / scene centre / origin). */
    Vec3d resolveAnchor(double screenX, double screenY, bool& onRay) const;

    /** @brief Re-aims the view at a point without moving the eye. */
    void aimCenterAt(const Vec3d& point);

    /** @brief Converts the FirstPerson left-drag into a `look()` step and nothing else.
     *
     * @details
     * - A device's whole share of a look is this conversion: pixels times `drag_sensitivity_`, with the sign the drag
     *   means (dragging right turns right, dragging up looks up). The turning itself is `look()`, so the keyboard's
     *   look and this one cannot disagree.
     *
     * @param screenDx Horizontal screen delta in pixels.
     * @param screenDy Vertical screen delta in pixels.
     */
    void lookRotate(double screenDx, double screenDy);

    /** @brief Rotates the whole camera rig about the press anchor.
     *
     * @details
     * - Yaws about the world-up axis through the pivot, then pitches about the resulting screen-right
     *   axis, carrying the camera up rigidly so the view may roll and sweep over the top (true 90 deg
     *   elevation and beyond). The rigid rotation keeps the pivot's screen position exactly fixed.
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

    /** @brief Gives the orthographic window the camera's own shape, keeping its height (the scale) and its centre. */
    void fitOrthoWindowToAspect();

    /** @brief Scales the orthographic window by @p factor, pinning the picked anchor when the switch is on. */
    void zoomOrthoWindow(double factor);

    /** @brief Gets the aspect of the picture this manipulator's interaction works in.
     *
     * @details
     * - The interaction works in screen space - orbit gains, framing and the picking ray all resolve
     *   there - so it needs the shape of the picture the user is pointing at, which is the CAMERA's own
     *   (see Camera): one number, read by the projection, the culling and the ray alike, so the
     *   interaction cannot disagree with the picture. Its own last onResize() rectangle is the
     *   fallback, and the only answer there is for a manipulator with no camera.
     *
     * @return Width / height of the picture (1 when it is a manipulator with no camera and no size yet).
     */
    double viewportAspect() const;

  private:
    /// The scene ray picking, anchor fallback and fitToScreen() read; BORROWED, and null when the host gave none.
    raw_ptr<Scene> scene_ = nullptr;

    Vec3d  center_{ 0.0, 0.0, 0.0 };
    double distance_ = 10.0;
    double yaw_      = 0.0;
    double pitch_    = 0.0;
    double roll_     = 0.0; // signed camera roll about its forward axis (-pi, pi].

    // Snapshot for home().
    Vec3d  home_center_{ 0.0, 0.0, 0.0 };
    double home_distance_ = 10.0;
    double home_yaw_      = 0.0;
    double home_pitch_    = 0.0;
    double home_roll_     = 0.0;

    bool   dolly_to_cursor_     = true;
    double min_orbit_distance_ = 0.0; // 0 => derive from the near plane.

    // Current interaction anchor (pick point, scene centre or origin): the
    // rotate pivot and the pan/dolly depth reference / dolly-to-cursor anchor.
    Vec3d anchor_{ 0.0, 0.0, 0.0 };
    bool  anchor_on_ray_ = false; // anchor was a geometry hit on the cursor ray.

    CameraManipulator::DragAction drag_   = CameraManipulator::DragAction::None;
    double                        last_x_ = 0.0;
    double                        last_y_ = 0.0;

    // Viewport size in device pixels (fed by onResize).
    int viewport_w_ = 800;
    int viewport_h_ = 600;

    // Sensitivity of drag-based interactions (radians / pixels).
    double drag_sensitivity_ = 0.01;
    // Dolly factor per wheel notch.
    double wheel_factor_     = 1.15;
    // First-person movement step per key event (world units).
    double move_step_        = 0.5;
    // First-person yaw/pitch rotation per key event (radians).
    double rotate_step_      = 0.05;
};

VN_GRAPHICS_NS_END
