#pragma once

#include <array>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <cmath>
#include <cstring>
#include <span>
#include <vector>

#include <vine/Buffer.hpp>
#include <vine/geometry/Array.hpp>
#include <vine/geometry/IndexedTriangleMesh.hpp>
#include <vine/geometry/Mesh.hpp>
#include <vine/geometry/TriangleMesh.hpp>
#include <vine/intrusive_ptr.hpp>
#include <vine/io/Stream.hpp>
#include <vine/io/Vfs.hpp>
#include <vine/math/Vector3.hpp>
#include <vine/robotics/io/robot_io_global.hpp>

#include "IoUtils.hpp"

VN_ROBOTICS_IO_NS_BEGIN

namespace detail
{

/// Bytes of the header every mesh file starts with.
inline constexpr std::size_t kMeshHeaderSize = 24;
/// Magic at the start of a mesh file: "Vine MeSH".
inline constexpr std::array<char, 4> kMeshMagic{ 'V', 'M', 'S', 'H' };
/// Layout version this build writes; v1 was the split-entry form, which had no header at all.
inline constexpr std::uint16_t kMeshVersion = 2;

/// Bytes of the prefix every binary STL file starts with: an 80-byte free-form header and a 32-bit triangle count.
inline constexpr std::size_t kStlPrefixSize = 84;
/// Bytes one binary STL triangle takes: a facet normal, three vertices, and a 16-bit attribute count.
inline constexpr std::size_t kStlTriangleSize = 50;

/**
 * @brief Writes a 32-bit number in the order binary STL is defined in.
 *
 * @param out Where to write the four bytes.
 * @param value The number; it is byte-swapped on a machine of the other order, since an STL file carries no note of its
 *        own about byte order.
 */
inline void putU32Le(char* out, std::uint32_t value) noexcept
{
    if constexpr (std::endian::native == std::endian::big) {
        value = (value >> 24) | ((value >> 8) & 0xFF00u) | ((value << 8) & 0xFF0000u) | (value << 24);
    }
    std::memcpy(out, &value, sizeof(value));
}

/**
 * @brief Reads a 32-bit number written in the order binary STL is defined in.
 *
 * @param in The four bytes to read.
 * @return The number.
 */
[[nodiscard]] inline std::uint32_t getU32Le(const char* in) noexcept
{
    std::uint32_t value = 0;
    std::memcpy(&value, in, sizeof(value));
    if constexpr (std::endian::native == std::endian::big) {
        value = (value >> 24) | ((value >> 8) & 0xFF00u) | ((value << 8) & 0xFF0000u) | (value << 24);
    }
    return value;
}

/**
 * @brief Writes a float in the order binary STL is defined in.
 *
 * @param out Where to write the four bytes.
 * @param value The number.
 */
inline void putFloatLe(char* out, float value) noexcept
{
    putU32Le(out, std::bit_cast<std::uint32_t>(value));
}

/**
 * @brief Reads a float written in the order binary STL is defined in.
 *
 * @param in The four bytes to read.
 * @return The number.
 */
[[nodiscard]] inline float getFloatLe(const char* in) noexcept
{
    return std::bit_cast<float>(getU32Le(in));
}

/**
 * @brief Works out the normal of three vertices.
 *
 * @param a First vertex.
 * @param b Second vertex.
 * @param c Third vertex.
 * @param out Receives the unit normal, or zeros for a triangle without area.
 */
inline void facetNormal(const float (&a)[3], const float (&b)[3], const float (&c)[3], float (&out)[3]) noexcept
{
    const float u[3]{ b[0] - a[0], b[1] - a[1], b[2] - a[2] };
    const float v[3]{ c[0] - a[0], c[1] - a[1], c[2] - a[2] };
    float       cross[3]{ u[1] * v[2] - u[2] * v[1], u[2] * v[0] - u[0] * v[2], u[0] * v[1] - u[1] * v[0] };
    const float length = std::sqrt(cross[0] * cross[0] + cross[1] * cross[1] + cross[2] * cross[2]);
    if (length > 0.0f) {
        out[0] = cross[0] / length;
        out[1] = cross[1] / length;
        out[2] = cross[2] / length;
        return;
    }
    out[0] = out[1] = out[2] = 0.0f; // a degenerate triangle has no direction to give
}

/**
 * @brief Whether a mesh carries something binary STL has no place for.
 *
 * STL holds a facet normal and three vertices per triangle, and nothing per vertex - so a mesh with texture
 * coordinates has to be written in the other form (`.vmesh`) instead of being quietly written without them. Per-vertex
 * normals are not part of this test: STL does carry a normal, it just carries one per facet, and a mesh whose normals
 * are per-vertex is a mesh that is textured in practice (which is what the texture coordinates say).
 *
 * @param mesh The mesh about to be written.
 * @return true when binary STL would lose part of it, false when it holds all of it.
 */
[[nodiscard]] inline bool stlWouldLose(const vn::geometry::Mesh& mesh) noexcept
{
    const auto texcoords = mesh.texcoordsBuffer();
    return texcoords != nullptr && texcoords->size() != 0;
}

/**
 * @brief What a mesh file holds, and in which byte order.
 *
 * Bits 0..7 say which blocks follow, and their order is the order the blocks are written in: a new block takes the next
 * free bit and never renumbers an old one, so a file that predates it still reads the same way. Bits 8..15 are format
 * flags, which say how to read the blocks rather than what they hold - they are kept apart so that adding a block cannot
 * collide with a statement about the whole file.
 */
enum class MeshFlag : std::uint16_t
{
    /// Vertex positions, three floats each; always set, since a file without them is not a mesh.
    Positions = 1u << 0,
    /// Per-vertex normals, three floats each.
    Normals = 1u << 1,
    /// Per-vertex texture coordinates, two floats each.
    Texcoords = 1u << 2,
    /// Triangle indices, one 32-bit number each.
    Indices = 1u << 3,

