#include <vine/graphics/OrbitCameraManipulator.hpp>

#include <algorithm>
#include <cmath>
#include <limits>

#include <vine/graphics/Camera.hpp>
#include <vine/graphics/Geometry.hpp>
#include <vine/graphics/RayIntersection.hpp>
#include <vine/graphics/Scene.hpp>
#include <vine/math/Math.hpp>
#include <vine/math/Point3.hpp>

VN_GRAPHICS_NS_BEGIN

namespace
{

constexpr double kDegToRad = vn::math::DEG_TO_RAD;
constexpr double kTiny     = 1e-6;

// Elevation policy for the spherical (programmatic / keyboard / pan-zoom)
// path: pitch is clamped to a full +/-90 deg (a true top-down / bottom-up
// view) and the no-roll up (viewUp) keeps the horizon level and free of
// 180-degree flips. The rotate DRAG is not spherical: it pivots rigidly about
// the press anchor and carries the camera up with it, so it is free to roll
// and to sweep past the poles.
constexpr double kMaxElevationRad = vn::math::PI_HALF;

// The world-up axis of the spherical model (eyeDirection / viewUp) and of the
// pivot yaw. Vine scenes are Z-up (robotics convention: X forward, Z up).
const Vec3d kWorldUp{ 0.0, 0.0, 1.0 };

/** @brief Rotates a vector around a unit axis (Rodrigues' formula).
 *
 * @param v     Vector to rotate.
 * @param axis  Unit rotation axis.
 * @param angle Rotation angle in radians.
 * @return The rotated vector.
 */
Vec3d rotateAround(const Vec3d& v, const Vec3d& axis, double angle)
{
    const double c = std::cos(angle);
    const double s = std::sin(angle);
    return v * c + axis.cross(v) * s + axis * (axis.dot(v) * (1.0 - c));
}

/** @brief Unit direction from the orbit centre towards the eye (yaw/pitch).
 *
 * Z-up world: the pole is +Z, so pitch is the elevation out of the horizontal
 * XY plane and yaw sweeps around Z. At pitch 0 the eye lies in the XY plane
 * at azimuth (sin yaw, cos yaw).
 */
Vec3d eyeDirection(double yaw, double pitch)
{
    const double cp = std::cos(pitch);
    const double sp = std::sin(pitch);
    return Vec3d{ cp * std::sin(yaw), cp * std::cos(yaw), sp };
}

/** @brief Unit camera up for lookAt(), matching eyeDirection() and extended
 * continuously to the poles.
 *
 * Away from the poles this is the no-roll up (the projection of the world-up
 * +Z onto the plane perpendicular to the view). As |pitch| approaches 90 deg
 * the projection shrinks to zero, so its continuous limit - a horizontal
 * vector that follows the azimuth - is used instead. This keeps the up well
 * defined and free of 180-degree flips over a full 90 deg orbit.
 *
 * @param yaw   Azimuth in radians.
 * @param pitch Signed elevation in radians, within +/-PI_HALF.
 * @return Unit up vector perpendicular to eyeDirection(yaw, pitch).
 */
Vec3d viewUp(double yaw, double pitch)
{
    const double sp = std::sin(pitch);
    const double cp = std::cos(pitch);
    return Vec3d{ -sp * std::sin(yaw), -sp * std::cos(yaw), cp };
}

/** @brief Extracts the yaw/pitch matching eyeDirection() from a unit vector.
 *
 * @param dir   Unit direction from the orbit centre towards the eye.
 * @param yaw   Receives the yaw.
 * @param pitch Receives the pitch.
 */
void sphericalFromDirection(const Vec3d& dir, double& yaw, double& pitch)
{
    pitch = std::asin(std::clamp(dir.z, -1.0, 1.0));
    yaw   = std::atan2(dir.x, dir.y);
}

} // namespace

OrbitCameraManipulator::OrbitCameraManipulator(raw_ptr<Camera> camera, raw_ptr<Scene> scene)
  : CameraManipulator(camera)
  , scene_(scene)
{
    syncFromCamera();
    home_center_   = center_;
    home_distance_ = distance_;
    home_yaw_      = yaw_;
    home_pitch_    = pitch_;
    home_roll_     = roll_;
}

OrbitCameraManipulator::~OrbitCameraManipulator() = default;

void OrbitCameraManipulator::orbit(double deltaYaw, double deltaPitch)
{
    yaw_ += deltaYaw;
    pitch_ += deltaPitch;
    pitch_ = std::clamp(pitch_, -kMaxElevationRad, kMaxElevationRad);
    apply();
}

