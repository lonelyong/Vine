#include <vine/graphics/MatrixTransform.hpp>

VN_GRAPHICS_NS_BEGIN

VN_OBJECT_META_IMPL(MatrixTransform, Group);

MatrixTransform::MatrixTransform() = default;

MatrixTransform::~MatrixTransform() = default;

Mat4d MatrixTransform::matrix() const
{
    return matrix_;
}

void MatrixTransform::setMatrix(const Mat4d& matrix)
{
    matrix_ = matrix;
    // The placement changed: every box above is stale (see Node::invalidateBounds).
    invalidateBounds();
}

Mat4d MatrixTransform::localTransformMatrix() const
{
    return matrix_;
}

VN_GRAPHICS_NS_END

