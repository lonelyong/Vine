#include <vine/graphics/Camera.hpp>

#include <vine/graphics/CameraSnapshot.hpp>

#include <vine/math/Transform3.hpp>

VN_GRAPHICS_NS_BEGIN

VN_OBJECT_META_IMPL(Camera, vn::Object);

Camera::Camera()
{
    lookAt(eye_, center_, up_);
    setPerspective(fov_, near_plane_, far_plane_);
}

Camera::~Camera() = default;

String Camera::name() const
{
    return name_;
}

void Camera::setName(const String& name)
{
    name_ = name;
}

Camera::ProjectionType Camera::projectionType() const
{
    return projection_type_;
}

void Camera::lookAt(const Vec3d& eye, const Vec3d& center, const Vec3d& up)
{
    eye_    = eye;
    center_ = center;
    up_     = up.normalized();
    view_   = vn::math::lookAt(vn::math::Point3d(eye.x, eye.y, eye.z), vn::math::Point3d(center.x, center.y, center.z), up_);
}

void Camera::setPerspective(double fovy, double zNear, double zFar)
{
    projection_type_ = ProjectionType::Perspective;
    fov_             = fovy;
    near_plane_      = zNear;
    far_plane_       = zFar;
}

void Camera::setOrthographic(double left, double right, double bottom, double top, double zNear, double zFar)
{
    projection_type_ = ProjectionType::Orthographic;
    // The window is kept whole (see the header): the height alone is enough for a query but not for a
    // consumer that rebuilds the projection, which would then render a centred frustum while the engine
    // culls with an off-centre one.
    ortho_left_      = left;
    ortho_right_     = right;
    ortho_bottom_    = bottom;
    ortho_top_       = top;
    near_plane_      = zNear;
    far_plane_       = zFar;
}

Vec3d Camera::eye() const
{
    return eye_;
}

Vec3d Camera::target() const
{
    return center_;
}

Vec3d Camera::up() const
{
    return up_;
}

double Camera::nearPlane() const
{
    return near_plane_;
}

double Camera::farPlane() const
{
    return far_plane_;
}

double Camera::fieldOfView() const
{
    return fov_;
}

double Camera::orthographicHeight() const
{
    return ortho_top_ - ortho_bottom_;
}

double Camera::orthographicLeft() const
{
    return ortho_left_;
}

double Camera::orthographicRight() const
{
    return ortho_right_;
}

double Camera::orthographicBottom() const
{
    return ortho_bottom_;
}

double Camera::orthographicTop() const
{
    return ortho_top_;
}

Mat4d Camera::viewMatrix() const
{
    return view_;
}

double Camera::aspect() const
{
    return aspect_;
}

void Camera::setAspect(double aspect) noexcept
{
    if (aspect > 0.0) {
        aspect_ = aspect;
    }
}

Mat4d Camera::projectionMatrix() const
{
    // Rebuilt per call from the state above. For an orthographic camera the window already carries the shape,
    // so the aspect is not read at all.
    if (projection_type_ == ProjectionType::Orthographic) {
        return vn::math::ortho<double>(ortho_left_, ortho_right_, ortho_bottom_, ortho_top_, near_plane_, far_plane_);
    }
    return vn::math::perspective<double>(fov_ * vn::math::DEG_TO_RAD, aspect_, near_plane_, far_plane_);
}

vn::geom::spatial::Rayd Camera::screenToWorldRay(const Vec2d& screenPos) const
{
    const double ndc_x = (2.0 * screenPos.x) - 1.0;
    const double ndc_y = 1.0 - (2.0 * screenPos.y);

    // The basis IS the view matrix's (its rows are right / up / backward — see vn::math::lookAt), so a
    // ray cannot aim anywhere the rendered picture is not: a second hand-rolled look-at would agree only
    // while the up vector happens to be perpendicular to the view direction, and divide by a length near
    // zero the moment it is not (lookAt resolves that case by picking a reference axis; a bare cross
    // product has nothing to fall back on and returns NaNs).
    const Vec3d right(view_.element(0, 0), view_.element(0, 1), view_.element(0, 2));
    const Vec3d up(view_.element(1, 0), view_.element(1, 1), view_.element(1, 2));
    const Vec3d backward(view_.element(2, 0), view_.element(2, 1), view_.element(2, 2));
    const Vec3d forward = -backward;

    if (projection_type_ == ProjectionType::Orthographic) {
        // Orthographic: direction is the view direction, and the origin travels across the projection
        // WINDOW — all four bounds, so an off-centre window aims where it renders. Screen [0, 1] maps
        // onto the window linearly (the same linearity the projection has).
        const double x      = ortho_left_ + ((ndc_x + 1.0) * 0.5) * (ortho_right_ - ortho_left_);
        const double y      = ortho_bottom_ + ((ndc_y + 1.0) * 0.5) * (ortho_top_ - ortho_bottom_);
        const Vec3d  origin = eye_ + right * x + up * y;
        return vn::geom::spatial::Rayd(vn::math::Point3d(origin.x, origin.y, origin.z), forward);
    }
    // Perspective: unproject through the near plane.
    const double fov_rad  = fov_ * vn::math::DEG_TO_RAD;
    const double tan_half = std::tan(fov_rad * 0.5);
    const double x        = ndc_x * tan_half * aspect_;
    const double y        = ndc_y * tan_half;
    const Vec3d  dir      = (forward + right * x + up * y).normalized();
    return vn::geom::spatial::Rayd(vn::math::Point3d(eye_.x, eye_.y, eye_.z), dir);
}

