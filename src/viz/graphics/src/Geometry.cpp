#include <vine/graphics/Geometry.hpp>

#include <span>
#include <vector>

#include <vine/graphics/Material.hpp>
#include <vine/graphics/ShaderProgram.hpp>
#include <vine/geometry/IndexedTriangleMesh.hpp>
#include <vine/geometry/Mesh.hpp>
#include <vine/math/Point3.hpp>
#include <vine/math/Transform3.hpp>
#include <vine/math/Vector3.hpp>

V_GRAPHICS_NS_BEGIN

using vine::math::Vec3d;

V_OBJECT_META_IMPL(Geometry, Node);

Geometry::Geometry() = default;

Geometry::~Geometry() = default;

namespace
{
/**
 * @brief Transforms a local-space AABB by a matrix into a world-space AABB.
 *
 * @param local Box in local space.
 * @param world World transform of the geometry.
 * @return World-space AABB (empty when the local box is empty).
 */
Aabbd transformBox(const Aabbd& local, const Mat4d& world)
{
    Aabbd result = Aabbd::empty();
    if (!local.isValid()) {
        return result;
    }
    const auto mn = local.min();
    const auto mx = local.max();
    const vine::math::Point3d corners[8] = {
        mn,
        vine::math::Point3d(mx.x, mn.y, mn.z),
        vine::math::Point3d(mn.x, mx.y, mn.z),
        vine::math::Point3d(mx.x, mx.y, mn.z),
        vine::math::Point3d(mn.x, mn.y, mx.z),
        vine::math::Point3d(mx.x, mn.y, mx.z),
        vine::math::Point3d(mn.x, mx.y, mx.z),
        mx,
    };
    for (const auto& c : corners) {
        const auto p = world * c;
        result.expandBy(Vec3d(p.x, p.y, p.z));
    }
    return result;
}

/// Scalars per vertex of an attribute channel — the mesh's own element layout, which sharing reads directly.
constexpr std::uint32_t kVec3Components = vine::geometry::Mesh::kVec3Components;
constexpr std::uint32_t kVec2Components = vine::geometry::Mesh::kVec2Components;

}  // namespace

void Geometry::addBuffer(std::uint32_t location, const AttributeBuffer& buffer)
{
    attributes_[location] = buffer;
    ++revision_;
}

void Geometry::removeBuffer(std::uint32_t location)
{
    if (attributes_.erase(location) != 0) {
        ++revision_;
    }
}

bool Geometry::hasBuffer(std::uint32_t location) const
{
    return attributes_.find(location) != attributes_.end();
}

const AttributeBuffer* Geometry::buffer(std::uint32_t location) const
{
    const auto it = attributes_.find(location);
    return it != attributes_.end() ? &it->second : nullptr;
}

std::size_t Geometry::bufferCount() const
{
    return attributes_.size();
}

std::vector<std::uint32_t> Geometry::bufferLocations() const
{
    std::vector<std::uint32_t> locations;
    locations.reserve(attributes_.size());
    for (const auto& entry : attributes_) {
        locations.push_back(entry.first);
    }
    return locations;
}

void Geometry::setPositions(intrusive_ptr<const vine::Buffer<float>> positions)
{
    addBuffer(0, AttributeBuffer::shared(std::move(positions), kVec3Components));
}

bool Geometry::hasPositions() const
{
    return hasBuffer(0);
}

std::size_t Geometry::positionCount() const
{
    const AttributeBuffer* positions = buffer(0);
    return positions != nullptr ? positions->vertexCount() : 0u;
}

void Geometry::setNormals(intrusive_ptr<const vine::Buffer<float>> normals)
{
    addBuffer(1, AttributeBuffer::shared(std::move(normals), kVec3Components));
}

bool Geometry::hasNormals() const
{
    return hasBuffer(1);
}

std::size_t Geometry::normalCount() const
{
    const AttributeBuffer* normals = buffer(1);
    return normals != nullptr ? normals->vertexCount() : 0u;
}

void Geometry::setTexcoords(intrusive_ptr<const vine::Buffer<float>> texcoords)
{
    addBuffer(kTexCoordLocation, AttributeBuffer::shared(std::move(texcoords), kVec2Components));
}

bool Geometry::hasTexcoords() const
{
    return hasBuffer(kTexCoordLocation);
}

std::size_t Geometry::texcoordCount() const
{
    const AttributeBuffer* texcoords = buffer(kTexCoordLocation);
    return texcoords != nullptr ? texcoords->vertexCount() : 0u;
}

void Geometry::setIndices(intrusive_ptr<const vine::Buffer<std::uint32_t>> indices)
{
    indices_ = std::move(indices);
    ++revision_;
}

