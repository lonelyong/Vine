#pragma once

#include <atomic>
#include <cstdint>

VN_GRAPHICS_NS_BEGIN

namespace detail
{

/**
 * @brief The scene graph's LAYER EPOCH: one counter every layer/structure change bumps.
 *
 * @details
 * - WHY A SEPARATE CLOCK. A node's subtree layer union is a pure function of the layer declarations and the tree
 *   shape, so it must not be thrown away by geometry, material, state or transform edits (which bump the content
 *   epoch instead, see Node::announceContentChange).
 * - WHAT BUMPS IT: `Node::setLayers` / `clearLayers` when the declaration really changes, `Group::addChild` /
 *   `removeChild` when the structure really changes, and `Scene::setRoot` / `clear`. It is deliberately not
 *   bumped by visibility, opacity, draw order, transforms or content edits.
 * - The clock is RELAXED and PROCESS-WIDE, unlike the content epoch (which is scoped to the root subtree): a union
 *   cache that misses a bump is simply recomputed on its next walk (a stale cache is never used for a prune
 *   decision, see Node::subtreeLayerUnion), and a layer edit in one scene never costs another scene an extra walk -
 *   only a recompute when that scene walks anyway.
 */
inline std::atomic<std::uint64_t> g_layer_epoch{ 0 };

/// @brief Announces that a resolved-layer-affecting change happened (see g_layer_epoch).
inline void announceLayerChange() noexcept
{
    g_layer_epoch.fetch_add(1, std::memory_order_relaxed);
}

/** @brief Gets the current layer epoch (see g_layer_epoch). */
[[nodiscard]] inline std::uint64_t layerEpoch() noexcept
{
    return g_layer_epoch.load(std::memory_order_relaxed);
}

}  // namespace detail

VN_GRAPHICS_NS_END
