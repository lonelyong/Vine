#include <vine/graphics/AutoTransform.hpp>

#include <algorithm>
#include <cmath>

#include <vine/math/Transform3.hpp>

VN_GRAPHICS_NS_BEGIN

namespace
{

/**
 * @brief Builds a rotation matrix whose columns are the given local axes in world space.
 *
 * @param x Local +X in world space.
 * @param y Local +Y in world space.
 * @param z Local +Z in world space.
 * @return The rotation matrix (translation zero, bottom-right one).
 */
Mat4d rotationFromBasis(const Vec3d& x, const Vec3d& y, const Vec3d& z)
{
    Mat4d rotation;
    rotation.element(0, 0) = x.x;
    rotation.element(0, 1) = y.x;
    rotation.element(0, 2) = z.x;
    rotation.element(1, 0) = x.y;
    rotation.element(1, 1) = y.y;
    rotation.element(1, 2) = z.y;
    rotation.element(2, 0) = x.z;
    rotation.element(2, 1) = y.z;
    rotation.element(2, 2) = z.z;
    return rotation;
}

/**
 * @brief Returns the inverse of the camera's view rotation (a screen-aligned billboard rotation).
 *
 * @param view The camera's world-to-view matrix (rigid: rotation plus translation).
 * @return The rotation whose local axes are the camera's world-space right / up / backward axes.
 */
Mat4d screenRotation(const Mat4d& view)
{
    Mat4d rotation;
    for (std::size_t row = 0; row < 3; ++row) {
        for (std::size_t col = 0; col < 3; ++col) {
            rotation.element(row, col) = view.element(col, row);
        }
    }
    return rotation;
}

/**
 * @brief Returns a rotation whose local +Z points from @p world_position at @p eye.
 *
 * @param world_position The billboard's world position.
 * @param eye            The camera's eye point.
 * @param up             The camera's up direction (keeps the billboard from rolling).
 * @return The world-space billboard rotation.
 */
Mat4d cameraRotation(const Point3d& world_position, const Vec3d& eye, const Vec3d& up)
{
    Vec3d to_eye = eye - Vec3d(world_position.x, world_position.y, world_position.z);
    if (to_eye.length2() <= 0.0) {
        // The eye sits ON the billboard: any orientation is as good as another. Use the camera's up as the
        // normal so the result stays finite instead of normalizing a zero vector.
        to_eye = up;
    }
    const Vec3d z = to_eye.normalized();
    Vec3d       x = up.cross(z);
    if (x.length2() <= 0.0) {
        // Looking straight along the up axis: choose a stable perpendicular instead of rolling arbitrarily.
        x = Vec3d(0.0, 1.0, 0.0).cross(z);
        if (x.length2() <= 0.0) {
            x = Vec3d(1.0, 0.0, 0.0).cross(z);
        }
    }
    x = x.normalized();
    const Vec3d y = z.cross(x);
    return rotationFromBasis(x, y, z);
}

}  // namespace

VN_OBJECT_META_IMPL(AutoTransform, Group);

AutoTransform::AutoTransform() = default;

AutoTransform::~AutoTransform() = default;

Point3d AutoTransform::position() const
{
    return position_;
}

void AutoTransform::setPosition(const Point3d& position)
{
    position_ = position;
    recompose();
}

Quatd AutoTransform::rotation() const
{
    return rotation_;
}

void AutoTransform::setRotation(const Quatd& rotation)
{
    rotation_ = rotation;
    recompose();
}

Vec3d AutoTransform::scale() const
{
    return scale_;
}

void AutoTransform::setScale(const Vec3d& scale)
{
    scale_ = scale;
    recompose();
}

Point3d AutoTransform::pivot() const
{
    return pivot_;
}

void AutoTransform::setPivot(const Point3d& pivot)
{
    pivot_ = pivot;
    recompose();
}

AutoTransform::AutoRotateMode AutoTransform::autoRotateMode() const
{
    return mode_;
}

void AutoTransform::setAutoRotateMode(AutoRotateMode mode)
{
    mode_ = mode;
    recompose();
}

Mat4d AutoTransform::matrix() const
{
    return matrix_;
}

Mat4d AutoTransform::localTransformMatrix() const
{
    return matrix_;
}

bool AutoTransform::localToWorldForView(const Mat4d& parent_world, const Mat4d& view, const Vec3d& eye,
                                        const Vec3d& up, Mat4d& world) const
{
    if (mode_ == AutoRotateMode::NoRotation) {
        return false;
    }

    // The position is in the parent frame; the automatic rotation is world-oriented, so only the parent's
    // placement (not its rotation or scale) reaches the billboard.
    const Point3d world_position = parent_world * position_;
    const Mat4d   auto_rotation =
        mode_ == AutoRotateMode::RotateToScreen ? screenRotation(view) : cameraRotation(world_position, eye, up);

    world = vn::math::translate(Vec3d(world_position.x, world_position.y, world_position.z)) * auto_rotation *
            vn::math::rotate(rotation_) * vn::math::scale(scale_) *
            vn::math::translate(Vec3d(-pivot_.x, -pivot_.y, -pivot_.z));
    return true;
}

Aabbd AutoTransform::boundingBox() const
{
    // An automatic billboard can point in any direction; a sphere around position covers every orientation.
    // The subtree box is already in this node's own frame (pivot and scale included), so its farthest corner
    // is the radius to use.
    Aabbd local = Aabbd::empty();
    if (!subtreeBounds(local) || !local.isValid()) {
        return Group::boundingBox();
    }

    const auto   mn = local.min();
    const auto   mx = local.max();
    double       radius = 0.0;
    const double xs[2]   = { mn.x, mx.x };
    const double ys[2]   = { mn.y, mx.y };
    const double zs[2]   = { mn.z, mx.z };
    for (const double x : xs) {
        for (const double y : ys) {
            for (const double z : zs) {
                // The node's own transform is T(position) * R * S * T(-pivot): measure from the pivot and
                // apply the scale before asking for a length, or a scaled billboard would under-report.
                const Vec3d offset((x - pivot_.x) * scale_.x, (y - pivot_.y) * scale_.y,
                                   (z - pivot_.z) * scale_.z);
                radius = std::max(radius, offset.length());
            }
        }
    }

    const Mat4d   parent_world = parent() != nullptr ? parent()->worldMatrix() : Mat4d();
    const Point3d world_position = parent_world * position_;
    Aabbd         box            = Aabbd::empty();
    box.expandBy(Point3d(world_position.x - radius, world_position.y - radius, world_position.z - radius));
    box.expandBy(Point3d(world_position.x + radius, world_position.y + radius, world_position.z + radius));
    return box;
}

void AutoTransform::recompose()
{
    matrix_ = vn::math::translate(Vec3d(position_.x, position_.y, position_.z)) * vn::math::rotate(rotation_) *
              vn::math::scale(scale_) * vn::math::translate(Vec3d(-pivot_.x, -pivot_.y, -pivot_.z));
    // The placement changed: every box above is stale and every collected command would say a different
    // matrix (see Node::invalidateBounds).
    invalidateBounds();
}

VN_GRAPHICS_NS_END
