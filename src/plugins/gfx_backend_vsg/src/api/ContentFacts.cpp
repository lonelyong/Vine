#include <vine/vsg/api/ContentFacts.hpp>

#include <algorithm>
#include <compare>
#include <cstddef>
#include <cstdint>
#include <utility>

VN_VSG_NS_BEGIN

namespace
{

/// @brief Counts the bits set in @p mask (the canonical channels a layout declares).
std::uint32_t popcount(std::uint32_t mask) noexcept
{
    std::uint32_t count = 0;
    while (mask != 0U)
    {
        count += mask & 1U;
        mask >>= 1U;
    }
    return count;
}

/// @brief How much of a lookup key takes part in the comparison: a PREFIX of the order a table is sorted by.
///
/// A prefix is what classifies a miss, and that is why a lookup runs three searches and not one: "this
/// identity is here at another revision" and "this identity is here at all" are the two answers the scan
/// tells apart (see findProgram), and each of them is the run of rows that share exactly that prefix.
enum class KeyFields : std::uint8_t
{
    Identity,  ///< The identity alone.
    Revision,  ///< Identity and revision.
    Kind,      ///< Identity, revision and kind (the program table's full key).
};

/// @brief Compares two identities: `std::less` is the only comparison the language gives a TOTAL order for.
[[nodiscard]] std::strong_ordering compareIdentity(const void* left, const void* right) noexcept
{
    const std::less<const void*> before;
    if (before(left, right))
    {
        return std::strong_ordering::less;
    }
    if (before(right, left))
    {
        return std::strong_ordering::greater;
    }
    return std::strong_ordering::equal;
}

/// @brief The key @ref findProgram searches by: the identity, the revision and the drawing CALL.
struct ProgramKey
{
    const void*    identity{nullptr};
    std::uint64_t  revision{0};
    core::DrawKind kind{core::DrawKind::Content};
};

/// @brief Orders one program row against one program key, over the first @p fields of the key.
[[nodiscard]] std::strong_ordering compareProgram(const ProgramFacts& row, const ProgramKey& key,
                                                  KeyFields fields) noexcept
{
    if (const std::strong_ordering identity = compareIdentity(row.program, key.identity);
        identity != std::strong_ordering::equal)
    {
        return identity;
    }
    if (fields == KeyFields::Identity)
    {
        return std::strong_ordering::equal;
    }
    if (row.revision != key.revision)
    {
        return row.revision <=> key.revision;
    }
    if (fields == KeyFields::Revision)
    {
        return std::strong_ordering::equal;
    }
    return row.kind <=> key.kind;
}

/// @brief The key @ref findGeometry searches by: the identity and the revision.
struct GeometryKey
{
    const void*   identity{nullptr};
    std::uint64_t revision{0};
};

/// @brief Orders one geometry row against one geometry key, over the first @p fields of the key.
[[nodiscard]] std::strong_ordering compareGeometry(const GeometryFacts& row, const GeometryKey& key,
                                                   KeyFields fields) noexcept
{
    if (const std::strong_ordering identity = compareIdentity(row.geometry, key.identity);
        identity != std::strong_ordering::equal)
    {
        return identity;
    }
    if (fields == KeyFields::Identity)
    {
        return std::strong_ordering::equal;
    }
    return row.revision <=> key.revision;
}

/// @brief The key @ref findMaterial searches by: the identity (the whole key - see that lookup).
struct MaterialKey
{
    const void* identity{nullptr};
};

/// @brief Orders one material row against one material key (the identity is the whole key).
[[nodiscard]] std::strong_ordering compareMaterial(const MaterialFacts& row, const MaterialKey& key,
                                                   KeyFields) noexcept
{
    return compareIdentity(row.material, key.identity);
}

/// @brief Gets the half-open range of @p order whose rows match @p key over @p fields, by bisection.
///
/// The range is what a lookup then reads: the rows it holds all share the key's prefix, and they are in
/// TABLE order inside it (the orders are built with a STABLE sort), so a search that must take "the first row
/// that matches" still means what it meant when it was a scan.
///
/// @param rows    The table's rows (the order indexes into these).
/// @param order   The table's row order, sorted by @p compare over the FULL key.
/// @param key     The key to match.
/// @param fields  How many of the key's fields take part.
/// @param compare The table's row-against-key relation.
/// @return The first and one-past-last position IN @p order.
template <typename Row, typename Key, typename Compare>
[[nodiscard]] std::pair<std::size_t, std::size_t> runOf(std::span<const Row> rows,
                                                        std::span<const std::uint32_t> order, const Key& key,
                                                        KeyFields fields, Compare compare) noexcept
{
    const auto row_before_key = [&](std::uint32_t row, const Key& wanted) {
        return compare(rows[row], wanted, fields) < 0;
    };
    const auto key_before_row = [&](const Key& wanted, std::uint32_t row) {
        return compare(rows[row], wanted, fields) > 0;
    };
    const auto lower = std::lower_bound(order.begin(), order.end(), key, row_before_key);
    const auto upper = std::upper_bound(lower, order.end(), key, key_before_row);
    return { static_cast<std::size_t>(lower - order.begin()), static_cast<std::size_t>(upper - order.begin()) };
}

/// @brief Gets whether a table's row order may be searched: it must cover EXACTLY the table's rows.
///
/// The requirement is not decoration: a row order that fell behind its table (a row appended or erased
/// without refreshing it) would make a bisection read a row that is no longer there, i.e. an index into a
/// table that does not hold it. So a lookup that finds an order it cannot trust answers by SCANNING instead -
/// the same answer, at the slow price (see api/ContentFacts) - and the invariant itself is asserted where the
/// tables are built (`ContentStoreTest`).
///
/// @param rows  How many rows the table has.
/// @param order The order the caller published.
/// @return true when the order may be bisected.
[[nodiscard]] bool searchable(std::size_t rows, std::span<const std::uint32_t> order) noexcept
{
    return !order.empty() && order.size() == rows;
}

/// @brief Gets whether a run holds anything (see runOf).
[[nodiscard]] bool holdsRows(const std::pair<std::size_t, std::size_t>& run) noexcept
{
    return run.first != run.second;
}

}  // namespace