void OrbitCameraManipulator::look(double deltaYaw, double deltaPitch)
{
    Camera* cam = camera_;
    if (cam == nullptr) {
        return;
    }
    if (std::abs(deltaYaw) < 1e-12 && std::abs(deltaPitch) < 1e-12) {
        return;
    }
    const Vec3d eye = cam->eye();
    Vec3d       fwd = (cam->target() - eye).normalized();
    Vec3d       up  = cam->up().normalized();
    if (fwd.length() < kTiny || up.length() < kTiny) {
        return;
    }
    // Yaw about the WORLD UP through the eye keeps the horizon level, which is what a look means in a Z-up world; the
    // camera up turns with it, so the pitch below is about the picture's own right axis. The eye does not move - only
    // the direction does - so the target follows at the same eye distance and the orbit state stays meaningful.
    fwd               = rotateAround(fwd, kWorldUp, deltaYaw);
    up                = rotateAround(up, kWorldUp, deltaYaw);
    const Vec3d right = fwd.cross(up).normalized();
    if (right.length() < kTiny) {
        return; // Looking along the world up: the pitch axis is undefined, so the step is refused rather than guessed.
    }
    fwd = rotateAround(fwd, right, deltaPitch);
    up  = rotateAround(up, right, deltaPitch).normalized();
    if (fwd.length() < kTiny || up.length() < kTiny) {
        return;
    }
    cam->lookAt(eye, eye + fwd * std::max(distance_, kTiny), up);
    syncFromCamera();
}

void OrbitCameraManipulator::setOrbitCenter(const Vec3d& center)
{
    center_ = center;
    apply();
}

Vec3d OrbitCameraManipulator::orbitCenter() const
{
    return center_;
}

double OrbitCameraManipulator::orbitDistance() const
{
    return distance_;
}

void OrbitCameraManipulator::setOrbitDistance(double distance)
{
    distance_ = std::max(distance, kTiny);
    apply();
}

void OrbitCameraManipulator::pan(double screenDx, double screenDy)
{
    Camera* cam = camera_;
    if (cam == nullptr) {
        return;
    }
    const double vh      = viewport_h_ > 0 ? static_cast<double>(viewport_h_) : 600.0;
    const Vec3d  eye     = cam->eye();
    const Vec3d  forward = (center_ - eye).normalized();
    if (forward.length() < kTiny) {
        return;
    }
    // The picture's own axes, with the camera's up first and the manipulator's own basis behind it: a camera whose
    // authored up is degenerate or parallel to the view (a case lookAt() resolves) would otherwise turn every pan into
    // a silent no-op, because a zero vector normalises to zero.
    Vec3d right = forward.cross(cam->up()).normalized();
    if (right.length() < kTiny) {
        right = forward.cross(viewUp(yaw_, pitch_)).normalized();
    }
    if (right.length() < kTiny) {
        right = forward.cross(kWorldUp).normalized();
    }
    if (right.length() < kTiny) {
        return;
    }
    const Vec3d up = right.cross(forward).normalized();

    double world_per_pixel = 0.0;
    if (cam->projectionType() == Camera::ProjectionType::Orthographic) {
        // ONE SCALE, AND IT IS THE WINDOW: a parallel picture has no depth, so the eye-to-centre distance and the field
        // of view (which an orthographic camera does not have) must not enter here. The window's height over the
        // picture's pixel height is the whole answer, and it is exact for an authored off-centre window too.
        world_per_pixel = cam->orthographicHeight() / vh;
    }
    else {
        const double tan_half = std::tan(cam->fieldOfView() * kDegToRad * 0.5);
        const double depth    = anchor_on_ray_ ? std::max((anchor_ - eye).dot(forward), kTiny) : std::max(distance_, kTiny);
        world_per_pixel       = 2.0 * depth * tan_half / vh;
    }

    const Vec3d delta = (-right * screenDx + up * screenDy) * world_per_pixel;
    center_ += delta;
    apply();
}

