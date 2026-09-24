#include <vine/geometry/IndexedTriangleMesh.hpp>

#include <utility>

VN_GEOMETRY_NS_BEGIN

VN_OBJECT_META_IMPL(IndexedTriangleMesh, Mesh)

IndexedTriangleMesh::IndexedTriangleMesh()
  : indices_(makeBuffer(UInt32Array{}))
{
    shape_type_ = ShapeType::IndexedTriangleMesh;
}

std::span<const std::uint32_t> IndexedTriangleMesh::indices() const
{
    return indices_->view();
}

intrusive_ptr<const Buffer<std::uint32_t>> IndexedTriangleMesh::indicesBuffer() const
{
    return indices_;
}

void IndexedTriangleMesh::setIndices(UInt32Array indices)
{
    indices_ = makeBuffer(std::move(indices));
}

std::uint32_t IndexedTriangleMesh::addVertex(const vn::math::Vec3f& position)
{
    positions_->push_back(position.x);
    positions_->push_back(position.y);
    positions_->push_back(position.z);
    // One announcement per vertex appended, not one per scalar pushed (see Mesh::announceChange).
    announceChange(positions_);
    return static_cast<std::uint32_t>(positions_->size() / kVec3Components - 1u);
}

void IndexedTriangleMesh::addTriangle(std::uint32_t i0, std::uint32_t i1, std::uint32_t i2)
{
    indices_->push_back(i0);
    indices_->push_back(i1);
    indices_->push_back(i2);
    announceChange(indices_);
}

void IndexedTriangleMesh::clear()
{
    clearAttributes();  // announces the attribute buffers it empties
    indices_->clear();
    announceChange(indices_);
}

std::size_t IndexedTriangleMesh::triangleCount() const
{
    return indices_->size() / 3;
}

bool IndexedTriangleMesh::isValid() const
{
    return !positions_->empty() && indices_->size() >= 3 && indices_->size() % 3 == 0;
}

VN_GEOMETRY_NS_END
