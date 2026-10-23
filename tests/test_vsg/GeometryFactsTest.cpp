/**
 * @brief Building a geometry's entry in the content tables, from the SDK object the host authored
 * (see `.ai/design/vsg-reimplementation.md` §11.17 and `api/ContentFacts.hpp`).
 *
 * Device-free by construction: the walk reads the SDK object and produces values (the only GPU-facing part,
 * the upload, happens later and elsewhere).
 *
 * What these cases pin - each one is a picture or a sharing decision on the other side:
 *
 *   * the CHANNEL ORDER of the entry is the vertex-binding order, so it has to be a rule (canonical roles
 *     first, in ABI order, then the custom locations ascending) rather than "whatever the map iterated";
 *   * the INDEX stream is normalized to its whole buffer, which is what lets two geometries over one index
 *     arena share ONE upload - the alternative (a key per segment) uploads the same bytes once per geometry;
 *   * a geometry WITHOUT indices is described, not refused: it is the SDK's own shape ("vertex streams plus
 *     optional indices", the shape of the demo's point cloud), and the entry says which of the two assembly
 *     modes the draw is - an indexed slice of a stream, or a vertex count over the streams themselves;
 *   * a SLICED channel uploads its own segment, and the indices stay relative to the geometry's own vertices;
 *   * a geometry that cannot be drawn as authored is reported as such instead of becoming a table entry that
 *     silently draws something else.
 */

#include <gtest/gtest.h>

#include <cstddef>
#include <cstdint>
#include <vector>

#include <vine/Buffer.hpp>
#include <vine/graphics/Geometry.hpp>

#include <vine/vsg/internal/ContentFacts.hpp>
#include <vine/vsg/internal/GeometryFacts.hpp>

#include "TestGeometryBuilders.hpp"

using vn::graphics::Geometry;
using vn::test::setPrimitiveIndices;
using vn::vsg::buildGeometryFacts;
using vn::vsg::channelsMatchLayout;
using vn::vsg::ChannelFacts;
using vn::vsg::FactMiss;
using vn::vsg::GeometryFacts;
using vn::graphics::backend::StreamKind;

namespace
{

/// @brief Three vertices of three scalars each, plus the triangle's indices.
struct Content
{
    vn::intrusive_ptr<const vn::Buffer<float>>     positions = vn::intrusive_ptr<const vn::Buffer<float>>(
        new vn::Buffer<float>(std::vector<float>{ 0.0F, 0.0F, 0.0F, 1.0F, 0.0F, 0.0F, 0.0F, 1.0F, 0.0F }));
    vn::intrusive_ptr<const vn::Buffer<float>>     normals = vn::intrusive_ptr<const vn::Buffer<float>>(
        new vn::Buffer<float>(std::vector<float>{ 0.0F, 0.0F, 1.0F, 0.0F, 0.0F, 1.0F, 0.0F, 0.0F, 1.0F }));
    vn::intrusive_ptr<const vn::Buffer<std::uint32_t>> indices =
        vn::intrusive_ptr<const vn::Buffer<std::uint32_t>>(
            new vn::Buffer<std::uint32_t>(std::vector<std::uint32_t>{ 0U, 1U, 2U }));
};

}  // namespace

TEST(GeometryFactsTest, AGeometryBecomesItsLayoutItsChannelsAndItsIndexStream)
{
    Content  content;
    Geometry geometry;
    geometry.setPositions(content.positions);
    geometry.setNormals(content.normals);
    setPrimitiveIndices(geometry, 0U, content.indices);
    geometry.setRevision(4U);

    GeometryFacts                facts;
    std::vector<ChannelFacts>   storage;
    std::vector<vn::vsg::PrimitiveFacts> storage_primitives;
    std::vector<vn::vsg::PrimitiveFacts>  primitives;
    ASSERT_EQ(buildGeometryFacts(geometry, facts, storage, primitives), FactMiss::None);

    EXPECT_EQ(facts.geometry, &geometry);
    EXPECT_EQ(facts.revision, 4U);

    // Position and normal, in ABI order: the layout's mask is the same fact the pipeline key reads.
    EXPECT_EQ(facts.layout.canonical_mask, 0x3U);
    EXPECT_TRUE(facts.layout.custom_locations.empty());
    ASSERT_EQ(facts.channels.size(), 2U);
    EXPECT_EQ(facts.channels[0].key.location, 0U);
    EXPECT_EQ(facts.channels[1].key.location, 1U);
    EXPECT_TRUE(channelsMatchLayout(facts)) << "the entry must be usable as authored";

    // ONE ROW PER DRAW: the geometry has one primitive, and it is the row that says how it assembles.
    ASSERT_EQ(facts.primitives.size(), 1U);
    const vn::vsg::PrimitiveFacts& draw = facts.primitives[0];
    EXPECT_EQ(draw.topology, vn::graphics::Topology::Triangles);
    EXPECT_EQ(draw.index_count, 3U);
    EXPECT_EQ(draw.first_index, 0U);
    EXPECT_EQ(facts.vertex_offset, 0U);
    ASSERT_TRUE(draw.indices.has_value()) << "the primitive draws indexed: it has an index stream";
    EXPECT_EQ(draw.indices->key.kind, StreamKind::Index);
    EXPECT_NE(draw.indices->values, nullptr);
    EXPECT_EQ(draw.indices->key.count, 0U) << "the index stream aliases the whole buffer";
}

