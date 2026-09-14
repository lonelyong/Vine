#include <vine/graphics/Geometry.hpp>

#include <algorithm>
#include <span>
#include <vector>

#include <vine/graphics/Material.hpp>
#include <vine/graphics/ShaderAbi.hpp>
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

void Geometry::addBuffer(std::uint32_t location, const AttributeChannel& buffer)
{
    attributes_[location] = buffer;
}

void Geometry::removeBuffer(std::uint32_t location)
{
    attributes_.erase(location);
}

bool Geometry::hasBuffer(std::uint32_t location) const
{
    return attributes_.find(location) != attributes_.end();
}

const AttributeChannel* Geometry::buffer(std::uint32_t location) const
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
    // The location is the shader ABI's (ShaderAbi.hpp), never a number written here: the built-in
    // shaders declare the attribute where attributeLocation() says, so the two move together.
    addBuffer(attributeLocation(VertexAttribute::Position),
              AttributeChannel::shared(std::move(positions), kVec3Components));
}

bool Geometry::hasPositions() const
{
    return hasBuffer(attributeLocation(VertexAttribute::Position));
}

std::size_t Geometry::positionCount() const
{
    const AttributeChannel* positions = buffer(attributeLocation(VertexAttribute::Position));
    return positions != nullptr ? positions->vertexCount() : 0u;
}

void Geometry::setPositions(intrusive_ptr<const vine::Buffer<float>> positions, std::size_t first_vertex,
                            std::size_t vertex_count)
{
    // ONE implementation path for both spellings: the segment overload is the general door
    // (addBuffer + AttributeChannel::slice), so a segment cannot come to mean one thing here and
    // another there. The stride is the role's, and slice() takes VERTICES.
    addBuffer(attributeLocation(VertexAttribute::Position),
              AttributeChannel::slice(std::move(positions), kVec3Components, first_vertex, vertex_count));
}

void Geometry::setNormals(intrusive_ptr<const vine::Buffer<float>> normals)
{
    addBuffer(attributeLocation(VertexAttribute::Normal),
              AttributeChannel::shared(std::move(normals), kVec3Components));
}

bool Geometry::hasNormals() const
{
    return hasBuffer(attributeLocation(VertexAttribute::Normal));
}

std::size_t Geometry::normalCount() const
{
    const AttributeChannel* normals = buffer(attributeLocation(VertexAttribute::Normal));
    return normals != nullptr ? normals->vertexCount() : 0u;
}

void Geometry::setNormals(intrusive_ptr<const vine::Buffer<float>> normals, std::size_t first_vertex,
                          std::size_t vertex_count)
{
    addBuffer(attributeLocation(VertexAttribute::Normal),
              AttributeChannel::slice(std::move(normals), kVec3Components, first_vertex, vertex_count));
}

void Geometry::setTexcoords2(intrusive_ptr<const vine::Buffer<float>> texcoords)
{
    addBuffer(kTexCoordLocation, AttributeChannel::shared(std::move(texcoords), kVec2Components));
}

bool Geometry::hasTexcoords() const
{
    return hasBuffer(kTexCoordLocation);
}

std::size_t Geometry::texcoordCount() const
{
    const AttributeChannel* texcoords = buffer(kTexCoordLocation);
    return texcoords != nullptr ? texcoords->vertexCount() : 0u;
}

std::uint32_t Geometry::texcoordComponents() const
{
    // The width of the slot, straight off the channel: 2 (setTexcoords2), 3 (setTexcoords3) or 0 when the
    // slot is empty. It is what a consumer reads to know which sampler the data is shaped for.
    const AttributeChannel* texcoords = buffer(kTexCoordLocation);
    return texcoords != nullptr ? texcoords->components : 0u;
}

void Geometry::setTexcoords2(intrusive_ptr<const vine::Buffer<float>> texcoords, std::size_t first_vertex,
                             std::size_t vertex_count)
{
    // Two scalars per vertex here, and a caller states VERTICES: a segment of three vertices is six scalars.
    addBuffer(kTexCoordLocation,
              AttributeChannel::slice(std::move(texcoords), kVec2Components, first_vertex, vertex_count));
}

void Geometry::setTexcoords3(intrusive_ptr<const vine::Buffer<float>> texcoords)
{
    // The SAME slot as setTexcoords2, with the other width: three scalars per vertex, which is what a cube
    // map is sampled by direction with. What the width MEANS stays the sampler's business (a user program
    // may read these three as a volume coordinate); the engine's own program reads it as a direction.
    addBuffer(kTexCoordLocation, AttributeChannel::shared(std::move(texcoords), kVec3Components));
}

void Geometry::setTexcoords3(intrusive_ptr<const vine::Buffer<float>> texcoords, std::size_t first_vertex,
                             std::size_t vertex_count)
{
    addBuffer(kTexCoordLocation,
              AttributeChannel::slice(std::move(texcoords), kVec3Components, first_vertex, vertex_count));
}

void Geometry::setIndices(intrusive_ptr<const vine::Buffer<std::uint32_t>> indices)
{
    // One implementation path for both spellings: the whole-buffer form IS the segment form with the
    // whole range stated, so the two cannot drift apart.
    setIndices(std::move(indices), 0u, 0u);
}

void Geometry::setIndices(intrusive_ptr<const vine::Buffer<std::uint32_t>> indices, std::size_t first_index,
                          std::size_t index_count)
{
    // The index stream is a SEGMENT of a buffer, described by the same structure an attribute
    // channel is (see BufferSlice): which buffer, where the segment starts, how much it covers —
    // so "the rest of the arena", "clamp past the end" and "follow the buffer as it grows" have one
    // definition for every stream a geometry reads, index or vertex, today or later.
    indices_ = Geometry::IndexStream::slice(std::move(indices), first_index, index_count);
}

bool Geometry::hasIndices() const
{
    return !indices_.empty();
}

std::span<const std::uint32_t> Geometry::indices() const
{
    return indices_.span();
}

std::size_t Geometry::firstIndex() const
{
    return indices_.begin();
}

std::size_t Geometry::indexCount() const
{
    return indices_.size();
}

intrusive_ptr<const vine::Buffer<std::uint32_t>> Geometry::indicesBuffer() const
{
    return indices_.values;
}

std::uint64_t Geometry::revision() const
{
    return revision_;
}

void Geometry::setRevision(std::uint64_t revision) noexcept
{
    revision_ = revision;
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
    const AttributeChannel* positions = geometry->buffer(attributeLocation(VertexAttribute::Position));
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
    auto       geometry  = make_intrusive<Geometry>();
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
        geometry->setTexcoords2(mesh->texcoordsBuffer());
    }
    if (const auto* indexed =
            dynamic_cast<const vine::geometry::IndexedTriangleMesh*>(&shape)) {
        geometry->setIndices(indexed->indicesBuffer());
    }
    return geometry;
}

V_GRAPHICS_NS_END