    /// The numbers are big-endian; a file says so itself, because this build writes the machine's own order.
    BigEndian = 1u << 8,
};

/// Every bit this build understands; anything else means a file from a later writer, which is refused rather than read.
inline constexpr std::uint16_t kMeshKnownFlags
    = static_cast<std::uint16_t>(MeshFlag::Positions) | static_cast<std::uint16_t>(MeshFlag::Normals)
      | static_cast<std::uint16_t>(MeshFlag::Texcoords) | static_cast<std::uint16_t>(MeshFlag::Indices)
      | static_cast<std::uint16_t>(MeshFlag::BigEndian);

/// Whether this machine writes its numbers big-endian, which is what a file written here says about itself.
inline constexpr bool kMeshHostIsBigEndian = (std::endian::native == std::endian::big);

/**
 * @brief What a mesh file says about itself: which arrays follow and how many elements each holds.
 *
 * The counts are the whole point of the format: with them inside the file, nothing outside it has to agree with what was
 * written - which is what the split-entry form required (an XML attribute per array, checked in a second place).
 */
struct MeshHeader
{
    std::uint16_t flags{ 0 };           ///< The MeshFlag bits of this file.
    std::uint64_t vertex_count{ 0 };    ///< Vertices the position, normal and texcoord blocks hold.
    std::uint64_t triangle_count{ 0 };  ///< Triangles the mesh has; the index block holds three per triangle.

    /**
     * @brief Reports whether the file holds one of the arrays.
     *
     * @param flag The array to ask about.
     * @return true when it is part of the file.
     */
    [[nodiscard]] bool has(MeshFlag flag) const noexcept
    {
        return (flags & static_cast<std::uint16_t>(flag)) != 0;
    }

    /**
     * @brief Reports how many indices the mesh has.
     *
     * @return Three per triangle.
     */
    [[nodiscard]] std::uint64_t indexCount() const noexcept { return triangle_count * 3; }

    /**
     * @brief Reports how many bytes the whole file takes, header included.
     *
     * @return The size in bytes; only meaningful for counts that were checked against a real file.
     */
    [[nodiscard]] std::uint64_t byteCount() const noexcept
    {
        std::uint64_t total = kMeshHeaderSize;
        if (has(MeshFlag::Positions)) {
            total += vertex_count * sizeof(vn::math::Vec3f);
        }
        if (has(MeshFlag::Normals)) {
            total += vertex_count * sizeof(vn::math::Vec3f);
        }
        if (has(MeshFlag::Texcoords)) {
            total += vertex_count * sizeof(vn::math::Vec2f);
        }
        if (has(MeshFlag::Indices)) {
            total += indexCount() * sizeof(std::uint32_t);
        }
        return total;
    }
};

/**
 * @brief Writes the header into its 24 bytes.
 *
 * @param out The bytes to fill; it has to hold kMeshHeaderSize of them.
 * @param header The header to write.
 */
inline void writeMeshHeader(std::span<char> out, const MeshHeader& header) noexcept
{
    std::memset(out.data(), 0, kMeshHeaderSize);
    std::memcpy(out.data(), kMeshMagic.data(), kMeshMagic.size());

    const std::uint16_t version = kMeshVersion;
    std::memcpy(out.data() + 4, &version, sizeof(version));
    std::memcpy(out.data() + 6, &header.flags, sizeof(header.flags));
    std::memcpy(out.data() + 8, &header.vertex_count, sizeof(header.vertex_count));
    std::memcpy(out.data() + 16, &header.triangle_count, sizeof(header.triangle_count));
}

/**
 * @brief Reads a header, refusing anything this build does not understand.
 *
 * A file from the other endianness is refused rather than read: the numbers would be wrong, and wrong numbers are worse
 * than a clear refusal. A flag bit this build has no name for is refused too, because a block whose length this build
 * cannot work out would have to be skipped, and a half-read mesh is exactly what the header exists to prevent. The
 * counts are only checked for being non-zero here - whether they add up to the entry's size is the reader's business,
 * since only it knows that size.
 *
 * @param bytes The first kMeshHeaderSize bytes of a mesh file.
 * @param out Receives the header; untouched when this returns false.
 * @return true when the bytes are a mesh file of a version this build reads.
 */
inline bool parseMeshHeader(std::span<const std::byte> bytes, MeshHeader& out) noexcept
{
    if (bytes.size() < kMeshHeaderSize) {
        return false;
    }
    if (std::memcmp(bytes.data(), kMeshMagic.data(), kMeshMagic.size()) != 0) {
        return false;
    }

    std::uint16_t version = 0;
    std::memcpy(&version, bytes.data() + 4, sizeof(version));
    if (version != kMeshVersion) {
        return false; // a layout this build does not know
    }

    MeshHeader header;
    std::memcpy(&header.flags, bytes.data() + 6, sizeof(header.flags));
    std::memcpy(&header.vertex_count, bytes.data() + 8, sizeof(header.vertex_count));
    std::memcpy(&header.triangle_count, bytes.data() + 16, sizeof(header.triangle_count));

    if (header.has(MeshFlag::BigEndian) != kMeshHostIsBigEndian) {
        return false; // written on a machine of the other byte order
    }
    if ((header.flags & static_cast<std::uint16_t>(~kMeshKnownFlags)) != 0) {
        return false; // a block or format statement this build does not know: its length cannot be checked, so it is not read
    }
    if (!header.has(MeshFlag::Positions) || header.vertex_count == 0 || header.triangle_count == 0) {
        return false;
    }
    out = header;
    return true;
}

/**
 * @brief The arrays of a mesh, taken so that they outlive the mesh itself.
 *
 * Both sources start the same way - with the arrays of a triangle mesh, whichever of the two shapes it is - and both
 * need the element convention to be the same one: a position array holds ONE `Vec3f` per vertex (`Buffer<float>` holds
 * three floats per vertex, which is why the vertex count is its size divided by three), and an index array holds one
 * number per index. Keeping that in one place is what keeps the two forms describing the same mesh.
 */
struct MeshArrays
{
    vn::intrusive_ptr<const vn::Buffer<float>>         positions;
    vn::intrusive_ptr<const vn::Buffer<float>>         normals;
    vn::intrusive_ptr<const vn::Buffer<float>>         texcoords;
    vn::intrusive_ptr<const vn::Buffer<std::uint32_t>> indices;
    std::uint64_t                                      vertex_count{ 0 };
    std::uint64_t                                      triangle_count{ 0 };

