#pragma once

#include <vine/geom/geom_global.hpp>

#include <array>
#include <cstdint>
#include <optional>
#include <span>
#include <utility>
#include <vector>

#include <vine/geom/spatial/BvhTopology.hpp>

VN_GEOM_SPATIAL_NS_BEGIN

namespace detail
{

/**
 * @brief Whether @p order is a permutation of `[0, count)`.
 *
 * @param order The primitive order to check.
 * @param count The primitive count it must cover exactly once each.
 * @return true when every id in `[0, count)` appears exactly once.
 */
[[nodiscard]] inline bool orderIsPermutation(std::span<const std::uint32_t> order, std::uint32_t count)
{
    std::vector<bool> seen(count, false);
    for (const std::uint32_t id : order) {
        if (id >= count || seen[id]) {
            return false;
        }
        seen[id] = true;
    }
    return true;
}

/**
 * @brief Whether every node's numbers land inside the arrays they refer to.
 *
 * @details
 * - Internal (`count == 0`): the left child is the next slot, so the right child has to follow it and stay in the node
 *   array.
 * - Leaf (`count > 0`): its run has to lie inside `[0, primitive_count)`.
 *
 * @param nodes           Nodes in depth-first order.
 * @param primitive_count Primitives the leaf runs may cover.
 * @return true when no node points outside the node array or the primitive order.
 */
[[nodiscard]] inline bool nodesFitPrimitives(std::span<const BvhNodeTopology> nodes, std::uint32_t primitive_count) noexcept
{
    const auto node_count = static_cast<std::uint32_t>(nodes.size());
    for (std::uint32_t index = 0; index < node_count; ++index) {
        const BvhNodeTopology& node = nodes[index];
        if (node.count == 0) {
            if (index + 1u >= node_count || node.offset <= index + 1u || node.offset >= node_count) {
                return false;
            }
            continue;
        }
        if (node.offset > primitive_count || node.count > primitive_count - node.offset) {
            return false;
        }
    }
    return true;
}

/**
 * @brief The height the node array actually describes, or nothing when it is not one tree.
 *
 * @details
 * - One walk from the root on the traversal's own stack shape, measuring the deepest leaf and counting the slots it
 *   reaches: an array with a stray slot is not a tree, however well-formed each entry looks on its own.
 * - Requires nodesFitPrimitives() to have accepted @p nodes - every index it follows is then in range.
 *
 * @param nodes Nodes in depth-first order.
 * @return The deepest leaf's depth, or nullopt when a slot is unreachable or the walk is deeper than any traversal.
 */
[[nodiscard]] inline std::optional<unsigned> measuredHeight(std::span<const BvhNodeTopology> nodes) noexcept
{
    unsigned                                                        height  = 0;
    std::uint32_t                                                   visited = 0;
    std::array<std::pair<std::uint32_t, unsigned>, kBvhMaxDepth + 2u> stack{};
    unsigned                                                        top = 0;
    stack[top++]                                                      = { 0u, 0u };
    while (top > 0) {
        const auto [index, level] = stack[--top];
        ++visited;
        height = level > height ? level : height;
        const BvhNodeTopology& node = nodes[index];
        if (node.count != 0) {
            continue;
        }
        if (top + 2u > stack.size()) {
            return std::nullopt; // deeper than any traversal stack: refuse rather than write a tree nobody can walk
        }
        stack[top++] = { index + 1u, level + 1u };
        stack[top++] = { node.offset, level + 1u };
    }
    return visited == nodes.size() ? std::optional<unsigned>(height) : std::nullopt;
}

/**
 * @brief The one statement of what makes a stored shape installable over @p primitive_count primitives.
 *
 * @details
 * - A shape is data from outside this process, so "it describes this tree" is checked rather than assumed: the arrays
 *   have to be the right sizes, the order a permutation, every node's numbers inside the primitive order, the node array
 *   one tree, and its real height the stored one.
 * - Both ways in read this: `RestoreTopology` copies a borrowed shape into a build job, `Bvh::restoreTopology()` adopts an
 *   owned one. Splitting the check from the copy is what lets the second path move instead of copy without growing a
 *   second notion of validity.
 *
 * @param shape      The shape to check.
 * @param primitives Number of primitives it must describe.
 * @param max_depth  Height limit the traversal stacks are sized for.
 * @return The measured height on success (it equals `shape.depth`), or nullopt when the shape does not describe the tree.
 */
[[nodiscard]] inline std::optional<unsigned> installableHeight(BvhTopologyView shape, std::uint32_t primitives, unsigned max_depth)
{
    if (primitives == 0 || shape.nodes.empty() || shape.nodes.size() > 2u * primitives) {
        return std::nullopt; // one slot per leaf at most, and an internal node per split on top of that
    }
    if (shape.order.size() != primitives) {
        return std::nullopt;
    }
    if (!orderIsPermutation(shape.order, primitives) || !nodesFitPrimitives(shape.nodes, primitives)) {
        return std::nullopt;
    }
    const std::optional<unsigned> height = measuredHeight(shape.nodes);
    if (!height.has_value() || *height != shape.depth || *height > max_depth) {
        return std::nullopt;
    }
    return height;
}

} // namespace detail

VN_GEOM_SPATIAL_NS_END
