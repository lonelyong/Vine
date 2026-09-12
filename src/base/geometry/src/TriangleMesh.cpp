#include <vine/geometry/TriangleMesh.hpp>

V_GEOMETRY_NS_BEGIN

V_OBJECT_META_IMPL(TriangleMesh, Mesh)

TriangleMesh::TriangleMesh()
{
    shape_type_ = ShapeType::TriangleMesh;
}

void TriangleMesh::addTriangle(const vine::math::Vec3f& a, const vine::math::Vec3f& b, const vine::math::Vec3f& c)
{
    positions_->push_back(a);
    positions_->push_back(b);
    positions_->push_back(c);
}

void TriangleMesh::clear()
{
    clearAttributes();
}

std::size_t TriangleMesh::triangleCount() const
{
    return positions_->size() / 3;
}

bool TriangleMesh::isValid() const
{
    return !positions_->empty() && positions_->size() % 3 == 0;
}

V_GEOMETRY_NS_END