TEST(GeometryFactsTest, TheChannelOrderIsCanonicalFirstAndCustomsAscending)
{
    Content  content;
    Geometry geometry;
    geometry.setPositions(content.positions);
    setPrimitiveIndices(geometry, 0U, content.indices);

    // A texcoord (the reserved location 8) and a custom channel at 4: the entry's order is fixed by the ABI,
    // not by the map's iteration order, because the entry's order IS the binding order.
    const vn::graphics::AttributeChannel texcoords = vn::graphics::AttributeChannel::packed(
        std::vector<float>{ 0.0F, 0.0F, 1.0F, 0.0F, 0.0F, 1.0F }, 2U);
    geometry.addBuffer(8U, texcoords);
    const vn::graphics::AttributeChannel custom = vn::graphics::AttributeChannel::packed(
        std::vector<float>{ 1.0F, 1.0F, 1.0F, 1.0F, 1.0F, 1.0F }, 2U);
    geometry.addBuffer(4U, custom);

    GeometryFacts             facts;
    std::vector<ChannelFacts>   storage;
    std::vector<vn::vsg::PrimitiveFacts> storage_primitives;
    ASSERT_EQ(buildGeometryFacts(geometry, facts, storage, storage_primitives), FactMiss::None);

    ASSERT_EQ(facts.channels.size(), 3U);
    EXPECT_EQ(facts.channels[0].key.location, 0U);  // positions
    EXPECT_EQ(facts.channels[1].key.location, 8U);  // the reserved texcoord slot
    EXPECT_EQ(facts.channels[2].key.location, 4U);  // then the customer
    EXPECT_EQ(facts.layout.canonical_mask, 0x1U | 0x8U);
    ASSERT_EQ(facts.layout.custom_locations.size(), 1U);
    EXPECT_EQ(facts.layout.custom_locations[0], 4U);
    EXPECT_TRUE(channelsMatchLayout(facts));
}

TEST(GeometryFactsTest, AGeometryWithoutPositionsCannotBeDescribed)
{
    Content  content;
    Geometry without_positions;
    without_positions.setNormals(content.normals);
    setPrimitiveIndices(without_positions, 0U, content.indices);

    GeometryFacts             facts;
    std::vector<ChannelFacts>   storage;
    std::vector<vn::vsg::PrimitiveFacts> storage_primitives;
    EXPECT_EQ(buildGeometryFacts(without_positions, facts, storage, storage_primitives), FactMiss::Malformed);

    Geometry empty;
    EXPECT_EQ(buildGeometryFacts(empty, facts, storage, storage_primitives), FactMiss::Unknown);

    // ...and an entry that was NOT built leaves no half-filled table behind for a later frame to find.
    EXPECT_TRUE(storage.empty());
}

TEST(GeometryFactsTest, AGeometryWithoutIndicesIsDrawnFromItsVertexStreams)
{
    // The SDK's point cloud shape: positions and nothing else - no index arena exists, so the draw is
    // `vkCmdDraw` over the streams themselves rather than a synthesised identity index array per vertex (which
    // is what the previous implementation built and what made this shape look unsupported).
    Content content;

    Geometry geometry;
    geometry.setPositions(content.positions);
    geometry.setNormals(content.normals);
    geometry.setRevision(2U);

    GeometryFacts             facts;
    std::vector<ChannelFacts>   storage;
    std::vector<vn::vsg::PrimitiveFacts> storage_primitives;
    ASSERT_EQ(buildGeometryFacts(geometry, facts, storage, storage_primitives), FactMiss::None);

    ASSERT_EQ(facts.primitives.size(), 1U);
    EXPECT_FALSE(facts.primitives[0].indices.has_value())
        << "there is no index stream, and that is a fact about the DRAW";
    EXPECT_EQ(facts.primitives[0].index_count, 0U);
    EXPECT_EQ(facts.primitives[0].first_index, 0U);
    // The count the draw carries is the position stream's own vertex count (three scalars per vertex here).
    EXPECT_EQ(facts.vertex_count, 3U);
    EXPECT_TRUE(channelsMatchLayout(facts)) << "an unindexed geometry is as drawable as an indexed one";
}

