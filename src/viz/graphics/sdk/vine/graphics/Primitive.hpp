#pragma once
#include "graphics_global.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <utility>

#include <vine/Buffer.hpp>
#include <vine/intrusive_ptr.hpp>
#include <vine/math/Matrix4x4.hpp>

// The two owning pointers are members of a VALUE type, so a caller that copies or destroys a Primitive needs
// both complete - including them here is what keeps every call site from having to remember that.
#include "Material.hpp"
#include "ShaderAbi.hpp"
#include "ShaderProgram.hpp"
#include "Topology.hpp"

VN_GRAPHICS_NS_BEGIN

/**
 * @brief One DRAW of a geometry's data: how the vertices assemble, which indices it reads, and how it is
 *        shaded.
 *
 * @details
 * - THIS IS THE UNIT THE API REQUIRES. A draw has exactly one primitive topology, one index span and one
 *   program/material; a `Geometry` (a set of vertex channels shared by all its draws) may hold SEVERAL
 *   primitives, which is how "one asset, several parts" is expressed: a mesh, its deduplicated edge lines
 *   and a selected-point set share one vertex buffer and are three primitives.
 * - INDICES ARE PER PRIMITIVE, and that is a data lifetime decision rather than symmetry: a generated edge
 *   list or a selection set is rewritten while the mesh's own indices never change, and the backend shares
 *   index uploads per BUFFER (a bind aliases the whole buffer, the draw states the span) - so putting them
 *   in one buffer would couple their invalidation. Pointing several primitives at one buffer is still
 *   possible and then shares one upload, which is the caller's choice to make.
 * - THE INSTANCE STREAM IS PER PRIMITIVE TOO: this is one draw, so "draw N placements of it" is one
 *   parameter of this draw. A primitive without a stream is one plain draw; one with a stream draws its
 *   visible prefix. Several primitives may share one buffer through their own slices, which is how a
 *   multi-part model uses one upload without introducing a scene-graph scope node.
 * - MATERIAL AND PROGRAM are the shading side of one draw, and they are optional: an empty pointer means
 *   \"the GEOMETRY's default\" (Geometry::material / program), and an empty geometry default means \"the
 *   nearest StateNode's\" - which is how a subtree restyles what it contains. A primitive that states one
 *   owns it - reparenting the geometry cannot change it.
 * - The vertex data is NOT here: positions, normals, texcoords and custom channels belong to the
 *   `Geometry`, because they are what its primitives have in common.
 *
 * @note A `Geometry` with no primitives draws nothing; the single-primitive case (the overwhelming
 *       majority) is a geometry with one entry.
 */
struct VN_GRAPHICS_API Primitive {
    /// The index span's type: a segment of an index buffer, described the same way an attribute channel is
    /// (see BufferSlice). Kept as a name here because it is THIS struct's field, and a caller that spells
    /// "the whole buffer" once should not have to write the template out.
    using IndexSpan = BufferSlice<std::uint32_t>;

    Topology topology{ Topology::Triangles }; // How the vertices assemble (a pipeline's identity).

    /// The index span this draw reads, or an empty buffer for a NON-indexed draw (the vertex count then
    /// comes from the geometry's position channel). A slice of a shared arena works exactly as it does for
    /// a vertex channel: `count == 0` means "the rest of the buffer from `first`".
    IndexSpan indices{};

    /// Shading, or nullptr to use the geometry's default material (then the nearest StateNode's).
    MaterialValuesPtr material{};
    /// Program, or nullptr to use the geometry's default (then StateNode, then the pass).
    intrusive_ptr<ShaderProgram> program{};

    /// The instance stream this draw reads: 16 scalars per instance, column-major. Empty = one plain draw.
    /// A BufferSlice holds the buffer, so copying a Primitive shares the stream rather than copying it.
    BufferSlice<float> instance_transforms{};
    /// How many instances this draw reads from the slice's start (the `instanceCount` draw parameter);
    /// nullopt means every instance the slice holds. A draw parameter, not a shape: changing it does not re-upload.
    std::optional<std::size_t> instance_count{};

    /** @brief Gets whether this draw has an instance stream.
     * @return true when the slice holds at least one whole instance matrix.
     */
    [[nodiscard]] bool hasInstances() const noexcept
    {
        return instanceCapacity() != 0u;
    }

