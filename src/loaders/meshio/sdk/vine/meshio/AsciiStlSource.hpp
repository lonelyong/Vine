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
 * @brief A mesh as text STL, written while the consumer pulls it.
 *
 * The lazy counterpart of MeshExporter's STL entry for the text variant, and the same idea as BinStlSource: nothing is
 * converted when the source is made, and the bytes are produced as they are pulled.
 *
 * THE LENGTH IS NOT KNOWN UP FRONT, and it does not have to be: size() reports kUnknownSize, and the pull ends when the
 * facets run out. A backend that can write without the length first takes it (a ZIP stores such an entry with a zip64
 * header whose length is patched in afterwards, which costs a few header bytes); one whose format has to record the
 * length before the bytes cannot.
 *
 * THE BYTES ARE THIS LIBRARY'S OWN. The file is not assimp's: the same geometry, laid out here, with a facet normal per
 * triangle taken from the winding and no per-vertex attributes. MeshExporter's STL entry goes through assimp and does not
 * promise identical bytes.
 *
 * @note A mesh the consumer must keep unchanged until the pull is finished is exactly the contract every borrowed
 *       source carries; this one turns a violation into a failed pull (IoError::InvalidData) rather than into wrong
 *       bytes.
 */
class VN_MESHIO_API AsciiStlSource final : public MeshSource
{
  public:
    /**
     * @brief Sources a mesh as text STL.
     *
     * @param mesh The mesh to write; the source holds it. A null, invalid, empty or unsupported mesh is reported through
     *        error() instead of failing here, so the caller sees it where every other source defect surfaces.
     * @param scale Factor applied to every vertex, matching what MeshExporter's scale_factor does; 1.0 writes the mesh
     *        as it is.
     */
    explicit AsciiStlSource(vn::intrusive_ptr<const vn::geometry::Mesh> mesh, double scale = 1.0);

    /// @brief Destroys the source.
    ~AsciiStlSource() override;

    /**
     * @brief Moves a source.
     *
     * @param other The source to move from.
     */
    AsciiStlSource(AsciiStlSource&& other) noexcept;

    /**
     * @brief Takes over another source's state.
     *
     * @param other The source to move from.
     * @return A reference to this source.
     */
    AsciiStlSource& operator=(AsciiStlSource&& other) noexcept;

  protected:
    /**
     * @brief Reports how many records the file is made of.
     *
     * @return One record for the leading "solid", one per facet, and one for the trailing "endsolid".
     */
    [[nodiscard]] std::uint64_t recordCount() const override;

    /**
     * @brief Reports how much space one record can need.
     *
     * @return Enough for a facet: its normal, its three vertices and the keywords around them.
     */
    [[nodiscard]] std::size_t maxRecordSize() const override;

    /**
     * @brief Renders the header, one facet, or the footer.
     *
     * @param index Record index; 0 is the header, the facets follow, and the footer is last.
     * @param out Scratch space of maxRecordSize() bytes.
     * @return The bytes the record holds, or 0 when the mesh does not hold the triangle.
     */
    std::size_t renderRecord(std::uint64_t index, std::span<char> out) override;
};

VN_MESHIO_NS_END
