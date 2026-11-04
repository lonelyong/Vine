#include <vine/geometry/Primitive.hpp>

VN_GEOMETRY_NS_BEGIN

VN_OBJECT_META_IMPL(Primitive, Shape)

VN_OBJECT_META_IMPL(Box, Primitive)

VN_OBJECT_META_IMPL(Cone, Primitive)

VN_OBJECT_META_IMPL(Cylinder, Primitive)

VN_OBJECT_META_IMPL(Ellipsoid, Primitive)

VN_OBJECT_META_IMPL(Sphere, Primitive)

Primitive::Primitive(ShapeType type)
{
    shape_type_ = type;
}

Box::Box()
  : Primitive(ShapeType::Box)
{}

Box::Box(float width, float height, float depth)
  : Primitive(ShapeType::Box)
  , width_(width)
  , height_(height)
  , depth_(depth)
{}

float Box::width() const noexcept
{
    return width_;
}

void Box::setWidth(float width) noexcept
{
    width_ = width;
}

float Box::height() const noexcept
{
    return height_;
}

void Box::setHeight(float height) noexcept
{
    height_ = height;
}

float Box::depth() const noexcept
{
    return depth_;
}

void Box::setDepth(float depth) noexcept
{
    depth_ = depth;
}

bool Box::isValid() const
{
    return width_ > 0.0f && height_ > 0.0f && depth_ > 0.0f;
}

bool Box::hasVolume(double eps) const
{
    const float tolerance = static_cast<float>(eps);
    return width_ > tolerance && height_ > tolerance && depth_ > tolerance;
}

Aabbf Box::bounds() const noexcept
{
    const float half_width  = width_ * 0.5f;
    const float half_height = height_ * 0.5f;
    const float half_depth  = depth_ * 0.5f;
    return Aabbf(-half_width, -half_height, -half_depth, half_width, half_height, half_depth);
}

Sphere::Sphere()
  : Primitive(ShapeType::Sphere)
{}

Sphere::Sphere(float radius)
  : Primitive(ShapeType::Sphere)
  , radius_(radius)
{}

float Sphere::radius() const noexcept
{
    return radius_;
}

void Sphere::setRadius(float radius) noexcept
{
    radius_ = radius;
}

bool Sphere::isValid() const
{
    return radius_ > 0.0f;
}

bool Sphere::hasVolume(double eps) const
{
    return radius_ > static_cast<float>(eps);
}

Aabbf Sphere::bounds() const noexcept
{
    return Aabbf(-radius_, -radius_, -radius_, radius_, radius_, radius_);
}

Cylinder::Cylinder()
  : Primitive(ShapeType::Cylinder)
{}

Cylinder::Cylinder(float radius, float height)
  : Primitive(ShapeType::Cylinder)
  , radius_(radius)
  , height_(height)
{}

float Cylinder::radius() const noexcept
{
    return radius_;
}

void Cylinder::setRadius(float radius) noexcept
{
    radius_ = radius;
}

float Cylinder::height() const noexcept
{
    return height_;
}

void Cylinder::setHeight(float height) noexcept
{
    height_ = height;
}

bool Cylinder::isValid() const
{
    return radius_ > 0.0f && height_ > 0.0f;
}

bool Cylinder::hasVolume(double eps) const
{
    const float tolerance = static_cast<float>(eps);
    return radius_ > tolerance && height_ > tolerance;
}

Aabbf Cylinder::bounds() const noexcept
{
    const float half_height = height_ * 0.5f;
    return Aabbf(-radius_, -radius_, -half_height, radius_, radius_, half_height);
}

Cone::Cone()
  : Primitive(ShapeType::Cone)
{}

Cone::Cone(float radius, float height)
  : Primitive(ShapeType::Cone)
  , radius_(radius)
  , height_(height)
{}

float Cone::radius() const noexcept
{
    return radius_;
}

void Cone::setRadius(float radius) noexcept
{
    radius_ = radius;
}

float Cone::height() const noexcept
{
    return height_;
}

void Cone::setHeight(float height) noexcept
{
    height_ = height;
}

bool Cone::isValid() const
{
    return radius_ > 0.0f && height_ > 0.0f;
}

bool Cone::hasVolume(double eps) const
{
    const float tolerance = static_cast<float>(eps);
    return radius_ > tolerance && height_ > tolerance;
}

Aabbf Cone::bounds() const noexcept
{
    const float half_height = height_ * 0.5f;
    return Aabbf(-radius_, -radius_, -half_height, radius_, radius_, half_height);
}

Ellipsoid::Ellipsoid()
  : Primitive(ShapeType::Ellipsoid)
{}

Ellipsoid::Ellipsoid(float radius_x, float radius_y, float radius_z)
  : Primitive(ShapeType::Ellipsoid)
  , radius_x_(radius_x)
  , radius_y_(radius_y)
  , radius_z_(radius_z)
{}

float Ellipsoid::radiusX() const noexcept
{
    return radius_x_;
}

void Ellipsoid::setRadiusX(float radius) noexcept
{
    radius_x_ = radius;
}

float Ellipsoid::radiusY() const noexcept
{
    return radius_y_;
}

void Ellipsoid::setRadiusY(float radius) noexcept
{
    radius_y_ = radius;
}

float Ellipsoid::radiusZ() const noexcept
{
    return radius_z_;
}

void Ellipsoid::setRadiusZ(float radius) noexcept
{
    radius_z_ = radius;
}

bool Ellipsoid::isValid() const
{
    return radius_x_ > 0.0f && radius_y_ > 0.0f && radius_z_ > 0.0f;
}

bool Ellipsoid::hasVolume(double eps) const
{
    const float tolerance = static_cast<float>(eps);
    return radius_x_ > tolerance && radius_y_ > tolerance && radius_z_ > tolerance;
}

Aabbf Ellipsoid::bounds() const noexcept
{
    return Aabbf(-radius_x_, -radius_y_, -radius_z_, radius_x_, radius_y_, radius_z_);
}

VN_GEOMETRY_NS_END
