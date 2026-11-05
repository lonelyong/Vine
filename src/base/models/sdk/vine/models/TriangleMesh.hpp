#pragma once

#include <vine/models/models_global.hpp>

#include <cstddef>
#include <cstdint>
#include <vine/math/Vector3.hpp>

#include <vine/models/Array.hpp>
#include <vine/models/Mesh.hpp>

VN_MODELS_NS_BEGIN

/**
 * @brief A triangle mesh whose optional index buffer determines whether it is indexed.
 *
 * @details
 * - Without indices the vertices are a triangle soup: every three consecutive vertices form one face.
 * - With indices every three entries name one triangle; the position storage may hold more vertices than the indices
 *   name, and those extra vertices are stored but not drawn.
 * - The mesh is filled through the setters inherited from Mesh (setPositions() + setIndices()); choosing whether it is
 *   indexed is a matter of passing indices or an empty array.
 */
class TriangleMesh : public Mesh {
    VN_OBJECT_META(TriangleMesh, Mesh);

  public:
    TriangleMesh()
    {
        this->shape_type_ = ShapeType::TriangleMesh;
    }

    /**
     * @brief Returns the number of triangles.
     *
     * @return Triangle count.
     */
    [[nodiscard]]
    std::size_t triangleCount() const
    {
        return this->hasIndices() ? this->indices_->size() / 3u : this->vertexCount() / 3u;
    }

    [[nodiscard]]
    bool isValid() const override
    {
        if (this->positions_->empty()) {
            return false;
        }
        if (!this->hasIndices()) {
            return this->vertexCount() % 3u == 0u;
        }
        if (this->indices_->size() < 3u || this->indices_->size() % 3u != 0u) {
            return false;
        }
        for (const std::uint32_t index : this->indices()) {
            if (index >= this->vertexCount()) {
                return false;
            }
        }
        return true;
    }
};

VN_MODELS_NS_END
