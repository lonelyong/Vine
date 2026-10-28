#include <vine/graphics/PositionAttitudeTransform.hpp>

#include <vine/math/Transform3.hpp>

VN_GRAPHICS_NS_BEGIN

VN_OBJECT_META_IMPL(PositionAttitudeTransform, Group);

PositionAttitudeTransform::PositionAttitudeTransform() = default;

PositionAttitudeTransform::~PositionAttitudeTransform() = default;

Point3d PositionAttitudeTransform::position() const
{
    return position_;
}

void PositionAttitudeTransform::setPosition(const Point3d& position)
{
    position_ = position;
    recompose();
}

Quatd PositionAttitudeTransform::attitude() const
{
    return attitude_;
}

void PositionAttitudeTransform::setAttitude(const Quatd& attitude)
{
    attitude_ = attitude;
    recompose();
}

Vec3d PositionAttitudeTransform::scale() const
{
    return scale_;
}

void PositionAttitudeTransform::setScale(const Vec3d& scale)
{
    scale_ = scale;
    recompose();
}

Point3d PositionAttitudeTransform::pivot() const
{
    return pivot_;
}

void PositionAttitudeTransform::setPivot(const Point3d& pivot)
{
    pivot_ = pivot;
    recompose();
}

Isometry3d PositionAttitudeTransform::transform() const
{
    return Isometry3d(position_, attitude_);
}

void PositionAttitudeTransform::setTransform(const Isometry3d& transform)
{
    position_ = transform.translation;
    attitude_ = transform.rotation;
    recompose();
}

Mat4d PositionAttitudeTransform::matrix() const
{
    return matrix_;
}

Mat4d PositionAttitudeTransform::localTransformMatrix() const
{
    return matrix_;
}

void PositionAttitudeTransform::recompose()
{
    matrix_ = vn::math::translate(Vec3d(position_.x, position_.y, position_.z)) * vn::math::rotate(attitude_) * vn::math::scale(scale_) *
              vn::math::translate(Vec3d(-pivot_.x, -pivot_.y, -pivot_.z));
    // The placement changed: every box above is stale and every collected command would say a different
    // matrix (see Node::invalidateBounds).
    invalidateBounds();
}

VN_GRAPHICS_NS_END
