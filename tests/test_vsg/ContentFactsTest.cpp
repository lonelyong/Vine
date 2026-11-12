/**
 * @brief The three tables a compiled command cannot carry (see `.ai/design/vsg-backend.md` §11.17).
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

#include <vine/vsg/internal/ContentFacts.hpp>

#include "TestProgramFacts.hpp"

using vn::graphics::backend::DrawKind;
using vn::graphics::backend::StreamKind;
using vn::vsg::blockFitsAbi;
using vn::vsg::ChannelFacts;
using vn::vsg::channelsMatchLayout;
using vn::vsg::ContentFacts;
using vn::vsg::FactMiss;
using vn::vsg::findGeometry;
using vn::vsg::findMaterial;
using vn::vsg::findProgram;
using vn::vsg::GeometryFacts;
using vn::vsg::IndexFacts;
using vn::vsg::MaterialFacts;
using vn::vsg::ProgramFacts;
using vn::vsg::ProgramVariant;

namespace
{

/// @brief A node id a test can name without building an object (the identity is the whole key).
vn::graphics::NodeId nodeId(std::uint32_t index)
{
    return vn::graphics::NodeId(vn::graphics::Handle::make(index, 1U, 0U));
}

/// @brief A material reference a test can name without building an object.
vn::graphics::ContentRef materialRef(std::uint32_t index, std::uint64_t revision)
{
    return vn::graphics::ContentRef{ vn::graphics::Handle::make(index, 1U, 0U), revision };
}

/// @brief The geometry reference a plan names: the identity and the revision, as one key.
vn::graphics::ContentRef geometryRef(vn::graphics::NodeId identity, std::uint64_t revision)
{
    return vn::graphics::contentRefOf(identity, revision);
}

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
IndexFacts indexStream()
{
    IndexFacts facts;
    facts.key.kind       = StreamKind::Index;
    facts.key.location   = 0U;
    facts.key.components = 1U;
    facts.key.revision   = 1U;
    return facts;
}

/// @brief Owns the storage a GeometryFacts points at: the tables BORROW, so a test must keep them alive.
struct GeometryHolder {
    std::vector<ChannelFacts>            channels;
    std::vector<vn::vsg::PrimitiveFacts> primitive_rows; // What `facts.primitives` points at.
    GeometryFacts                        facts;

    GeometryHolder(vn::graphics::NodeId identity, std::uint64_t revision, std::uint32_t canonical_mask, std::size_t channel_count, std::uint32_t custom_locations = 0U)
      : channels(channel_count, channel(0U))
    {
        facts.geometry              = geometryRef(identity, revision);
        facts.layout.canonical_mask = canonical_mask;
        facts.channels              = channels;
        primitive_rows.emplace_back();
        primitive_rows.back().indices.emplace(indexStream());
        primitive_rows.back().index_count = 3U;
        facts.primitives                  = primitive_rows;
        for (std::uint32_t extra = 0; extra < custom_locations; ++extra) {
            facts.layout.custom_locations.push_back(3U + extra);
        }
    }
};

/// @brief Owns the bytes a MaterialFacts points at.
struct MaterialHolder {
    std::vector<std::byte> block;
    MaterialFacts          facts;

    MaterialHolder(vn::graphics::ContentRef identity, std::size_t bytes)
      : block(bytes, std::byte{ 0 })
    {
        facts.material = identity;
        facts.block    = block;
    }
};

/// @brief The three tables, built from entries the case owns.
struct Tables {
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

} // namespace

TEST(ContentFactsTest, AProgramIsFoundByItsIdentityAndRevisionAndNothingElse)
{
    Tables tables;
    tables.programs.push_back(ProgramFacts{ vn::test::programRef(1, 7U), {} });
    tables.programs.push_back(ProgramFacts{ vn::test::programRef(2, 1U), {} });
    const ContentFacts facts = tables.view();

    const auto found = findProgram(facts, vn::test::programRef(1, 7U), ProgramVariant{}, DrawKind::Content);
    ASSERT_TRUE(found.found());
    EXPECT_EQ(found.entry->program, vn::test::programRef(1, 7U));
    EXPECT_EQ(found.miss, FactMiss::None);

    // The same identity at a different revision is NOT the same content: the plan describes a program that has
    // moved on, and recording the newer one would draw a picture the frame never asked for.
    EXPECT_EQ(findProgram(facts, vn::test::programRef(1, 6U), ProgramVariant{}, DrawKind::Content).miss, FactMiss::Revision);
    EXPECT_FALSE(findProgram(facts, vn::test::programRef(1, 6U), ProgramVariant{}, DrawKind::Content).found());
    EXPECT_EQ(findProgram(facts, vn::test::programRef(1, 8U), ProgramVariant{}, DrawKind::Content).miss, FactMiss::Revision);

    EXPECT_EQ(findProgram(facts, vn::graphics::ContentRef{}, ProgramVariant{}, DrawKind::Content).miss, FactMiss::Unknown);
    EXPECT_EQ(findProgram(facts, vn::test::programRef(3, 7U), ProgramVariant{}, DrawKind::Content).miss, FactMiss::Unknown);
}

TEST(ContentFactsTest, AGeometryIsFoundByItsIdentityAndRevision)
{
    const auto identity  = nodeId(1U);
    const auto unrelated = nodeId(2U);

    GeometryHolder holder(identity, 3U, /*canonical_mask*/ 0x3U, /*channels*/ 2U);
    Tables         tables;
    tables.geometries.push_back(holder.facts);

    const ContentFacts facts = tables.view();
    const auto         found = findGeometry(facts, geometryRef(identity, 3U));
    ASSERT_TRUE(found.found());
    ASSERT_EQ(found.entry->primitives.size(), 1U);
    EXPECT_EQ(found.entry->primitives[0].index_count, 3U);
    EXPECT_EQ(found.entry->channels.size(), 2U);
    EXPECT_EQ(found.miss, FactMiss::None);

    EXPECT_EQ(findGeometry(facts, geometryRef(identity, 2U)).miss, FactMiss::Revision);
    EXPECT_EQ(findGeometry(facts, geometryRef(unrelated, 3U)).miss, FactMiss::Unknown);
    EXPECT_EQ(findGeometry(facts, geometryRef(vn::graphics::NodeId{}, 3U)).miss, FactMiss::Unknown);
}