bool Geometry::hasIndices() const
{
    return indices_ != nullptr && !indices_->empty();
}

std::span<const std::uint32_t> Geometry::indices() const
{
    return indices_ != nullptr ? indices_->view() : std::span<const std::uint32_t>{};
}

std::uint64_t Geometry::revision() const
{
    return revision_;
}

std::size_t Geometry::vertexCount() const
{
    return positionCount();
}

namespace
{
/**
 * @brief Computes the local-space AABB of the location-0 position buffer.
 *
 * The buffer's declared components are its stride, so a vec4 position channel
 * contributes its xyz and skips the trailing w (mirroring the backend, which
 * unpacks locations the same way). A channel that cannot carry xyz (< 3
 * components) bounds nothing — previously the loop assumed three floats per
 * vertex, which mis-read vec4 data and produced a WRONG bound (and therefore
 * wrong frustum culling) for such geometry.
 *
 * @param geometry Geometry to bound.
 * @return Local-space AABB (empty when no usable positions are present).
 */
Aabbd localBounds(const Geometry* geometry)
{
    const AttributeBuffer* positions = geometry->buffer(0);
    if (positions == nullptr || positions->empty() || positions->components < 3u) {
        return Aabbd::empty();
    }
    Aabbd box = Aabbd::empty();
    const std::size_t count = positions->vertexCount();
    for (std::size_t v = 0; v < count; ++v) {
        const std::array<float, 3> p = positions->xyz(v);
        box.expandBy(Vec3d(p[0], p[1], p[2]));
    }
    return box;
}
}  // namespace

raw_ptr<Material> Geometry::material() const
{
    return material_.get();
}

void Geometry::setMaterial(intrusive_ptr<Material> m)
{
    material_ = std::move(m);
}

raw_ptr<ShaderProgram> Geometry::program() const
{
    return program_.get();
}

void Geometry::setProgram(intrusive_ptr<ShaderProgram> program)
{
    program_ = std::move(program);
}

Aabbd Geometry::boundingBox() const
{
    // The local data box (location 0 positions) placed in world space by the
    // enclosing MatrixTransform chain.
    return transformBox(localBounds(this), worldMatrix());
}

intrusive_ptr<Buffer<float>> packAttribute(std::span<const vine::math::Vec3f> vertices)
{
    std::vector<float> scalars;
    scalars.reserve(vertices.size() * kVec3Components);
    for (const auto& v : vertices) {
        scalars.push_back(v.x);
        scalars.push_back(v.y);
        scalars.push_back(v.z);
    }
    return intrusive_ptr<Buffer<float>>(new Buffer<float>(std::move(scalars)));
}

intrusive_ptr<Buffer<float>> packAttribute(std::span<const vine::math::Vec2f> vertices)
{
    std::vector<float> scalars;
    scalars.reserve(vertices.size() * kVec2Components);
    for (const auto& v : vertices) {
        scalars.push_back(v.x);
        scalars.push_back(v.y);
    }
    return intrusive_ptr<Buffer<float>>(new Buffer<float>(std::move(scalars)));
}

intrusive_ptr<Buffer<std::uint32_t>> packIndices(std::span<const std::uint32_t> indices)
{
    return intrusive_ptr<Buffer<std::uint32_t>>(
        new Buffer<std::uint32_t>(std::vector<std::uint32_t>(indices.begin(), indices.end())));
}

GeometryPtr geometryFromShape(const vine::geometry::Shape& shape)
{
    const auto* mesh = dynamic_cast<const vine::geometry::Mesh*>(&shape);
    if (mesh == nullptr) {
        return GeometryPtr();
    }
    auto       geometry  = GeometryPtr(new Geometry());
    const auto positions = mesh->positions();
    const auto normals   = mesh->normals();
    const auto texcoords = mesh->texcoords();

    // The mesh OWNS the vertex data and the geometry BORROWS it, so both sides read ONE allocation. The
    // mesh already stores the scalars, so there is no conversion to do and nothing to repack.
    geometry->setPositions(mesh->positionsBuffer());
    if (normals.size() == positions.size()) {
        geometry->setNormals(mesh->normalsBuffer());
    }
    if (texcoords.size() == positions.size()) {
        geometry->setTexcoords(mesh->texcoordsBuffer());
    }
    if (const auto* indexed =
            dynamic_cast<const vine::geometry::IndexedTriangleMesh*>(&shape)) {
        geometry->setIndices(indexed->indicesBuffer());
    }
    return geometry;
}

V_GRAPHICS_NS_END
