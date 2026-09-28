#include <vine/meshio/MeshSource.hpp>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <vector>

#include <vine/Buffer.hpp>
#include <vine/Object.hpp>
#include <vine/geometry/IndexedTriangleMesh.hpp>
#include <vine/geometry/TriangleMesh.hpp>

VN_MESHIO_NS_BEGIN

namespace
{

using Mesh                = vn::geometry::Mesh;
using TriangleMesh        = vn::geometry::TriangleMesh;
using IndexedTriangleMesh = vn::geometry::IndexedTriangleMesh;

/// Floats per vertex in the mesh's position and normal storage.
constexpr std::size_t kFloatsPerVertex = 3;
/// Corners per triangle.
constexpr std::uint64_t kCornersPerTriangle = 3;

} // namespace

struct MeshSource::Impl
{
    /// The position storage as it was handed over: holding it keeps the bytes alive, and its revision tells an edit.
    vn::intrusive_ptr<const vn::Buffer<float>> positions;
    /// The normal storage, or null when the mesh carries none.
    vn::intrusive_ptr<const vn::Buffer<float>> normals;
    /// The index storage, or null for a triangle soup (three consecutive positions per triangle).
    vn::intrusive_ptr<const vn::Buffer<std::uint32_t>> indices;

    std::uint64_t positions_revision{ 0 };
    std::uint64_t normals_revision{ 0 };
    std::uint64_t indices_revision{ 0 };

    float         scale{ 1.0f };
    std::uint64_t vertices{ 0 };
    std::uint64_t triangles{ 0 };

    /// The length a format stated, or kUnknownSize while it can only be found by pulling to the end.
    std::uint64_t stated_size{ vn::io::kUnknownSize };

    std::uint64_t cursor_record{ 0 };  ///< Record the pull is in.
    std::size_t   cursor_offset{ 0 };  ///< Bytes of that record already handed over.
    std::size_t   record_size{ 0 };    ///< Bytes the record holds.
    std::uint64_t produced{ 0 };       ///< Bytes handed over since the last rewind().

    std::vector<char> record;          ///< Scratch for one record, built again whenever a buffer cut the last one.
    bool              scratch_ready{ false };

    vn::io::IoError setup_error{ vn::io::IoError::Ok }; ///< What the constructor settled; every rewind() keeps it.
    vn::io::IoError error{ vn::io::IoError::Ok };
};

MeshSource::MeshSource(vn::intrusive_ptr<const Mesh> mesh, double scale)
  : impl(std::make_unique<Impl>())
{
    impl->scale = static_cast<float>(scale);

    if (mesh == nullptr || !mesh->isValid()) {
        refuse();
        return;
    }

    switch (mesh->shapeType()) {
    case vn::geometry::ShapeType::IndexedTriangleMesh: {
        const auto& indexed = obj_cast<IndexedTriangleMesh>(*mesh);
        impl->positions     = indexed.positionsBuffer();
        impl->normals       = indexed.normalsBuffer();
        impl->indices       = indexed.indicesBuffer();
        impl->triangles     = indexed.triangleCount();
        break;
    }
    case vn::geometry::ShapeType::TriangleMesh: {
        const auto& soup = obj_cast<TriangleMesh>(*mesh);
        impl->positions  = soup.positionsBuffer();
        impl->normals    = soup.normalsBuffer();
        impl->triangles  = soup.triangleCount();
        break;
    }
    default:
        refuse(); // B-rep shapes and the like are not triangle meshes
        return;
    }

    if (impl->positions == nullptr) {
        refuse();
        return;
    }
    impl->vertices = impl->positions->size() / kFloatsPerVertex;
    if (impl->triangles == 0) {
        refuse(); // there is no content to write
        return;
    }
    if (impl->normals != nullptr && impl->normals->size() / kFloatsPerVertex != impl->vertices) {
        impl->normals = nullptr; // a normal count that does not match the positions is not a per-vertex normal
    }

    impl->positions_revision = impl->positions->revision();
    if (impl->normals != nullptr) {
        impl->normals_revision = impl->normals->revision();
    }
    if (impl->indices != nullptr) {
        impl->indices_revision = impl->indices->revision();
    }
}

MeshSource::~MeshSource() = default;

MeshSource::MeshSource(MeshSource&& other) noexcept = default;

MeshSource& MeshSource::operator=(MeshSource&& other) noexcept = default;

bool MeshSource::ready() const noexcept
{
    return impl->setup_error == vn::io::IoError::Ok;
}

void MeshSource::refuse() noexcept
{
    impl->setup_error = vn::io::IoError::InvalidData;
    impl->error       = impl->setup_error;
}

void MeshSource::stateSize(std::uint64_t bytes) noexcept
{
    impl->stated_size = bytes;
}

std::uint64_t MeshSource::triangleCount() const noexcept
{
    return impl->triangles;
}

std::uint64_t MeshSource::vertexCount() const noexcept
{
    return impl->vertices;
}

bool MeshSource::hasNormals() const noexcept
{
    return impl->normals != nullptr;
}

float MeshSource::scale() const noexcept
{
    return impl->scale;
}

std::uint64_t MeshSource::size() const noexcept
{
    // A refused source produces nothing, so its length is zero rather than "unknown" - error() carries the reason.
    return ready() ? impl->stated_size : 0;
}

