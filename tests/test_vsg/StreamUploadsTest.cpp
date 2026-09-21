/**
 * @brief The device-facing half of geometry aliasing: one bind (and one upload) per stream identity.
 *
 * The mechanism these cases pin: a bind command OWNS its `BufferInfo`, and a `BufferInfo` is what becomes one
 * device buffer plus its upload. So "two drawables read one stream" has an observable meaning that needs no
 * GPU: they are handed the SAME bind object. Everything else follows - a different slice, a refilled buffer
 * or a different buffer must NOT be handed the same bind, because those are different bytes.
 *
 * The store is also where the "may this be shared at all" rule lives, so the cases pin the refusals too: a
 * derived channel (the white opacity carrier and friends) and a custom channel are refused, and the caller
 * builds a private bind - sharing either would draw every peer with the first drawable's data.
 *
 * No device is created here: vsg's bind and array objects are host-side until a graph is compiled, which is
 * exactly the boundary that makes this testable in-process.
 */

#include <gtest/gtest.h>

#include <cstdint>

#include <vsg/commands/BindIndexBuffer.h>
#include <vsg/commands/BindVertexBuffers.h>
#include <vsg/core/Array.h>
#include <vsg/core/Data.h>

#include <vine/graphics/ShaderAbi.hpp>
#include <vine/vsg/api/StreamUploads.hpp>
#include <vine/vsg/core/Streams.hpp>

using vine::vsg::StreamUploads;
using vine::vsg::core::StreamKey;
using vine::vsg::core::StreamKind;

namespace
{

int model_buffer_a = 0;
int model_buffer_b = 0;

/// @brief A canonical vertex stream: positions at their shader location, read from a stand-in model buffer.
StreamKey vertexKey(const void* buffer, std::uint64_t revision, std::uint64_t offset, std::uint64_t count)
{
    StreamKey key;
    key.kind       = StreamKind::Vertex;
    key.location   = vine::graphics::attributeLocation(vine::graphics::VertexAttribute::Position);
    key.components = 3;
    key.buffer     = buffer;
    key.revision   = revision;
    key.offset     = offset;
    key.count      = count;
    return key;
}

/// @brief The index stream of a geometry, as a span inside an index arena.
StreamKey indexKey(const void* buffer, std::uint64_t revision, std::uint64_t first, std::uint64_t count)
{
    StreamKey key;
    key.kind       = StreamKind::Index;
    key.location   = 0;
    key.components = 1;
    key.buffer     = buffer;
    key.revision   = revision;
    key.offset     = first;
    key.count      = count;
    return key;
}

/// @brief An array that stands in for the model's own bytes (what the caller would bind anyway).
::vsg::ref_ptr<::vsg::floatArray> floats(std::size_t count)
{
    return ::vsg::floatArray::create(count);
}

/// @brief An index array that stands in for the model's own indices.
::vsg::ref_ptr<::vsg::uintArray> indices(std::size_t count)
{
    return ::vsg::uintArray::create(count);
}

}  // namespace

TEST(StreamUploadsTest, TheSecondDrawableOfOneStreamAliasesTheUpload)
{
    StreamUploads store;
    const StreamKey key = vertexKey(&model_buffer_a, 1, 0, 36);

    const auto first  = store.acquireVertex(key, floats(36));
    const auto second = store.acquireVertex(key, floats(36));

    EXPECT_EQ(first.action, StreamUploads::Action::Uploaded);
    EXPECT_EQ(second.action, StreamUploads::Action::Aliased)
        << "the same stream is one bind, and therefore one device buffer and one upload";
    ASSERT_NE(first.bind, nullptr);
    EXPECT_EQ(first.bind, second.bind) << "the SAME bind object is what makes the bytes exist once";
    EXPECT_EQ(store.uploads(), 1U);
    EXPECT_EQ(store.aliases(), 1U);
    EXPECT_EQ(store.live(), 1U);
    EXPECT_EQ(store.objects(), 1U);
    EXPECT_TRUE(store.agreesWithRegistry());
}

TEST(StreamUploadsTest, ASliceThatMovedIsANewUpload)
{
    StreamUploads store;
    const StreamKey first = vertexKey(&model_buffer_a, 1, 0, 12);
    const StreamKey next  = vertexKey(&model_buffer_a, 1, 12, 12);

    const auto a = store.acquireVertex(first, floats(12));
    const auto b = store.acquireVertex(next, floats(12));

    EXPECT_EQ(b.action, StreamUploads::Action::Uploaded) << "the next geometry in the arena reads other bytes";
    EXPECT_NE(a.bind, b.bind);
    EXPECT_EQ(store.uploads(), 2U);
}

TEST(StreamUploadsTest, ARefilledBufferIsANewUpload)
{
    StreamUploads store;
    const auto first  = store.acquireVertex(vertexKey(&model_buffer_a, 1, 0, 36), floats(36));
    const auto filled = store.acquireVertex(vertexKey(&model_buffer_a, 2, 0, 36), floats(36));

    EXPECT_EQ(filled.action, StreamUploads::Action::Uploaded)
        << "the revision moved: serving the old bind would draw the previous bytes";
    EXPECT_NE(first.bind, filled.bind);
}

TEST(StreamUploadsTest, TheIndexBindAliasesTheWholeBufferSoTwoSpansShareIt)
{
    StreamUploads store;
    // Two geometries slicing ONE index arena: same buffer, same revision, different spans.
    const StreamKey first  = indexKey(&model_buffer_b, 4, 0, 36);
    const StreamKey second = indexKey(&model_buffer_b, 4, 36, 12);

    const auto a = store.acquireIndex(first, indices(48));
    const auto b = store.acquireIndex(second, indices(48));

    EXPECT_EQ(a.action, StreamUploads::Action::Uploaded);
    EXPECT_EQ(b.action, StreamUploads::Action::Aliased)
        << "the bind covers the whole buffer and the DRAW states the span, so both read one upload";
    EXPECT_EQ(a.bind, b.bind);
    EXPECT_EQ(store.uploads(), 1U);
}

