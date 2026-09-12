#pragma once

#include "geometry_global.hpp"

#include <cstddef>
#include <cstdint>
#include <span>

#include "Array.hpp"
#include "Mesh.hpp"

V_GEOMETRY_NS_BEGIN

/**
 * @brief A triangle mesh with shared vertices referenced by indices.
 *
 * Vertices live in a single position array; each triangle references three
 * vertex indices. Normals and texture coordinates, when present, match the
 * position array 1:1.
 */
class V_GEOMETRY_API IndexedTriangleMesh : public Mesh {
    V_OBJECT_META_DECL;

  public:
    IndexedTriangleMesh();

    /**
     * @brief Returns the triangle indices.
     *
     * The result is a borrowed view; see Mesh for the lifetime rules.
     *
     * @return Read-only view over the indices (3 entries per triangle).
     */
    std::span<const std::uint32_t> indices() const;

    /**
     * @brief Returns the shareable handle to the triangle indices.
     *
     * See Mesh::positionsBuffer() for the lifetime and revision contract.
     *
     * @return Handle to the index buffer (never null; empty when the mesh has no indices).
     */
    intrusive_ptr<const Buffer<std::uint32_t>> indicesBuffer() const;

    /**
     * @brief Replaces the triangle indices.
     *
     * @param indices New index array (moved in, not copied).
     */
    void setIndices(UInt32Array indices);

    /**
     * @brief Appends a vertex and returns its index.
     *
     * @param position Vertex position.
     * @return Index of the appended vertex.
     */
    std::uint32_t addVertex(const vine::math::Vec3f& position);

    /**
     * @brief Appends one triangle from vertex indices.
     *
     * @param i0 First vertex index.
     * @param i1 Second vertex index.
     * @param i2 Third vertex index.
     */
    void addTriangle(std::uint32_t i0, std::uint32_t i1, std::uint32_t i2);

    /**
     * @brief Removes all vertices and indices.
     */
    void clear();

    /**
     * @brief Returns the number of triangles.
     *
     * @return Triangle count.
     */
    [[nodiscard]]
    std::size_t triangleCount() const;

    [[nodiscard]]
    bool isValid() const override;

  private:
    /// Vertex indices, three per triangle; never null, empty when the mesh has none.
    intrusive_ptr<Buffer<std::uint32_t>> indices_;
};

V_GEOMETRY_NS_END
