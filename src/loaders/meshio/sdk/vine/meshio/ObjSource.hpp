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
 * @brief A mesh as Wavefront OBJ, written while the consumer pulls it.
 *
 * The lazy counterpart of MeshExporter's OBJ entry: nothing is converted when the source is made, and the lines are
 * produced as they are pulled. The source holds the mesh, so it stays valid without the caller tracking a lifetime.
 *
 * THE LENGTH IS NOT KNOWN UP FRONT, and it does not have to be: size() reports kUnknownSize, and the pull ends when the
 * lines run out. A backend that can write without the length first takes it (a ZIP stores such an entry with a zip64
 * header whose length is patched in afterwards, which costs a few header bytes); one whose format has to record the
 * length before the bytes cannot.
 *
 * GEOMETRY ONLY. The file carries positions, and the mesh's per-vertex normals when it has them ("f v//vn"); it carries
 * no material library, which is a second file that one stream cannot hold. That is also what MeshExporter's stream entry
 * does, so the two agree in spirit - but the BYTES ARE THIS LIBRARY'S OWN, not assimp's: the same geometry, written here
 * with one line per vertex and per triangle, without assimp's vertex dedup or material grouping.
 *
 * @note A mesh the consumer must keep unchanged until the pull is finished is exactly the contract every borrowed
 *       source carries; this one turns a violation into a failed pull (IoError::InvalidData) rather than into wrong
 *       bytes.
 */
class VN_MESHIO_API ObjSource final : public MeshSource
{
  public:
    /**
     * @brief Sources a mesh as Wavefront OBJ.
     *
     * @param mesh The mesh to write; the source holds it. A null, invalid, empty or unsupported mesh is reported through
     *        error() instead of failing here, so the caller sees it where every other source defect surfaces.
     * @param scale Factor applied to every vertex, matching what MeshExporter's scale_factor does; 1.0 writes the mesh
     *        as it is.
     */
    explicit ObjSource(vn::intrusive_ptr<const vn::geometry::Mesh> mesh, double scale = 1.0);

    /// @brief Destroys the source.
    ~ObjSource() override;

    /**
     * @brief Moves a source.
     *
     * @param other The source to move from.
     */
    ObjSource(ObjSource&& other) noexcept;

    /**
     * @brief Takes over another source's state.
     *
     * @param other The source to move from.
     * @return A reference to this source.
     */
    ObjSource& operator=(ObjSource&& other) noexcept;

  protected:
    /**
     * @brief Reports how many records the file is made of.
     *
     * @return One record per vertex, one per normal when the mesh has them, then one per triangle - the order OBJ
     *         expects, since a face may only name vertices that are already defined.
     */
    [[nodiscard]] std::uint64_t recordCount() const override;

    /**
     * @brief Reports how much space one record can need.
     *
     * @return Enough for the longest line this format writes: a face naming three vertices and three normals.
     */
    [[nodiscard]] std::size_t maxRecordSize() const override;

    /**
     * @brief Renders one "v", "vn" or "f" line.
     *
     * @param index Record index, walking vertices, then normals, then triangles.
     * @param out Scratch space of maxRecordSize() bytes.
     * @return The bytes the line holds, or 0 when the mesh does not hold what the line names.
     */
    std::size_t renderRecord(std::uint64_t index, std::span<char> out) override;
};

VN_MESHIO_NS_END
