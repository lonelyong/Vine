/**
 * @brief Channels that read a SEGMENT of one buffer: "one arena, one segment per geometry" (P7).
 *
 * `AttributeChannel::offset` / `scalarCount` are what make an arena expressible — several geometries share one
 * buffer and each reads its own scalars — and the geometry's INDEX stream takes a span the same way
 * (`Geometry::setIndices(buffer, first_index, index_count)`).
 *
 * What these tests pin is that the slice is the channel, all the way through the SDK:
 *
 *  * its own length is what `floatCount()` / `vertexCount()` / `scalars()` report, never the buffer's;
 *  * it keeps following a buffer that grows (a slice with no fixed count is the whole-buffer case);
 *  * an offset past the end is empty rather than out of range, so a malformed arena cannot read a peer;
 *  * everything derived from positions — the bounding box, a ray hit, the vertex count — covers the SEGMENT,
 *    which is what makes frustum culling and picking correct for an arena.
 */

#include <gtest/gtest.h>

#include <vine/Buffer.hpp>
#include <vine/graphics/Geometry.hpp>
#include <vine/graphics/Ray.hpp>
#include <vine/graphics/RayIntersection.hpp>
#include <vine/math/Vector3.hpp>
#include <vine/math/Point3.hpp>
#include <vine/math/Rect3.hpp>

#include <cstddef>
#include <cstdint>
#include <utility>
#include <vector>

using namespace vine::graphics;
using vine::intrusive_ptr;
using vine::math::Mat4d;
using vine::math::Vec3d;

namespace
{

/**
 * @brief An arena buffer holding @p segments vertices each, laid out ten units apart on y.
 *
 * Each segment is a right triangle whose base sits at `segment_index * 10`, so a test can tell WHICH segment
 * a channel or a bound reads from the numbers alone.
 *
 * @param segments Segments to lay out.
 * @return Buffer holding every segment's three vertices.
 */
intrusive_ptr<vine::Buffer<float>> arenaBuffer(std::size_t segments)
{
    std::vector<float> floats;
    for (std::size_t i = 0; i < segments; ++i) {
        const float y = static_cast<float>(i) * 10.0f;
        floats.insert(floats.end(), { 0.0f, y, 0.0f, 1.0f, y, 0.0f, 0.0f, y + 1.0f, 0.0f });
    }
    return intrusive_ptr<vine::Buffer<float>>(new vine::Buffer<float>(std::move(floats)));
}

/**
 * @brief A geometry drawing one segment of @p arena as its positions.
 *
 * A slice is a channel like any other, so it is attached through addBuffer(0, ...): the convenience setter
 * states a whole buffer, which is exactly the case a slice is not.
 *
 * @param arena        Arena to read.
 * @param first_vertex First vertex of the segment.
 * @return Geometry reading that segment.
 */
intrusive_ptr<Geometry> segmentGeometry(const intrusive_ptr<const vine::Buffer<float>>& arena,
                                       std::size_t                                     first_vertex)
{
    auto geom = intrusive_ptr<Geometry>(new Geometry());
    geom->addBuffer(0u, AttributeChannel::slice(arena, 3u, first_vertex, 3u));
    return geom;
}

}  // namespace

TEST(AttributeChannelTest, AChannelReadsOnlyItsOwnSlice)
{
    // The second segment of a two-segment arena: vertex 3, which is scalar 9 of a three-component channel.
    const auto buffer = arenaBuffer(2u);
    const auto sliced = AttributeChannel::slice(buffer, 3u, 3u, 2u);

    EXPECT_EQ(sliced.offset, 9u) << "vertex 3 of a three-component channel is scalar 9";
    EXPECT_EQ(sliced.floatCount(), 6u) << "the slice's scalars, not the buffer's eighteen";
    EXPECT_EQ(sliced.vertexCount(), 2u);
    EXPECT_FALSE(sliced.empty());
    ASSERT_EQ(sliced.scalars().size(), 6u);
    EXPECT_FLOAT_EQ(sliced.scalars()[1], 10.0f) << "the slice starts at the second segment";

    // xyz() is relative to the CHANNEL: vertex 0 of the slice is the segment's first vertex.
    EXPECT_FLOAT_EQ(sliced.xyz(0u)[1], 10.0f);

    // And the Vec3 view is the slice, so a consumer of the typed face sees the same vertices.
    ASSERT_EQ(sliced.vec3View().size(), 2u);
    EXPECT_FLOAT_EQ(sliced.vec3View()[0].y, 10.0f);
}