void OrbitCameraManipulator::dolly(double factor)
{
    Camera* cam = camera_;
    if (cam == nullptr || !(factor > 0.0) || !std::isfinite(factor)) {
        return;
    }

    // A PARALLEL PICTURE IS ZOOMED BY ITS WINDOW, never by the eye: the eye's position along the view axis changes
    // nothing but clipping, so moving it here would be a change the user cannot see, and deriving the window from it
    // (what this class used to do) made a zoom-in impossible wherever the eye had to stay outside the content.
    if (cam->projectionType() == Camera::ProjectionType::Orthographic) {
        zoomOrthoWindow(factor);
        return;
    }

    const double d0 = distance_;
    double       d1 = clampDistance(d0 * factor);
    if (std::abs(d1 - d0) < 1e-9) {
        return;
    }

    // Keep the anchor (a picked world point under the cursor) fixed on screen while zooming: slide the orbit centre
    // laterally so the anchor's screen offset is unchanged after the distance change. The invariant is the anchor's
    // lateral-offset-over-depth ratio, so the slide is exact rather than a correction that has to be iterated.
    if (dolly_to_cursor_ && anchor_on_ray_) {
        const Vec3d eye     = cam->eye();
        const Vec3d forward = (center_ - eye).normalized();
        if (forward.length() >= kTiny) {
            const double pz = std::max((anchor_ - eye).dot(forward), kTiny);
            // Zooming in shortens every depth by (d0 - d1), so an anchor nearer than that would pass BEHIND the eye,
            // where a picture position has no meaning (the ratio would mirror it to the wrong side). The zoom stops
            // at the surface instead - which is what makes the region just approached the next orbit centre.
            if (d1 < d0) {
                d1 = std::max(d1, std::min(d0, d0 - pz + cam->nearPlane()));
                if (std::abs(d1 - d0) < 1e-9) {
                    return;
                }
            }
            const double pz1 = pz + (d1 - d0);
            if (pz1 > kTiny) {
                const Vec3d base    = eye + forward * pz;
                const Vec3d lateral = anchor_ - base;
                center_ += lateral * (1.0 - pz1 / pz);
            }
        }
    }
    distance_ = d1;
    apply();
}

void OrbitCameraManipulator::moveForward(double distance)
{
    Camera* cam = camera_;
    if (cam == nullptr) {
        return;
    }
    const Vec3d forward = (cam->target() - cam->eye()).normalized();
    cam->lookAt(cam->eye() + forward * distance, cam->target() + forward * distance, cam->up());
    syncFromCamera();
}

void OrbitCameraManipulator::moveRight(double distance)
{
    Camera* cam = camera_;
    if (cam == nullptr) {
        return;
    }
    const Vec3d forward = (cam->target() - cam->eye()).normalized();
    const Vec3d right   = forward.cross(cam->up()).normalized();
    cam->lookAt(cam->eye() + right * distance, cam->target() + right * distance, cam->up());
    syncFromCamera();
}

void OrbitCameraManipulator::moveUp(double distance)
{
    Camera* cam = camera_;
    if (cam == nullptr) {
        return;
    }
    cam->lookAt(cam->eye() + cam->up() * distance, cam->target() + cam->up() * distance, cam->up());
    syncFromCamera();
}

bool OrbitCameraManipulator::fitToScreen()
{
    Camera* cam = camera_;
    if (cam == nullptr || scene_ == nullptr) {
        return false;
    }
    const Aabbd bounds = scene_->boundingBox();
    if (!bounds.isValid()) {
        return false;
    }
    const vn::math::Point3d centre_p = bounds.center();
    const Vec3d             centre(centre_p.x, centre_p.y, centre_p.z);
    const Vec3d             size = bounds.size();
    // Bounding-sphere radius around the box centre.
    const double radius          = std::max((size * 0.5).length(), 1e-9);

    center_              = centre;
    const double fov_rad = cam->fieldOfView() * kDegToRad;
    const double aspect  = std::max(viewportAspect(), 1e-3);
    const double margin  = 1.15;

    // The EYE is placed the same way for both projections - far enough that the content sits between the clip planes -
    // but only a perspective picture reads that distance as a SCALE. A parallel picture is fitted by its window below.
    if (cam->projectionType() == Camera::ProjectionType::Orthographic) {
        distance_ = radius * 2.0 + margin;
    }
    else {
        const double tan_half        = std::tan(fov_rad * 0.5);
        const double vertical_half   = fov_rad * 0.5;
        const double horizontal_half = std::atan(tan_half * aspect);
        const double half_angle      = std::min(vertical_half, horizontal_half);
        const double sin_half        = std::sin(half_angle);
        distance_                    = std::max(radius * margin / std::max(sin_half, 1e-6), radius * 1.05);
    }
    distance_               = clampDistance(distance_);
    // The camera's clip planes belong to the content as much as the distance does: the default 0.1..1000 frustum
    // stops at the unit scale, so a millimetre-scale model - and the distance a fit gives it - would sit entirely
    // behind the far plane. Scale both with the fitted distance, keeping the 1:10000 depth ratio the defaults use.
    const double near_plane = std::max(distance_ * 0.01, 1e-4);
    const double far_plane  = std::max(distance_ * 100.0, near_plane * 1000.0);
    if (cam->projectionType() == Camera::ProjectionType::Orthographic) {
        // The window is the picture, so the fit sizes the WINDOW: one height that covers the bounding sphere in both
        // dimensions (the tighter axis decides), because a parallel picture's extent is what shows the content.
        const double height = 2.0 * radius * margin / std::min(1.0, aspect);
        const double width  = height * aspect;
        cam->setOrthographic(-width * 0.5, width * 0.5, -height * 0.5, height * 0.5, near_plane, far_plane);
    }
    else {
        cam->setPerspective(cam->fieldOfView(), near_plane, far_plane);
    }
    // Frame the model level: drop any accumulated roll.
    roll_ = 0.0;
    apply();
    return true;
}

