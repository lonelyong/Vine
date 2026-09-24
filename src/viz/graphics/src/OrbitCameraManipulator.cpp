#include <vine/graphics/OrbitCameraManipulator.hpp>

#include <algorithm>
#include <cmath>
#include <limits>

#include <vine/graphics/Camera.hpp>
#include <vine/graphics/Geometry.hpp>
#include <vine/graphics/Ray.hpp>
#include <vine/graphics/RayIntersection.hpp>
#include <vine/graphics/Scene.hpp>
#include <vine/math/Point3.hpp>
#include <vine/math/Math.hpp>

VN_GRAPHICS_NS_BEGIN

namespace
{

constexpr double kDegToRad = vn::math::DEG_TO_RAD;
constexpr double kTiny = 1e-6;

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
    yaw = std::atan2(dir.x, dir.y);
}

}  // namespace

OrbitCameraManipulator::OrbitCameraManipulator(raw_ptr<Camera> camera, raw_ptr<Scene> scene)
  : CameraManipulator(camera)
  , scene_(scene)
{
    syncFromCamera();
    home_center_ = center_;
    home_distance_ = distance_;
    home_yaw_ = yaw_;
    home_pitch_ = pitch_;
    home_roll_ = roll_;
}

OrbitCameraManipulator::~OrbitCameraManipulator() = default;

