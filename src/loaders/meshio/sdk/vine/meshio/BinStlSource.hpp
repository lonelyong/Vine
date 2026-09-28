#pragma once

#include "meshio_global.hpp"

#include <cstddef>
#include <cstdint>
#include <span>

#include <vine/intrusive_ptr.hpp>
#include <vine/geometry/Mesh.hpp>

#include "MeshSource.hpp"

VN_MESHIO_NS_BEGIN

/**
 * @brief A mesh as binary STL, written while the consumer pulls it.
 *
 * The lazy counterpart of MeshExporter's STL entry: the mesh is not converted when the source is made, and the bytes are
 * produced as they are pulled. The source holds the mesh, so it stays valid without the caller tracking a lifetime.
 *
 * THE LENGTH IS EXACT. Binary STL is fixed up - an 80-byte header, a triangle count, and 50 bytes per triangle - so
 * size() is known before a single byte is written, which is what lets a backend that records the length first take it.
 * AsciiStlSource cannot state its length that way and reports kUnknownSize instead.
 *
 * THE BYTES ARE THIS LIBRARY'S OWN. The file is not assimp's: the same geometry, laid out here, with a facet normal per
 * triangle and no per-vertex attributes. MeshExporter's STL entry goes through assimp and does not promise identical
 * bytes.
 *
 * @note A mesh the consumer must keep unchanged until the pull is finished is exactly the contract every borrowed
 *       source carries; this one turns a violation into a failed pull (IoError::InvalidData) rather than into wrong
 *       bytes.
 */
class VN_MESHIO_API BinStlSource final : public MeshSource
{
  public:
    /**
     * @brief Sources a mesh as binary STL.
     *
     * @param mesh The mesh to write; the source holds it. A null, invalid, empty or unsupported mesh is reported through
     *        error() instead of failing here, so the caller sees it where every other source defect surfaces.
     * @param scale Factor applied to every vertex, matching what MeshExporter's scale_factor does; 1.0 writes the mesh
     *        as it is.
     */
    explicit BinStlSource(vn::intrusive_ptr<const vn::geometry::Mesh> mesh, double scale = 1.0);

    /// @brief Destroys the source.
    ~BinStlSource() override;

    /**
     * @brief Moves a source.
     *
     * @param other The source to move from.
     */
    BinStlSource(BinStlSource&& other) noexcept;

    /**
     * @brief Takes over another source's state.
     *
     * @param other The source to move from.
     * @return A reference to this source.
     */
    BinStlSource& operator=(BinStlSource&& other) noexcept;

  protected:
    /**
     * @brief Reports how many records the file is made of.
     *
     * @return One record for the 84-byte prefix, then one per triangle.
     */
    [[nodiscard]] std::uint64_t recordCount() const override;

    /**
     * @brief Reports how much space one record needs.
     *
     * @return The 84-byte prefix, the largest record this format has.
     */
    [[nodiscard]] std::size_t maxRecordSize() const override;

    /**
     * @brief Renders the prefix or one triangle.
     *
     * @param index Record index; 0 is the prefix, the rest are triangles.
     * @param out Scratch space of maxRecordSize() bytes.
     * @return 84 or 50 bytes, or 0 when the mesh does not hold the triangle.
     */
    std::size_t renderRecord(std::uint64_t index, std::span<char> out) override;

  private:
    /// Bytes a binary STL spends before its first triangle: the 80-byte header and the triangle count.
    static constexpr std::size_t kPrefixSize = 84;
    /// Bytes one binary STL triangle takes: twelve floats and the attribute count.
    static constexpr std::size_t kTriangleSize = 50;
};

VN_MESHIO_NS_END
