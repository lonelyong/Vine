/**
 * @brief The three tables' ROW ORDER: the lookups' fast path, and the proof that it is not a second truth.
 *
 * See `api/ContentFacts` for what the order is and `.ai/design/vsg-reimplementation.md` §11.16bz for why it
 * exists and what it costs.
 *
 * WHY THIS CASE IS A DIFFERENTIAL ONE. The order is DERIVED state: the rows are the truth, and "the order is
 * a permutation of the rows, sorted by the identity the lookup names" is a claim about them. A lookup that
 * reads the order therefore has two ways to be wrong and neither shows up in a picture or a counter:
 *
 *   * an order that is not a permutation - a row no lookup can reach, or one reached twice;
 *   * a run scan that answers with a DIFFERENT row than the scan would - which is exactly what the tables
 *     this backend produces contain: several revisions of one identity, one program's two KINDS and several
 *     VARIANTS at one revision, a material replaced in place, and a malformed row sitting beside a good one.
 *
 * So every case here asks the SAME question twice - once through a table that carries its order, once through
 * the same rows with none - and asserts the two answer identically, misses included. Device-free by
 * construction: the tables hold content, and the lookups are pure.
 */

#include <gtest/gtest.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

#include <vine/vsg/api/ContentFacts.hpp>

using vn::vsg::ChannelFacts;
using vn::vsg::ContentFacts;
using vn::vsg::findGeometry;
using vn::vsg::findMaterial;
using vn::vsg::findProgram;
using vn::vsg::GeometryFacts;
using vn::vsg::MaterialFacts;
using vn::vsg::orderGeometryRows;
using vn::vsg::orderMaterialRows;
using vn::vsg::orderProgramRows;
using vn::vsg::ProgramFacts;
using vn::vsg::ProgramVariant;
using vn::vsg::core::DrawKind;
using vn::vsg::core::ProgramRef;
using vn::vsg::core::StreamKind;

namespace
{

constexpr std::uint32_t kCanonicalPositions = 0x1U;  ///< The one channel a synthetic geometry feeds.

/// @brief The identities the tables are built from: a few objects, so one identity owns SEVERAL rows.
struct Identities
{
    int geometries[3]{};
    int programs[3]{};
    int materials[3]{};
};

/// @brief The rows, and the storage their spans point at (the tables borrow, so the test must own it).
struct Rows
{
    std::vector<ProgramFacts>              programs;
    std::vector<GeometryFacts>             geometries;
    std::vector<MaterialFacts>             materials;
    std::vector<std::vector<ChannelFacts>> channels;  ///< One per geometry row: what `GeometryFacts::channels` spans.
    std::vector<std::vector<std::byte>>    blocks;    ///< One per material row: what `MaterialFacts::block` spans.

    /// @brief Adds one geometry row: @p channel_count streams of @p mask's shape (the malformed row is one
    ///        whose count does not match what its layout declares).
    void addGeometry(const void* identity, std::uint64_t revision, std::uint32_t mask, std::size_t channel_count,
                     bool indexed)
    {
        channels.emplace_back(channel_count);
        for (std::size_t index = 0U; index < channel_count; ++index)
        {
            ChannelFacts& channel   = channels.back()[index];
            channel.key.kind        = StreamKind::Vertex;
            channel.key.location    = static_cast<std::uint32_t>(index);
            channel.key.components  = 3U;
            channel.key.revision    = 1U;
        }
        GeometryFacts row;
        row.geometry              = identity;
        row.revision              = revision;
        row.layout.canonical_mask = mask;
        row.channels              = channels.back();
        if (indexed)
        {
            row.indices.emplace();
            row.indices->key.kind   = StreamKind::Index;
            row.indices->key.revision = 1U;
            row.index_count         = 3U;
        }
        else
        {
            row.vertex_count = 3U;
        }
        geometries.push_back(row);
    }

    /// @brief Adds one program row at @p revision, @p kind and @p variant.
    void addProgram(const void* identity, std::uint64_t revision, DrawKind kind, const ProgramVariant& variant)
    {
        ProgramFacts row;
        row.program  = identity;
        row.revision = revision;
        row.kind     = kind;
        row.variant  = variant;
        programs.push_back(row);
    }

    /// @brief Adds one material row whose block is @p bytes long (the ABI's size is what makes it well-formed).
    void addMaterial(const void* identity, std::uint64_t revision, std::size_t bytes)
    {
        blocks.emplace_back(bytes, std::byte{ 0 });
        MaterialFacts row;
        row.material = identity;
        row.revision = revision;
        row.block    = blocks.back();
        materials.push_back(row);
    }

    /// @brief Gets the tables as the lookups see them, indexed or not.
    [[nodiscard]] ContentFacts view(bool indexed) const
    {
        ContentFacts facts;
        facts.programs   = programs;
        facts.geometries = geometries;
        facts.materials  = materials;
        if (indexed)
        {
            orderProgramRows(facts.programs, program_order);
            orderGeometryRows(facts.geometries, geometry_order);
            orderMaterialRows(facts.materials, material_order);
            facts.program_order  = program_order;
            facts.geometry_order = geometry_order;
            facts.material_order = material_order;
        }
        return facts;
    }

