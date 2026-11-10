#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <vector>

#include "graphics_global.hpp"

VN_GRAPHICS_NS_BEGIN

/**
 * @brief The kinds of GPU residency a backend retains, one row each in a @ref ResidencyLedger.
 *
 * @details
 * - One value per KIND, not per object: a row answers "how much of this sort is resident", which is what a
 *   HUD, a gate or a host trimming its content can act on. The kinds are the ones this framework's backends
 *   actually retain (see `.ai/decisions/ADR-2026-11-10-gpu-residency-pinned-by-content.md`).
 * - A kind is a NAME for the accounting, not an identity for the resource: two backends may create different
 *   GPU objects for one stream and still report the same kind.
 */
enum class ResidencyKind : std::uint8_t
{
    Streams,        // Vertex / index / instance streams (keyed by buffer + that buffer's revision + the slice).
    Images,         // Uploaded textures.
    MaterialBlocks, // Per-material uniform blocks (the material arena's mapped region).
    Halves,         // Compiled pipelines (program + layout + variant + topology + target shape).
    Sets,           // Descriptor sets bound by content draws.
    Count,          // Not a kind: the number of kinds, so a table can be sized without a literal.
};

/**
 * @brief The number of @ref ResidencyKind values, as a size a table can be sized by.
 */
inline constexpr std::size_t kResidencyKindCount = static_cast<std::size_t>(ResidencyKind::Count);

/**
 * @brief What one kind of residency currently holds, and what it has cost so far.
 *
 * @details
 * - ABSENT IS NOT ZERO (the rule `FrameReport` states for its own parts): a table that does not keep a number
 *   answers with a disengaged `std::optional`, never with 0. A backend that holds no material blocks and one
 *   that does not count them are different answers, and only the first may be read as "nothing is resident".
 * - `live` is engaged always: every table can say how many entries it holds. The three cumulative counters
 *   (`built` / `retired` / `refused`) are the ones a table may not keep.
 * - `bytes` is the GPU bytes this kind occupies NOW (`live`'s total), not a cumulative figure: a release has to
 *   make it go DOWN, which is what makes it usable as a trim signal.
 */
struct ResidencyRow
{
    ResidencyKind                 kind{ ResidencyKind::Streams }; // Which kind this row describes.
    std::uint32_t                 live{ 0 };                      // Entries resident now.
    std::optional<std::uint64_t>  bytes;                          // GPU bytes those entries occupy, when counted.
    std::optional<std::uint64_t>  built;                          // Cumulative: how many were built (upload/compile).
    std::optional<std::uint64_t>  retired;                        // Cumulative: how many were let go.
    std::optional<std::uint64_t>  refused;                        // Cumulative: how many requests this kind refused.
};

/**
 * @brief The one answer a @ref ResidencyLedger gives about all its rows together.
 *
 * @details
 * - `bytes` is engaged only when EVERY row reports bytes: a total that adds "known" to "unknown" would be a
 *   number nobody can act on, and `unknown` says how many kinds are missing instead of hiding them.
 * - An empty ledger (a backend that retains nothing, or does not account at all) keeps `bytes` disengaged as
 *   well: "nothing is resident" and "nobody counted" stay different answers, which is the same rule the rows
 *   follow one level down.
 */
struct ResidencyTotals
{
    std::uint32_t                live{ 0 };   // Sum of every row's `live`.
    std::optional<std::uint64_t> bytes;       // Sum of every row's `bytes`, engaged only when all report it.
    std::uint32_t                unknown{ 0 }; // How many rows did not report `bytes`.
};

/**
 * @brief What a backend retains, per kind - the accounting half of "GPU residency", without any policy.
 *
 * @details
 * - WHAT IT IS, AND WHAT IT DELIBERATELY IS NOT. A ledger holds one row per @ref ResidencyKind: it is the
 *   VOCABULARY a host, a HUD or a gate reads, and it owns no resources, releases nothing and decides nothing.
 *   The policy that says when a residency may be dropped lives in the backend that creates it (see the ADR);
 *   keeping the two apart is what lets a host show "how much is resident" without learning any backend's rules.
 * - WHY IT LIVES OUTSIDE `backend/`. A host that renders through `vine/graphics/` must be able to read
 *   this answer, and the `backend/` half is only reachable by a backend (`BackendHalfGuard.hpp` makes that a
 *   compile error). Residency is therefore a VALUE in the host half; the tables that feed it stay in the backend.
 * - STEADY STATE IS FREE. A ledger reserves one row per kind when it is built, so `note()` never grows the
 *   storage: a backend may rebuild it every frame (and `VsgBackend` does, when a host asks for a report).
 * - ONE ROW PER KIND. `note()` replaces the row of the same kind, so filling the same kind twice is a mistake
 *   in the caller's arithmetic rather than a second row nobody reads.
 */
class VN_GRAPHICS_API ResidencyLedger {
  public:
    /**
     * @brief Builds an empty ledger with storage for every kind.
     */
    ResidencyLedger() noexcept;

    /**
     * @brief Forgets every row, keeping the storage (the object is reused across reports).
     */
    void reset() noexcept;

    /**
     * @brief Records one kind's row, replacing whatever the same kind said before.
     *
     * @param row The facts about one kind (see @ref ResidencyRow for what may be absent).
     */
    void note(ResidencyRow row) noexcept;

    /**
     * @brief Gets the rows recorded so far, in the order they were first recorded.
     *
     * @return A view of the rows; it stays valid until the next `note()` or `reset()`.
     */
    [[nodiscard]] std::span<const ResidencyRow> rows() const noexcept;

    /**
     * @brief Gets the totals of every recorded row (see @ref ResidencyTotals).
     *
     * @return The aggregate; `bytes` is engaged only when every row reports it.
     */
    [[nodiscard]] ResidencyTotals totals() const noexcept;

    /**
     * @brief Gets the stable name of a kind, for a log line or a HUD label.
     *
     * @param kind The kind to name.
     * @return A null-terminated name that outlives the call ("streams", "images", ...).
     */
    [[nodiscard]] static const char* nameOf(ResidencyKind kind) noexcept;

  private:
    std::vector<ResidencyRow> rows_{};
};

VN_GRAPHICS_NS_END