TEST(StreamUploadsTest, TheIndexBufferIsReleasedUnderTheSameNormalisedKey)
{
    StreamUploads store;
    const StreamKey first  = indexKey(&model_buffer_b, 4, 0, 36);
    const StreamKey second = indexKey(&model_buffer_b, 4, 36, 12);

    (void)store.acquireIndex(first, indices(48));
    (void)store.acquireIndex(second, indices(48));

    EXPECT_FALSE(store.release(first)) << "one reader left";
    EXPECT_TRUE(store.release(second)) << "the draw's span is not part of the bind's identity";
    EXPECT_EQ(store.live(), 0U);
    EXPECT_EQ(store.objects(), 0U);

    const auto again = store.acquireIndex(first, indices(48));
    EXPECT_EQ(again.action, StreamUploads::Action::Uploaded) << "nothing holds those bytes any more";
    EXPECT_EQ(store.uploads(), 2U);
}

TEST(StreamUploadsTest, ADerivedChannelIsRefusedAndCounted)
{
    StreamUploads store;
    StreamKey     derived = vertexKey(nullptr, 0, 0, 36);  // what the builder makes for a geometry that authors none

    const auto refused = store.acquireVertex(derived, floats(36));

    EXPECT_EQ(refused.action, StreamUploads::Action::Refused);
    EXPECT_EQ(refused.bind, nullptr);
    EXPECT_EQ(store.refusals(), 1U);
    EXPECT_EQ(store.live(), 0U) << "a refusal changes no state: the caller builds its own bind";
    EXPECT_TRUE(store.agreesWithRegistry());
}

TEST(StreamUploadsTest, ACustomChannelIsRefusedBecauseItsBindIsTheWholeLayout)
{
    StreamUploads store;
    StreamKey     custom = vertexKey(&model_buffer_a, 1, 0, 36);
    custom.location      = 5;  // a forwarded custom channel (>= 3, never the reserved texcoord slot)

    const auto refused = store.acquireVertex(custom, floats(36));

    EXPECT_EQ(refused.action, StreamUploads::Action::Refused)
        << "the custom channels share one command whose identity is the whole layout";
    EXPECT_EQ(store.refusals(), 1U);
}

TEST(StreamUploadsTest, TheCanonicalBindingMappingIsThisStoreSpelling)
{
    using vine::graphics::attributeLocation;
    using vine::graphics::VertexAttribute;

    EXPECT_EQ(StreamUploads::bindingOfCanonical(attributeLocation(VertexAttribute::Position)), 0U);
    EXPECT_EQ(StreamUploads::bindingOfCanonical(attributeLocation(VertexAttribute::Normal)), 1U);
    EXPECT_EQ(StreamUploads::bindingOfCanonical(attributeLocation(VertexAttribute::TexCoord0)), 2U)
        << "texcoords at 2 is vsg's order for its canonical arrays";
    EXPECT_EQ(StreamUploads::bindingOfCanonical(attributeLocation(VertexAttribute::Color)), 3U);
    EXPECT_EQ(StreamUploads::bindingOfCanonical(5U), StreamUploads::kNoBinding) << "a custom channel has none";
}

TEST(StreamUploadsTest, TheLastReaderLeavingDropsTheBindAndTheNextAcquireUploadsAgain)
{
    StreamUploads store;
    const StreamKey key = vertexKey(&model_buffer_a, 7, 0, 36);

    (void)store.acquireVertex(key, floats(36));
    (void)store.acquireVertex(key, floats(36));

    EXPECT_FALSE(store.release(key)) << "one reader left, one still holds it";
    EXPECT_TRUE(store.release(key)) << "the last reader leaving drops the bind";
    EXPECT_EQ(store.live(), 0U);
    EXPECT_EQ(store.objects(), 0U);

    const auto again = store.acquireVertex(key, floats(36));
    EXPECT_EQ(again.action, StreamUploads::Action::Uploaded) << "the bytes are uploaded again";
    EXPECT_EQ(store.uploads(), 2U);
    EXPECT_TRUE(store.agreesWithRegistry());
}

TEST(StreamUploadsTest, ACapacityEvictionDropsTheBindAndTheTwoMapsStayInStep)
{
    StreamUploads store(2);
    const StreamKey first  = vertexKey(&model_buffer_a, 1, 0, 12);
    const StreamKey second = vertexKey(&model_buffer_a, 1, 12, 12);
    const StreamKey third  = vertexKey(&model_buffer_a, 1, 24, 12);

    (void)store.acquireVertex(first, floats(12));
    (void)store.acquireVertex(second, floats(12));
    const auto evicting = store.acquireVertex(third, floats(12));

    EXPECT_EQ(evicting.action, StreamUploads::Action::Uploaded);
    EXPECT_EQ(store.evictions(), 1U);
    EXPECT_EQ(store.live(), 2U);
    EXPECT_EQ(store.objects(), 2U) << "the evicted entry's bind went with it";
    EXPECT_TRUE(store.agreesWithRegistry());

    const auto again = store.acquireVertex(first, floats(12));
    EXPECT_EQ(again.action, StreamUploads::Action::Uploaded) << "its lookup left the map";
    EXPECT_EQ(store.objects(), 2U) << "the store still holds one bind per entry";
    EXPECT_TRUE(store.agreesWithRegistry());
}
