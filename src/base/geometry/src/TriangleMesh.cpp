#include <vine/geometry/TriangleMesh.hpp>

V_GEOMETRY_NS_BEGIN

V_OBJECT_META_IMPL(TriangleMesh, Mesh)

TriangleMesh::TriangleMesh()
{
    shape_type_ = ShapeType::TriangleMesh;
}

void TriangleMesh::addTriangle(const vine::math::Vec3f& a, const vine::math::Vec3f& b, const vine::math::Vec3f& c)
{
    const vine::math::Vec3f* const vertices[3] = { &a, &b, &c };
    for (const auto* vertex : vertices) {
        positions_->push_back(vertex->x);
        positions_->push_back(vertex->y);
        positions_->push_back(vertex->z);
    }
    // One announcement per triangle appended, not one per scalar pushed (see Mesh::announceChange).
    announceChange(positions_);
}

void TriangleMesh::clear()
{
    clearAttributes();
}

std::size_t TriangleMesh::triangleCount() const
{
    return positions_->size() / (3u * kVec3Components);
}

bool TriangleMesh::isValid() const
{
    return !positions_->empty() && positions_->size() % (3u * kVec3Components) == 0u;
}

V_GEOMETRY_NS_END