    /// Whether this holds a mesh that can be written: positions and at least one triangle.
    [[nodiscard]] bool valid() const noexcept { return positions != nullptr && vertex_count != 0 && triangle_count != 0; }

    /**
     * @brief Takes the arrays of a mesh; the mesh is not needed afterwards.
     *
     * A mesh of another shape type (a B-rep, say) and one without positions or triangles take nothing, which valid()
     * then reports.
     *
     * @param mesh The mesh to take the arrays of.
     * @return The arrays; empty when the mesh is not one this module writes.
     */
    [[nodiscard]] static MeshArrays take(const vn::geometry::Mesh& mesh) noexcept
    {
        MeshArrays arrays;
        if (!mesh.isValid()) {
            return arrays;
        }

        switch (mesh.shapeType()) {
        case vn::geometry::ShapeType::IndexedTriangleMesh: {
            const auto& indexed  = obj_cast<vn::geometry::IndexedTriangleMesh>(mesh);
            arrays.positions     = indexed.positionsBuffer();
            arrays.normals       = indexed.normalsBuffer();
            arrays.texcoords     = indexed.texcoordsBuffer();
            arrays.indices       = indexed.indicesBuffer();
            arrays.triangle_count = indexed.triangleCount();
            break;
        }
        case vn::geometry::ShapeType::TriangleMesh: {
            const auto& soup       = obj_cast<vn::geometry::TriangleMesh>(mesh);
            arrays.positions       = soup.positionsBuffer();
            arrays.normals         = soup.normalsBuffer();
            arrays.texcoords       = soup.texcoordsBuffer();
            arrays.triangle_count  = soup.triangleCount();
            break;
        }
        default:
            return arrays; // B-rep shapes and the like are not triangle meshes
        }

        if (arrays.positions == nullptr) {
            return arrays;
        }
        arrays.vertex_count = static_cast<std::uint64_t>(arrays.positions->size()) / 3u;

        // The model hands out a buffer for every array, empty when the mesh does not have that array at all: empty means
        // "none" here, which is what both writers and the file forms mean by it.
        arrays.normals   = arrays.normals != nullptr && arrays.normals->size() != 0 ? arrays.normals : nullptr;
        arrays.texcoords = arrays.texcoords != nullptr && arrays.texcoords->size() != 0 ? arrays.texcoords : nullptr;
        arrays.indices   = arrays.indices != nullptr && arrays.indices->size() != 0 ? arrays.indices : nullptr;

        // A mesh whose arrays do not describe the same mesh is refused here, once, for both forms: a file holding a normal
        // array that is shorter than the positions, or an index that no vertex backs, would say something the mesh does
        // not - and the alternative (writing the file without the offending array) is a silent change to the content.
        const bool whole_vertices = arrays.positions->size() % 3u == 0;
        const bool whole_triangles
            = arrays.indices == nullptr || arrays.indices->size() == arrays.triangle_count * 3u;
        const bool strided_arrays
            = (arrays.normals == nullptr || arrays.normals->size() == arrays.vertex_count * 3u)
              && (arrays.texcoords == nullptr || arrays.texcoords->size() == arrays.vertex_count * 2u);
        if (!whole_vertices || !whole_triangles || !strided_arrays || !cornersAreInRange(arrays)) {
            return MeshArrays{};
        }
        return arrays;
    }

