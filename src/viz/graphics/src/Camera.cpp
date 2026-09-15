#include <vine/graphics/Camera.hpp>

#include <vine/math/Transform3.hpp>

V_GRAPHICS_NS_BEGIN

V_OBJECT_META_IMPL(Camera, vine::Object);

Camera::Camera()
{
    setViewMatrixAsLookAt(eye_, center_, up_);
    setProjectionMatrixAsPerspective(fov_, aspect_ratio_, near_plane_, far_plane_);
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

void Camera::setViewMatrixAsLookAt(const Vec3d& eye, const Vec3d& center, const Vec3d& up)
{
    eye_ = eye;
    center_ = center;
    up_ = up.normalized();
    view_ = vine::math::lookAt(vine::math::Point3d(eye.x, eye.y, eye.z),
                               vine::math::Point3d(center.x, center.y, center.z),
                               up_);
}

void Camera::setProjectionMatrixAsPerspective(double fovy, double aspect, double zNear, double zFar)
{
    projection_type_ = ProjectionType::Perspective;
    fov_ = fovy;
    aspect_ratio_ = aspect;
    near_plane_ = zNear;
    far_plane_ = zFar;
    const double fov_rad = fovy * vine::math::DEG_TO_RAD;
    projection_ = vine::math::perspective<double>(fov_rad, aspect, zNear, zFar);
}

void Camera::setProjectionMatrixAsOrtho(double left, double right, double bottom, double top,
                                        double zNear, double zFar)
{
    projection_type_ = ProjectionType::Orthographic;
    // The window is kept whole (see the header): the height alone is enough for a query but not for a
    // consumer that rebuilds the projection, which would then render a centred frustum while the engine
    // culls with an off-centre one.
    ortho_left_   = left;
    ortho_right_  = right;
    ortho_bottom_ = bottom;
    ortho_top_    = top;
    near_plane_ = zNear;
    far_plane_  = zFar;
    projection_ = vine::math::ortho<double>(left, right, bottom, top, zNear, zFar);
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

double Camera::aspectRatio() const
{
    return aspect_ratio_;
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

Mat4d Camera::projectionMatrix() const
{
    return projection_;
}

Ray Camera::screenToWorldRay(const Vec2d& screenPos) const
{
    const double ndc_x = (2.0 * screenPos.x) - 1.0;
    const double ndc_y = 1.0 - (2.0 * screenPos.y);

    // The basis IS the view matrix's (its rows are right / up / backward — see vine::math::lookAt), so a
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
        const double x = ortho_left_ + ((ndc_x + 1.0) * 0.5) * (ortho_right_ - ortho_left_);
        const double y = ortho_bottom_ + ((ndc_y + 1.0) * 0.5) * (ortho_top_ - ortho_bottom_);
        const Vec3d  origin = eye_ + right * x + up * y;
        return Ray(origin, forward);
    }
    // Perspective: unproject through the near plane.
    const double fov_rad = fov_ * vine::math::DEG_TO_RAD;
    const double tan_half = std::tan(fov_rad * 0.5);
    const double x = ndc_x * tan_half * aspect_ratio_;
    const double y = ndc_y * tan_half;
    const Vec3d dir = (forward + right * x + up * y).normalized();
    return Ray(eye_, dir);
}

V_GRAPHICS_NS_END
