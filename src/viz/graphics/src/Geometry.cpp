#include <vine/graphics/Geometry.hpp>

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

/**
 * @brief Packs a Vec3 array into a three-component float attribute buffer.
 *
 * @param src Typed vertex array.
 * @return Packed attribute buffer (components = 3).
 */
AttributeBuffer packVec3(const vine::geometry::Vec3fArray& src)
{
    AttributeBuffer out;
    out.components = 3;
    out.data       = std::make_shared<std::vector<float>>();
    out.data->reserve(src.size() * 3u);
    for (const auto& v : src) {
        out.data->push_back(v.x);
        out.data->push_back(v.y);
        out.data->push_back(v.z);
    }
    return out;
}
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

void Geometry::setPositions(const vine::geometry::Vec3fArray& positions)
{
    addBuffer(0, packVec3(positions));
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

void Geometry::setNormals(const vine::geometry::Vec3fArray& normals)
{
    addBuffer(1, packVec3(normals));
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

void Geometry::setIndices(std::shared_ptr<vine::geometry::UInt32Array> indices)
{
    indices_ = std::move(indices);
    ++revision_;
}

void Geometry::setIndices(const vine::geometry::UInt32Array& indices)
{
    setIndices(std::make_shared<vine::geometry::UInt32Array>(indices));
}

bool Geometry::hasIndices() const
{
    return indices_ != nullptr && !indices_->empty();
}

const vine::geometry::UInt32Array* Geometry::indices() const
{
    return indices_.get();
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

GeometryPtr geometryFromShape(const vine::geometry::Shape& shape)
{
    const auto* mesh = dynamic_cast<const vine::geometry::Mesh*>(&shape);
    if (mesh == nullptr) {
        return GeometryPtr();
    }
    auto geometry         = GeometryPtr(new Geometry());
    const auto& positions = mesh->positions();
    geometry->setPositions(positions);
    const auto& normals = mesh->normals();
    if (normals.size() == positions.size()) {
        geometry->setNormals(normals);
    }
    if (const auto* indexed =
            dynamic_cast<const vine::geometry::IndexedTriangleMesh*>(&shape)) {
        geometry->setIndices(indexed->indices());
    }
    return geometry;
}

V_GRAPHICS_NS_END
