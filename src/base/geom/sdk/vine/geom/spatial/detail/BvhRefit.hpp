#pragma once

#include <vine/geom/geom_global.hpp>

#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

#include <vine/geom/spatial/BvhTopology.hpp>
#include <vine/geom/spatial/detail/VolumeTraits.hpp>

VN_GEOM_SPATIAL_NS_BEGIN

namespace detail
{

/**
 * @brief Recomputes every node's volume from the primitives, keeping the topology.
 *
 * @details
 * - ONE reverse pass: children always sit after their parent, which makes the sweep a valid post-order, so a parent is
 *   merged only after both of its children are complete.
 * - This is the only implementation of "derive the boxes from the current primitives": `Bvh::refit()` (after moving
 *   primitives) and a restore (after installing a grouping) both call it, so a restored tree cannot end up with a
 *   different notion of what a box is.
 * - It writes the node BOUNDS only. The topology arrays are inputs, and nothing here can change them - which is what
 *   makes "a refit never changes the shape" a property of the signature rather than of the body.
 *
 * @tparam T      Floating-point scalar type.
 * @tparam Volume Bounding-volume template.
 * @param nodes       The node combinatorics, in depth-first order; read-only.
 * @param node_bounds The per-node volumes, parallel to @p nodes; written.
 * @param order       The primitive permutation the leaves index into.
 * @param bounds      The primitives' volumes, indexed by primitive id.
 */
template <typename T, template <typename> class Volume>
void refitNodes(std::vector<BvhNodeTopology>& nodes,
                std::vector<Volume<T>>&           node_bounds,
                std::span<const std::uint32_t>    order,
                std::span<const Volume<T>>        bounds)
{
    using Traits = VolumeTraits<Volume<T>>;
    for (std::size_t index = nodes.size(); index-- > 0;) {
        const BvhNodeTopology& slot = nodes[index];
        if (slot.count != 0) {
            Volume<T> merged = bounds[order[slot.offset]];
            for (std::uint32_t taken = slot.offset + 1u; taken < slot.offset + slot.count; ++taken) {
                merged = Traits::merged(merged, bounds[order[taken]]);
            }
            node_bounds[index] = merged;
            continue;
        }
        node_bounds[index] = Traits::merged(node_bounds[index + 1u], node_bounds[slot.offset]);
    }
}

} // namespace detail

VN_GEOM_SPATIAL_NS_END
