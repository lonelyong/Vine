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

VN_GRAPHICS_NS_BEGIN

using vn::math::Vec3d;

VN_OBJECT_META_IMPL(Geometry, Node);

Geometry::Geometry() = default;

Geometry::~Geometry() = default;

namespace
{
/// Scalars per vertex of an attribute channel — the mesh's own element layout, which sharing reads directly.
constexpr std::uint32_t kVec3Components = vn::geometry::Mesh::kVec3Components;
constexpr std::uint32_t kVec2Components = vn::geometry::Mesh::kVec2Components;

}  // namespace

void Geometry::addBuffer(std::uint32_t location, const AttributeChannel& buffer)
{
    attributes_[location] = buffer;
    // DATA IMPLIES A DRAW, and this is the ONE door every channel comes through (setPositions and the other
    // convenience setters are addBuffer with a stated location): positions are the one required channel, so
    // installing them states the default draw (triangles, non-indexed) - which is what a single-part geometry
    // means, and what makes "I set data and nothing was drawn" impossible. A caller that wants another
    // topology or several parts edits/appends its primitives.
    if (location == attributeLocation(VertexAttribute::Position) && primitives_.empty()) {
        primitives_.emplace_back();
    }
    // The data box may have changed: the cached boxes above this leaf are stale (see
    // Node::invalidateBounds).
    invalidateBounds();
}

