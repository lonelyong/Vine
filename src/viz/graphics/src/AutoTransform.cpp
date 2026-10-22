#include <vine/graphics/AutoTransform.hpp>

#include <vine/math/Transform3.hpp>

VN_GRAPHICS_NS_BEGIN

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

Isometry3d AutoTransform::transform() const
{
    return Isometry3d(position_, rotation_);
}

void AutoTransform::setTransform(const Isometry3d& transform)
{
    position_ = transform.translation;
    rotation_ = transform.rotation;
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

void AutoTransform::recompose()
{
    matrix_ = vn::math::translate(Vec3d(position_.x, position_.y, position_.z)) * vn::math::rotate(rotation_) *
              vn::math::scale(scale_);
    // The placement changed: every box above is stale and every collected command would say a different
    // matrix (see Node::invalidateBounds).
    invalidateBounds();
}

VN_GRAPHICS_NS_END