TEST(AttributeChannelTest, ASliceMayHaveNoFixedLengthAndNeverRunsPastTheBuffer)
{
    const auto buffer = arenaBuffer(2u);

    // The sentinel every channel used before slices existed: "whatever the buffer holds from offset on" —
    // which is what keeps a channel following a buffer that grows under it.
    EXPECT_EQ(AttributeChannel::shared(buffer, 3u).floatCount(), buffer->size());
    const auto from_second = AttributeChannel::shared(buffer, 3u, 9u);
    EXPECT_EQ(from_second.vertexCount(), 3u);
    EXPECT_EQ(from_second.floatCount(), buffer->size() - 9u);

    buffer->push_back(20.0f);
    buffer->push_back(20.0f);
    buffer->push_back(0.0f);
    buffer->setRevision(buffer->revision() + 1u);
    EXPECT_EQ(from_second.floatCount(), buffer->size() - 9u) << "no fixed length: it follows the buffer";

    // A fixed length truncates; an offset past the end is empty rather than out of range.
    EXPECT_EQ(AttributeChannel::shared(buffer, 3u, 9u, 3u).vertexCount(), 1u);
    EXPECT_TRUE(AttributeChannel::shared(buffer, 3u, buffer->size() + 100u).empty());
    EXPECT_GT(AttributeChannel::shared(buffer, 3u, 9u, 0u).floatCount(), 0u) << "0 means the rest, not empty";
}

TEST(AttributeChannelTest, SliceStatesTheSegmentInVertices)
{
    // What a caller has in hand is a vertex, and the multiplication by the stride is the part that is easy to
    // get wrong, so the factory does it.
    const auto buffer = arenaBuffer(2u);
    const auto sliced = AttributeChannel::slice(buffer, 3u, 3u, 0u);

    EXPECT_EQ(sliced.offset, 9u) << "vertex 3 of a three-component channel is scalar 9";
    EXPECT_EQ(sliced.vertexCount(), 3u) << "0 vertices means the rest of the buffer";
    EXPECT_FLOAT_EQ(sliced.xyz(0u)[1], 10.0f);

    // A fixed count truncates, in the same unit.
    EXPECT_EQ(AttributeChannel::slice(buffer, 3u, 3u, 1u).vertexCount(), 1u);
}

TEST(AttributeChannelTest, TheWholeBufferAndTheSegmentSpellingsAgree)
{
    // Every canonical role states its data two ways: a whole buffer, or a segment of an arena. They are
    // ONE door underneath (both end at addBuffer() + AttributeChannel::slice()), which this pins — a
    // second implementation would let the two spellings drift apart silently, and the pixels would only
    // show it as a geometry that draws the wrong vertices.
    const auto arena  = arenaBuffer(2u);
    const auto loc    = vine::graphics::attributeLocation(VertexAttribute::Position);
    const auto uv_loc = Geometry::kTexCoordLocation;

    auto whole = intrusive_ptr<Geometry>(new Geometry());
    whole->setPositions(arena);
    auto segment = intrusive_ptr<Geometry>(new Geometry());
    segment->setPositions(arena, 0u, 6u); // the same data, stated as a segment of six vertices

    const AttributeChannel* a = whole->buffer(loc);
    const AttributeChannel* b = segment->buffer(loc);
    ASSERT_NE(a, nullptr);
    ASSERT_NE(b, nullptr);
    EXPECT_EQ(a->values, b->values) << "the same buffer, read in place rather than copied";
    EXPECT_EQ(a->offset, b->offset);
    EXPECT_EQ(a->components, b->components);
    EXPECT_EQ(a->floatCount(), b->floatCount()) << "the same coverage today";
    // ... stated differently, and the difference is the point: "the whole buffer" is 0 = FOLLOW it, a
    // segment is a fixed count. A buffer that grows is covered by the first and not by the second.
    EXPECT_EQ(a->scalarCount, 0u) << "the whole-buffer spelling follows the buffer as it grows";
    EXPECT_EQ(b->scalarCount, 18u) << "two segments of three vertices, stated in vertices";

    // The same coverage, on both faces: a buffer that grows is still the whole channel, while the
    // segment keeps the six vertices it stated.
    arena->append(std::vector<float>{ 9.0f, 9.0f, 9.0f });
    EXPECT_EQ(a->floatCount(), 21u) << "the whole-buffer channel followed the growth";
    EXPECT_EQ(b->floatCount(), 18u) << "the segment stayed the segment";

    // And it is the SAME channel the general door builds, byte for byte.
    auto via_general = intrusive_ptr<Geometry>(new Geometry());
    via_general->addBuffer(loc, AttributeChannel::slice(arena, 3u, 0u, 6u));
    EXPECT_EQ(via_general->buffer(loc)->offset, b->offset);
    EXPECT_EQ(via_general->buffer(loc)->scalarCount, b->scalarCount);

    // The unit is VERTICES, not scalars: three vertices of a three-component channel are nine scalars, and
    // the segment is what the geometry reports (count, bounds) — the arena tests cover the rest.
    auto part = intrusive_ptr<Geometry>(new Geometry());
    part->setPositions(arena, 3u, 3u);
    EXPECT_EQ(part->positionCount(), 3u);
    EXPECT_EQ(part->buffer(loc)->scalarCount, 9u);
    EXPECT_TRUE(part->boundingBox().isValid());

    // A count of 0 means "the rest of the buffer", NOT an empty channel: that is the growing-arena case,
    // and it is the same rule the index stream and BufferSlice follow.
    auto rest = intrusive_ptr<Geometry>(new Geometry());
    rest->setPositions(arena, 3u, 0u);
    EXPECT_EQ(rest->positionCount(), 4u)
        << "from vertex 3 to the end of the buffer as it is now (seven vertices after the growth)";

    // Each role keeps ITS OWN stride: a texcoord segment of three vertices is six scalars, not nine. A
    // copy-pasted overload that reused the position stride would pass the test above and fail here.
    auto uvs = intrusive_ptr<Geometry>(new Geometry());
    uvs->setTexcoords(arena, 3u, 3u);
    ASSERT_NE(uvs->buffer(uv_loc), nullptr);
    EXPECT_EQ(uvs->buffer(uv_loc)->components, 2u);
    EXPECT_EQ(uvs->buffer(uv_loc)->scalarCount, 6u);
    EXPECT_EQ(uvs->texcoordCount(), 3u);

    // Normals take a segment too (a geometry that authors them from an arena), with the same stride rule.
    auto normals = intrusive_ptr<Geometry>(new Geometry());
    normals->setNormals(arena, 0u, 3u);
    ASSERT_NE(normals->buffer(vine::graphics::attributeLocation(VertexAttribute::Normal)), nullptr);
    EXPECT_EQ(normals->normalCount(), 3u);
}