    mutable std::vector<std::uint32_t> program_order;
    mutable std::vector<std::uint32_t> geometry_order;
    mutable std::vector<std::uint32_t> material_order;
};

/// @brief Builds the rows every case searches: duplicates, both kinds, several variants, malformed pairs.
void buildRows(const Identities& ids, Rows& out)
{
    // GEOMETRIES: two revisions per identity. Two rows are deliberately a malformed/good PAIR at one revision
    // and in both orders, because "the first matching row decides" is a rule the scan has and the run must
    // keep: the pair (malformed, good) answers Malformed, the pair (good, malformed) answers the good row.
    for (int index = 0; index < 3; ++index)
    {
        out.addGeometry(&ids.geometries[index], 1U, kCanonicalPositions, 1U, /*indexed*/ true);
        out.addGeometry(&ids.geometries[index], 2U, kCanonicalPositions, 1U, /*indexed*/ true);
    }
    out.addGeometry(&ids.geometries[0], 2U, kCanonicalPositions, 0U, /*indexed*/ true);   // malformed, first
    out.addGeometry(&ids.geometries[1], 2U, kCanonicalPositions, 0U, /*indexed*/ true);   // malformed, second
    out.addGeometry(&ids.geometries[2], 3U, kCanonicalPositions, 1U, /*indexed*/ false);  // a third revision

    // PROGRAMS: every identity at two revisions, in BOTH kinds, in three variants - so the run of one
    // (identity, revision, kind) is several rows long and only the variant tells them apart. Identity 0 also
    // has a third revision, which is the shape "the revision is here but not this text".
    const ProgramVariant plain{};
    const ProgramVariant mapped{ true, false, false };
    const ProgramVariant coloured{ false, true, false };
    for (int index = 0; index < 3; ++index)
    {
        for (std::uint64_t revision = 1U; revision <= 2U; ++revision)
        {
            for (const DrawKind kind : { DrawKind::Content, DrawKind::Screen })
            {
                out.addProgram(&ids.programs[index], revision, kind, plain);
                out.addProgram(&ids.programs[index], revision, kind, mapped);
                out.addProgram(&ids.programs[index], revision, kind, coloured);
            }
        }
    }
    out.addProgram(&ids.programs[0], 3U, DrawKind::Content, plain);

    // MATERIALS: the default (null) entry, the three identities, a REPLACED pair for one identity (two rows,
    // one identity: the first is the answer - see findMaterial) and a malformed block for another.
    out.addMaterial(nullptr, 0U, sizeof(vn::graphics::VineMaterialBlock));
    out.addMaterial(&ids.materials[0], 1U, sizeof(vn::graphics::VineMaterialBlock));
    out.addMaterial(&ids.materials[1], 1U, sizeof(vn::graphics::VineMaterialBlock));
    out.addMaterial(&ids.materials[1], 2U, sizeof(vn::graphics::VineMaterialBlock));
    out.addMaterial(&ids.materials[2], 1U, 8U);  // not the ABI's size: Malformed
}

/// @brief Gets a row's index in its own table, or -1 when the lookup found nothing.
template <typename Row>
std::ptrdiff_t rowIndex(std::span<const Row> rows, const Row* found) noexcept
{
    return found == nullptr ? -1 : found - rows.data();
}

}  // namespace