CameraSnapshot CameraSnapshot::of(const Camera& camera)
{
    // The ONE conversion from an authored camera to the value a frame carries: the same four numbers the view block,
    // the culling and the picking ray read, taken together so one frame cannot see a camera that moved in between.
    CameraSnapshot snapshot;
    snapshot.present    = true;
    snapshot.view       = camera.viewMatrix();
    // The CAMERA's own shape is what the projection is built with (see Camera): the engine culls with the same
    // number, so the frustum culled against and the projection rendered with cannot drift.
    snapshot.projection = camera.projectionMatrix();
    snapshot.eye        = camera.eye();
    snapshot.target     = camera.target();
    snapshot.up         = camera.up();
    return snapshot;
}

bool CameraSnapshot::screenRay(const vn::math::Vec2d& screen01, Vec3d& origin, Vec3d& direction) const
{
    if (!present) {
        return false;
    }
    // NDC first: a window pointer is x-right / y-DOWN over the picture, clip space is y-UP.
    const double ndc_x = (2.0 * screen01.x) - 1.0;
    const double ndc_y = 1.0 - (2.0 * screen01.y);

    // BOTH ENDS OF THE PIXEL, unprojected through this snapshot's own matrices. Which clip depth is NEAR is the
    // projection's business (this tree renders Reverse-Z: the near plane is clip 1), so nothing here assumes it - the
    // two world points say which is which by their distance to the eye.
    const Mat4d inverse = (projection * view).inverted();
    const auto  worldAt = [&inverse](double x, double y, double clip_z) {
        const double point[4]{ x, y, clip_z, 1.0 };
        double       mapped[4]{ 0.0, 0.0, 0.0, 0.0 };
        for (int row = 0; row < 4; ++row) {
            for (int index = 0; index < 4; ++index) {
                mapped[row] += inverse.element(row, index) * point[index];
            }
        }
        const double w = mapped[3] != 0.0 ? mapped[3] : 1.0;
        return Vec3d(mapped[0] / w, mapped[1] / w, mapped[2] / w);
    };

    const Vec3d end_a = worldAt(ndc_x, ndc_y, 1.0);
    const Vec3d end_b = worldAt(ndc_x, ndc_y, 0.0);
    const auto  offset = [](const Vec3d& from, const Vec3d& to) { return Vec3d(to.x - from.x, to.y - from.y, to.z - from.z); };
    const auto  squared_length = [](const Vec3d& v) { return (v.x * v.x) + (v.y * v.y) + (v.z * v.z); };
    const auto  dot = [](const Vec3d& lhs, const Vec3d& rhs) { return (lhs.x * rhs.x) + (lhs.y * rhs.y) + (lhs.z * rhs.z); };

    const Vec3d to_a = offset(eye, end_a);
    const Vec3d to_b = offset(eye, end_b);
    const bool  a_is_nearer = squared_length(to_a) <= squared_length(to_b);
    const Vec3d near_point = a_is_nearer ? end_a : end_b;
    const Vec3d far_point  = a_is_nearer ? end_b : end_a;

    const Vec3d  along          = offset(near_point, far_point);
    const double squared_length_of_segment = squared_length(along);
    if (squared_length_of_segment < 1e-24) {
        return false; // the projection collapsed the picture to a point: there is no ray to give
    }
    const double length = std::sqrt(squared_length_of_segment);
    const Vec3d  unit(along.x / length, along.y / length, along.z / length);

    // PERSPECTIVE OR ORTHOGRAPHIC, answered by geometry rather than by a flag: a perspective's ray passes through the
    // eye (so it starts there), an orthographic one does not (so it starts where the pixel crosses the near plane).
    const Vec3d  to_near       = offset(eye, near_point);
    const double along_to_near = dot(to_near, unit);
    const Vec3d  perpendicular =
        Vec3d(to_near.x - (unit.x * along_to_near), to_near.y - (unit.y * along_to_near), to_near.z - (unit.z * along_to_near));
    const double scale        = std::max(1.0, std::sqrt(squared_length(to_near)));
    const bool   eye_on_the_ray = squared_length(perpendicular) <= (1e-18 * scale * scale);

    origin    = eye_on_the_ray ? eye : near_point;
    direction = unit;
    return true;
}

VN_GRAPHICS_NS_END
