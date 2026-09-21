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

#include <vine/vsg/api/ContentFacts.hpp>
#include <vine/vsg/api/GeometryFacts.hpp>

using vine::graphics::Geometry;
using vine::vsg::buildGeometryFacts;
using vine::vsg::channelsMatchLayout;
using vine::vsg::ChannelFacts;
using vine::vsg::FactMiss;
using vine::vsg::GeometryFacts;
using vine::vsg::core::StreamKind;

namespace
{

/// @brief Three vertices of three scalars each, plus the triangle's indices.
struct Content
{
    vine::intrusive_ptr<const vine::Buffer<float>>     positions = vine::intrusive_ptr<const vine::Buffer<float>>(
        new vine::Buffer<float>(std::vector<float>{ 0.0F, 0.0F, 0.0F, 1.0F, 0.0F, 0.0F, 0.0F, 1.0F, 0.0F }));
    vine::intrusive_ptr<const vine::Buffer<float>>     normals = vine::intrusive_ptr<const vine::Buffer<float>>(
        new vine::Buffer<float>(std::vector<float>{ 0.0F, 0.0F, 1.0F, 0.0F, 0.0F, 1.0F, 0.0F, 0.0F, 1.0F }));
    vine::intrusive_ptr<const vine::Buffer<std::uint32_t>> indices =
        vine::intrusive_ptr<const vine::Buffer<std::uint32_t>>(
            new vine::Buffer<std::uint32_t>(std::vector<std::uint32_t>{ 0U, 1U, 2U }));
};

}  // namespace

TEST(GeometryFactsTest, AGeometryBecomesItsLayoutItsChannelsAndItsIndexStream)
{
    Content  content;
    Geometry geometry;
    geometry.setPositions(content.positions);
    geometry.setNormals(content.normals);
    geometry.setIndices(content.indices);
    geometry.setRevision(4U);

    GeometryFacts              facts;
    std::vector<ChannelFacts>  storage;
    ASSERT_EQ(buildGeometryFacts(geometry, facts, storage), FactMiss::None);

    EXPECT_EQ(facts.geometry, &geometry);
    EXPECT_EQ(facts.revision, 4U);

    // Position and normal, in ABI order: the layout's mask is the same fact the pipeline key reads.
    EXPECT_EQ(facts.layout.canonical_mask, 0x3U);
    EXPECT_TRUE(facts.layout.custom_locations.empty());
    ASSERT_EQ(facts.channels.size(), 2U);
    EXPECT_EQ(facts.channels[0].key.location, 0U);
    EXPECT_EQ(facts.channels[1].key.location, 1U);
    EXPECT_TRUE(channelsMatchLayout(facts)) << "the entry must be usable as authored";

    EXPECT_EQ(facts.index_count, 3U);
    EXPECT_EQ(facts.first_index, 0U);
    EXPECT_EQ(facts.vertex_offset, 0U);
    EXPECT_EQ(facts.indices.key.kind, StreamKind::Index);
    EXPECT_NE(facts.indices.data, nullptr);
    EXPECT_EQ(facts.indices.key.count, 0U) << "the index stream aliases the whole buffer";
}

TEST(GeometryFactsTest, TheChannelOrderIsCanonicalFirstAndCustomsAscending)
{
    Content  content;
    Geometry geometry;
    geometry.setPositions(content.positions);
    geometry.setIndices(content.indices);

    // A texcoord (the reserved location 8) and a custom channel at 4: the entry's order is fixed by the ABI,
    // not by the map's iteration order, because the entry's order IS the binding order.
    const vine::graphics::AttributeChannel texcoords = vine::graphics::AttributeChannel::packed(
        std::vector<float>{ 0.0F, 0.0F, 1.0F, 0.0F, 0.0F, 1.0F }, 2U);
    geometry.addBuffer(8U, texcoords);
    const vine::graphics::AttributeChannel custom = vine::graphics::AttributeChannel::packed(
        std::vector<float>{ 1.0F, 1.0F, 1.0F, 1.0F, 1.0F, 1.0F }, 2U);
    geometry.addBuffer(4U, custom);

    GeometryFacts             facts;
    std::vector<ChannelFacts> storage;
    ASSERT_EQ(buildGeometryFacts(geometry, facts, storage), FactMiss::None);

    ASSERT_EQ(facts.channels.size(), 3U);
    EXPECT_EQ(facts.channels[0].key.location, 0U);  // positions
    EXPECT_EQ(facts.channels[1].key.location, 8U);  // the reserved texcoord slot
    EXPECT_EQ(facts.channels[2].key.location, 4U);  // then the customer
    EXPECT_EQ(facts.layout.canonical_mask, 0x1U | 0x8U);
    ASSERT_EQ(facts.layout.custom_locations.size(), 1U);
    EXPECT_EQ(facts.layout.custom_locations[0], 4U);
    EXPECT_TRUE(channelsMatchLayout(facts));
}