void orderProgramRows(std::span<const ProgramFacts> rows, std::vector<std::uint32_t>& order)
{
    order.resize(rows.size());
    for (std::size_t row = 0U; row < rows.size(); ++row)
    {
        order[row] = static_cast<std::uint32_t>(row);
    }
    // A STABLE sort, because the rows of one (identity, revision, kind) must stay in table order: the lookup
    // scans that run and takes the first row whose variant it means, which is the row the scan would answer
    // with (see findProgram).
    std::stable_sort(order.begin(), order.end(), [rows](std::uint32_t left, std::uint32_t right) {
        const ProgramFacts& row = rows[right];
        const ProgramKey    key{ row.program, row.revision, row.kind };
        return compareProgram(rows[left], key, KeyFields::Kind) < 0;
    });
}

void orderGeometryRows(std::span<const GeometryFacts> rows, std::vector<std::uint32_t>& order)
{
    order.resize(rows.size());
    for (std::size_t row = 0U; row < rows.size(); ++row)
    {
        order[row] = static_cast<std::uint32_t>(row);
    }
    std::stable_sort(order.begin(), order.end(), [rows](std::uint32_t left, std::uint32_t right) {
        const GeometryFacts& row = rows[right];
        const GeometryKey    key{ row.geometry, row.revision };
        return compareGeometry(rows[left], key, KeyFields::Revision) < 0;
    });
}

void orderMaterialRows(std::span<const MaterialFacts> rows, std::vector<std::uint32_t>& order)
{
    order.resize(rows.size());
    for (std::size_t row = 0U; row < rows.size(); ++row)
    {
        order[row] = static_cast<std::uint32_t>(row);
    }
    std::stable_sort(order.begin(), order.end(), [rows](std::uint32_t left, std::uint32_t right) {
        const MaterialFacts& row = rows[right];
        const MaterialKey    key{ row.material };
        return compareMaterial(rows[left], key, KeyFields::Identity) < 0;
    });
}

bool channelsMatchLayout(const GeometryFacts& facts) noexcept
{
    const std::uint32_t declared = popcount(facts.layout.canonical_mask) +
                                   static_cast<std::uint32_t>(facts.layout.custom_locations.size());
    if (declared != facts.channels.size())
    {
        return false;
    }
    // The stream the draw is assembled from: an indexed geometry must be drawn THROUGH its index stream (its
    // KIND is the mode), an unindexed one needs vertices to assemble (see GeometryFacts) - a geometry that
    // offers neither draws nothing, and "described but not drawable" must stay a miss rather than become an
    // empty picture. Whether the stream's PAYLOAD is there is the upload layer's business, not this check's.
    if (facts.indices.has_value())
    {
        return facts.indices->key.kind == core::StreamKind::Index;
    }
    return facts.vertex_count > 0U;
}

bool blockFitsAbi(const MaterialFacts& facts) noexcept
{
    return facts.block.size() == sizeof(vn::graphics::VineMaterialBlock);
}

