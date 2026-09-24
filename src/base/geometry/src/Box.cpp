#include <vine/geometry/Box.hpp>

VN_GEOMETRY_NS_BEGIN

VN_OBJECT_META_IMPL(Box, Primitive)

Box::Box()
{
    shape_type_ = ShapeType::Box;
}

Box::Box(double width, double height, double depth)
  : width_(width)
  , height_(height)
  , depth_(depth)
{
    shape_type_ = ShapeType::Box;
}

double Box::width() const
{
    return width_;
}

void Box::setWidth(double width)
{
    width_ = width;
}

double Box::height() const
{
    return height_;
}

void Box::setHeight(double height)
{
    height_ = height;
}

double Box::depth() const
{
    return depth_;
}

void Box::setDepth(double depth)
{
    depth_ = depth;
}

bool Box::isValid() const
{
    return width_ > 0.0 && height_ > 0.0 && depth_ > 0.0;
}

bool Box::hasVolume(double eps) const
{
    return width_ > eps && height_ > eps && depth_ > eps;
}

VN_GEOMETRY_NS_END