TEST(ContentFactsTest, AGeometryWhoseChannelsDoNotMatchItsLayoutCannotBeDrawn)
{
    // The layout declares two canonical channels but only one stream answers for them: a pipeline built from
    // that layout would declare an attribute nothing feeds.
    const GeometryHolder one_short(nodeId(1U), 1U, /*canonical_mask*/ 0x3U, /*channels*/ 1U);
    EXPECT_FALSE(channelsMatchLayout(one_short.facts));

    // The other direction: a stream nothing declares is data nothing consumes.
    const GeometryHolder extra(nodeId(1U), 1U, /*canonical_mask*/ 0x1U, /*channels*/ 2U);
    EXPECT_FALSE(channelsMatchLayout(extra.facts));

    // Custom locations count as declarations: position + one custom channel is a complete pair.
    const GeometryHolder custom(nodeId(1U), 1U, /*canonical_mask*/ 0x1U, /*channels*/ 2U, /*custom*/ 1U);
    EXPECT_TRUE(channelsMatchLayout(custom.facts));

    // A geometry whose "indices" are a vertex stream is not drawable this way: the mode is the stream's KIND,
    // so a stream that is not an index stream states nothing about how the draw is assembled.
    GeometryHolder not_indexed(nodeId(1U), 1U, /*canonical_mask*/ 0x1U, /*channels*/ 1U);
    not_indexed.primitive_rows[0].indices->key.kind = StreamKind::Vertex;
    EXPECT_FALSE(channelsMatchLayout(not_indexed.facts));

    // ...while a geometry that names NO index stream is the unindexed mode and is drawable from its vertices.
    GeometryHolder unindexed(nodeId(1U), 1U, /*canonical_mask*/ 0x1U, /*channels*/ 1U);
    unindexed.facts.primitives = {};
    EXPECT_FALSE(channelsMatchLayout(unindexed.facts)) << "no index stream and no vertex count: nothing to draw";
    // A geometry whose primitives are all non-indexed IS drawable from its vertices.
    unindexed.primitive_rows.assign(1U, vn::vsg::PrimitiveFacts{});
    unindexed.facts.primitives   = unindexed.primitive_rows;
    unindexed.facts.vertex_count = 3U;
    EXPECT_TRUE(channelsMatchLayout(unindexed.facts));

    // ...and a short geometry is reported as Malformed rather than "found but useless".
    const auto      identity = nodeId(1U);
    GeometryHolder broken(identity, 1U, /*canonical_mask*/ 0x3U, /*channels*/ 1U);
    Tables         tables;
    tables.geometries.push_back(broken.facts);
    const ContentFacts facts = tables.view();
    EXPECT_EQ(findGeometry(facts, geometryRef(identity, 1U)).miss, FactMiss::Malformed);
}

TEST(ContentFactsTest, AMaterialWhoseBlockIsNotTheAbisSizeCannotBeDrawn)
{
    const auto identity = materialRef(1U, 1U);

    const MaterialHolder right(identity, sizeof(vn::graphics::VineMaterialBlock));
    EXPECT_TRUE(blockFitsAbi(right.facts));

    const MaterialHolder short_block(identity, sizeof(vn::graphics::VineMaterialBlock) - 1U);
    EXPECT_FALSE(blockFitsAbi(short_block.facts));

    Tables tables;
    tables.materials.push_back(short_block.facts);
    const ContentFacts facts = tables.view();
    EXPECT_EQ(findMaterial(facts, identity).miss, FactMiss::Malformed);

    // The REVISION IS PART OF THE KEY, exactly like the geometry table: a plan that names revision R must be served
    // R's bytes, so another revision is a miss (the frame in flight kept what it started with).
    EXPECT_TRUE(findMaterial(facts, identity).miss == FactMiss::Malformed);
    EXPECT_EQ(findMaterial(facts, materialRef(1U, 0U)).miss, FactMiss::Unknown);
    EXPECT_EQ(findMaterial(facts, vn::graphics::ContentRef{}).miss, FactMiss::Unknown);
}

TEST(ContentFactsTest, AnEmptyTableAnswersUnknownRatherThanGuessing)
{
    const ContentFacts empty{};

    EXPECT_EQ(findProgram(empty, vn::test::programRef(1, 0U), ProgramVariant{}, DrawKind::Content).miss, FactMiss::Unknown);
    EXPECT_EQ(findGeometry(empty, geometryRef(nodeId(1U), 0U)).miss, FactMiss::Unknown);
    EXPECT_EQ(findMaterial(empty, materialRef(1U, 0U)).miss, FactMiss::Unknown);
}