void OrbitCameraManipulator::home()
{
    center_   = home_center_;
    distance_ = home_distance_;
    yaw_      = home_yaw_;
    pitch_    = home_pitch_;
    roll_     = home_roll_;
    apply();
}

void OrbitCameraManipulator::setDollyToCursor(bool enabled)
{
    dolly_to_cursor_ = enabled;
}

bool OrbitCameraManipulator::dollyToCursor() const
{
    return dolly_to_cursor_;
}

void OrbitCameraManipulator::setMinOrbitDistance(double distance)
{
    min_orbit_distance_ = distance;
}

double OrbitCameraManipulator::minOrbitDistance() const
{
    return min_orbit_distance_;
}

void OrbitCameraManipulator::setScene(raw_ptr<Scene> scene)
{
    scene_ = scene;
}

raw_ptr<Scene> OrbitCameraManipulator::scene() const
{
    return scene_;
}

bool OrbitCameraManipulator::pickAt(double screenX, double screenY, Vec3d& outPoint) const
{
    if (scene_ == nullptr || camera_ == nullptr) {
        return false;
    }
    Vec3d origin;
    Vec3d direction;
    makeScreenRay(screenX, screenY, origin, direction);
    RayIntersectionResult result = RayIntersection::intersectScene(vn::geom::spatial::Rayd(vn::math::Point3d(origin.x, origin.y, origin.z), direction), scene_);
    if (!result.hit) {
        return false;
    }
    outPoint = result.point;
    return true;
}

bool OrbitCameraManipulator::setCenterFromScreen(double screenX, double screenY)
{
    bool        on_ray = false;
    const Vec3d anchor = resolveAnchor(screenX, screenY, on_ray);
    aimCenterAt(anchor);
    return on_ray;
}

void OrbitCameraManipulator::apply()
{
    Camera* cam = camera_;
    if (cam == nullptr) {
        return;
    }
    pitch_ = std::clamp(pitch_, -kMaxElevationRad, kMaxElevationRad);
    if (cam->projectionType() == Camera::ProjectionType::Orthographic) {
        // A parallel picture has no eye depth to speak of: keeping the eye outside the content is a CLIPPING placement
        // (geometry between the eye and the near plane is cut), so it may move the eye freely - it must never reach
        // the window, which is the picture.
        clampEyeOutsideBounds();
    }
    distance_ = clampDistance(distance_);

    // Unit direction from the orbit centre towards the eye.
    const Vec3d dir = eyeDirection(yaw_, pitch_);
    const Vec3d eye = center_ + dir * distance_;

    // Level no-roll up, then any accumulated roll about the view axis (a
    // pivot drag may leave the camera rolled; pan / zoom / home keep it).
    Vec3d up = viewUp(yaw_, pitch_);
    if (std::abs(roll_) > 1e-9) {
        const Vec3d view_fwd = (center_ - eye).normalized();
        up                   = rotateAround(up, view_fwd, roll_);
    }
    cam->lookAt(eye, center_, up);
    // The projection is the CAMERA's: an orthographic window is written only where the user asks for it to change (a
    // zoom, a resize that reshapes it, fitToScreen), never from the viewpoint state. That is what lets an authored
    // off-centre window survive a gesture.
}