void Geometry::removeBuffer(std::uint32_t location)
{
    attributes_.erase(location);
    invalidateBounds();
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

void Geometry::setPositions(intrusive_ptr<const vn::Buffer<float>> positions, std::size_t first_vertex,
                            std::size_t vertex_count)
{
    // The location is the shader ABI's (ShaderAbi.hpp), never a number written here: the built-in
    // shaders declare the attribute where attributeLocation() says, so the two move together.
    // ONE door: a whole buffer is the segment form with the whole range stated (slice() takes VERTICES),
    // so "which part of an arena" cannot come to mean one thing here and another there.
    addBuffer(attributeLocation(VertexAttribute::Position),
              AttributeChannel::slice(std::move(positions), kVec3Components, first_vertex, vertex_count));
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

void Geometry::setNormals(intrusive_ptr<const vn::Buffer<float>> normals, std::size_t first_vertex,
                          std::size_t vertex_count)
{
    addBuffer(attributeLocation(VertexAttribute::Normal),
              AttributeChannel::slice(std::move(normals), kVec3Components, first_vertex, vertex_count));
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

void Geometry::setTexcoords2(intrusive_ptr<const vn::Buffer<float>> texcoords, std::size_t first_vertex,
                             std::size_t vertex_count)
{
    // Two scalars per vertex here, and a caller states VERTICES: a segment of three vertices is six scalars.
    addBuffer(kTexCoordLocation,
              AttributeChannel::slice(std::move(texcoords), kVec2Components, first_vertex, vertex_count));
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

void Geometry::setTexcoords3(intrusive_ptr<const vn::Buffer<float>> texcoords, std::size_t first_vertex,
                             std::size_t vertex_count)
{
    // The SAME slot as setTexcoords2, with the other width: three scalars per vertex, which is what a cube
    // map is sampled by direction with. What the width MEANS stays the sampler's business (a user program
    // may read these three as a volume coordinate); the engine's own program reads it as a direction.
    addBuffer(kTexCoordLocation,
              AttributeChannel::slice(std::move(texcoords), kVec3Components, first_vertex, vertex_count));
}

namespace
{
/// @brief Whether two primitives describe the same draw (an assignment that changes nothing).
bool samePrimitive(const Primitive& a, const Primitive& b) noexcept
{
    return a.topology == b.topology && a.indices.values.get() == b.indices.values.get() &&
           a.indices.first == b.indices.first && a.indices.count == b.indices.count &&
           a.material.get() == b.material.get() && a.program.get() == b.program.get();
}
}  // namespace

void Geometry::addPrimitive(Primitive primitive)
{
    primitives_.push_back(std::move(primitive));
    // A new draw of the same data is a different picture (and possibly a different extent): invalidateBounds()
    // announces the content change as well, so the frame memo cannot answer from the list it built before this call.
    invalidateBounds();
}

void Geometry::setPrimitive(std::size_t index, Primitive primitive)
{
    if (index >= primitives_.size()) {
        ++ignored_primitive_edits_;
        return;
    }
    if (samePrimitive(primitives_[index], primitive)) {
        return;   // an assignment that changes nothing must not cost the frame's collection
    }
    primitives_[index] = std::move(primitive);
    invalidateBounds();
}

void Geometry::clearPrimitives()
{
    if (primitives_.empty()) {
        return;
    }
    primitives_.clear();
    invalidateBounds();
}

const Primitive* Geometry::primitive(std::size_t index) const
{
    return index < primitives_.size() ? &primitives_[index] : nullptr;
}

std::size_t Geometry::primitiveCount() const
{
    return primitives_.size();
}

std::span<const Primitive> Geometry::primitives() const
{
    return primitives_;
}

std::uint64_t Geometry::ignoredPrimitiveEdits() const noexcept
{
    return ignored_primitive_edits_;
}

void Geometry::setIndices(intrusive_ptr<const vn::Buffer<std::uint32_t>> indices, std::size_t first_index,
                          std::size_t index_count)
{
    Primitive draw;
    if (const Primitive* existing = primitive(0)) {
        draw = *existing;   // the other fields of the one-draw spelling stay what they were
    }
    draw.indices = Primitive::IndexSpan::slice(std::move(indices), first_index, index_count);
    if (primitiveCount() == 0) {
        addPrimitive(std::move(draw));
        return;
    }
    setPrimitive(0, std::move(draw));
}

bool Geometry::hasIndices() const
{
    const Primitive* draw = primitive(0);
    return draw != nullptr && draw->isIndexed();
}

std::span<const std::uint32_t> Geometry::indices() const
{
    const Primitive* draw = primitive(0);
    return draw != nullptr ? draw->indices.span() : std::span<const std::uint32_t>{};
}

std::size_t Geometry::firstIndex() const
{
    const Primitive* draw = primitive(0);
    return draw != nullptr ? draw->indices.begin() : 0U;
}

std::size_t Geometry::indexCount() const
{
    const Primitive* draw = primitive(0);
    return draw != nullptr ? draw->indexCount() : 0U;
}

intrusive_ptr<const vn::Buffer<std::uint32_t>> Geometry::indicesBuffer() const
{
    const Primitive* draw = primitive(0);
    return draw != nullptr ? draw->indices.values : intrusive_ptr<const vn::Buffer<std::uint32_t>>{};
}

raw_ptr<Material> Geometry::material() const
{
    const Primitive* draw = primitive(0);
    return draw != nullptr ? draw->material.get() : nullptr;
}

void Geometry::setMaterial(intrusive_ptr<Material> m)
{
    if (primitiveCount() == 0) {
        Primitive draw;
        draw.material = std::move(m);
        addPrimitive(std::move(draw));
        return;
    }
    for (std::size_t index = 0; index < primitiveCount(); ++index) {
        Primitive draw = *primitive(index);
        draw.material  = m;
        setPrimitive(index, std::move(draw));
    }
}

raw_ptr<ShaderProgram> Geometry::program() const
{
    const Primitive* draw = primitive(0);
    return draw != nullptr ? draw->program.get() : nullptr;
}

void Geometry::setProgram(intrusive_ptr<ShaderProgram> program)
{
    if (primitiveCount() == 0) {
        Primitive draw;
        draw.program = std::move(program);
        addPrimitive(std::move(draw));
        return;
    }
    for (std::size_t index = 0; index < primitiveCount(); ++index) {
        Primitive draw = *primitive(index);
        draw.program   = program;
        setPrimitive(index, std::move(draw));
    }
}

std::uint64_t Geometry::revision() const
{
    return revision_;
}

void Geometry::setRevision(std::uint64_t revision) noexcept
{
    revision_ = revision;
    // The announced revision keys the data box AND every cached box above this leaf: a host that edits
    // bytes through a buffer announces here (see Node::invalidateBounds).
    invalidateBounds();
}

void Geometry::bumpRevision() noexcept
{
    ++revision_;
    invalidateBounds();
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
Aabbd scanLocalBounds(const Geometry* geometry)
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

Aabbd Geometry::localBounds() const
{
    // The bound of the location-0 positions in the leaf's OWN frame: the"data-derived" half of
    // boundingBox(), and the half a collection walk can place itself (it already holds the accumulated
    // world matrix - see Scene.cpp).
    //
    // The LOCAL half is remembered between calls: the pass asks every geometry for its bound on every
    // frame it walks (a moving camera invalidates the frame memo), and scanning the vertices for a box
    // that nothing announced a change to was O(visible vertices) per frame — the whole cost of frustum
    // culling, paid before any culling decision could save anything. The keys are the stream the box was
    // computed from plus this geometry's revision, so a writer that announces an edit gets a fresh box
    // (the same "announce it and consumers may cache" contract every other cache here follows).
    const AttributeChannel* positions = buffer(attributeLocation(VertexAttribute::Position));
    const void* const       buffer_ptr = positions != nullptr ? positions->values.get() : nullptr;
    const std::uint64_t     buffer_revision =
        positions != nullptr && positions->values != nullptr ? positions->values->revision() : 0u;
    const std::size_t positions_offset = positions != nullptr ? positions->offset : 0u;
    const std::size_t positions_scalars = positions != nullptr ? positions->scalarCount : 0u;

    const bool cached = local_bounds_.valid && local_bounds_.buffer == buffer_ptr &&
                        local_bounds_.revision == buffer_revision &&
                        local_bounds_.offset == positions_offset && local_bounds_.scalars == positions_scalars &&
                        local_bounds_.geometry_revision == revision_;
    if (!cached) {
        local_bounds_.buffer            = buffer_ptr;
        local_bounds_.revision          = buffer_revision;
        local_bounds_.offset            = positions_offset;
        local_bounds_.scalars           = positions_scalars;
        local_bounds_.geometry_revision = revision_;
        local_bounds_.box               = scanLocalBounds(this);
        local_bounds_.valid             = true;
        ++local_bounds_computations_;
    }
    return local_bounds_.box;
}

Aabbd Geometry::boundingBox() const
{
    // The same LOCAL box a collection walk transforms with the matrix IT accumulated (see Scene.cpp's
    // bounds cache), placed by this leaf's own worldMatrix(): one spelling of "place the box"
    // (transformBox), so the walk's answer and this one cannot disagree (a test pins it).
    return transformBox(localBounds(), worldMatrix());
}

std::uint64_t Geometry::localBoundsComputationCount() const noexcept
{
    return local_bounds_computations_;
}

intrusive_ptr<Buffer<float>> packAttribute(std::span<const vn::math::Vec3f> vertices)
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

intrusive_ptr<Buffer<float>> packAttribute(std::span<const vn::math::Vec2f> vertices)
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

GeometryPtr geometryFromShape(const vn::geometry::Shape& shape)
{
    const auto* mesh = dynamic_cast<const vn::geometry::Mesh*>(&shape);
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
    // A mesh shape is ONE triangle draw: the primitive states the topology and the index span, which is where
    // both belong now (see Primitive.hpp). An unindexed mesh states no indices and draws its vertex streams.
    Primitive primitive;
    primitive.topology = Topology::Triangles;
    if (const auto* indexed = dynamic_cast<const vn::geometry::IndexedTriangleMesh*>(&shape)) {
        primitive.indices = Primitive::IndexSpan::whole(indexed->indicesBuffer());
    }
    geometry->setPrimitive(0U, std::move(primitive));
    return geometry;
}

VN_GRAPHICS_NS_END
