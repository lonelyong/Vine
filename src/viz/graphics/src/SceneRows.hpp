#pragma once

#include <cstdint>

#include <vine/graphics/Scene.hpp>

#include "DrawListBake.hpp"
#include "SceneSnapshot.hpp"

VN_GRAPHICS_NS_BEGIN

namespace detail
{

/**
 * @brief THE ONE DOOR onto a Scene's rows as an immutable snapshot.
 *
 * @details
 * - WHY A DOOR AND NOT A METHOD. The rows are `detail::SceneBake`, an implementation type the public SDK must not
 *   name - but the snapshot builder lives in exactly those rows (`G1`: one table, one algorithm). A friend accessor
 *   is the narrowest way to say that: the public surface does not grow, and the coupling is one internal class the
 *   module owns.
 * - IT IS A CONTENT-THREAD API. It may build and box-refresh the scene's rows, which reads the graph and each
 *   leaf's data box - the one step of a collection that needs the thread that owns the content. The engine side
 *   reads the immutable result, never the graph.
 */
class SceneRows {
  public:
    /**
     * @brief Builds (or returns the cached) immutable snapshot at the scene's current published version.
     *
     * @details
     * - This is the bridge between the mutable `SceneBake` and the chunked snapshot: the copy drops the node record
     *   and takes the material values as they stand, so the result can be selected without touching the live graph.
     * - The returned value is immutable; editing the scene afterwards does not change what an older snapshot
     *   selects (the material / geometry values carried in its rows keep their revision).
     * - `Scene::publish` and the engine's `SnapshotSource` share this one builder; a batch keeps the previous cached
     *   snapshot because the published version is frozen.
     *
     * @param scene The scene whose rows are published.
     * @return The immutable snapshot; never null.
     */
    [[nodiscard]] static SceneSnapshotPtr snapshot(const Scene& scene);
};

} // namespace detail

VN_GRAPHICS_NS_END
