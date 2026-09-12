#include <vine/geometry/Mesh.hpp>

#include <utility>

V_GEOMETRY_NS_BEGIN

V_OBJECT_META_IMPL(Mesh, Shape)

Mesh::Mesh()
  : positions_(makeBuffer(Vec3fArray{}))
  , normals_(makeBuffer(Vec3fArray{}))
  , texcoords_(makeBuffer(Vec2fArray{}))
{
}

std::span<const vine::math::Vec3f> Mesh::positions() const
{
    return positions_->view();
}

std::span<const vine::math::Vec3f> Mesh::normals() const
{
    return normals_->view();
}

std::span<const vine::math::Vec2f> Mesh::texcoords() const
{
    return texcoords_->view();
}

intrusive_ptr<const Buffer<vine::math::Vec3f>> Mesh::positionsBuffer() const
{
    return positions_;
}

intrusive_ptr<const Buffer<vine::math::Vec3f>> Mesh::normalsBuffer() const
{
    return normals_;
}

intrusive_ptr<const Buffer<vine::math::Vec2f>> Mesh::texcoordsBuffer() const
{
    return texcoords_;
}

void Mesh::setPositions(Vec3fArray positions)
{
    positions_ = makeBuffer(std::move(positions));
}

void Mesh::setNormals(Vec3fArray normals)
{
    normals_ = makeBuffer(std::move(normals));
}

void Mesh::setTexcoords(Vec2fArray texcoords)
{
    texcoords_ = makeBuffer(std::move(texcoords));
}

std::size_t Mesh::vertexCount() const
{
    return positions_->size();
}

const Aabbf& Mesh::aabb() const
{
    return aabb_;
}

void Mesh::setAabb(const Aabbf& aabb)
{
    aabb_ = aabb;
}

Aabbf Mesh::computeAabb()
{
    aabb_ = Aabbf::compute(positions_->view());
    return aabb_;
}

void Mesh::clearAttributes()
{
    positions_->clear();
    normals_->clear();
    texcoords_->clear();
    aabb_ = Aabbf::empty();
}

V_GEOMETRY_NS_END
