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
 *   HUD, a gate or a host trimming its content can act on.
 * - THE NAMES ARE THE HOST'S CONTENT MODEL, not a backend's object model: `Geometry` / `Textures` / `Materials` /
 *   `Programs` are the four things a host holds and releases (`releaseContent`), so a host reads its own
 *   vocabulary back instead of translating a backend's "streams" and "halves".
 * - EVERY BACKEND NOTES EVERY KIND: the row's EXISTENCE is the promise (this backend accounts for this kind),
 *   while a number inside it may stay absent (`bytes` unset, say). A backend that retains nothing of a kind
 *   reports `live = 0`, which is a measurement rather than a missing answer.
 */
enum class ResidencyKind : std::uint8_t
{
    Geometry,  // Vertex / index / instance streams.
    Textures,  // Uploaded textures.
    Materials, // Per-material uniform blocks (what a material arena keeps resident).
    Programs,  // Compiled pipelines (program + layout + variant + topology + target shape).
    Count,     // Not a kind: the number of kinds, so a table can be sized without a literal.
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
    ResidencyKind                 kind{ ResidencyKind::Geometry }; // Which kind this row describes.
    std::uint32_t                 live{ 0 };                       // Entries resident now.
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
 * - `PROGRAMS` IS WHY THE TOTAL IS STILL SILENT, and it is a property of the APIs rather than a missing tally:
 *   no GPU API reports the memory a compiled pipeline occupies - Vulkan has no query, D3D12's allocation info covers
 *   resources and not PSOs, Metal's `allocatedSize` likewise, and WebGPU exposes neither - so that row reports a
 *   count and leaves `bytes` disengaged everywhere. Every other kind IS measured (vk reports a real 0 for
 *   `Materials`, nothing surviving a frame; vsg sums its resident stream bytes), so `unknown` is 1 on either
 *   backend and a host reads the ROWS it needs rather than this figure.
 * - THE TOTAL IS NOT THE ANSWER TO "HOW MUCH VRAM": that figure belongs to the DEVICE (vk measures it internally,
 *   D3D12 `QueryVideoMemoryInfo`, Metal `currentAllocatedSize`), and reaching a host is a capability plus a report
 *   part (`.ai/backlog/graphics.md` T46). This struct stays what it is - a sum of the rows that could report -
 *   and T45 records the decision about its two fields.
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
 * - WHY IT IS A VALUE AND NOT A TYPE. A host that renders through `vine/graphics/` must be able to read this
 *   answer without naming anything a backend owns, so residency is a VALUE and the tables that feed it stay in
 *   each backend. Which facts may be public at all is `.ai/design/graphics-architecture.md` 2.15, and it is why a
 *   resource type never appears here.
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
     * @return A null-terminated name that outlives the call ("geometry", "textures", ...).
     */
    [[nodiscard]] static const char* nameOf(ResidencyKind kind) noexcept;

  private:
    std::vector<ResidencyRow> rows_{};
};

VN_GRAPHICS_NS_END