    /**
     * @brief Reports whether every index a mesh has names a vertex it has.
     *
     * @param arrays The arrays to check.
     * @return true when no index points past the position array, and a soup names no more vertices than it holds.
     */
    [[nodiscard]] static bool cornersAreInRange(const MeshArrays& arrays) noexcept
    {
        if (arrays.indices == nullptr) {
            return arrays.triangle_count * 3u <= arrays.vertex_count; // a soup names its own vertices in order
        }
        const auto* const corners = arrays.indices->data();
        const std::size_t count   = arrays.indices->size();
        for (std::size_t i = 0; i < count; ++i) {
            if (corners[i] >= arrays.vertex_count) {
                return false;
            }
        }
        return true;
    }
};

/**
 * @brief A mesh as one file, written while the consumer pulls it.
 *
 * The header says which arrays follow and how many elements each holds, so the file describes itself. The arrays are read
 * straight from the mesh's buffers, so a package stores the geometry without a copy of it - and the source holds those
 * buffers, so a backend that writes the package later needs nothing kept alive by the caller.
 *
 * A mesh that was edited after it was handed over is refused (`IoError::InvalidData`), the way every borrowed source here
 * refuses one.
 */
class MeshBinSource final : public vn::io::DataSource
{
  public:
    /**
     * @brief Sources a mesh as one mesh file.
     *
     * Only the mesh's buffers are taken, and they are held: the mesh itself is not needed afterwards, so a caller can let
     * it go as soon as it has been written. A null, invalid, empty or unsupported mesh is reported through error() rather
     * than by throwing here.
     *
     * @param mesh The mesh to write; this reads its arrays and holds those.
     */
    explicit MeshBinSource(const vn::geometry::Mesh& mesh)
    {
        const MeshArrays arrays = MeshArrays::take(mesh);
        if (!arrays.valid()) {
            error_ = vn::io::IoError::InvalidData; // nothing here to write a file from
            return;
        }
        positions_             = arrays.positions;
        normals_               = arrays.normals;
        texcoords_             = arrays.texcoords;
        indices_               = arrays.indices;
        header_.vertex_count   = arrays.vertex_count;
        header_.triangle_count = arrays.triangle_count;

        // Every array bit is set by the block that is actually written, so the header and the blocks cannot disagree.
        header_.flags = kMeshHostIsBigEndian ? static_cast<std::uint16_t>(MeshFlag::BigEndian) : 0;
        addBlock(positions_, MeshFlag::Positions, header_.vertex_count, sizeof(vn::math::Vec3f));
        addBlock(normals_, MeshFlag::Normals, header_.vertex_count, sizeof(vn::math::Vec3f));
        addBlock(texcoords_, MeshFlag::Texcoords, header_.vertex_count, sizeof(vn::math::Vec2f));
        addBlock(indices_, MeshFlag::Indices, header_.indexCount(), sizeof(std::uint32_t));

        writeMeshHeader(header_bytes_, header_);
        revisions_[0] = positions_->revision();
        revisions_[1] = normals_ != nullptr ? normals_->revision() : 0;
        revisions_[2] = texcoords_ != nullptr ? texcoords_->revision() : 0;
        revisions_[3] = indices_ != nullptr ? indices_->revision() : 0;
    }

    /**
     * @brief Reports the length of the file.
     *
     * @return The exact byte count, or 0 when the mesh was refused (see error()).
     */
    [[nodiscard]] std::uint64_t size() const noexcept override
    {
        return error_ == vn::io::IoError::Ok ? header_.byteCount() : 0;
    }

    /**
     * @brief Restarts the file from its first byte.
     */
    void rewind() override { cursor_ = 0; }

    /**
     * @brief Writes the next chunk of the file.
     *
     * @param out Buffer to fill.
     * @return The number of bytes written, or 0 at the end of the content (see error()).
     */
    [[nodiscard]] std::size_t read(std::span<std::byte> out) override
    {
        if (out.empty() || error_ != vn::io::IoError::Ok) {
            return 0;
        }
        if (!intact()) {
            error_ = vn::io::IoError::InvalidData; // a buffer was edited after the mesh was handed over
            return 0;
        }

        const std::uint64_t total = size();
        if (cursor_ >= total) {
            return 0;
        }
        const std::size_t take = static_cast<std::size_t>(
            std::min<std::uint64_t>(out.size(), total - cursor_));

        std::size_t written = 0;
        if (cursor_ < kMeshHeaderSize) {
            const std::size_t from_header = static_cast<std::size_t>(
                std::min<std::uint64_t>(take, kMeshHeaderSize - cursor_));
            std::memcpy(out.data(), header_bytes_.data() + cursor_, from_header);
            written = from_header;
        }

        std::uint64_t at = written > 0 ? kMeshHeaderSize : cursor_; // where in the file the rest starts
        while (written < take) {
            const std::uint64_t in_blocks = at - kMeshHeaderSize;
            std::uint64_t       block_at  = 0;
            std::size_t         index     = 0;
            for (; index < block_count_; ++index) {
                if (in_blocks < block_at + blocks_[index].bytes) {
                    break;
                }
                block_at += blocks_[index].bytes;
            }
            if (index >= block_count_) {
                break;
            }

            const std::size_t within = static_cast<std::size_t>(in_blocks - block_at);
            const std::size_t take_from_block =
                std::min<std::size_t>(take - written, static_cast<std::size_t>(blocks_[index].bytes) - within);
            std::memcpy(out.data() + written,
                        reinterpret_cast<const std::byte*>(blocks_[index].data) + within, take_from_block);
            written += take_from_block;
            at += take_from_block;
        }

        cursor_ += written;
        return written;
    }

    /**
     * @brief Reports a failure that read() cannot express.
     *
     * @return IoError::Ok while the mesh writes cleanly, IoError::InvalidData when it was refused or was edited after the
     *         source was handed over.
     */
    [[nodiscard]] vn::io::IoError error() const override { return error_; }

  private:
    /// One array of the file, as it is written.
    struct Block
    {
        const void*   data{ nullptr };
        std::uint64_t bytes{ 0 };
    };