void OrbitCameraManipulator::onMousePress(const vn::window::MouseEvent& event)
{
    last_x_ = event.x;
    last_y_ = event.y;

    switch (event.button) {
    case vn::window::MouseButton::Left: drag_ = CameraManipulator::DragAction::Rotate; break;
    case vn::window::MouseButton::Middle:
    case vn::window::MouseButton::Right: drag_ = CameraManipulator::DragAction::Pan; break;
    default: drag_ = CameraManipulator::DragAction::None; break;
    }

    // Resolve the interaction anchor: the picked model point under the cursor,
    // falling back to the scene bounding-box centre or the world origin. It is
    // the rotate pivot (the whole rig turns about it during a rotate drag, so
    // the grabbed point stays under the cursor) and the pan / zoom depth
    // reference. It is not applied on press, so a press never moves the
    // camera (no jump).
    anchor_ = resolveAnchor(event.x, event.y, anchor_on_ray_);
}

void OrbitCameraManipulator::onMouseMove(const vn::window::MouseEvent& event)
{
    const double dx = event.x - last_x_;
    const double dy = event.y - last_y_;
    last_x_         = event.x;
    last_y_         = event.y;

    switch (drag_) {
    case CameraManipulator::DragAction::Rotate:
        if (mode_ == Mode::FirstPerson) {
            // A first-person look turns ABOUT THE EYE: the eye must not travel an arc of the orbit radius while the
            // user looks (that is what orbit() does, and it is not a look).
            lookRotate(dx, dy);
        }
        else {
            // Rotate the whole rig rigidly about the press anchor (the
            // grabbed model point, or the scene centre on empty space), so
            // that point stays pinned under the cursor. The up is carried
            // with the rig, which lets the view roll over the top to a
            // true 90 deg elevation (and beyond) without gimbal shake or a
            // sudden 180 deg flip.
            pivotRotate(dx, dy);
        }
        break;
    case CameraManipulator::DragAction::Pan: pan(dx, dy); break;
    default: break;
    }
}

void OrbitCameraManipulator::onMouseRelease(const vn::window::MouseEvent& event)
{
    (void)event;
    drag_ = CameraManipulator::DragAction::None;
}

void OrbitCameraManipulator::onScroll(const vn::window::ScrollEvent& event)
{
    Camera* cam = camera_;
    if (cam == nullptr || std::abs(event.deltaY) < 1e-6) {
        return;
    }
    // THE WHEEL IS ONE ZOOM, and `dolly()` is that operation: the event picks the point it aims at and states a
    // direction; the factor per notch, the anchor policy (`setDollyToCursor`) and the projection's own scale all live
    // in the one implementation. A positive deltaY is the wheel turned away from the user (the window's convention)
    // and zooms in, which is the direction this manipulator has always shown.
    anchor_ = resolveAnchor(event.x, event.y, anchor_on_ray_);
    dolly(event.deltaY > 0.0 ? 1.0 / wheel_factor_ : wheel_factor_);
}

void OrbitCameraManipulator::onKeyDown(const vn::window::KeyEvent& event)
{
    if (event.code == vn::window::KeyCode::Home) {
        if (!fitToScreen()) {
            home();
        }
        return;
    }
    if (mode_ != Mode::FirstPerson) {
        return;
    }
    switch (event.code) {
    case vn::window::KeyCode::W: moveForward(move_step_); break;
    case vn::window::KeyCode::S: moveForward(-move_step_); break;
    case vn::window::KeyCode::A: moveRight(-move_step_); break;
    case vn::window::KeyCode::D: moveRight(move_step_); break;
    case vn::window::KeyCode::E: moveUp(move_step_); break;
    case vn::window::KeyCode::Q: moveUp(-move_step_); break;
    // The arrows LOOK, through the one look verb the left-drag reaches. They used to call `orbit()` - the turntable's
    // verb - so the keyboard carried the eye round an arc of the orbit radius while the drag in the same mode turned
    // about it: one mode, two pictures of "look" (review 19 Z8).
    case vn::window::KeyCode::Left: look(rotate_step_, 0.0); break;
    case vn::window::KeyCode::Right: look(-rotate_step_, 0.0); break;
    case vn::window::KeyCode::Up: look(0.0, rotate_step_); break;
    case vn::window::KeyCode::Down: look(0.0, -rotate_step_); break;
    default: break;
    }
}

void OrbitCameraManipulator::onKeyUp(const vn::window::KeyEvent& event)
{
    (void)event;
    // Movement is event-driven per key press; nothing to track on release.
}