    /** @brief Gets how many instances the slice holds (its capacity).
     * @return Number of whole 16-scalar matrices.
     */
    [[nodiscard]] std::size_t instanceCapacity() const noexcept
    {
        return instance_transforms.size() / kInstanceMatrixScalars;
    }

    /** @brief Gets the first instance this draw reads (the slice's first scalar / 16).
     * @return The draw's firstInstance, or 0 when there is no stream.
     */
    [[nodiscard]] std::size_t firstInstance() const noexcept
    {
        return instance_transforms.values != nullptr ? instance_transforms.begin() / kInstanceMatrixScalars : 0u;
    }

    /** @brief Gets how many instances this draw reads, clamped to the slice's capacity.
     *
     * @details
     * - The value maps to the `instanceCount` draw parameter (see DrawItem::instance_count); the slice's own capacity
     *   is instanceCapacity(), which is a different question.
     *
     * @return Instances drawn by this primitive.
     */
    [[nodiscard]] std::size_t instanceCount() const noexcept
    {
        const std::size_t capacity = instanceCapacity();
        return instance_count.has_value() ? std::min(*instance_count, capacity) : capacity;
    }

    /** @brief Sets the instance stream after checking that it covers whole matrices.
     *
     * @details
     * - An empty slice clears the stream. A slice that does not start on an instance boundary, or whose
     *   resolved length is not a whole number of matrices, is rejected and leaves this primitive unchanged.
     * - The slice's own start is the draw's firstInstance; instance_count is not changed by this call.
     *
     * @param transforms Instance matrices (16 scalars each, column-major).
     * @return true when the stream was accepted; false when it is not a whole number of matrices.
     */
    bool setInstanceTransforms(BufferSlice<float> transforms)
    {
        if (transforms.values == nullptr) {
            instance_transforms = {};
            instance_count.reset();
            return true;
        }
        if (transforms.first % kInstanceMatrixScalars != 0u || transforms.size() % kInstanceMatrixScalars != 0u) {
            return false;
        }
        instance_transforms = std::move(transforms);
        return true;
    }

    /** @brief Clears the instance stream (the primitive becomes a plain, one-instance draw). */
    void clearInstanceTransforms() noexcept
    {
        instance_transforms = {};
        instance_count.reset();
    }

    /** @brief Limits how many instances this draw reads (a prefix of the slice).
     *
     * @param count Instances to draw; clamped by instanceCount().
     */
    void setInstanceCount(std::size_t count) noexcept
    {
        instance_count = count;
    }

    /** @brief Reads every instance again. */
    void clearInstanceCount() noexcept
    {
        instance_count.reset();
    }

    /** @brief Gets one instance's matrix, relative to the slice.
     *
     * @param instance Index in [0, instanceCapacity()).
     * @return The matrix, or nullopt when the index is outside the slice.
     */
    [[nodiscard]] std::optional<vn::math::Mat4d> instanceMatrix(std::size_t instance) const
    {
        if (instance >= instanceCapacity()) {
            return std::nullopt;
        }
        const std::span<const float> scalars = instance_transforms.span();
        const std::size_t            offset  = instance * kInstanceMatrixScalars;
        if (offset + kInstanceMatrixScalars > scalars.size()) {
            return std::nullopt;
        }
        std::array<double, kInstanceMatrixScalars> values{};
        for (std::size_t index = 0u; index < kInstanceMatrixScalars; ++index) {
            values[index] = static_cast<double>(scalars[offset + index]);
        }
        return vn::math::Mat4d(values.data());
    }

    /** @brief The scalars one instance matrix occupies. */
    static constexpr std::size_t kInstanceMatrixScalars = static_cast<std::size_t>(vn::graphics::kInstanceMatrixComponents);

    /** @brief Gets whether this draw is indexed.
     * @return true when it reads indices (a non-empty index buffer).
     */
    [[nodiscard]] bool isIndexed() const noexcept
    {
        return !indices.empty();
    }

    /** @brief Gets how many indices this draw reads (0 for a non-indexed draw).
     * @return The resolved span length, following a growing buffer when `count` is 0.
     */
    [[nodiscard]] std::size_t indexCount() const noexcept
    {
        return indices.size();
    }
};

VN_GRAPHICS_NS_END
