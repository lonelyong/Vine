#pragma once

#include <cstdint>

#include <vine/graphics/Scene.hpp>

#include "DrawListBake.hpp"
#include "SceneSnapshot.hpp"

VN_GRAPHICS_NS_BEGIN

namespace detail
{

/**
 * @brief THE ONE DOOR onto a Scene's rows: their version, and a copy of them as values.
 *
 * @details
 * - WHY A DOOR AND NOT A METHOD. The rows are `detail::SceneBake`, an implementation type the public SDK must not
 *   name - but the engine side's mirror is published from exactly those rows (`G1`: one table, one algorithm, two
 *   owners). A friend accessor is the narrowest way to say that: the public surface does not grow, and the coupling
 *   is one internal class the module owns.
 * - IT IS A CONTENT-THREAD API. Both calls may build and box-refresh the scene's rows, which reads the graph and each
 *   leaf's data box - the one step of a collection that needs the thread that owns the content. The engine side never
 *   calls them: it drains what `publishRows` put on its channel (`ContentMirror`).
 * - WHY A VERSION AT ALL. A mirror must be able to ask "is what I hold still what the content says" without walking
 *   anything. Node edits move the root's content epoch, and a BYTE edit does not (it announces itself on a buffer,
 *   `2.4`) - it moves a box instead - so the version is both halves, and a mirror that compares only the epoch would
 *   keep a stale box forever.
 */
class SceneRows {
  public:
    /// @brief What a mirror compares to know whether its copy is still the content's answer.
    struct Version {
        std::uint64_t epoch{ 0 }; ///< The root subtree's content epoch the rows were built at (node edits).
        std::uint64_t boxes{ 0 }; ///< How many times a re-read box actually moved (byte edits).
        /// How many times a MATERIAL value moved under these rows (see detail::refreshMaterials): a material edit is
        /// announced by no node, so it needs its own number - and it is what makes a mid-batch edit invisible here.
        std::uint64_t materials{ 0 };

        /** @brief Returns whether two versions describe the same rows. */
        [[nodiscard]] bool operator==(const Version& other) const noexcept = default;
    };

  public:
    /**
     * @brief Brings the scene's rows up to date and answers their version.
     *
     * @param scene The scene whose rows the caller mirrors.
     * @return The version those rows have now.
     */
    [[nodiscard]] static Version version(const Scene& scene);

    /**
     * @brief Copies the scene's rows into @p out, as values, without the node record.
     *
     * @details
     * - `SceneBake::nodes` is the content's own bookkeeping (it is what re-reads a box), and the engine side must not
     *   name a node - so it is dropped here, exactly as `RenderWorld::adopt` would drop it.
     * - The copy is the price of P1's coarse publish: it happens when the version moved, not per frame.
     *
     * @param scene The scene whose rows the caller mirrors.
     * @param out   Receives the rows (replacing its contents).
     */
    static void publish(const Scene& scene, SceneBake& out);

    /**
     * @brief Copies the scene's rows into an immutable snapshot at their current version.
     *
     * @details
     * - This is the bridge between today's mutable `SceneBake` and the chunked snapshot: the copy drops the node
     *   record and re-takes the material values exactly like `publish`, so the result can be selected without
     *   touching the live graph.
     * - The returned value is immutable; editing the scene afterwards does not change what an older snapshot
     *   selects (the material / geometry values carried in its rows keep their revision).
     *
     * @param scene The scene whose rows are published.
     * @return The immutable snapshot; never null.
     */
    [[nodiscard]] static SceneSnapshotPtr snapshot(const Scene& scene);
};

} // namespace detail

VN_GRAPHICS_NS_END