FactResult<ProgramFacts> findProgram(const ContentFacts& facts, const core::ProgramRef& program,
                                     const ProgramVariant& variant, core::DrawKind kind) noexcept
{
    if (program.program == nullptr)
    {
        // "No program of its own" is resolved to the frame's default by the compiler, so a null identity
        // reaching this lookup means the caller asked about a command that has no program AT ALL - which is
        // content the host did not shade, not content this table could answer for.
        return { nullptr, FactMiss::Unknown };
    }

    // An INDEXED table answers the same questions as the scan below, each by bisection instead of by reading
    // every row: the run that shares the (identity, revision, kind) the caller names holds the variant's
    // entry, and the two PREFIXES of that key are the runs "this revision" and "this identity" - which is
    // exactly how the scan classifies what it did not find, so the two spellings cannot disagree.
    if (searchable(facts.programs.size(), facts.program_order))
    {
        const ProgramKey key{ program.program, program.revision, kind };
        const auto searched = runOf(facts.programs, facts.program_order, key, KeyFields::Kind, compareProgram);
        for (std::size_t position = searched.first; position < searched.second; ++position)
        {
            const ProgramFacts& row = facts.programs[facts.program_order[position]];
            if (row.variant == variant)
            {
                return { &row, FactMiss::None };
            }
        }
        if (holdsRows(runOf(facts.programs, facts.program_order, key, KeyFields::Revision, compareProgram)))
        {
            // The revision the plan names is here, but not as this drawable's entry (see the scan below).
            return { nullptr, FactMiss::Unknown };
        }
        return { nullptr, holdsRows(runOf(facts.programs, facts.program_order, key, KeyFields::Identity,
                                          compareProgram))
                              ? FactMiss::Revision
                              : FactMiss::Unknown };
    }

    // The whole scan has to run: a table may hold several entries of one identity (its other texts, the two
    // KINDS of call one text can serve, or a revision a replaced entry still answers), so the first entry of
    // that identity is not the answer.
    bool identity_known = false;
    bool revision_known = false;
    for (const ProgramFacts& entry : facts.programs)
    {
        if (entry.program != program.program)
        {
            continue;
        }
        identity_known = true;
        if (entry.revision != program.revision)
        {
            continue;
        }
        revision_known = true;
        if (entry.variant == variant && entry.kind == kind)
        {
            return { &entry, FactMiss::None };
        }
    }
    if (revision_known)
    {
        // The program is here at the revision the plan names, but not AS THIS DRAWABLE'S ENTRY: what is
        // missing is that text of that revision (see api/ProgramVariant - the fixes are "build that variant"),
        // or the entry of the other kind (a full-screen call answered with the content entry would compile the
        // wrong ABI - see ProgramFacts::kind).
        return { nullptr, FactMiss::Unknown };
    }
    return { nullptr, identity_known ? FactMiss::Revision : FactMiss::Unknown };
}

FactResult<GeometryFacts> findGeometry(const ContentFacts& facts, const void* geometry,
                                       std::uint64_t revision) noexcept
{
    if (geometry == nullptr)
    {
        return { nullptr, FactMiss::Unknown };
    }

    // An INDEXED table: the run of (identity, revision) is the whole answer, and its FIRST row is the one a
    // scan would have decided with - the malformed check is about the row that answers, not about the table.
    if (searchable(facts.geometries.size(), facts.geometry_order))
    {
        const GeometryKey key{ geometry, revision };
        const auto        searched =
            runOf(facts.geometries, facts.geometry_order, key, KeyFields::Revision, compareGeometry);
        if (holdsRows(searched))
        {
            const GeometryFacts& row = facts.geometries[facts.geometry_order[searched.first]];
            if (!channelsMatchLayout(row))
            {
                return { nullptr, FactMiss::Malformed };
            }
            return { &row, FactMiss::None };
        }
        return { nullptr,
                 holdsRows(runOf(facts.geometries, facts.geometry_order, key, KeyFields::Identity, compareGeometry))
                     ? FactMiss::Revision
                     : FactMiss::Unknown };
    }

    // The whole scan has to run: a table may hold a replaced revision while the frames that named it are
    // still in flight (see api/ContentStore), so the first entry of that identity is not the answer.
    bool identity_known = false;
    for (const GeometryFacts& entry : facts.geometries)
    {
        if (entry.geometry != geometry)
        {
            continue;
        }
        identity_known = true;
        if (entry.revision != revision)
        {
            continue;
        }
        if (!channelsMatchLayout(entry))
        {
            return { nullptr, FactMiss::Malformed };
        }
        return { &entry, FactMiss::None };
    }
    return { nullptr, identity_known ? FactMiss::Revision : FactMiss::Unknown };
}

FactResult<MaterialFacts> findMaterial(const ContentFacts& facts, const void* material) noexcept
{
    // A null identity is NOT special-cased: the engine's material is optional, and the default material is an
    // entry like any other (see api/ContentSources.hpp). A table that carries one answers for content without
    // a material; a table that does not says Unknown, and the content layer decides what that means.
    //
    // An INDEXED table answers with the FIRST row of the identity's run, which is the row the scan returns:
    // the order is a stable sort, so the run is in table order (see orderMaterialRows).
    if (searchable(facts.materials.size(), facts.material_order))
    {
        const MaterialKey key{ material };
        const auto        searched =
            runOf(facts.materials, facts.material_order, key, KeyFields::Identity, compareMaterial);
        if (!holdsRows(searched))
        {
            return { nullptr, FactMiss::Unknown };
        }
        const MaterialFacts& row = facts.materials[facts.material_order[searched.first]];
        if (!blockFitsAbi(row))
        {
            return { nullptr, FactMiss::Malformed };
        }
        return { &row, FactMiss::None };
    }

    for (const MaterialFacts& entry : facts.materials)
    {
        if (entry.material != material)
        {
            continue;
        }
        if (!blockFitsAbi(entry))
        {
            return { nullptr, FactMiss::Malformed };
        }
        return { &entry, FactMiss::None };
    }
    return { nullptr, FactMiss::Unknown };
}

VN_VSG_NS_END