    /**
     * @brief Adds one array to the file when the mesh has it.
     *
     * @tparam T The array's element type.
     * @param buffer The array, or null when the mesh has none.
     * @param flag The MeshFlag this array sets.
     * @param count Elements the array is expected to hold, in the file's own element type.
     * @param element Bytes one of those elements takes.
     */
    template <typename T>
    void addBlock(const vn::intrusive_ptr<const vn::Buffer<T>>& buffer, MeshFlag flag, std::uint64_t count,
                  std::uint64_t element) noexcept
    {
        const std::uint64_t bytes = buffer == nullptr ? 0 : buffer->size() * sizeof(T);
        if (buffer == nullptr || bytes != count * element) {
            // take() already refused arrays that do not line up, so this is a second line of defence - but it is what
            // keeps a bit from being set without the block that backs it, which is the one thing the header must not do.
            return;
        }
        // Every bit this sets is backed by the block right below it, so the header cannot claim a block that is not there.
        header_.flags |= static_cast<std::uint16_t>(flag);
        blocks_[block_count_++] = Block{ buffer->data(), bytes };
    }

    /**
     * @brief Reports whether every array still holds what it held when the mesh was handed over.
     *
     * @return true when none of them was edited.
     */
    [[nodiscard]] bool intact() const noexcept
    {
        return (positions_ == nullptr || positions_->revision() == revisions_[0])
               && (normals_ == nullptr || normals_->revision() == revisions_[1])
               && (texcoords_ == nullptr || texcoords_->revision() == revisions_[2])
               && (indices_ == nullptr || indices_->revision() == revisions_[3]);
    }

    vn::intrusive_ptr<const vn::Buffer<float>>         positions_;
    vn::intrusive_ptr<const vn::Buffer<float>>         normals_;
    vn::intrusive_ptr<const vn::Buffer<float>>         texcoords_;
    vn::intrusive_ptr<const vn::Buffer<std::uint32_t>> indices_;

    MeshHeader                        header_{};
    std::array<char, kMeshHeaderSize> header_bytes_{};
    std::array<Block, 4>              blocks_{};
    std::size_t                       block_count_{ 0 };
    std::array<std::uint64_t, 4>      revisions_{};
    std::uint64_t                     cursor_{ 0 };
    vn::io::IoError                   error_{ vn::io::IoError::Ok };
};

/// How far apart two of a triangle's corners may be in their normal and still count as one facet's normal.
inline constexpr float kSameNormalTolerance = 1e-4f;

/// The largest triangle count binary STL can state: the prefix carries it in one 32-bit number.
inline constexpr std::uint64_t kMaxStlTriangles = 0xFFFFFFFFull;

/**
 * @brief A mesh as one binary STL file, written while the consumer pulls it.
 *
 * Binary STL holds a facet normal and three vertices per triangle and nothing per vertex, so it is the form for a mesh
 * that has no texture coordinates (see stlWouldLose()); the other form is for the rest. The bytes are laid out here
 * rather than by a third-party writer, so what a package holds does not depend on another library's idea of STL.
 *
 * The facet normal a mesh already carries is written as it is when all three of a triangle's corners agree on one - which
 * is what a mesh that came from STL looks like. Recomputing it from the winding would silently overwrite what the model
 * says, and would flip it wherever the model's winding is the other way round. Only a mesh with no normals, or with
 * per-vertex ones, gets a normal worked out from the geometry.
 *
 * The format fixes the length itself (84 bytes, then 50 per triangle), so size() is exact before a byte is written. The
 * numbers are little-endian, which is the order binary STL is defined in - the one place in this module that does not
 * write the machine's own order and say so.
 *
 * A mesh that was edited after it was handed over is refused (`IoError::InvalidData`), the way every borrowed source
 * here refuses one.
 */
class StlMeshSource final : public vn::io::DataSource
{
  public:
    /**
     * @brief Sources a mesh as binary STL.
     *
     * Only the mesh's arrays are taken, and they are held: the mesh itself is not needed afterwards. A null, invalid,
     * empty, unsupported or too-large mesh is reported through error() rather than by throwing here.
     *
     * @param mesh The mesh to write; this reads its arrays and holds those.
     */
    explicit StlMeshSource(const vn::geometry::Mesh& mesh)
    {
        arrays_ = MeshArrays::take(mesh);
        if (!arrays_.valid()) {
            error_ = vn::io::IoError::InvalidData; // nothing here to write a file from
            return;
        }
        if (arrays_.triangle_count > kMaxStlTriangles) {
            error_ = vn::io::IoError::CapacityExceeded; // more triangles than the format can state
            return;
        }
        // The 80 bytes are free-form on purpose; they must not start with "solid", which is how a reader tells the text
        // form of STL apart from this one.
        static constexpr char kHeader[] = "binary STL written by vine::robotics::io";
        static_assert(sizeof(kHeader) - 1 <= kStlPrefixSize - sizeof(std::uint32_t),
                      "the text has to fit the 80 bytes binary STL reserves for its header");
        std::memset(prefix_.data(), 0, prefix_.size());
        std::memcpy(prefix_.data(), kHeader, sizeof(kHeader) - 1);
        putU32Le(prefix_.data() + 80, static_cast<std::uint32_t>(arrays_.triangle_count));

        revisions_[0] = arrays_.positions->revision();
        revisions_[1] = arrays_.indices != nullptr ? arrays_.indices->revision() : 0;
    }

