#pragma once

#include "geometry_global.hpp"

#include <cstddef>
#include <cstdint>
#include <stdexcept>
#include <utility>

#include "Mesh.hpp"
#include "TriangleMesh.hpp"

VN_GEOMETRY_NS_BEGIN

/**
 * @brief A quad mesh; each face has four corners and may use indexed or sequential vertices.
 *
 * @details
 * - The mesh is filled through the setters inherited from Mesh (setPositions() + setIndices()); choosing whether it is
 *   indexed is a matter of passing corner indices or an empty array.
 */
class QuadMesh : public Mesh {
    VN_OBJECT_META(QuadMesh, Mesh);

  public:
    /**
     * @brief Constructs an empty quad mesh.
     */
    QuadMesh()
    {
        this->shape_type_ = ShapeType::QuadMesh;
    }

    /**
     * @brief Returns the number of quad faces.
     *
     * @return Number of faces.
     */
    [[nodiscard]]
    std::size_t quadCount() const
    {
        const std::size_t corner_count = this->hasIndices() ? this->indices().size() : this->vertexCount();
        return corner_count / 4u;
    }

    /**
     * @brief Converts this valid quad mesh into an indexed triangle mesh.
     *
     * Each quad is split along the diagonal from its first to third corner.
     * Vertex attributes and the shape name are copied to the result.
     *
     * @warning Throws std::invalid_argument when the face corner count or indices are invalid.
     *
     * @return The converted triangle mesh.
     */
    [[nodiscard]]
    intrusive_ptr<TriangleMesh> toTriangleMesh() const
    {
        if (!isValid()) {
            throw std::invalid_argument("QuadMesh::toTriangleMesh requires a valid mesh");
        }

        const std::size_t face_count = quadCount();
        UInt32Array triangle_indices;
        triangle_indices.reserve(face_count * 6u);
        for (std::size_t face = 0; face < face_count; ++face) {
            const std::size_t first = face * 4u;
            const std::uint32_t a = this->hasIndices() ? this->indices()[first] : static_cast<std::uint32_t>(first);
            const std::uint32_t b = this->hasIndices() ? this->indices()[first + 1u] : static_cast<std::uint32_t>(first + 1u);
            const std::uint32_t c = this->hasIndices() ? this->indices()[first + 2u] : static_cast<std::uint32_t>(first + 2u);
            const std::uint32_t d = this->hasIndices() ? this->indices()[first + 3u] : static_cast<std::uint32_t>(first + 3u);
            triangle_indices.insert(triangle_indices.end(), { a, b, c, a, c, d });
        }

        auto triangle_mesh = intrusive_ptr<TriangleMesh>(new TriangleMesh());
        const auto positions = this->positions();
        triangle_mesh->setPositions({ positions.begin(), positions.end() });
        const auto normals = this->normals();
        if (!normals.empty()) {
            triangle_mesh->setNormals({ normals.begin(), normals.end() });
        }
        const auto texcoords = this->texcoords();
        if (!texcoords.empty()) {
            triangle_mesh->setTexcoords({ texcoords.begin(), texcoords.end() });
        }
        const auto colors = this->colors();
        if (!colors.empty()) {
            triangle_mesh->setColors({ colors.begin(), colors.end() });
        }
        triangle_mesh->setIndices(std::move(triangle_indices));
        triangle_mesh->setName(this->name());
        return triangle_mesh;
    }

    /**
     * @brief Reports whether the vertices and four-corner faces form a valid quad mesh.
     *
     * @return true when the mesh has at least one valid face.
     */
    [[nodiscard]]
    bool isValid() const override
    {
        if (this->positions_->empty()) {
            return false;
        }
        const std::size_t corner_count = this->hasIndices() ? this->indices().size() : this->vertexCount();
        if (corner_count == 0u || corner_count % 4u != 0u) {
            return false;
        }
        if (this->hasIndices()) {
            for (const std::uint32_t index : this->indices()) {
                if (index >= this->vertexCount()) {
                    return false;
                }
            }
        }
        return true;
    }
};

VN_GEOMETRY_NS_END
