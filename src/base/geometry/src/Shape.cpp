#include <vine/geometry/Shape.hpp>

VN_GEOMETRY_NS_BEGIN

VN_OBJECT_META_IMPL(Shape, vn::Object, vn::INameable)

Shape::Shape()
{}

bool Shape::isValid() const
{
    // A shape whose type was never assigned is not usable.
    return shape_type_ != ShapeType::Unknown;
}

bool Shape::hasVolume(double eps) const
{
    // Fallback for a primitive that does not restate its own dimensions: the family is closed by construction. Meshes
    // and B-reps fall through to false because neither tracks its surface closure, so the tolerance is not used here.
    (void)eps;
    switch (shape_type_) {
    case ShapeType::Box:
    case ShapeType::Cylinder:
    case ShapeType::Cone:
    case ShapeType::Sphere:
    case ShapeType::Ellipsoid: return true;
    default: return false;
    }
}

ShapeKind Shape::shapeKind() const
{
    switch (shape_type_) {
    case ShapeType::Box:
    case ShapeType::Cylinder:
    case ShapeType::Cone:
    case ShapeType::Sphere:
    case ShapeType::Ellipsoid: return ShapeKind::Primitive;
    case ShapeType::TriangleMesh:
    case ShapeType::QuadMesh:
    case ShapeType::MixedMesh: return ShapeKind::Mesh;
    case ShapeType::Brep: return ShapeKind::Brep;
    case ShapeType::PointCloud: return ShapeKind::PointCloud;
    default: return ShapeKind::Unknown;
    }
}

VN_GEOMETRY_NS_END