TEST(AttributeChannelTest, AChannelIsASegmentPlusAStride)
{
    // The storage description is BufferSlice's, not the channel's own: a channel hands its segment out
    // (scalarSlice()) and can be built from one (fromSlice()). That is the base concept every stream
    // shares — an attribute channel, the index stream, whatever comes next — and the relation is
    // COMPOSITION on purpose: a channel is a segment INTERPRETED with a stride, so one passed where a
    // segment is expected (by value) would silently lose where each vertex begins.
    const auto buffer  = arenaBuffer(2u);
    const auto channel = AttributeChannel::slice(buffer, 3u, 3u, 2u);

    const vine::BufferSlice<float> segment = channel.scalarSlice();
    EXPECT_EQ(segment.values, buffer);
    EXPECT_EQ(segment.first, channel.offset);
    EXPECT_EQ(segment.count, channel.scalarCount);
    EXPECT_EQ(segment.size(), channel.floatCount()) << "one length, whichever face you read it from";

    // ... and back: the same channel, rebuilt from its own segment plus the stride.
    const AttributeChannel rebuilt = AttributeChannel::fromSlice(segment, 3u);
    EXPECT_EQ(rebuilt.vertexCount(), channel.vertexCount());
    ASSERT_EQ(rebuilt.scalars().size(), channel.scalars().size());
    EXPECT_FLOAT_EQ(rebuilt.scalars()[1], channel.scalars()[1]);

    // The stride is what the segment cannot carry, and it is not optional: without it the same bytes are
    // not vertices. A segment built from a channel and no stride describes an empty channel, not a
    // silently different one.
    EXPECT_EQ(AttributeChannel::fromSlice(segment, 0u).vertexCount(), 0u);
}