TEST(GeometryFactsTest, ANonIndexedSliceCountsItsOwnVertices)
{
    // Six vertices in the buffer, the geometry reading the last three: the draw must assemble three vertices,
    // not six - the slice IS the stream (see the file note).
    vn::intrusive_ptr<const vn::Buffer<float>> shared(new vn::Buffer<float>(
        std::vector<float>{ 9.0F, 9.0F, 9.0F, 9.0F, 9.0F, 9.0F, 9.0F, 9.0F, 9.0F,
                            0.0F, 0.0F, 0.0F, 1.0F, 0.0F, 0.0F, 0.0F, 1.0F, 0.0F }));

    Geometry geometry;
    geometry.setPositions(shared, /*first_vertex*/ 3U, /*vertex_count*/ 3U);

    GeometryFacts             facts;
    std::vector<ChannelFacts>   storage;
    std::vector<vn::vsg::PrimitiveFacts> storage_primitives;
    ASSERT_EQ(buildGeometryFacts(geometry, facts, storage, storage_primitives), FactMiss::None);

    ASSERT_EQ(facts.channels.size(), 1U);
    EXPECT_EQ(facts.channels[0].key.offset, 9U) << "the channel reads the slice";
    EXPECT_EQ(facts.vertex_count, 3U) << "and the draw reads the slice's own vertices";
    EXPECT_TRUE(channelsMatchLayout(facts));
}

TEST(GeometryFactsTest, ASlicedChannelUploadsItsOwnSegment)
{
    // A shared vertex buffer of six vertices; the channel reads the LAST three of them.
    vn::intrusive_ptr<const vn::Buffer<float>> shared(new vn::Buffer<float>(
        std::vector<float>{ 9.0F, 9.0F, 9.0F, 9.0F, 9.0F, 9.0F, 9.0F, 9.0F, 9.0F,
                            0.0F, 0.0F, 0.0F, 1.0F, 0.0F, 0.0F, 0.0F, 1.0F, 0.0F }));
    Content content;

    Geometry geometry;
    geometry.setPositions(shared, /*first_vertex*/ 3U, /*vertex_count*/ 3U);
    setPrimitiveIndices(geometry, 0U, content.indices);

    GeometryFacts             facts;
    std::vector<ChannelFacts>   storage;
    std::vector<vn::vsg::PrimitiveFacts> storage_primitives;
    ASSERT_EQ(buildGeometryFacts(geometry, facts, storage, storage_primitives), FactMiss::None);

    ASSERT_EQ(facts.channels.size(), 1U);
    const ChannelFacts& positions = facts.channels[0];

    // The identity says which slice of which buffer (so two geometries over one arena do not share the wrong
    // bytes), and the bytes uploaded are the slice's own.
    EXPECT_EQ(positions.key.buffer, shared.get());
    EXPECT_EQ(positions.key.offset, 9U);
    EXPECT_EQ(positions.key.count, 9U);
    ASSERT_NE(positions.values, nullptr);
    EXPECT_EQ(positions.scalars().size(), 9U);

    // The indices stay relative to the geometry's own vertices.
    EXPECT_EQ(facts.vertex_offset, 0U);
}

