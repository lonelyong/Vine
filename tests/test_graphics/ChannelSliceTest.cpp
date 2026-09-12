/**
 * @brief Channels that read a SEGMENT of one buffer: "one arena, one segment per geometry" (P7).
 *
 * `AttributeBuffer::offset` / `scalarCount` are what make an arena expressible — several geometries share one
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
    geom->addBuffer(0u, AttributeBuffer::slice(arena, 3u, first_vertex, 3u));
    return geom;
}

}  // namespace

TEST(AttributeBufferTest, AChannelReadsOnlyItsOwnSlice)
{
    // The second segment of a two-segment arena: vertex 3, which is scalar 9 of a three-component channel.
    const auto buffer = arenaBuffer(2u);
    const auto sliced = AttributeBuffer::slice(buffer, 3u, 3u, 2u);

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

TEST(AttributeBufferTest, ASliceMayHaveNoFixedLengthAndNeverRunsPastTheBuffer)
{
    const auto buffer = arenaBuffer(2u);

    // The sentinel every channel used before slices existed: "whatever the buffer holds from offset on" —
    // which is what keeps a channel following a buffer that grows under it.
    EXPECT_EQ(AttributeBuffer::shared(buffer, 3u).floatCount(), buffer->size());
    const auto from_second = AttributeBuffer::shared(buffer, 3u, 9u);
    EXPECT_EQ(from_second.vertexCount(), 3u);
    EXPECT_EQ(from_second.floatCount(), buffer->size() - 9u);

    buffer->push_back(20.0f);
    buffer->push_back(20.0f);
    buffer->push_back(0.0f);
    buffer->setRevision(buffer->revision() + 1u);
    EXPECT_EQ(from_second.floatCount(), buffer->size() - 9u) << "no fixed length: it follows the buffer";

    // A fixed length truncates; an offset past the end is empty rather than out of range.
    EXPECT_EQ(AttributeBuffer::shared(buffer, 3u, 9u, 3u).vertexCount(), 1u);
    EXPECT_TRUE(AttributeBuffer::shared(buffer, 3u, buffer->size() + 100u).empty());
    EXPECT_GT(AttributeBuffer::shared(buffer, 3u, 9u, 0u).floatCount(), 0u) << "0 means the rest, not empty";
}

TEST(AttributeBufferTest, SliceStatesTheSegmentInVertices)
{
    // What a caller has in hand is a vertex, and the multiplication by the stride is the part that is easy to
    // get wrong, so the factory does it.
    const auto buffer = arenaBuffer(2u);
    const auto sliced = AttributeBuffer::slice(buffer, 3u, 3u, 0u);

    EXPECT_EQ(sliced.offset, 9u) << "vertex 3 of a three-component channel is scalar 9";
    EXPECT_EQ(sliced.vertexCount(), 3u) << "0 vertices means the rest of the buffer";
    EXPECT_FLOAT_EQ(sliced.xyz(0u)[1], 10.0f);

    // A fixed count truncates, in the same unit.
    EXPECT_EQ(AttributeBuffer::slice(buffer, 3u, 3u, 1u).vertexCount(), 1u);
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
