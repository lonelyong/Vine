#pragma once

#include <vine/geom/geom_global.hpp>

#include <cstdint>
#include <span>
#include <vector>

#include <vine/geom/spatial/BvhTopology.hpp>
#include <vine/geom/spatial/detail/VolumeTraits.hpp>

VN_GEOM_SPATIAL_NS_BEGIN

namespace detail
{

/**
 * @brief Everything a build algorithm may read and write: the tree's own arrays plus two limits.
 *
 * @details
 * - The tree owns the arrays; a builder sees this view and nothing else, so it cannot change the primitive bounds, the
 *   capacity policy or any other part of the structure.
 * - The node combinatorics and the node volumes are TWO arrays that must stay parallel: every slot a builder appends
 *   exists in both, which is what lets a stored shape be the tree's own node array rather than a transcription of it.
 *   `appendNode()` is the only way to grow them, so "the two arrays have the same size" cannot drift.
 * - A builder must leave `order` a permutation of `[0, primitiveCount())`, write nodes from index 0 in depth-first order
 *   with the left child at `node + 1`, keep every parent enclosing its children, and stay within `max_depth`.
 *   `Bvh::validate()` checks exactly that.
 *
 * @tparam T      Floating-point scalar type.
 * @tparam Volume Bounding-volume template.
 */
template <typename T, template <typename> class Volume>
struct BuildJob
{
    using Scalar  = T;
    using VolumeType = Volume<T>;
    using PrimId  = std::uint32_t;
    using Node    = BvhNodeTopology;

    /// Primitive volumes, indexed by `PrimId`; read-only.
    std::span<const VolumeType> prim_bounds;

    /// Primitive order to fill; a permutation of the indices into @ref prim_bounds.
    std::span<PrimId> order;

    /// Node combinatorics to fill; a builder appends to it, so the tree grows the arena when a split needs two slots.
    std::vector<Node>& nodes;

    /// Per-node volumes, parallel to @ref nodes; written by the same calls that append a slot.
    std::vector<VolumeType>& node_bounds;

    /// Maximum primitives per leaf.
    unsigned leaf_size{ 4 };

    /// Hard limit on the tree height.
    unsigned max_depth{ 64 };

    /// Written by the builder: the deepest leaf's depth.
    unsigned depth{ 0 };

    /// Written by the builder: how many node slots were used.
    PrimId node_count{ 0 };

    /**
     * @brief Appends a node slot and returns its index.
     *
     * @details
     * - Appending rather than writing into a pre-sized arena is what keeps the memory proportional to the tree that was
     *   actually built: a split can produce far more leaves than `count / leaf_size`, and sizing for the worst case
     *   would cost several times the memory the tree needs.
     * - Both parallel arrays grow together, so a slot always has a volume to write into.
     *
     * @return The index of the new slot, which is also the node count before the call.
     */
    PrimId appendNode()
    {
        const auto index = static_cast<PrimId>(nodes.size());
        nodes.emplace_back();
        node_bounds.emplace_back();
        node_count = static_cast<PrimId>(nodes.size());
        return index;
    }

    /**
     * @brief Returns the volume enclosing a run of the primitive order.
     *
     * @param first First index into @ref order.
     * @param count Number of primitives; must not be zero.
     * @return The merged volume; the first primitive's volume seeds it, so no "empty volume" concept is needed.
     */
    [[nodiscard]] VolumeType rangeBounds(PrimId first, PrimId count) const
    {
        VolumeType out = prim_bounds[order[first]];
        for (PrimId index = first + 1; index < first + count; ++index) {
            out = VolumeTraits<VolumeType>::merged(out, prim_bounds[order[index]]);
        }
        return out;
    }

    /**
     * @brief Fills a node slot as a leaf covering a run of the primitive order.
     *
     * @param node  Node index.
     * @param first First index into @ref order.
     * @param count Number of primitives in the leaf.
     */
    void writeLeaf(PrimId node, PrimId first, PrimId count)
    {
        writeLeaf(node, rangeBounds(first, count), first, count);
    }

    /**
     * @brief Fills a node slot as a leaf over an already computed volume.
     *
     * @param node        Node index.
     * @param leaf_bounds Volume enclosing the leaf.
     * @param first       First index into @ref order.
     * @param count       Number of primitives in the leaf.
     */
    void writeLeaf(PrimId node, const VolumeType& leaf_bounds, PrimId first, PrimId count)
    {
        node_bounds[node]  = leaf_bounds;
        nodes[node].offset = first;
        nodes[node].count  = count;
    }
};

} // namespace detail

VN_GEOM_SPATIAL_NS_END