void OrbitCameraManipulator::onResize(const vn::window::ResizeEvent& event)
{
    if (event.width <= 0 || event.height <= 0) {
        return;
    }
    viewport_w_ = event.width;
    viewport_h_ = event.height;
    Camera* cam = camera_;
    if (cam == nullptr) {
        return;
    }
    // The camera already holds the shape of its picture (see Camera): the view that owns it pushes the pane's shape
    // there and calls this with the same rectangle, so what onResize keeps is the PIXEL extent the interaction math
    // needs (gains per pixel, the pointer's default centre), not a second copy of the aspect (see viewportAspect).
    // A parallel picture is the exception that proves the rule: its window IS the picture, so a new shape has to reach
    // the window - and only the SHAPE does, because the height is the scale the user zoomed to.
    if (cam->projectionType() == Camera::ProjectionType::Orthographic) {
        fitOrthoWindowToAspect();
    }
}

void OrbitCameraManipulator::makeScreenRay(double screenX, double screenY, Vec3d& origin, Vec3d& direction) const
{
    Camera* cam = camera_;
    origin      = cam != nullptr ? cam->eye() : Vec3d();
    direction   = Vec3d(0.0, 0.0, -1.0);
    if (cam == nullptr && !presented_camera_.has_value()) {
        return;
    }
    const double w = viewport_w_ > 0 ? static_cast<double>(viewport_w_) : 1.0;
    const double h = viewport_h_ > 0 ? static_cast<double>(viewport_h_) : 1.0;
    // A POINTER NAMES A PIXEL, NOT A CORNER (review 17 W4): the position a window reports is the pixel's INDEX and
    // the picture is normalised 0..1 across its extent, so the index's centre sits half a pixel further along. Without
    // the half the ray is biased up-left by half a texel everywhere, which a pick at a silhouette answers from the
    // wrong side. The picture's own centre is therefore (w/2 - 0.5, h/2 - 0.5) in this spelling.
    const double picture_x = (screenX + 0.5) / w;
    const double picture_y = (screenY + 0.5) / h;
    // THE PICTURE THE USER IS AIMING AT, when the host says it is not the live camera (design N17): the camera may have
    // moved since the frame that is on screen was answered, and a pick that used the new pose would resolve a pivot or
    // a selection off by exactly that much. With nothing set (inline mode, a still picture) this is the live camera, as
    // it has always been.
    if (presented_camera_.has_value() && presented_camera_->screenRay(Vec2d(picture_x, picture_y), origin, direction)) {
        return;
    }
    if (cam == nullptr) {
        return;
    }
    const vn::geom::spatial::Rayd ray = cam->screenToWorldRay(Vec2d(picture_x, picture_y));
    origin                       = Vec3d(ray.origin.x, ray.origin.y, ray.origin.z);
    direction                    = ray.direction;
}

Vec3d OrbitCameraManipulator::resolveAnchor(double screenX, double screenY, bool& onRay) const
{
    onRay = false;
    Vec3d hit;
    if (pickAt(screenX, screenY, hit)) {
        onRay = true;
        return hit;
    }
    if (scene_ != nullptr) {
        const Aabbd bounds = scene_->boundingBox();
        if (bounds.isValid()) {
            const vn::math::Point3d centre_p = bounds.center();
            return Vec3d(centre_p.x, centre_p.y, centre_p.z);
        }
    }
    return Vec3d(0.0, 0.0, 0.0);
}

void OrbitCameraManipulator::aimCenterAt(const Vec3d& point)
{
    Camera* cam = camera_;
    if (cam == nullptr) {
        return;
    }
    const Vec3d  eye    = cam->eye();
    const Vec3d  offset = eye - point;
    const double d      = offset.length();
    if (d < 1e-9) {
        return;
    }
    center_   = point;
    distance_ = clampDistance(d);
    sphericalFromDirection(offset / d, yaw_, pitch_);
    apply();
}