void OrbitCameraManipulator::orbit(double deltaYaw, double deltaPitch)
{
    yaw_ += deltaYaw;
    pitch_ += deltaPitch;
    pitch_ = std::clamp(pitch_, -kMaxElevationRad, kMaxElevationRad);
    apply();
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

double OrbitCameraManipulator::orbitRadius() const
{
    return distance_;
}

void OrbitCameraManipulator::setOrbitRadius(double radius)
{
    distance_ = std::max(radius, kTiny);
    apply();
}

void OrbitCameraManipulator::pan(double screenDx, double screenDy)
{
    Camera* cam = camera_;
    if (cam == nullptr) {
        return;
    }
    const double vh = viewport_h_ > 0 ? static_cast<double>(viewport_h_) : 600.0;
    const Vec3d eye = cam->eye();
    const Vec3d forward = (center_ - eye).normalized();
    if (forward.length() < kTiny) {
        return;
    }
    const Vec3d right = forward.cross(cam->up()).normalized();
    const Vec3d up = right.cross(forward).normalized();

    const double tan_half = std::tan(cam->fieldOfView() * kDegToRad * 0.5);
    double world_per_pixel = 0.0;
    if (cam->projectionType() == Camera::ProjectionType::Orthographic) {
        world_per_pixel = 2.0 * distance_ * tan_half / vh;
    } else {
        const double depth = anchor_on_ray_
            ? std::max((anchor_ - eye).dot(forward), kTiny)
            : std::max(distance_, kTiny);
        world_per_pixel = 2.0 * depth * tan_half / vh;
    }

    const Vec3d delta = (-right * screenDx + up * screenDy) * world_per_pixel;
    center_ += delta;
    apply();
}

void OrbitCameraManipulator::zoom(double factor)
{
    if (camera_ == nullptr || factor <= 0.0) {
        return;
    }
    const double d0 = distance_;
    const double d1 = clampDistance(d0 * factor);
    if (std::abs(d1 - d0) < 1e-9) {
        return;
    }

    // Keep the anchor (a picked world point under the cursor) fixed on screen
    // while zooming: slide the orbit centre laterally so the anchor's screen
    // offset is unchanged after the distance change.
    if (zoom_to_cursor_ && anchor_on_ray_) {
        const Vec3d eye = camera_->eye();
        const Vec3d forward = (center_ - eye).normalized();
        if (forward.length() >= kTiny) {
            double pz = 0.0;
            double pz1 = 0.0;
            if (camera_->projectionType() == Camera::ProjectionType::Orthographic) {
                // In orthographic, the screen offset of a point scales with 1 / distance.
                pz = d0;
                pz1 = d1;
            } else {
                pz = std::max((anchor_ - eye).dot(forward), kTiny);
                pz1 = pz + (d1 - d0);
            }
            if (pz > kTiny) {
                const Vec3d base = eye + forward * pz;
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
    cam->setViewMatrixAsLookAt(cam->eye() + forward * distance,
                               cam->target() + forward * distance, cam->up());
    syncFromCamera();
}

void OrbitCameraManipulator::moveRight(double distance)
{
    Camera* cam = camera_;
    if (cam == nullptr) {
        return;
    }
    const Vec3d forward = (cam->target() - cam->eye()).normalized();
    const Vec3d right = forward.cross(cam->up()).normalized();
    cam->setViewMatrixAsLookAt(cam->eye() + right * distance,
                               cam->target() + right * distance, cam->up());
    syncFromCamera();
}

void OrbitCameraManipulator::moveUp(double distance)
{
    Camera* cam = camera_;
    if (cam == nullptr) {
        return;
    }
    cam->setViewMatrixAsLookAt(cam->eye() + cam->up() * distance,
                               cam->target() + cam->up() * distance, cam->up());
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
    const Vec3d centre(centre_p.x, centre_p.y, centre_p.z);
    const Vec3d size = bounds.size();
    // Bounding-sphere radius around the box centre.
    const double radius = (size * 0.5).length();

    center_ = centre;
    const double fov_rad = cam->fieldOfView() * kDegToRad;
    const double aspect = std::max(cam->aspectRatio(), 1e-6);
    const double margin = 1.15;

    if (cam->projectionType() == Camera::ProjectionType::Orthographic) {
        // Frustum half-height needed to frame the sphere in both dimensions.
        const double half_w = radius * margin * aspect;
        const double needed_half_h = std::max(radius * margin, half_w / aspect);
        const double tan_half = std::tan(fov_rad * 0.5);
        distance_ = std::max(needed_half_h / tan_half, radius * 1.05);
    } else {
        const double tan_half = std::tan(fov_rad * 0.5);
        const double vertical_half = fov_rad * 0.5;
        const double horizontal_half = std::atan(tan_half * aspect);
        const double half_angle = std::min(vertical_half, horizontal_half);
        const double sin_half = std::sin(half_angle);
        distance_ = std::max(radius * margin / std::max(sin_half, 1e-6), radius * 1.05);
    }
    distance_ = clampDistance(distance_);
    // Frame the model level: drop any accumulated roll.
    roll_ = 0.0;
    apply();
    return true;
}

void OrbitCameraManipulator::home()
{
    center_ = home_center_;
    distance_ = home_distance_;
    yaw_ = home_yaw_;
    pitch_ = home_pitch_;
    roll_ = home_roll_;
    apply();
}

void OrbitCameraManipulator::setZoomToCursor(bool enabled)
{
    zoom_to_cursor_ = enabled;
}

bool OrbitCameraManipulator::zoomToCursor() const
{
    return zoom_to_cursor_;
}

void OrbitCameraManipulator::setMinDistance(double distance)
{
    min_distance_ = distance;
}

double OrbitCameraManipulator::minDistance() const
{
    return min_distance_;
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
    RayIntersectionResult result =
        RayIntersection::intersectScene(Ray(origin, direction), scene_);
    if (!result.hit) {
        return false;
    }
    outPoint = result.point;
    return true;
}

bool OrbitCameraManipulator::setCenterFromScreen(double screenX, double screenY)
{
    bool on_ray = false;
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
        up = rotateAround(up, view_fwd, roll_);
    }
    cam->setViewMatrixAsLookAt(eye, center_, up);

    if (cam->projectionType() == Camera::ProjectionType::Orthographic) {
        updateOrthoProjection();
    }
}

void OrbitCameraManipulator::onMousePress(const vn::window::MouseEvent& event)
{
    last_x_ = event.x;
    last_y_ = event.y;
    pointer_known_ = true;

    switch (event.button) {
        case vn::window::MouseButton::Left:
            drag_ = CameraManipulator::DragAction::Rotate;
            break;
        case vn::window::MouseButton::Middle:
        case vn::window::MouseButton::Right:
            drag_ = CameraManipulator::DragAction::Pan;
            break;
        default:
            drag_ = CameraManipulator::DragAction::None;
            break;
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
    last_x_ = event.x;
    last_y_ = event.y;
    pointer_known_ = true;

    switch (drag_) {
        case CameraManipulator::DragAction::Rotate:
            if (mode_ == Mode::FirstPerson) {
                orbit(dx * drag_sensitivity_, -dy * drag_sensitivity_);
            } else {
                // Rotate the whole rig rigidly about the press anchor (the
                // grabbed model point, or the scene centre on empty space), so
                // that point stays pinned under the cursor. The up is carried
                // with the rig, which lets the view roll over the top to a
                // true 90 deg elevation (and beyond) without gimbal shake or a
                // sudden 180 deg flip.
                pivotRotate(dx, dy);
            }
            break;
        case CameraManipulator::DragAction::Pan:
            pan(dx, dy);
            break;
        default:
            break;
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
    // Anchor at the current pointer position (falls back to the view centre).
    const double sx = pointer_known_ ? last_x_ : viewport_w_ * 0.5;
    const double sy = pointer_known_ ? last_y_ : viewport_h_ * 0.5;
    anchor_ = resolveAnchor(sx, sy, anchor_on_ray_);

    // The wheel performs a dolly (translate the camera along a direction)
    // rather than shrinking a radius about a fixed point, so in perspective
    // the camera can move through the scene instead of stopping at a target.
    //
    // While the picked anchor lies in front of the camera the dolly dives
    // along the ray eye -> anchor, which keeps that model point pinned under
    // the cursor (anchored zoom). Once the eye has passed through the anchor
    // (it falls behind the near plane) the dive direction is released and the
    // camera keeps moving straight along the view axis, so it travels through
    // the model instead of stopping at its surface.
    const Vec3d eye = cam->eye();
    const Vec3d forward = (cam->target() - eye).normalized();
    if (forward.length() < kTiny) {
        return;
    }
    Vec3d dir = forward;
    if (anchor_on_ray_) {
        const Vec3d to_anchor = anchor_ - eye;
        const double d = to_anchor.length();
        // Keep the dive aimed at the anchor while it is clearly in front (it
        // then stays pinned under the cursor). Once the eye is essentially at
        // the surface (the anchor is within a few near planes), release the
        // anchor and keep flying straight so the camera passes through the
        // model instead of hovering in front of it.
        const double release_dist = cam->nearPlane() * 3.0;
        if (d > release_dist && to_anchor.dot(forward) > 0.0) {
            dir = to_anchor / d;
        }
    }

    // Move by a fraction of the current eye-to-centre distance per notch so
    // the dolly speed tracks the zoom level.
    const double ratio = 1.0 - 1.0 / wheel_factor_;
    const double step = (event.deltaY > 0.0 ? distance_ * ratio : -distance_ * ratio);
    center_ += dir * step;
    apply();
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
        case vn::window::KeyCode::W:
            moveForward(move_step_);
            break;
        case vn::window::KeyCode::S:
            moveForward(-move_step_);
            break;
        case vn::window::KeyCode::A:
            moveRight(-move_step_);
            break;
        case vn::window::KeyCode::D:
            moveRight(move_step_);
            break;
        case vn::window::KeyCode::E:
            moveUp(move_step_);
            break;
        case vn::window::KeyCode::Q:
            moveUp(-move_step_);
            break;
        case vn::window::KeyCode::Left:
            orbit(-rotate_step_, 0.0);
            break;
        case vn::window::KeyCode::Right:
            orbit(rotate_step_, 0.0);
            break;
        case vn::window::KeyCode::Up:
            orbit(0.0, -rotate_step_);
            break;
        case vn::window::KeyCode::Down:
            orbit(0.0, rotate_step_);
            break;
        default:
            break;
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
    const double aspect = static_cast<double>(event.width) / static_cast<double>(event.height);
    if (cam->projectionType() == Camera::ProjectionType::Perspective) {
        cam->setProjectionMatrixAsPerspective(cam->fieldOfView(), aspect,
                                              cam->nearPlane(), cam->farPlane());
    } else {
        updateOrthoProjection();
    }
}

void OrbitCameraManipulator::makeScreenRay(double screenX, double screenY,
                                           Vec3d& origin, Vec3d& direction) const
{
    Camera* cam = camera_;
    origin = cam != nullptr ? cam->eye() : Vec3d();
    direction = Vec3d(0.0, 0.0, -1.0);
    if (cam == nullptr) {
        return;
    }
    const double w = viewport_w_ > 0 ? static_cast<double>(viewport_w_) : 1.0;
    const double h = viewport_h_ > 0 ? static_cast<double>(viewport_h_) : 1.0;
    const Ray ray = cam->screenToWorldRay(Vec2d(screenX / w, screenY / h));
    origin = ray.origin;
    direction = ray.direction;
}

Vec3d OrbitCameraManipulator::resolveAnchor(double screenX, double screenY,
                                            bool& onRay) const
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
    const Vec3d eye = cam->eye();
    const Vec3d offset = eye - point;
    const double d = offset.length();
    if (d < 1e-9) {
        return;
    }
    center_ = point;
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
    const double ayaw = -screenDx * drag_sensitivity_;
    const double apitch = -screenDy * drag_sensitivity_;
    if (std::abs(ayaw) < 1e-12 && std::abs(apitch) < 1e-12) {
        return;
    }

    const Vec3d eye = cam->eye();
    const Vec3d target = cam->target();
    const Vec3d pivot = anchor_;
    const Vec3d eye_offset = eye - pivot;
    if (eye_offset.length() < kTiny) {
        return;  // Eye on the pivot: no meaningful rotation.
    }
    const Vec3d fwd0 = (target - eye).normalized();
    const Vec3d up0 = cam->up().normalized();
    if (fwd0.length() < kTiny || up0.length() < kTiny) {
        return;
    }

    // Rotate the whole camera rig rigidly about the pivot: first yaw about
    // the world-up axis through the pivot, then pitch about the yawed
    // screen-right axis. Eye, forward and up are carried together, so the
    // pivot stays pinned at its screen position and the view may roll as it
    // sweeps over the top (no gimbal shake, no sudden 180 deg flip).
    const Vec3d eye1 = pivot + rotateAround(eye_offset, kWorldUp, ayaw);
    const Vec3d fwd1 = rotateAround(fwd0, kWorldUp, ayaw);
    const Vec3d up1 = rotateAround(up0, kWorldUp, ayaw);
    const Vec3d right1 = fwd1.cross(up1).normalized();
    if (right1.length() < kTiny) {
        return;
    }
    const Vec3d eye2 = pivot + rotateAround(eye1 - pivot, right1, apitch);
    const Vec3d fwd2 = rotateAround(fwd1, right1, apitch).normalized();
    const Vec3d up2 = rotateAround(up1, right1, apitch).normalized();

    // Keep the eye-to-target distance so the frame scale is unchanged.
    const Vec3d target2 = eye2 + fwd2 * (target - eye).length();
    cam->setViewMatrixAsLookAt(eye2, target2, up2);
    syncFromCamera();
}

void OrbitCameraManipulator::syncFromCamera()
{
    Camera* cam = camera_;
    if (cam == nullptr) {
        return;
    }
    const Vec3d eye = cam->eye();
    center_ = cam->target();
    const Vec3d offset = eye - center_;
    const double d = offset.length();
    if (d > 1e-9) {
        distance_ = d;
        const Vec3d dir = offset / d;
        pitch_ = std::asin(std::clamp(dir.z, -1.0, 1.0));
        // Near a pole the azimuth is ill defined; keep the previous yaw so the
        // reconstructed up (viewUp) stays continuous through a 90 deg orbit.
        if (std::hypot(dir.x, dir.y) > 1e-4) {
            yaw_ = std::atan2(dir.x, dir.y);
        }
        // Roll: the signed rotation about the forward (eye -> centre) axis
        // that takes the level no-roll up (viewUp) to the camera's actual up.
        const Vec3d view_fwd = -dir;
        const Vec3d level_up = viewUp(yaw_, pitch_);
        const Vec3d cam_up = cam->up().normalized();
        roll_ = std::atan2(level_up.cross(cam_up).dot(view_fwd), level_up.dot(cam_up));
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
        const double d = axis == 0 ? dir.x : (axis == 1 ? dir.y : dir.z);
        const double o = axis == 0 ? center_.x : (axis == 1 ? center_.y : center_.z);
        const double lo = axis == 0 ? bounds.min().x
                          : (axis == 1 ? bounds.min().y : bounds.min().z);
        const double hi = axis == 0 ? bounds.max().x
                          : (axis == 1 ? bounds.max().y : bounds.max().z);
        if (std::abs(d) < 1e-12) {
            if (o < lo || o > hi) {
                return;  // Parallel to the slab and outside: the ray misses the box.
            }
        } else {
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
    if (distance_ >= t_min && distance_ <= t_max
        && t_max < std::numeric_limits<double>::max()) {
        distance_ = t_max * 1.02 + 1e-6;
    }
}

double OrbitCameraManipulator::clampDistance(double distance) const
{
    double low = min_distance_;
    if (low <= 0.0) {
        low = camera_ != nullptr ? std::max(camera_->nearPlane() * 1.2, 1e-4) : 1e-3;
    }
    return std::max(distance, low);
}

void OrbitCameraManipulator::updateOrthoProjection()
{
    Camera* cam = camera_;
    if (cam == nullptr || cam->projectionType() != Camera::ProjectionType::Orthographic) {
        return;
    }
    const double tan_half = std::tan(cam->fieldOfView() * kDegToRad * 0.5);
    const double half_h = distance_ * tan_half;
    const double half_w = half_h * std::max(cam->aspectRatio(), 1e-6);
    cam->setProjectionMatrixAsOrtho(-half_w, half_w, -half_h, half_h,
                                    cam->nearPlane(), cam->farPlane());
}

VN_GRAPHICS_NS_END
