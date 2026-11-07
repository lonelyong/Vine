#pragma once

#include <vine/models/models_global.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <initializer_list>
#include <span>
#include <stdexcept>
#include <utility>

#include <vine/models/Mesh.hpp>
#include <vine/models/TriangleMesh.hpp>

VN_MODELS_NS_BEGIN

/**
 * @brief A polygon mesh whose face offsets delimit faces with three or more corners.
 *
 * @details
 * - The mesh is filled through the setters inherited from Mesh plus setFaceOffsets(): positions, optional indices and the
 *   face boundaries are stated together, and the boundaries are what makes the corner runs into faces.
 */
class MixedMesh : public Mesh {
    VN_OBJECT_META(MixedMesh, Mesh);

  public:
    /**
     * @brief Constructs an empty mixed polygon mesh.
     */
    MixedMesh()
    {
        this->shape_type_ = ShapeType::MixedMesh;
    }

    /**
     * @brief Returns the face corner offsets.
     *
     * The array starts with zero and ends with the number of stored indices, or vertices when non-indexed.
     *
     * @return Read-only view containing one boundary per face plus the terminal boundary.
     */
    [[nodiscard]]
    std::span<const std::uint32_t> faceOffsets() const noexcept
    {
        return face_offsets_ != nullptr ? face_offsets_->view() : std::span<const std::uint32_t>{};
    }

    /**
     * @brief Returns the shared face-offset storage.
     *
     * @return Buffer containing the corner boundary offsets, or null when unset.
     */
    [[nodiscard]]
    intrusive_ptr<const Buffer<std::uint32_t>> faceOffsetsBuffer() const noexcept
    {
        return face_offsets_;
    }

    /**
     * @brief Shares a buffer containing the face corner offsets.
     *
     * @note
     * - The boundaries are what turn corners into triangles, so replacing them drops a built triangle tree.
     *
     * @param offsets Buffer containing boundaries from zero to the corner count; null clears the offsets.
     */
    void setFaceOffsets(intrusive_ptr<const Buffer<std::uint32_t>> offsets)
    {
        face_offsets_ = std::move(offsets);
        this->clearBvh();
    }

    /**
     * @brief Replaces the face corner offsets by adopting an array into a buffer.
     *
     * @param offsets Face boundaries, starting at zero and ending at the corner count.
     */
    void setFaceOffsets(UInt32Array offsets)
    {
        face_offsets_ = this->template makeBuffer<std::uint32_t>(std::move(offsets));
        this->clearBvh();
    }

    /**
     * @brief Replaces face offsets from an initializer list.
     *
     * @param offsets Face boundaries, starting at zero and ending at the corner count.
     */
    void setFaceOffsets(std::initializer_list<std::uint32_t> offsets)
    {
        setFaceOffsets(UInt32Array(offsets));
    }

    /**
     * @brief Returns the polygon face count.
     *
     * @return Number of faces.
     */
    [[nodiscard]]
    std::size_t faceCount() const noexcept
    {
        const auto offsets = faceOffsets();
        return offsets.empty() ? 0u : offsets.size() - 1u;
    }

    /**
     * @brief Returns how many triangles the fans make: one fewer than the corners, per face.
     *
     * @details
     * - A face with `k` corners splits into `k - 2` triangles, so the total is the corner count minus twice the face
     *   count. The count is taken from the offsets instead of being stored: a second number that must agree with them
     *   is a second thing to get wrong.
     *
     * @return Triangle count; zero when the offsets do not describe the corner stream.
     */
    [[nodiscard]]
    std::size_t triangleCount() const override
    {
        const auto        offsets = faceOffsets();
        const std::size_t faces   = faceCount();
        if (faces == 0u) {
            return 0u;
        }
        const std::size_t corners = this->hasIndices() ? this->indices().size() : this->vertexCount();
        if (offsets.front() != 0u || static_cast<std::size_t>(offsets.back()) != corners || corners < 2u * faces) {
            return 0u;
        }
        return corners - 2u * faces;
    }