void OrbitCameraManipulator::pivotRotate(double screenDx, double screenDy)
{
    Camera* cam = camera_;
    if (cam == nullptr) {
        return;
    }
    const double ayaw   = -screenDx * drag_sensitivity_;
    const double apitch = -screenDy * drag_sensitivity_;
    if (std::abs(ayaw) < 1e-12 && std::abs(apitch) < 1e-12) {
        return;
    }

    const Vec3d eye        = cam->eye();
    const Vec3d target     = cam->target();
    const Vec3d pivot      = anchor_;
    const Vec3d eye_offset = eye - pivot;
    if (eye_offset.length() < kTiny) {
        return; // Eye on the pivot: no meaningful rotation.
    }
    const Vec3d fwd0 = (target - eye).normalized();
    const Vec3d up0  = cam->up().normalized();
    if (fwd0.length() < kTiny || up0.length() < kTiny) {
        return;
    }

    // Rotate the whole camera rig rigidly about the pivot: first yaw about
    // the world-up axis through the pivot, then pitch about the yawed
    // screen-right axis. Eye, forward and up are carried together, so the
    // pivot stays pinned at its screen position and the view may roll as it
    // sweeps over the top (no gimbal shake, no sudden 180 deg flip).
    const Vec3d eye1   = pivot + rotateAround(eye_offset, kWorldUp, ayaw);
    const Vec3d fwd1   = rotateAround(fwd0, kWorldUp, ayaw);
    const Vec3d up1    = rotateAround(up0, kWorldUp, ayaw);
    const Vec3d right1 = fwd1.cross(up1).normalized();
    if (right1.length() < kTiny) {
        return;
    }
    const Vec3d eye2 = pivot + rotateAround(eye1 - pivot, right1, apitch);
    const Vec3d fwd2 = rotateAround(fwd1, right1, apitch).normalized();
    const Vec3d up2  = rotateAround(up1, right1, apitch).normalized();

    // Keep the eye-to-target distance so the frame scale is unchanged.
    const Vec3d target2 = eye2 + fwd2 * (target - eye).length();
    cam->lookAt(eye2, target2, up2);
    syncFromCamera();
}

void OrbitCameraManipulator::lookRotate(double screenDx, double screenDy)
{
    // The drag's share of a look is only this: pixels become angles (a drag to the right turns the view right, a drag
    // up looks up). The turning itself is `look()`, which the arrow keys reach too - one look, two devices.
    look(-screenDx * drag_sensitivity_, -screenDy * drag_sensitivity_);
}

void OrbitCameraManipulator::syncFromCamera()
{
    Camera* cam = camera_;
    if (cam == nullptr) {
        return;
    }
    const Vec3d eye     = cam->eye();
    center_             = cam->target();
    const Vec3d  offset = eye - center_;
    const double d      = offset.length();
    if (d > 1e-9) {
        distance_       = d;
        const Vec3d dir = offset / d;
        pitch_          = std::asin(std::clamp(dir.z, -1.0, 1.0));
        // Near a pole the azimuth is ill defined; keep the previous yaw so the
        // reconstructed up (viewUp) stays continuous through a 90 deg orbit.
        if (std::hypot(dir.x, dir.y) > 1e-4) {
            yaw_ = std::atan2(dir.x, dir.y);
        }
        // Roll: the signed rotation about the forward (eye -> centre) axis
        // that takes the level no-roll up (viewUp) to the camera's actual up.
        const Vec3d view_fwd = -dir;
        const Vec3d level_up = viewUp(yaw_, pitch_);
        const Vec3d cam_up   = cam->up().normalized();
        roll_                = std::atan2(level_up.cross(cam_up).dot(view_fwd), level_up.dot(cam_up));
    }
}

void OrbitCameraManipulator::clampEyeOutsideBounds()
{
    Camera* cam = camera_;
    if (cam == nullptr || scene_ == nullptr) {
        return;
    }
    const Aabbd bounds = scene_->boundingBox();
    if (!bounds.isValid()) {
        return;
    }
    const Vec3d dir = eyeDirection(yaw_, pitch_);

    // Slab ray from the orbit centre along the eye direction: the eye must
    // stay outside the box, so it may not sit between the entry and exit.
    double t_min = -std::numeric_limits<double>::max();
    double t_max = std::numeric_limits<double>::max();
    for (int axis = 0; axis < 3; ++axis) {
        const double d  = axis == 0 ? dir.x : (axis == 1 ? dir.y : dir.z);
        const double o  = axis == 0 ? center_.x : (axis == 1 ? center_.y : center_.z);
        const double lo = axis == 0 ? bounds.min().x : (axis == 1 ? bounds.min().y : bounds.min().z);
        const double hi = axis == 0 ? bounds.max().x : (axis == 1 ? bounds.max().y : bounds.max().z);
        if (std::abs(d) < 1e-12) {
            if (o < lo || o > hi) {
                return; // Parallel to the slab and outside: the ray misses the box.
            }
        }
        else {
            double t1 = (lo - o) / d;
            double t2 = (hi - o) / d;
            if (t1 > t2) {
                std::swap(t1, t2);
            }
            t_min = std::max(t_min, t1);
            t_max = std::min(t_max, t2);
        }
    }
    // Clamp only when the current distance would place the eye inside the box.
    if (distance_ >= t_min && distance_ <= t_max && t_max < std::numeric_limits<double>::max()) {
        distance_ = t_max * 1.02 + 1e-6;
    }
}

