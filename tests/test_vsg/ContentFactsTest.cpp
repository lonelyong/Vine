/**
 * @brief The three tables a compiled command cannot carry (see `.ai/design/vsg-reimplementation.md` §11.17).
 *
 * Device-free by construction: the tables hold what the content layer is given, and the lookups are pure.
 *
 * What these cases pin - the plan names a program, a geometry and a material by IDENTITY (and revision), and
 * the answer has to be one of exactly four things. The one that is not allowed is the tempting one: a lookup
 * that finds an entry by identity and quietly records content the plan did not describe (yesterday's vertex
 * bytes, the previous revision of a program) produces a picture with no relationship to the frame, and every
 * counter stays green while it happens.
 */

#include <gtest/gtest.h>

#include <cstddef>
#include <cstdint>
#include <vector>

#include <vine/vsg/api/ContentFacts.hpp>

using vine::vsg::blockFitsAbi;
using vine::vsg::channelsMatchLayout;
using vine::vsg::ChannelFacts;
using vine::vsg::ContentFacts;
using vine::vsg::FactMiss;
using vine::vsg::findGeometry;
using vine::vsg::findMaterial;
using vine::vsg::findProgram;
using vine::vsg::GeometryFacts;
using vine::vsg::MaterialFacts;
using vine::vsg::ProgramFacts;
using vine::vsg::ProgramVariant;
using vine::vsg::core::ProgramRef;
using vine::vsg::core::StreamKind;

namespace
{

/// @brief One vertex channel with no bytes (the lookup never reads them).
ChannelFacts channel(std::uint32_t location)
{
    ChannelFacts facts;
    facts.key.kind       = StreamKind::Vertex;
    facts.key.location   = location;
    facts.key.components = 3U;
    facts.key.revision   = 1U;
    return facts;
}

/// @brief The index stream an INDEXED geometry draws through (see GeometryFacts::indices).
ChannelFacts indexStream()
{
    ChannelFacts facts;
    facts.key.kind       = StreamKind::Index;
    facts.key.location   = 0U;
    facts.key.components = 1U;
    facts.key.revision   = 1U;
    return facts;
}

/// @brief Owns the storage a GeometryFacts points at: the tables BORROW, so a test must keep them alive.
struct GeometryHolder
{
    std::vector<ChannelFacts> channels;
    GeometryFacts             facts;

    GeometryHolder(const void* identity, std::uint64_t revision, std::uint32_t canonical_mask,
                   std::size_t channel_count, std::uint32_t custom_locations = 0U)
        : channels(channel_count, channel(0U))
    {
        facts.geometry              = identity;
        facts.revision              = revision;
        facts.layout.canonical_mask = canonical_mask;
        facts.channels              = channels;
        facts.indices.emplace(indexStream());
        facts.index_count           = 3U;
        for (std::uint32_t extra = 0; extra < custom_locations; ++extra)
        {
            facts.layout.custom_locations.push_back(3U + extra);
        }
    }
};

/// @brief Owns the bytes a MaterialFacts points at.
struct MaterialHolder
{
    std::vector<std::byte> block;
    MaterialFacts          facts;

    MaterialHolder(const void* identity, std::uint64_t revision, std::size_t bytes)
        : block(bytes, std::byte{ 0 })
    {
        facts.material = identity;
        facts.revision = revision;
        facts.block    = block;
    }
};

/// @brief The three tables, built from entries the case owns.
struct Tables
{
    std::vector<ProgramFacts>  programs;
    std::vector<GeometryFacts> geometries;
    std::vector<MaterialFacts> materials;

    ContentFacts view() const
    {
        ContentFacts facts;
        facts.programs   = programs;
        facts.geometries = geometries;
        facts.materials  = materials;
        return facts;
    }
};

}  // namespace

TEST(ContentFactsTest, AProgramIsFoundByItsIdentityAndRevisionAndNothingElse)
{
    static int first     = 0;
    static int second    = 0;
    int        unrelated = 0;

    Tables tables;
    tables.programs.push_back(ProgramFacts{ &first, 7U, {} });
    tables.programs.push_back(ProgramFacts{ &second, 1U, {} });
    const ContentFacts facts = tables.view();

    const auto found = findProgram(facts, ProgramRef{ &first, 7U }, ProgramVariant{});
    ASSERT_TRUE(found.found());
    EXPECT_EQ(found.entry->program, &first);
    EXPECT_EQ(found.miss, FactMiss::None);

    // The same identity at a different revision is NOT the same content: the plan describes a program that has
    // moved on, and recording the newer one would draw a picture the frame never asked for.
    EXPECT_EQ(findProgram(facts, ProgramRef{ &first, 6U }, ProgramVariant{}).miss, FactMiss::Revision);
    EXPECT_FALSE(findProgram(facts, ProgramRef{ &first, 6U }, ProgramVariant{}).found());
    EXPECT_EQ(findProgram(facts, ProgramRef{ &first, 8U }, ProgramVariant{}).miss, FactMiss::Revision);

    EXPECT_EQ(findProgram(facts, ProgramRef{ nullptr, 7U }, ProgramVariant{}).miss, FactMiss::Unknown);
    EXPECT_EQ(findProgram(facts, ProgramRef{ &unrelated, 7U }, ProgramVariant{}).miss, FactMiss::Unknown);
}