TEST(ContentFactsIndexTest, AnIndexedTableAnswersExactlyWhatAScanWould)
{
    Identities ids;
    Rows       rows;
    buildRows(ids, rows);

    const ContentFacts scan    = rows.view(/*indexed*/ false);
    const ContentFacts indexed = rows.view(/*indexed*/ true);

    // Every key shape the compiler can produce: the revisions that exist, one that does not, and a null
    // identity (the "content without a material" case, which is matched by the table's default entry).
    const std::uint64_t revisions[]{ 0U, 1U, 2U, 3U, 9U };
    const DrawKind      kinds[]{ DrawKind::Content, DrawKind::Screen };
    const ProgramVariant variants[]{ ProgramVariant{}, ProgramVariant{ true, false, false },
                                     ProgramVariant{ false, true, false },
                                     ProgramVariant{ false, false, true } };

    for (int index = 0; index < 3; ++index)
    {
        for (const std::uint64_t revision : revisions)
        {
            const auto scanned   = findGeometry(scan, &ids.geometries[index], revision);
            const auto searched  = findGeometry(indexed, &ids.geometries[index], revision);
            EXPECT_EQ(static_cast<int>(searched.miss), static_cast<int>(scanned.miss));
            EXPECT_EQ(rowIndex(scan.geometries, scanned.entry), rowIndex(indexed.geometries, searched.entry))
                << "geometry identity " << index << " revision " << revision;

            for (const DrawKind kind : kinds)
            {
                for (const ProgramVariant& variant : variants)
                {
                    const ProgramRef key{ &ids.programs[index], revision };
                    const auto scanned_program  = findProgram(scan, key, variant, kind);
                    const auto searched_program = findProgram(indexed, key, variant, kind);
                    EXPECT_EQ(static_cast<int>(searched_program.miss), static_cast<int>(scanned_program.miss));
                    EXPECT_EQ(rowIndex(scan.programs, scanned_program.entry),
                              rowIndex(indexed.programs, searched_program.entry))
                        << "program identity " << index << " revision " << revision
                        << " kind " << static_cast<int>(kind);
                }
            }
        }

        const auto scanned_material  = findMaterial(scan, &ids.materials[index]);
        const auto searched_material = findMaterial(indexed, &ids.materials[index]);
        EXPECT_EQ(static_cast<int>(searched_material.miss), static_cast<int>(scanned_material.miss));
        EXPECT_EQ(rowIndex(scan.materials, scanned_material.entry),
                  rowIndex(indexed.materials, searched_material.entry))
            << "material identity " << index;
    }

    // The null material identity, and an identity the tables never heard of: the two answers that are not
    // "a row" have to agree as well (the default entry is a row like any other).
    const auto scanned_default  = findMaterial(scan, nullptr);
    const auto searched_default = findMaterial(indexed, nullptr);
    EXPECT_EQ(static_cast<int>(searched_default.miss), static_cast<int>(scanned_default.miss));
    EXPECT_EQ(rowIndex(scan.materials, scanned_default.entry), rowIndex(indexed.materials, searched_default.entry));

    int unknown = 0;
    const auto scanned_unknown  = findProgram(scan, ProgramRef{ &unknown, 1U }, ProgramVariant{}, DrawKind::Content);
    const auto searched_unknown = findProgram(indexed, ProgramRef{ &unknown, 1U }, ProgramVariant{},
                                              DrawKind::Content);
    EXPECT_EQ(static_cast<int>(searched_unknown.miss), static_cast<int>(scanned_unknown.miss));
    EXPECT_EQ(scanned_unknown.entry, nullptr);
}

TEST(ContentFactsIndexTest, TheOrderIsAPermutationOfTheRowsAndItsRunsAreContiguous)
{
    Identities ids;
    Rows       rows;
    buildRows(ids, rows);

    const ContentFacts indexed = rows.view(/*indexed*/ true);
    ASSERT_EQ(indexed.program_order.size(), rows.programs.size());
    ASSERT_EQ(indexed.geometry_order.size(), rows.geometries.size());
    ASSERT_EQ(indexed.material_order.size(), rows.materials.size());

    const auto is_permutation_of = [](std::span<const std::uint32_t> order, std::size_t count) {
        std::vector<std::uint32_t> sorted(order.begin(), order.end());
        std::sort(sorted.begin(), sorted.end());
        for (std::size_t index = 0U; index < count; ++index)
        {
            if (sorted[index] != index)
            {
                return false;  // a row twice, or a row no lookup can reach
            }
        }
        return true;
    };
    EXPECT_TRUE(is_permutation_of(indexed.program_order, rows.programs.size()));
    EXPECT_TRUE(is_permutation_of(indexed.geometry_order, rows.geometries.size()));
    EXPECT_TRUE(is_permutation_of(indexed.material_order, rows.materials.size()));

    // SORTED by the key the lookup names, and the runs therefore contiguous: the assertion is the one a
    // binary search depends on, stated directly instead of inferred from a lookup that happens to hit. The
    // comparison is `std::less`, the only comparison the language gives a total order for and the one the
    // builders sort by.
    const std::less<const void*> before;
    const auto                   program_before = [&before](const ProgramFacts& left, const ProgramFacts& right) {
        if (before(left.program, right.program))
        {
            return true;
        }
        if (before(right.program, left.program))
        {
            return false;
        }
        if (left.revision != right.revision)
        {
            return left.revision < right.revision;
        }
        return static_cast<int>(left.kind) < static_cast<int>(right.kind);
    };
    const auto geometry_before = [&before](const GeometryFacts& left, const GeometryFacts& right) {
        if (before(left.geometry, right.geometry))
        {
            return true;
        }
        if (before(right.geometry, left.geometry))
        {
            return false;
        }
        return left.revision < right.revision;
    };
    const auto material_before = [&before](const MaterialFacts& left, const MaterialFacts& right) {
        return before(left.material, right.material);
    };
    const auto ascending = [](std::span<const std::uint32_t> order, auto&& rows_of, auto&& row_before) {
        for (std::size_t position = 1U; position < order.size(); ++position)
        {
            if (row_before(rows_of[order[position]], rows_of[order[position - 1U]]))
            {
                return false;
            }
        }
        return true;
    };
    EXPECT_TRUE(ascending(indexed.program_order, rows.programs, program_before));
    EXPECT_TRUE(ascending(indexed.geometry_order, rows.geometries, geometry_before));
    EXPECT_TRUE(ascending(indexed.material_order, rows.materials, material_before));
}