TEST(GeometryFactsTest, TwoGeometriesOverOneIndexArenaShareOneStreamIdentity)
{
    Content content;

    Geometry first;
    first.setPositions(content.positions);
    setPrimitiveIndices(first, 0U, content.indices);

    Geometry second;
    second.setPositions(content.positions);
    setPrimitiveIndices(second, 0U, content.indices, /*first_index*/ 0U, /*index_count*/ 3U);

    GeometryFacts             first_facts;
    GeometryFacts             second_facts;
    std::vector<ChannelFacts>   storage_a;
    std::vector<vn::vsg::PrimitiveFacts> storage_a_primitives;
    std::vector<ChannelFacts>   storage_b;
    std::vector<vn::vsg::PrimitiveFacts> storage_b_primitives;
    ASSERT_EQ(buildGeometryFacts(first, first_facts, storage_a, storage_a_primitives), FactMiss::None);
    ASSERT_EQ(buildGeometryFacts(second, second_facts, storage_b, storage_b_primitives), FactMiss::None);

    // The stream identity is the BUFFER, so the upload is shared; the segment travels with the DRAW.
    ASSERT_EQ(first_facts.primitives.size(), 1U);
    ASSERT_EQ(second_facts.primitives.size(), 1U);
    ASSERT_TRUE(first_facts.primitives[0].indices.has_value());
    ASSERT_TRUE(second_facts.primitives[0].indices.has_value());
    EXPECT_EQ(first_facts.primitives[0].indices->key.buffer, second_facts.primitives[0].indices->key.buffer);
    EXPECT_EQ(first_facts.primitives[0].indices->key.count, 0U);
    EXPECT_EQ(second_facts.primitives[0].indices->key.count, 0U);
    EXPECT_EQ(first_facts.primitives[0].first_index, 0U);
    EXPECT_EQ(second_facts.primitives[0].index_count, 3U);
}

/*
 * @brief A geometry with three parts is THREE draws, each with its own topology and index span.
 *
 * This is what primitives exist for: one vertex buffer, one index arena sliced three ways - a triangle
 * mesh, its edge lines and a point set - and the table has to answer per DRAW, because the plan names a
 * draw by its primitive index (CompiledCommand::primitive). The rows must therefore be in the geometry's
 * order, carry that primitive's topology, and state its span; the two primitives over one arena share the
 * stream identity (the buffer), which is what makes one upload serve both.
 */
TEST(GeometryFactsTest, AMultiPrimitiveGeometryDescribesOneRowPerDraw)
{
    Content  content;
    Geometry geometry;
    geometry.setPositions(content.positions);
    geometry.setNormals(content.normals);

    // One index arena sliced three ways: triangles (all three indices), edges (the last two), points
    // (non-indexed).
    const auto arena = content.indices;   // six indices; the third slice reuses it with a short count
    vn::graphics::Primitive triangles;
    triangles.topology = vn::graphics::Topology::Triangles;
    triangles.indices  = vn::graphics::Primitive::IndexSpan::slice(arena, 0U, 3U);
    vn::graphics::Primitive edges;
    edges.topology = vn::graphics::Topology::Lines;
    edges.indices  = vn::graphics::Primitive::IndexSpan::slice(arena, 1U, 2U);
    vn::graphics::Primitive points;
    points.topology = vn::graphics::Topology::Points;   // non-indexed: draws the vertex streams themselves
    geometry.setPrimitive(0U, std::move(triangles));   // the one 'setPositions' created
    geometry.addPrimitive(std::move(edges));
    geometry.addPrimitive(std::move(points));

    GeometryFacts                        facts;
    std::vector<ChannelFacts>            channels;
    std::vector<vn::vsg::PrimitiveFacts> primitives;
    ASSERT_EQ(buildGeometryFacts(geometry, facts, channels, primitives), FactMiss::None);

    ASSERT_EQ(facts.primitives.size(), 3U) << "one row per draw, in the geometry's order";
    EXPECT_EQ(facts.primitives[0].topology, vn::graphics::Topology::Triangles);
    EXPECT_EQ(facts.primitives[1].topology, vn::graphics::Topology::Lines);
    EXPECT_EQ(facts.primitives[2].topology, vn::graphics::Topology::Points);

    ASSERT_TRUE(facts.primitives[0].indices.has_value());
    ASSERT_TRUE(facts.primitives[1].indices.has_value());
    EXPECT_FALSE(facts.primitives[2].indices.has_value()) << "the point set is a non-indexed draw";

    EXPECT_EQ(facts.primitives[0].index_count, 3U);
    EXPECT_EQ(facts.primitives[0].first_index, 0U);
    EXPECT_EQ(facts.primitives[1].index_count, 2U);
    EXPECT_EQ(facts.primitives[1].first_index, 1U) << "the segment travels with the draw";

    // The SAME arena: the stream identity is the buffer, so the upload is shared between the two indexed
    // draws (one upload, two spans), which is the whole reason the key normalizes the slice away.
    EXPECT_EQ(facts.primitives[0].indices->key.buffer, facts.primitives[1].indices->key.buffer);
    EXPECT_EQ(facts.primitives[0].indices->key.count, 0U);
    EXPECT_EQ(facts.primitives[1].indices->key.count, 0U);

    // The non-indexed draw assembles the geometry's own vertex count.
    EXPECT_EQ(facts.vertex_count, 3U);
    EXPECT_TRUE(channelsMatchLayout(facts));
}