    /**
     * @brief Returns the three vertex indices of one triangle of the fans.
     *
     * @details
     * - Face `f` contributes triangles `offsets[f] - 2·f` up to `offsets[f + 1] - 2·(f + 1)`; that start index is
     *   strictly increasing over a valid topology, so the face holding a triangle ordinal is found by binary search and
     *   no per-triangle table is stored.
     * - Triangle `t` of face `f` is `(c0, c1+n, c2+n)` with `n = t - start(f)` - the same fan toTriangleMesh() uses,
     *   which keeps a triangle's ordinal stable across the conversion.
     *
     * @param triangle Triangle ordinal, below triangleCount().
     * @param corners  Receives the three vertex indices; untouched when the call fails.
     * @return true when the triangle exists and all three indices are in range; false when the offsets do not delimit
     *         faces of at least three corners.
     */
    [[nodiscard]]
    bool triangleCorners(std::size_t triangle, std::array<std::uint32_t, 3>& corners) const override
    {
        const auto        offsets = faceOffsets();
        const std::size_t faces   = faceCount();
        if (faces == 0u || triangle >= triangleCount()) {
            return false;
        }
        const auto start_of = [&](std::size_t face) {
            return static_cast<std::size_t>(offsets[face]) - 2u * face;
        };
        std::size_t low  = 0u;
        std::size_t high = faces - 1u;
        while (low < high) {
            const std::size_t middle = (low + high + 1u) / 2u;
            if (start_of(middle) <= triangle) {
                low = middle;
            }
            else {
                high = middle - 1u;
            }
        }
        const std::uint32_t begin = offsets[low];
        const std::uint32_t end   = offsets[low + 1u];
        const std::size_t   fan   = triangle - start_of(low);
        if (fan + 3u > static_cast<std::size_t>(end - begin)) {
            return false;
        }
        const std::uint32_t a = cornerIndex(begin);
        const std::uint32_t b = cornerIndex(static_cast<std::uint32_t>(begin + 1u + fan));
        const std::uint32_t c = cornerIndex(static_cast<std::uint32_t>(begin + 2u + fan));
        const std::size_t   vertices = this->vertexCount();
        if (a >= vertices || b >= vertices || c >= vertices) {
            return false;
        }
        corners = { a, b, c };
        return true;
    }

    /**
     * @brief Converts this valid polygon mesh into an indexed triangle mesh using fan triangulation.
     *
     * Vertex attributes and the shape name are copied to the result.
     *
     * @warning Throws std::invalid_argument when offsets or indices are invalid.
     *
     * @return The converted triangle mesh.
     */
    [[nodiscard]]
    intrusive_ptr<TriangleMesh> toTriangleMesh() const
    {
        if (!isValid()) {
            throw std::invalid_argument("MixedMesh::toTriangleMesh requires a valid mesh");
        }

        UInt32Array triangle_indices;
        const auto offsets = faceOffsets();
        const std::size_t triangle_count = [&] {
            std::size_t count = 0u;
            for (std::size_t face = 0; face < faceCount(); ++face) {
                count += static_cast<std::size_t>(offsets[face + 1u] - offsets[face] - 2u);
            }
            return count;
        }();
        triangle_indices.reserve(triangle_count * 3u);
        for (std::size_t face = 0; face < faceCount(); ++face) {
            const std::uint32_t begin = offsets[face];
            const std::uint32_t end = offsets[face + 1u];
            for (std::uint32_t corner = begin + 1u; corner < end - 1u; ++corner) {
                triangle_indices.push_back(cornerIndex(begin));
                triangle_indices.push_back(cornerIndex(corner));
                triangle_indices.push_back(cornerIndex(corner + 1u));
            }
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
     * @brief Reports whether offsets and face corners form valid polygon faces.
     *
     * @return true when every face has at least three valid corners.
     */
    [[nodiscard]]
    bool isValid() const override
    {
        const std::size_t corner_count = this->hasIndices() ? this->indices().size() : this->vertexCount();
        const auto offsets = faceOffsets();
        if (this->positions_->empty() || offsets.size() < 2u || offsets.front() != 0u || offsets.back() != corner_count) {
            return false;
        }
        for (std::size_t face = 0; face < faceCount(); ++face) {
            const std::uint32_t begin = offsets[face];
            const std::uint32_t end = offsets[face + 1u];
            if (end < begin || end - begin < 3u) {
                return false;
            }
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

  private:
    [[nodiscard]]
    std::uint32_t cornerIndex(std::uint32_t corner) const
    {
        return this->hasIndices() ? this->indices()[corner] : corner;
    }

    intrusive_ptr<const Buffer<std::uint32_t>> face_offsets_;
};

VN_MODELS_NS_END