    /**
     * @brief Reports the length of the file.
     *
     * @return The exact byte count, or 0 when the mesh was refused (see error()).
     */
    [[nodiscard]] std::uint64_t size() const noexcept override
    {
        return error_ == vn::io::IoError::Ok ? kStlPrefixSize + kStlTriangleSize * arrays_.triangle_count : 0;
    }

    /**
     * @brief Restarts the file from its first byte.
     */
    void rewind() override { cursor_ = 0; }

    /**
     * @brief Writes the next chunk of the file.
     *
     * @param out Buffer to fill.
     * @return The number of bytes written, or 0 at the end of the content (see error()).
     */
    [[nodiscard]] std::size_t read(std::span<std::byte> out) override
    {
        if (out.empty() || error_ != vn::io::IoError::Ok) {
            return 0;
        }
        if (!intact()) {
            error_ = vn::io::IoError::InvalidData; // an array was edited after the mesh was handed over
            return 0;
        }

        const std::uint64_t total = size();
        if (cursor_ >= total) {
            return 0;
        }
        const std::size_t take = static_cast<std::size_t>(std::min<std::uint64_t>(out.size(), total - cursor_));

        std::size_t written = 0;
        if (cursor_ < kStlPrefixSize) {
            const std::size_t from_prefix
                = std::min<std::size_t>(take, kStlPrefixSize - static_cast<std::size_t>(cursor_));
            std::memcpy(out.data(), prefix_.data() + cursor_, from_prefix);
            written = from_prefix;
        }

        while (written < take) {
            const std::uint64_t at    = cursor_ + written;      // where in the file the next byte belongs
            const std::uint64_t in    = at - kStlPrefixSize;    // how far into the triangles that is
            const std::uint64_t index = in / kStlTriangleSize;  // the triangle it falls in
            const std::size_t  within = static_cast<std::size_t>(in % kStlTriangleSize);

            std::array<char, kStlTriangleSize> record{};
            renderTriangle(index, record.data());

            const std::size_t part = std::min<std::size_t>(take - written, kStlTriangleSize - within);
            std::memcpy(out.data() + written, record.data() + within, part);
            written += part;
        }

        cursor_ += written;
        return written;
    }

    /**
     * @brief Reports a failure that read() cannot express.
     *
     * @return IoError::Ok while the mesh writes cleanly, IoError::InvalidData when it was refused or edited after the
     *         source was handed over, IoError::CapacityExceeded when it has more triangles than STL can state.
     */
    [[nodiscard]] vn::io::IoError error() const override { return error_; }

  private:
    /**
     * @brief Reports whether the arrays still hold what they held when the mesh was handed over.
     *
     * @return true when neither of the arrays was edited.
     */
    [[nodiscard]] bool intact() const noexcept
    {
        return (arrays_.positions == nullptr || arrays_.positions->revision() == revisions_[0])
               && (arrays_.indices == nullptr || arrays_.indices->revision() == revisions_[1]);
    }

    /**
     * @brief Writes one triangle's 50 bytes.
     *
     * @param index The triangle to write; the caller only asks for triangles the mesh has.
     * @param out Receives kStlTriangleSize bytes.
     */
    void renderTriangle(std::uint64_t index, char* out) const noexcept
    {
        std::uint32_t corner[3]{};
        if (arrays_.indices == nullptr) {
            corner[0] = static_cast<std::uint32_t>(index * 3u);
            corner[1] = corner[0] + 1u;
            corner[2] = corner[0] + 2u;
        }
        else {
            const auto* const indices = arrays_.indices->data();
            corner[0]                 = indices[index * 3u];
            corner[1]                 = indices[index * 3u + 1u];
            corner[2]                 = indices[index * 3u + 2u];
        }

        float a[3]{};
        float b[3]{};
        float c[3]{};
        vertexAt(corner[0], a);
        vertexAt(corner[1], b);
        vertexAt(corner[2], c);

        float normal[3]{};
        if (!facetNormalOfMesh(corner, normal)) {
            facetNormal(a, b, c, normal); // the mesh has no normal for this facet, so the geometry decides
        }

        for (std::size_t i = 0; i < 3; ++i) {
            putFloatLe(out + 0 + static_cast<std::ptrdiff_t>(i) * 4, normal[i]);
            putFloatLe(out + 12 + static_cast<std::ptrdiff_t>(i) * 4, a[i]);
            putFloatLe(out + 24 + static_cast<std::ptrdiff_t>(i) * 4, b[i]);
            putFloatLe(out + 36 + static_cast<std::ptrdiff_t>(i) * 4, c[i]);
        }
        const std::uint16_t attribute = 0; // "the attribute byte count is zero" is what the format expects
        std::memcpy(out + 48, &attribute, sizeof(attribute));
    }

    /**
     * @brief Reads the one normal the mesh gives a whole triangle, when it gives one.
     *
     * @param corner The triangle's three corners.
     * @param out Receives the normal.
     * @return true when the mesh's three corners agree on a normal, false when it has none or gives each vertex its own.
     */
    [[nodiscard]] bool facetNormalOfMesh(const std::uint32_t (&corner)[3], float (&out)[3]) const noexcept
    {
        if (arrays_.normals == nullptr || arrays_.normals->size() < arrays_.vertex_count * 3u) {
            return false; // no normals to keep
        }
        const float* const normals = arrays_.normals->data();
        for (std::size_t k = 0; k < 3; ++k) {
            out[k] = normals[static_cast<std::size_t>(corner[0]) * 3u + k];
        }
        for (std::size_t c = 1; c < 3; ++c) {
            for (std::size_t k = 0; k < 3; ++k) {
                const float other = normals[static_cast<std::size_t>(corner[c]) * 3u + k];
                if (other < out[k] - kSameNormalTolerance || other > out[k] + kSameNormalTolerance) {
                    return false; // per-vertex normals: a facet normal has to come from the geometry instead
                }
            }
        }
        return true;
    }