TEST(GeometryTest, TheIndexStreamIsTheSameKindOfSegment)
{
    // The index run and an attribute channel are the same KIND of thing (BufferSlice): "the rest of the
    // buffer" and "an offset past the end" mean the same for both, resolved by the same function, so a
    // consumer that walks a geometry's streams has no per-stream arithmetic to get wrong. IndexSliceExposes
    // ItsSpan pins the numbers; this pins that the numbers come from the shared rule and the shared type.
    const auto indices = intrusive_ptr<const vine::Buffer<std::uint32_t>>(
        new vine::Buffer<std::uint32_t>(std::vector<std::uint32_t>{ 0u, 1u, 2u, 0u, 1u, 2u }));
    auto geometry = intrusive_ptr<Geometry>(new Geometry());

    geometry->setIndices(indices, 4u, 0u); // "the rest of the arena"
    EXPECT_EQ(geometry->indexCount(), 2u);
    EXPECT_EQ(geometry->firstIndex(), 4u);
    geometry->setIndices(indices, 6u, 0u); // past the end clamps to empty
    EXPECT_FALSE(geometry->hasIndices());
    EXPECT_EQ(geometry->firstIndex(), 6u) << "the clamped start, not the unclamped one";

    // The same inputs, the same rule: an element slice and a scalar channel resolve their length with one
    // function, so "the rest" cannot come out as two different numbers for two streams of one geometry.
    const auto arena = arenaBuffer(2u); // two segments of three vertices = eighteen scalars
    EXPECT_EQ(Geometry::IndexStream::resolvedLength(indices->size(), 4u, 0u), 2u) << "six indices from 4";
    EXPECT_EQ(vine::BufferSlice<float>::resolvedLength(arena->size(), 8u, 0u), 10u) << "eighteen from 8";
    EXPECT_EQ(AttributeChannel::shared(arena, 3u, 8u, 0u).floatCount(), 10u)
        << "a channel with the same offset and no count reads the same rest of its buffer";
    EXPECT_EQ(AttributeChannel::shared(arena, 3u, 18u, 0u).floatCount(), 0u) << "starting past the end is empty";
}

TEST(GeometryTest, BoundingBoxCoversOnlyTheGeometrysSegment)
{
    const auto arena = arenaBuffer(3u);
    const auto geom  = segmentGeometry(arena, 6u);  // the third segment: y in [20, 21]

    const Aabbd box = geom->boundingBox();
    ASSERT_TRUE(box.isValid());
    EXPECT_NEAR(box.min().y, 20.0, 1e-9) << "the arena's other segments must not widen the bound";
    EXPECT_NEAR(box.max().y, 21.0, 1e-9);
    EXPECT_EQ(geom->positionCount(), 3u);
}

TEST(GeometryTest, TheCubeDirectionsSpellTheTexcoordSlotWithAThirdComponent)
{
    // A cube map is sampled by DIRECTION: the texcoord slot carries three scalars per vertex instead of the
    // two a 2-D map uses. Same location, same binding, ONE shader set — the shape is what the renderer reads
    // to select the samplerCube variant, so it has to be the shape the setter states.
    const auto directions = intrusive_ptr<vine::Buffer<float>>(
        new vine::Buffer<float>(std::vector<float>{ 1, 0, 0, -1, 0, 0, 0, 1, 0, 0, -1, 0, 0, 0, 1, 0, 0, -1 }));

    auto whole = intrusive_ptr<Geometry>(new Geometry());
    whole->setCubeDirections(directions);
    auto segment = intrusive_ptr<Geometry>(new Geometry());
    segment->setCubeDirections(directions, 0u, 6u);

    ASSERT_NE(whole->buffer(Geometry::kTexCoordLocation), nullptr);
    EXPECT_EQ(whole->buffer(Geometry::kTexCoordLocation)->components, 3u);
    EXPECT_TRUE(whole->hasTexcoords());
    EXPECT_EQ(whole->texcoordCount(), 6u);
    EXPECT_EQ(segment->texcoordCount(), 6u);
    EXPECT_EQ(whole->buffer(Geometry::kTexCoordLocation)->vec3View().size(), 6u);

    // The OTHER shape of the same slot — which is what keeps a mesh's UVs a UV pair, and what the renderer
    // reads to pick the sampler2D variant instead.
    const auto uv = intrusive_ptr<vine::Buffer<float>>(
        new vine::Buffer<float>(std::vector<float>{ 0.0f, 0.0f, 1.0f, 1.0f, 0.5f, 0.5f }));
    auto uv_geometry = intrusive_ptr<Geometry>(new Geometry());
    uv_geometry->setTexcoords(uv);
    ASSERT_NE(uv_geometry->buffer(Geometry::kTexCoordLocation), nullptr);
    EXPECT_EQ(uv_geometry->buffer(Geometry::kTexCoordLocation)->components, 2u);
    EXPECT_EQ(uv_geometry->texcoordCount(), 3u);
}

