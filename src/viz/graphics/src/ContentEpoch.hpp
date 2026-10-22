#pragma once

#include <atomic>
#include <cstdint>

VN_GRAPHICS_NS_BEGIN

namespace detail
{

/**
 * @brief The scene graph's process-wide CONTENT EPOCH: one counter every node mutation bumps.
 *
 * @details
 * - WHY A PROCESS-WIDE COUNTER. A collected command list is a pure function of the scene graph, so reusing one
 *   across frames is only sound while NOTHING in the graph changed. The nodes cannot reach their Scene (there is no
 *   back-pointer, deliberately: see .ai/design/graphics-skeleton-review-2026-10-20.md §0), so the change has to be
 *   announced to something both ends can see - this counter.
 * - WHAT IT BUYS: `Scene`'s collection memo can outlive one frame. A static scene then costs one tree walk when it
 *   changes instead of one per frame (measured in
 *   `SceneTest.MeasureWhatOneCollectionCostsWithAMovingCamera`: the `per_frame` column).
 * - WHAT IT COSTS, honestly: any mutation anywhere invalidates the memo of EVERY scene (relaxed atomic, one bump per
 *   edit - nothing per frame). With the one scene an application renders that is exactly right; a scene being edited
 *   while another is drawn pays one extra walk.
 * - The clock is RELAXED: it orders nothing, it only answers "did the graph change since I looked", and a collect
 *   that misses a bump in the same instant re-reads the next time it asks.
 */
inline std::atomic<std::uint64_t> g_content_epoch{ 0 };

/**
 * @brief A second, narrower epoch: changes that can alter a node's RESOLVED layers.
 *
 * @details
 * - WHY IT IS NOT THE CONTENT EPOCH. The content epoch is bumped by every geometry, material, state and
 *   transform edit. The subtree layer union only depends on the layer declarations and the tree shape, so
 *   reusing the content epoch would throw away the pruning cache on edits that cannot change a layer.
 * - WHAT BUMPS IT: `Node::setLayers` / `clearLayers` when the declaration really changes, `Group::addChild` /
 *   `removeChild` when the structure really changes, and `Scene::setRoot` / `clear`. It is deliberately not
 *   bumped by visibility, opacity, draw order, transforms or content edits.
 * - The clock is RELAXED and process-wide, like the content epoch: nodes cannot reach their Scene, and a
 *   cached union that misses a bump is simply recomputed on the next walk (a stale cache is never used for a
 *   prune decision, see Node::subtreeLayerUnion).
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

/// @brief Announces that the scene graph changed (see g_content_epoch).
inline void announceContentChange() noexcept
{
    g_content_epoch.fetch_add(1, std::memory_order_relaxed);
}

/**
 * @brief Gets the current content epoch.
 *
 * @return The counter's value; equal values mean "nothing was announced in between".
 */
[[nodiscard]] inline std::uint64_t contentEpoch() noexcept
{
    return g_content_epoch.load(std::memory_order_relaxed);
}

}  // namespace detail

VN_GRAPHICS_NS_END