TEST(GeometryFactsTest, AGeometryWithoutPositionsOrWithoutIndicesCannotBeDescribed)
{
    Content  content;
    Geometry without_positions;
    without_positions.setNormals(content.normals);
    without_positions.setIndices(content.indices);

    GeometryFacts             facts;
    std::vector<ChannelFacts> storage;
    EXPECT_EQ(buildGeometryFacts(without_positions, facts, storage), FactMiss::Malformed);

    Geometry without_indices;
    without_indices.setPositions(content.positions);
    EXPECT_EQ(buildGeometryFacts(without_indices, facts, storage), FactMiss::Malformed);

    Geometry empty;
    EXPECT_EQ(buildGeometryFacts(empty, facts, storage), FactMiss::Unknown);

    // ...and an entry that was NOT built leaves no half-filled table behind for a later frame to find.
    EXPECT_TRUE(storage.empty());
}

TEST(GeometryFactsTest, ASlicedChannelUploadsItsOwnSegment)
{
    // A shared vertex buffer of six vertices; the channel reads the LAST three of them.
    vine::intrusive_ptr<const vine::Buffer<float>> shared(new vine::Buffer<float>(
        std::vector<float>{ 9.0F, 9.0F, 9.0F, 9.0F, 9.0F, 9.0F, 9.0F, 9.0F, 9.0F,
                            0.0F, 0.0F, 0.0F, 1.0F, 0.0F, 0.0F, 0.0F, 1.0F, 0.0F }));
    Content content;

    Geometry geometry;
    geometry.setPositions(shared, /*first_vertex*/ 3U, /*vertex_count*/ 3U);
    geometry.setIndices(content.indices);

    GeometryFacts             facts;
    std::vector<ChannelFacts> storage;
    ASSERT_EQ(buildGeometryFacts(geometry, facts, storage), FactMiss::None);

    ASSERT_EQ(facts.channels.size(), 1U);
    const ChannelFacts& positions = facts.channels[0];

    // The identity says which slice of which buffer (so two geometries over one arena do not share the wrong
    // bytes), and the bytes uploaded are the slice's own.
    EXPECT_EQ(positions.key.buffer, shared.get());
    EXPECT_EQ(positions.key.offset, 9U);
    EXPECT_EQ(positions.key.count, 9U);
    ASSERT_NE(positions.data, nullptr);
    EXPECT_EQ(positions.data->valueCount(), 9U);

    // The indices stay relative to the geometry's own vertices.
    EXPECT_EQ(facts.vertex_offset, 0U);
}

TEST(GeometryFactsTest, TwoGeometriesOverOneIndexArenaShareOneStreamIdentity)
{
    Content content;

    Geometry first;
    first.setPositions(content.positions);
    first.setIndices(content.indices);

    Geometry second;
    second.setPositions(content.positions);
    second.setIndices(content.indices, /*first_index*/ 0U, /*index_count*/ 3U);

    GeometryFacts             first_facts;
    GeometryFacts             second_facts;
    std::vector<ChannelFacts> storage_a;
    std::vector<ChannelFacts> storage_b;
    ASSERT_EQ(buildGeometryFacts(first, first_facts, storage_a), FactMiss::None);
    ASSERT_EQ(buildGeometryFacts(second, second_facts, storage_b), FactMiss::None);

    // The stream identity is the BUFFER, so the upload is shared; the segment travels with the draw.
    EXPECT_EQ(first_facts.indices.key.buffer, second_facts.indices.key.buffer);
    EXPECT_EQ(first_facts.indices.key.count, 0U);
    EXPECT_EQ(second_facts.indices.key.count, 0U);
    EXPECT_EQ(first_facts.first_index, 0U);
    EXPECT_EQ(second_facts.index_count, 3U);
}