void MeshSource::rewind()
{
    impl->cursor_record = 0;
    impl->cursor_offset = 0;
    impl->record_size   = 0;
    impl->produced      = 0;
    impl->error         = impl->setup_error;
}

std::size_t MeshSource::read(std::span<std::byte> out)
{
    if (out.empty() || !ready() || impl->error != vn::io::IoError::Ok) {
        return 0;
    }

    // The mesh is read live, so an edit after it was handed over would be written as a mix of old and new content:
    // a revision that moved says exactly that. Replacing a buffer is not an edit - this source keeps reading the
    // storage it was given, which is the content it promised.
    if (impl->positions->revision() != impl->positions_revision
        || (impl->normals != nullptr && impl->normals->revision() != impl->normals_revision)
        || (impl->indices != nullptr && impl->indices->revision() != impl->indices_revision)) {
        impl->error = vn::io::IoError::InvalidData;
        return 0;
    }

    if (!impl->scratch_ready) {
        impl->record.resize(maxRecordSize());
        impl->scratch_ready = true;
    }

    const std::uint64_t records = recordCount();
    std::size_t         written = 0;
    while (written < out.size()) {
        if (impl->cursor_offset == impl->record_size) {
            if (impl->cursor_record >= records) {
                // The content is complete. A format that stated a length has to have produced exactly it, otherwise
                // its size() lied to whoever planned the storage (a ZIP writes the length into the entry).
                if (impl->stated_size != vn::io::kUnknownSize && impl->produced != impl->stated_size) {
                    impl->error = vn::io::IoError::InvalidData;
                }
                break;
            }
            impl->record_size = renderRecord(impl->cursor_record, std::span<char>(impl->record));
            ++impl->cursor_record;
            impl->cursor_offset = 0;
            if (impl->record_size == 0) {
                impl->error = vn::io::IoError::InvalidData; // the mesh does not hold what the record needs
                return written;
            }
            continue;
        }

        const std::size_t take = std::min(out.size() - written, impl->record_size - impl->cursor_offset);
        std::memcpy(out.data() + written, impl->record.data() + impl->cursor_offset, take);
        impl->cursor_offset += take;
        impl->produced += take;
        written += take;
    }
    return written;
}

vn::io::IoError MeshSource::error() const
{
    return impl->error;
}

bool MeshSource::vertexAt(std::uint64_t vertex, float (&out)[3]) const
{
    if (impl->positions == nullptr || vertex >= impl->vertices) {
        return false;
    }
    const float* values = impl->positions->data() + vertex * kFloatsPerVertex;
    out[0]              = values[0] * impl->scale;
    out[1]              = values[1] * impl->scale;
    out[2]              = values[2] * impl->scale;
    return true;
}

bool MeshSource::normalAt(std::uint64_t vertex, float (&out)[3]) const
{
    if (impl->normals == nullptr || vertex >= impl->vertices) {
        return false;
    }
    const float* values = impl->normals->data() + vertex * kFloatsPerVertex;
    out[0]              = values[0];
    out[1]              = values[1];
    out[2]              = values[2];
    return true;
}

bool MeshSource::cornerAt(std::uint64_t triangle, std::uint32_t (&out)[3]) const
{
    if (triangle >= impl->triangles) {
        return false;
    }

    const std::uint64_t base = triangle * kCornersPerTriangle;
    if (impl->indices == nullptr) {
        // A triangle soup is three consecutive positions per triangle, which is the vertex index itself.
        if (base + kCornersPerTriangle > impl->vertices) {
            return false;
        }
        out[0] = static_cast<std::uint32_t>(base + 0);
        out[1] = static_cast<std::uint32_t>(base + 1);
        out[2] = static_cast<std::uint32_t>(base + 2);
        return true;
    }

    for (std::uint64_t corner = 0; corner < kCornersPerTriangle; ++corner) {
        const std::uint32_t index = (*impl->indices)[static_cast<std::size_t>(base + corner)];
        if (index >= impl->vertices) {
            return false; // an index that does not fit its position array cannot be written
        }
        out[corner] = index;
    }
    return true;
}

bool MeshSource::triangleAt(std::uint64_t triangle, float (&a)[3], float (&b)[3], float (&c)[3]) const
{
    std::uint32_t corners[3]{};
    if (!cornerAt(triangle, corners)) {
        return false;
    }
    return vertexAt(corners[0], a) && vertexAt(corners[1], b) && vertexAt(corners[2], c);
}

void MeshSource::facetNormal(const float (&a)[3], const float (&b)[3], const float (&c)[3], float (&out)[3]) noexcept
{
    const float u[3]{ b[0] - a[0], b[1] - a[1], b[2] - a[2] };
    const float v[3]{ c[0] - a[0], c[1] - a[1], c[2] - a[2] };
    float       normal[3]{ u[1] * v[2] - u[2] * v[1], u[2] * v[0] - u[0] * v[2], u[0] * v[1] - u[1] * v[0] };
    const float length = std::sqrt(normal[0] * normal[0] + normal[1] * normal[1] + normal[2] * normal[2]);
    if (length > 0.0f) {
        out[0] = normal[0] / length;
        out[1] = normal[1] / length;
        out[2] = normal[2] / length;
        return;
    }
    out[0] = out[1] = out[2] = 0.0f;
}

VN_MESHIO_NS_END