TEST(ContentFactsTest, AGeometryIsFoundByItsIdentityAndRevision)
{
    static int   identity  = 0;
    int          unrelated = 0;

    GeometryHolder holder(&identity, 3U, /*canonical_mask*/ 0x3U, /*channels*/ 2U);
    Tables         tables;
    tables.geometries.push_back(holder.facts);

    const ContentFacts facts = tables.view();
    const auto         found = findGeometry(facts, &identity, 3U);
    ASSERT_TRUE(found.found());
    EXPECT_EQ(found.entry->index_count, 3U);
    EXPECT_EQ(found.entry->channels.size(), 2U);
    EXPECT_EQ(found.miss, FactMiss::None);

    EXPECT_EQ(findGeometry(facts, &identity, 2U).miss, FactMiss::Revision);
    EXPECT_EQ(findGeometry(facts, &unrelated, 3U).miss, FactMiss::Unknown);
    EXPECT_EQ(findGeometry(facts, nullptr, 3U).miss, FactMiss::Unknown);
}

TEST(ContentFactsTest, AGeometryWhoseChannelsDoNotMatchItsLayoutCannotBeDrawn)
{
    // The layout declares two canonical channels but only one stream answers for them: a pipeline built from
    // that layout would declare an attribute nothing feeds.
    const GeometryHolder one_short(nullptr, 1U, /*canonical_mask*/ 0x3U, /*channels*/ 1U);
    EXPECT_FALSE(channelsMatchLayout(one_short.facts));

    // The other direction: a stream nothing declares is data nothing consumes.
    const GeometryHolder extra(nullptr, 1U, /*canonical_mask*/ 0x1U, /*channels*/ 2U);
    EXPECT_FALSE(channelsMatchLayout(extra.facts));

    // Custom locations count as declarations: position + one custom channel is a complete pair.
    const GeometryHolder custom(nullptr, 1U, /*canonical_mask*/ 0x1U, /*channels*/ 2U, /*custom*/ 1U);
    EXPECT_TRUE(channelsMatchLayout(custom.facts));

    // A geometry whose "indices" are a vertex stream is not drawable this way: the mode is the stream's KIND,
    // so a stream that is not an index stream states nothing about how the draw is assembled.
    GeometryHolder not_indexed(nullptr, 1U, /*canonical_mask*/ 0x1U, /*channels*/ 1U);
    not_indexed.facts.indices->key.kind = StreamKind::Vertex;
    EXPECT_FALSE(channelsMatchLayout(not_indexed.facts));

    // ...while a geometry that names NO index stream is the unindexed mode and is drawable from its vertices.
    GeometryHolder unindexed(nullptr, 1U, /*canonical_mask*/ 0x1U, /*channels*/ 1U);
    unindexed.facts.indices.reset();
    unindexed.facts.index_count = 0U;
    EXPECT_FALSE(channelsMatchLayout(unindexed.facts)) << "no index stream and no vertex count: nothing to draw";
    unindexed.facts.vertex_count = 3U;
    EXPECT_TRUE(channelsMatchLayout(unindexed.facts));

    // ...and a short geometry is reported as Malformed rather than "found but useless".
    static int       identity = 0;
    GeometryHolder   broken(&identity, 1U, /*canonical_mask*/ 0x3U, /*channels*/ 1U);
    Tables           tables;
    tables.geometries.push_back(broken.facts);
    const ContentFacts facts = tables.view();
    EXPECT_EQ(findGeometry(facts, &identity, 1U).miss, FactMiss::Malformed);
}

TEST(ContentFactsTest, AMaterialWhoseBlockIsNotTheAbisSizeCannotBeDrawn)
{
    static int identity = 0;

    const MaterialHolder right(&identity, 1U, sizeof(vine::graphics::VineMaterialBlock));
    EXPECT_TRUE(blockFitsAbi(right.facts));

    const MaterialHolder short_block(&identity, 1U, sizeof(vine::graphics::VineMaterialBlock) - 1U);
    EXPECT_FALSE(blockFitsAbi(short_block.facts));

    Tables tables;
    tables.materials.push_back(short_block.facts);
    const ContentFacts facts = tables.view();
    EXPECT_EQ(findMaterial(facts, &identity).miss, FactMiss::Malformed);

    // The IDENTITY is the whole key for a material, unlike the other two tables: the plan cannot name a
    // material revision (the SDK type has no revision accessor), so the table's entry IS the current account of
    // its bytes - a "different revision" cannot be asked for and therefore cannot be missed.
    EXPECT_TRUE(findMaterial(facts, &identity).miss == FactMiss::Malformed);
    EXPECT_EQ(findMaterial(facts, nullptr).miss, FactMiss::Unknown);
}

TEST(ContentFactsTest, AnEmptyTableAnswersUnknownRatherThanGuessing)
{
    const ContentFacts empty{};

    int identity = 0;
    EXPECT_EQ(findProgram(empty, ProgramRef{ &identity, 0U }, ProgramVariant{}).miss, FactMiss::Unknown);
    EXPECT_EQ(findGeometry(empty, &identity, 0U).miss, FactMiss::Unknown);
    EXPECT_EQ(findMaterial(empty, &identity).miss, FactMiss::Unknown);
}
