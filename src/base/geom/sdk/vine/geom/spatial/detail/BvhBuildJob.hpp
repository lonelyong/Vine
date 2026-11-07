#pragma once

#include <vine/geom/geom_global.hpp>

#include <cstdint>
#include <span>
#include <vector>

#include <vine/geom/spatial/detail/VolumeTraits.hpp>

VN_GEOM_SPATIAL_NS_BEGIN

namespace detail
{

/**
 * @brief One node of the flattened tree.
 *
 * @details
 * - `count == 0` marks an internal node and `count > 0` marks a leaf, so no separate tag or padding is needed.
 * - Internal node: the left child is always the next slot (`node + 1`) and `offset` holds the right child, which is
 *   what makes a depth-first layout possible with a single offset field.
 * - Leaf: `offset` is the first index into the primitive order and `count` is how many of it the leaf covers.
 * - For `float` and `Aabb` a node is 32 bytes, half a cache line.
 *
 * @tparam T Floating-point scalar type.
 * @tparam Volume Bounding-volume template.
 */
template <typename T, template <typename> class Volume>
struct BvhNode
{
    /// Volume enclosing everything the subtree below this node covers.
    Volume<T> bounds;

    /// Right-child index for an internal node; first primitive index for a leaf.
    std::uint32_t offset{ 0 };

    /// Primitive count for a leaf; zero for an internal node.
    std::uint32_t count{ 0 };
};

/**
 * @brief Everything a build algorithm may read and write: three spans and two limits.
 *
 * @details
 * - The tree owns the arrays; a builder sees this view and nothing else, so it cannot change the primitive bounds, the
 *   capacity policy or any other part of the structure.
 * - A builder must leave `order` a permutation of `[0, primitiveCount())`, write nodes from index 0 in depth-first order
 *   with the left child at `node + 1`, keep every parent enclosing its children, and stay within `max_depth`.
 *   `Bvh::validate()` checks exactly that.
 *
 * @tparam T Floating-point scalar type.
 * @tparam Volume Bounding-volume template.
 */
template <typename T, template <typename> class Volume>
struct BuildJob
{
    using Scalar  = T;
    using VolumeT = Volume<T>;
    using PrimId  = std::uint32_t;
    using Node    = BvhNode<T, Volume>;

    /// Primitive volumes, indexed by `PrimId`; read-only.
    std::span<const VolumeT> bounds;

    /// Primitive order to fill; a permutation of the indices into `bounds`.
    std::span<PrimId> order;

    /// Node storage to fill; a builder appends to it, so the tree grows the arena when a split needs two more slots.
    std::vector<Node>& nodes;

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
     *
     * @return The index of the new slot, which is also the node count before the call.
     */
    PrimId appendNode()
    {
        nodes.emplace_back();
        node_count = static_cast<PrimId>(nodes.size());
        return static_cast<PrimId>(nodes.size() - 1);
    }

    /**
     * @brief Returns the volume enclosing a run of the primitive order.
     *
     * @param first First index into `order`.
     * @param count Number of primitives; must not be zero.
     * @return The merged volume; the first primitive's volume seeds it, so no "empty volume" concept is needed.
     */
    [[nodiscard]] VolumeT rangeBounds(PrimId first, PrimId count) const
    {
        VolumeT out = bounds[order[first]];
        for (PrimId index = first + 1; index < first + count; ++index) {
            out = VolumeTraits<VolumeT>::merged(out, bounds[order[index]]);
        }
        return out;
    }

    /**
     * @brief Fills a node slot as a leaf covering a run of the primitive order.
     *
     * @param node  Node index.
     * @param first First index into `order`.
     * @param count Number of primitives in the leaf.
     */
    void writeLeaf(PrimId node, PrimId first, PrimId count)
    {
        nodes[node].bounds = rangeBounds(first, count);
        nodes[node].offset = first;
        nodes[node].count  = count;
    }

    /**
     * @brief Fills a node slot as a leaf over an already computed volume.
     *
     * @param node   Node index.
     * @param bounds_leaf Volume enclosing the leaf.
     * @param first  First index into `order`.
     * @param count  Number of primitives in the leaf.
     */
    void writeLeaf(PrimId node, const VolumeT& bounds_leaf, PrimId first, PrimId count)
    {
        nodes[node].bounds = bounds_leaf;
        nodes[node].offset = first;
        nodes[node].count  = count;
    }
};

} // namespace detail

VN_GEOM_SPATIAL_NS_END