    /**
     * @brief Reads one vertex out of the position array.
     *
     * @param index The vertex to read; the mesh's corners were checked when its arrays were taken, so it is inside the
     *        array.
     * @param out Receives the three floats.
     */
    void vertexAt(std::uint32_t index, float (&out)[3]) const noexcept
    {
        const float* const positions = arrays_.positions->data();
        out[0]                       = positions[index * 3u];
        out[1]                       = positions[index * 3u + 1u];
        out[2]                       = positions[index * 3u + 2u];
    }

    MeshArrays                        arrays_{};
    std::array<char, kStlPrefixSize>  prefix_{};
    std::array<std::uint64_t, 2>      revisions_{};
    std::uint64_t                     cursor_{ 0 };
    vn::io::IoError                   error_{ vn::io::IoError::Ok };
};

/**
 * @brief Reports whether a virtual path names a binary STL file.
 *
 * The description's own path says which form it points at, and both readers then check that the bytes match that claim -
 * so a file named for the wrong form is refused rather than guessed at.
 *
 * @param path The virtual path of a mesh file.
 * @return true when the path ends in `.stl`.
 */
[[nodiscard]] inline bool isStlPath(const std::filesystem::path& path) noexcept
{
    return path.extension() == std::filesystem::path(u8".stl");
}

/**
 * @brief The arrays one mesh file holds.
 */
struct MeshBinData
{
    vn::geometry::Vec3fArray  positions; ///< Vertex positions.
    vn::geometry::Vec3fArray  normals;   ///< Per-vertex normals, empty when the file has none.
    vn::geometry::Vec2fArray  texcoords; ///< Per-vertex texture coordinates, empty when the file has none.
    vn::geometry::UInt32Array indices;   ///< Triangle indices, empty for a triangle soup.
};

/**
 * @brief Reads one mesh file into its arrays.
 *
 * The counts are checked against the entry's own size before a single byte is allocated, so a corrupt header cannot ask
 * for more memory than the file could possibly hold - and a file whose blocks do not add up is refused rather than read
 * as far as it happens to go.
 *
 * @param vfs The tree to read from.
 * @param path The virtual path of the mesh file.
 * @param out Receives the arrays.
 * @return true when the file was read in full, false when it is not a mesh file this build reads or is damaged.
 */
inline bool readMeshBin(vn::io::Vfs& vfs, const std::filesystem::path& path, MeshBinData& out)
{
    auto opened = vfs.openRead(path);
    if (!opened.ok()) {
        return false;
    }
    vn::io::VfsEntrySource& source = **opened;

    const std::uint64_t total = source.size();
    if (total < kMeshHeaderSize) {
        return false;
    }

    std::array<std::byte, kMeshHeaderSize> bytes{};
    if (!readExact(source, bytes)) {
        return false;
    }

    MeshHeader header;
    if (!parseMeshHeader(bytes, header)) {
        return false;
    }

    // Every count has to fit what the entry can hold, which is also what keeps the multiplication below from wrapping.
    const auto fits = [total](std::uint64_t count, std::uint64_t element) { return count <= total / element; };
    if (!fits(header.vertex_count, sizeof(vn::math::Vec3f))
        || (header.has(MeshFlag::Texcoords) && !fits(header.vertex_count, sizeof(vn::math::Vec2f)))
        || (header.has(MeshFlag::Indices) && (!fits(header.indexCount(), sizeof(std::uint32_t)) || header.indexCount() % 3u != 0))) {
        return false;
    }
    if (header.byteCount() != total) {
        return false; // the file does not hold what its own header says
    }

    if (!readBlock(source, header.vertex_count, out.positions)) {
        return false;
    }
    if (header.has(MeshFlag::Normals) && !readBlock(source, header.vertex_count, out.normals)) {
        return false;
    }
    if (header.has(MeshFlag::Texcoords) && !readBlock(source, header.vertex_count, out.texcoords)) {
        return false;
    }
    if (header.has(MeshFlag::Indices) && !readBlock(source, header.indexCount(), out.indices)) {
        return false;
    }
    return source.error() == vn::io::IoError::Ok;
}

/**
 * @brief Reads a mesh file into a mesh of the kind the description names.
 *
 * @param vfs The tree to read from.
 * @param path The virtual path of the mesh file.
 * @param indexed Whether the description says the mesh is an indexed one.
 * @return The mesh, or null when the file cannot be read.
 */
inline vn::intrusive_ptr<vn::geometry::Mesh> meshFromBin(vn::io::Vfs& vfs, const std::filesystem::path& path,
                                                         bool indexed)
{
    MeshBinData data;
    if (!readMeshBin(vfs, path, data)) {
        return {};
    }
    if (indexed && data.indices.empty()) {
        return {}; // an indexed mesh without indices is not what the description promised
    }

    if (!indexed) {
        auto mesh = vn::make_intrusive<vn::geometry::TriangleMesh>();
        mesh->setPositions(std::move(data.positions));
        if (!data.normals.empty()) {
            mesh->setNormals(std::move(data.normals));
        }
        if (!data.texcoords.empty()) {
            mesh->setTexcoords(std::move(data.texcoords));
        }
        return mesh;
    }

    auto mesh = vn::make_intrusive<vn::geometry::IndexedTriangleMesh>();
    mesh->setPositions(std::move(data.positions));
    if (!data.normals.empty()) {
        mesh->setNormals(std::move(data.normals));
    }
    if (!data.texcoords.empty()) {
        mesh->setTexcoords(std::move(data.texcoords));
    }
    mesh->setIndices(std::move(data.indices));
    return mesh;
}

/**
 * @brief Reads one binary STL file into its arrays.
 *
 * The file describes itself completely: the prefix states how many triangles follow, and each of them takes a fixed 50
 * bytes - so a length that does not add up is refused instead of read as far as it happens to go. What comes back is a
 * soup: one `Vec3f` per vertex, a per-vertex normal that repeats the facet's, and no indices, since a facet normal is
 * all the file holds per triangle.
 *
 * @param vfs The tree to read from.
 * @param path The virtual path of the STL file.
 * @param out Receives the arrays.
 * @return true when the file was read in full, false when it is not a binary STL file this build reads.
 */
inline bool readStl(vn::io::Vfs& vfs, const std::filesystem::path& path, MeshBinData& out)
{
    auto opened = vfs.openRead(path);
    if (!opened.ok()) {
        return false;
    }
    vn::io::VfsEntrySource& source = **opened;

    const std::uint64_t total = source.size();
    if (total < kStlPrefixSize) {
        return false;
    }

    std::array<std::byte, kStlPrefixSize> prefix{};
    if (!readExact(source, prefix)) {
        return false;
    }

    const std::uint32_t triangles = getU32Le(reinterpret_cast<const char*>(prefix.data() + 80));
    if (triangles == 0) {
        return false; // an STL file without triangles holds no mesh
    }
    if (static_cast<std::uint64_t>(triangles) * kStlTriangleSize != total - kStlPrefixSize) {
        return false; // the file does not hold what its own count says
    }

    out.positions.clear();
    out.normals.clear();
    out.texcoords.clear(); // a form that cannot hold them leaves none behind
    out.indices.clear();
    out.positions.reserve(static_cast<std::size_t>(triangles) * 3u);
    out.normals.reserve(triangles);

    // A record is a normal followed by three vertices, so the arrays cannot take it as one block: records are pulled in
    // chunks and split into the two arrays as they arrive. A chunk ends wherever a read happens to end, which is why the
    // leftover bytes are moved to the front rather than dropped.
    std::vector<std::byte> chunk(kStlTriangleSize * 1024u);
    std::size_t            have = 0;
    std::uint64_t          left = triangles;
    while (left != 0) {
        while (have < kStlTriangleSize) {
            const std::size_t got = source.read(std::span<std::byte>(chunk.data() + have, chunk.size() - have));
            if (got == 0) {
                return false; // the entry ended before the records it promised arrived
            }
            have += got;
        }

        std::size_t whole = have / kStlTriangleSize;
        if (whole > left) {
            whole = static_cast<std::size_t>(left);
        }
        for (std::size_t i = 0; i < whole; ++i) {
            const char* const record = reinterpret_cast<const char*>(chunk.data()) + i * kStlTriangleSize;
            const float       nx     = getFloatLe(record + 0);
            const float       ny     = getFloatLe(record + 4);
            const float       nz     = getFloatLe(record + 8);
            for (int corner = 0; corner < 3; ++corner) {
                const char* const vertex = record + 12 + corner * 12;
                out.positions.push_back(
                    vn::math::Vec3f(getFloatLe(vertex), getFloatLe(vertex + 4), getFloatLe(vertex + 8)));
                out.normals.push_back(vn::math::Vec3f(nx, ny, nz));
            }
        }
        left -= whole;

        const std::size_t used = whole * kStlTriangleSize;
        std::memmove(chunk.data(), chunk.data() + used, have - used);
        have -= used;
    }
    return source.error() == vn::io::IoError::Ok;
}

/**
 * @brief Reads a binary STL file as a triangle soup.
 *
 * @param vfs The tree to read from.
 * @param path The virtual path of the STL file.
 * @return The mesh, or null when the file cannot be read.
 */
inline vn::intrusive_ptr<vn::geometry::Mesh> meshFromStl(vn::io::Vfs& vfs, const std::filesystem::path& path)
{
    MeshBinData data;
    if (!readStl(vfs, path, data)) {
        return {};
    }
    auto mesh = vn::make_intrusive<vn::geometry::TriangleMesh>();
    mesh->setPositions(std::move(data.positions));
    mesh->setNormals(std::move(data.normals));
    return mesh;
}

/**
 * @brief Reads whichever mesh file the description points at.
 *
 * The path says which form the file is in, and either reader refuses bytes that do not match that claim - so a file named
 * for the wrong form fails instead of being read as something it is not.
 *
 * @param vfs The tree to read from.
 * @param path The virtual path of the mesh file.
 * @param indexed Whether the description says the mesh is an indexed one.
 * @return The mesh, or null when the file cannot be read or is not what the description promised.
 */
inline vn::intrusive_ptr<vn::geometry::Mesh> meshFromFile(vn::io::Vfs& vfs, const std::filesystem::path& path,
                                                          bool indexed)
{
    if (!isStlPath(path)) {
        return meshFromBin(vfs, path, indexed);
    }
    if (indexed) {
        return {}; // an STL file holds a soup: a description that promises indices does not match it
    }
    return meshFromStl(vfs, path);
}

} // namespace detail

VN_ROBOTICS_IO_NS_END