double OrbitCameraManipulator::clampDistance(double distance) const
{
    double low = min_orbit_distance_;
    if (low <= 0.0) {
        low = camera_ != nullptr ? std::max(camera_->nearPlane() * 1.2, 1e-4) : 1e-3;
    }
    return std::max(distance, low);
}

void OrbitCameraManipulator::fitOrthoWindowToAspect()
{
    Camera* cam = camera_;
    if (cam == nullptr || cam->projectionType() != Camera::ProjectionType::Orthographic) {
        return;
    }
    // The HEIGHT is the picture's scale - it is what a zoom changes - so a new shape changes only the width and keeps
    // the window's own centre: an off-centre window (a tiled view, one eye of a stereo pair) stays off-centre.
    const double height = cam->orthographicHeight();
    if (!(height > 0.0)) {
        return;
    }
    const double centre_x = 0.5 * (cam->orthographicLeft() + cam->orthographicRight());
    const double centre_y = 0.5 * (cam->orthographicBottom() + cam->orthographicTop());
    const double width    = height * viewportAspect();
    cam->setOrthographic(centre_x - 0.5 * width, centre_x + 0.5 * width, centre_y - 0.5 * height, centre_y + 0.5 * height, cam->nearPlane(), cam->farPlane());
}

void OrbitCameraManipulator::zoomOrthoWindow(double factor)
{
    Camera* cam = camera_;
    if (cam == nullptr || cam->projectionType() != Camera::ProjectionType::Orthographic) {
        return;
    }
    double left   = cam->orthographicLeft() * factor;
    double right  = cam->orthographicRight() * factor;
    double bottom = cam->orthographicBottom() * factor;
    double top    = cam->orthographicTop() * factor;
    // A window with no extent has no inverse (the projection divides by it), so a scale that would collapse it is
    // refused instead of passed on. Precision, not a limit: fp64 carries about fifteen decades of it.
    if (!(top - bottom > 0.0) || !(right - left > 0.0) || !std::isfinite(top) || !std::isfinite(right)) {
        return;
    }
    // Pin the picked point: scaling the window about the camera's own axis moves every picture position except the
    // axis itself, so the window slides by exactly the amount that keeps the anchor where it was. The identity is
    // left' = factor * left + x * (1 - factor) for the anchor's view-space x, and the same for the vertical pair.
    if (dolly_to_cursor_ && anchor_on_ray_) {
        const Vec3d eye = cam->eye();
        const Vec3d fwd = (center_ - eye).normalized();
        if (fwd.length() >= kTiny) {
            const Vec3d right_axis = fwd.cross(cam->up()).normalized();
            const Vec3d up_axis    = right_axis.cross(fwd).normalized();
            if (right_axis.length() >= kTiny && up_axis.length() >= kTiny) {
                const Vec3d  offset  = anchor_ - eye;
                const double slide_x = offset.dot(right_axis) * (1.0 - factor);
                const double slide_y = offset.dot(up_axis) * (1.0 - factor);
                left += slide_x;
                right += slide_x;
                bottom += slide_y;
                top += slide_y;
            }
        }
    }
    cam->setOrthographic(left, right, bottom, top, cam->nearPlane(), cam->farPlane());
}

double OrbitCameraManipulator::viewportAspect() const
{
    // The shape of the picture the interaction works in. Under ONE CAMERA, ONE PICTURE SHAPE (see Camera) that is the
    // CAMERA's own aspect: the same number the projection is built with, the culling uses and the picking ray
    // (Camera::screenToWorldRay) follows - so orbit gains, framing and the orthographic window cannot disagree with
    // what the user is pointing at. A manipulator with no camera has nothing to draw through, and falls back to the
    // rectangle it was last resized to, which is all it has (guarded: an unlaid-out viewport must not divide by
    // zero or produce a degenerate frustum).
    if (camera_ != nullptr) {
        return camera_->aspect();
    }
    if (viewport_w_ <= 0 || viewport_h_ <= 0) {
        return 1.0;
    }
    return std::max(static_cast<double>(viewport_w_) / static_cast<double>(viewport_h_), 1e-6);
}

VN_GRAPHICS_NS_END