TEST(GeometryTest, TheIndexStreamsTwoSpellingsShareOneSegment)
{
    // The index stream states the same two cases as the vertex roles (a whole buffer, or a segment), and
    // the views the draw uses read that ONE segment: nothing here can disagree with anything else.
    const auto arena = intrusive_ptr<vine::Buffer<std::uint32_t>>(
        new vine::Buffer<std::uint32_t>(std::vector<std::uint32_t>{ 0u, 1u, 2u, 2u, 1u, 0u }));

    auto whole = intrusive_ptr<Geometry>(new Geometry());
    whole->setIndices(arena);
    auto segment = intrusive_ptr<Geometry>(new Geometry());
    segment->setIndices(arena, 0u, 6u); // the same range, stated as a segment

    EXPECT_EQ(whole->indicesBuffer(), arena);
    EXPECT_EQ(segment->indicesBuffer(), arena);
    EXPECT_EQ(whole->firstIndex(), 0u);
    EXPECT_EQ(segment->firstIndex(), 0u);
    EXPECT_EQ(whole->indexCount(), 6u);
    EXPECT_EQ(segment->indexCount(), 6u);
    EXPECT_EQ(whole->indices().size(), segment->indices().size());

    // The difference between the two spellings is the same as on the channel side: a whole buffer is
    // "follow it", a segment keeps the count it stated.
    arena->append(std::vector<std::uint32_t>{ 0u, 1u, 2u });
    EXPECT_EQ(whole->indexCount(), 9u) << "the whole-buffer spelling followed the buffer";
    EXPECT_EQ(segment->indexCount(), 6u) << "the segment stayed the segment";

    // And a geometry that states no buffer at all has an empty stream, whichever view is asked.
    auto none = intrusive_ptr<Geometry>(new Geometry());
    EXPECT_FALSE(none->hasIndices());
    EXPECT_TRUE(none->indices().empty());
    EXPECT_EQ(none->indexCount(), 0u);
    EXPECT_EQ(none->firstIndex(), 0u);
}

TEST(GeometryTest, IndexSliceExposesItsSpan)
{
    const auto indices = intrusive_ptr<const vine::Buffer<std::uint32_t>>(
        new vine::Buffer<std::uint32_t>(std::vector<std::uint32_t>{ 0u, 1u, 2u, 0u, 1u, 2u }));
    auto geom = intrusive_ptr<Geometry>(new Geometry());

    geom->setIndices(indices, 3u, 3u);
    EXPECT_TRUE(geom->hasIndices());
    EXPECT_EQ(geom->firstIndex(), 3u);
    EXPECT_EQ(geom->indexCount(), 3u);
    ASSERT_EQ(geom->indices().size(), 3u) << "the drawn span, not the buffer's six";

    // The buffer itself is still reachable whole: that is what a backend aliases and what an index arena
    // shares between its geometries.
    EXPECT_EQ(geom->indicesBuffer()->size(), 6u);

    // No fixed count means "to the end", and the slice is never longer than the buffer.
    geom->setIndices(indices, 3u, 0u);
    EXPECT_EQ(geom->indexCount(), 3u);
    geom->setIndices(indices, 5u, 0u);
    EXPECT_EQ(geom->indexCount(), 1u);
    EXPECT_TRUE(geom->hasIndices());
    geom->setIndices(indices, 6u, 0u);
    EXPECT_FALSE(geom->hasIndices()) << "a span that starts past the end draws nothing";

    geom->setIndices(indices, 0u, 0u);
    EXPECT_EQ(geom->indexCount(), 6u);
}

TEST(RayIntersectionTest, HitsTheSegmentNotTheArena)
{
    const auto arena = arenaBuffer(3u);
    // Straight down onto the second segment's plane. The segments lie in parallel planes ten units apart, so
    // the point decides which one is hit: (0.25, 10.5) is inside segment 1 and far outside segment 2.
    const Ray ray(Vec3d(0.25, 10.5, 40.0), Vec3d(0.0, 0.0, -1.0));

    const auto hit = RayIntersection::intersect(ray, segmentGeometry(arena, 3u).get(), Mat4d());
    ASSERT_TRUE(hit.hit);
    EXPECT_NEAR(hit.point.y, 10.5, 1e-6) << "the hit must be on the segment the channel reads";
    EXPECT_NEAR(hit.point.z, 0.0, 1e-6);

    // The third segment of the same arena: the ray passes over it and misses, so a slice that were ignored
    // (segment 0 bound for every geometry) would have been hit instead.
    EXPECT_FALSE(RayIntersection::intersect(ray, segmentGeometry(arena, 6u).get(), Mat4d()).hit);
}
