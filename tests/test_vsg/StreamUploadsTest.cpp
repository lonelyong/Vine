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
#include <vector>

#include <vsg/commands/BindIndexBuffer.h>
#include <vsg/commands/BindVertexBuffers.h>
#include <vsg/core/Array.h>
#include <vsg/core/Data.h>
#include <vsg/state/BufferInfo.h>

#include <vine/Buffer.hpp>
#include <vine/graphics/ShaderAbi.hpp>
#include <vine/graphics/backend/Streams.hpp>
#include <vine/intrusive_ptr.hpp>
#include <vine/vsg/internal/StreamUploads.hpp>

using vn::graphics::backend::StreamKey;
using vn::graphics::backend::StreamKind;
using vn::vsg::StreamUploads;

namespace
{

int model_buffer_a = 0;
int model_buffer_b = 0;

/// @brief A canonical vertex stream: positions at their shader location, read from a stand-in model buffer.
StreamKey vertexKey(const void* buffer, std::uint64_t revision, std::uint64_t offset, std::uint64_t count)
{
    StreamKey key;
    key.kind       = StreamKind::Vertex;
    key.location   = vn::graphics::attributeLocation(vn::graphics::VertexAttribute::Position);
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

/// @brief The scalars of @p array, as the upload layer takes them (a view; the bytes are read on a miss).
inline std::span<const float> scalarsOf(const ::vsg::ref_ptr<::vsg::floatArray>& array) noexcept
{
    return ::vsg::ref_ptr<::vsg::floatArray>(array) == nullptr ? std::span<const float>{} : std::span<const float>(array->data(), array->size());
}

/// @brief The indices of @p array, as the upload layer takes them.
inline std::span<const std::uint32_t> indicesOf(const ::vsg::ref_ptr<::vsg::uintArray>& array) noexcept
{
    return array == nullptr ? std::span<const std::uint32_t>{} : std::span<const std::uint32_t>(array->data(), array->size());
}

} // namespace

TEST(StreamUploadsTest, TheSecondDrawableOfOneStreamAliasesTheUpload)
{
    StreamUploads   store;
    const StreamKey key = vertexKey(&model_buffer_a, 1, 0, 36);

    const auto first  = store.acquireVertex(key, scalarsOf(floats(36)));
    const auto second = store.acquireVertex(key, scalarsOf(floats(36)));

    EXPECT_EQ(first.action, StreamUploads::Action::Uploaded);
    EXPECT_EQ(second.action, StreamUploads::Action::Aliased) << "the same stream is one bind, and therefore one device buffer and one upload";
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
    StreamUploads   store;
    const StreamKey first = vertexKey(&model_buffer_a, 1, 0, 12);
    const StreamKey next  = vertexKey(&model_buffer_a, 1, 12, 12);

    const auto a = store.acquireVertex(first, scalarsOf(floats(12)));
    const auto b = store.acquireVertex(next, scalarsOf(floats(12)));

    EXPECT_EQ(b.action, StreamUploads::Action::Uploaded) << "the next geometry in the arena reads other bytes";
    EXPECT_NE(a.bind, b.bind);
    EXPECT_EQ(store.uploads(), 2U);
}

TEST(StreamUploadsTest, ARefilledBufferIsANewUpload)
{
    StreamUploads store;
    const auto    first  = store.acquireVertex(vertexKey(&model_buffer_a, 1, 0, 36), scalarsOf(floats(36)));
    const auto    filled = store.acquireVertex(vertexKey(&model_buffer_a, 2, 0, 36), scalarsOf(floats(36)));

    EXPECT_EQ(filled.action, StreamUploads::Action::Uploaded) << "the revision moved: serving the old bind would draw the previous bytes";
    EXPECT_NE(first.bind, filled.bind);
}

TEST(StreamUploadsTest, TheIndexBindAliasesTheWholeBufferSoTwoSpansShareIt)
{
    StreamUploads store;
    // Two geometries slicing ONE index arena: same buffer, same revision, different spans.
    const StreamKey first  = indexKey(&model_buffer_b, 4, 0, 36);
    const StreamKey second = indexKey(&model_buffer_b, 4, 36, 12);

    const auto a = store.acquireIndex(first, indicesOf(indices(48)));
    const auto b = store.acquireIndex(second, indicesOf(indices(48)));

    EXPECT_EQ(a.action, StreamUploads::Action::Uploaded);
    EXPECT_EQ(b.action, StreamUploads::Action::Aliased) << "the bind covers the whole buffer and the DRAW states the span, so both read one upload";
    EXPECT_EQ(a.bind, b.bind);
    EXPECT_EQ(store.uploads(), 1U);
}

TEST(StreamUploadsTest, TheIndexEntryLeavesUnderTheSameNormalisedKeyTheAcquireUsed)
{
    StreamUploads   store;
    const StreamKey first  = indexKey(&model_buffer_b, 4, 0, 36);
    const StreamKey second = indexKey(&model_buffer_b, 4, 36, 12);

    store.beginFrame(1U, 1U);
    (void)store.acquireIndex(first, indicesOf(indices(48)));
    (void)store.acquireIndex(second, indicesOf(indices(48)));
    EXPECT_EQ(store.live(), 1U) << "the bind covers the whole buffer, so both spans are ONE entry";

    // The sweep looks an entry up by the identity the acquire stored, so the DRAW's span (which is not part of
    // that identity) cannot leave anything behind: the bind goes with the entry, under the whole-buffer key.
    store.beginFrame(9U, 1U);
    EXPECT_EQ(store.releaseUnseen(), 1U);
    EXPECT_EQ(store.live(), 0U);
    EXPECT_EQ(store.objects(), 0U);
    EXPECT_TRUE(store.agreesWithRegistry());

    const auto again = store.acquireIndex(first, indicesOf(indices(48)));
    EXPECT_EQ(again.action, StreamUploads::Action::Uploaded) << "nothing holds those bytes any more";
    EXPECT_EQ(store.uploads(), 2U);
}

TEST(StreamUploadsTest, ADerivedChannelIsRefusedAndCounted)
{
    StreamUploads store;
    StreamKey     derived = vertexKey(nullptr, 0, 0, 36); // what the builder makes for a geometry that authors none

    const auto refused = store.acquireVertex(derived, scalarsOf(floats(36)));

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
    custom.location      = 5; // a forwarded custom channel (>= 3, never the reserved texcoord slot)

    const auto refused = store.acquireVertex(custom, scalarsOf(floats(36)));

    EXPECT_EQ(refused.action, StreamUploads::Action::Refused) << "the custom channels share one command whose identity is the whole layout";
    EXPECT_EQ(store.refusals(), 1U);
}

TEST(StreamUploadsTest, TheCanonicalBindingMappingIsThisStoreSpelling)
{
    using vn::graphics::attributeLocation;
    using vn::graphics::VertexAttribute;

    EXPECT_EQ(StreamUploads::bindingOfCanonical(attributeLocation(VertexAttribute::Position)), 0U);
    EXPECT_EQ(StreamUploads::bindingOfCanonical(attributeLocation(VertexAttribute::Normal)), 1U);
    EXPECT_EQ(StreamUploads::bindingOfCanonical(attributeLocation(VertexAttribute::TexCoord0)), 2U) << "texcoords at 2 is vsg's order for its canonical arrays";
    EXPECT_EQ(StreamUploads::bindingOfCanonical(attributeLocation(VertexAttribute::Color)), 3U);
    EXPECT_EQ(StreamUploads::bindingOfCanonical(5U), StreamUploads::kNoBinding) << "a custom channel has none";
}

TEST(StreamUploadsTest, AStreamNoFrameNamesAnyMoreDropsItsBindAndTheNextAcquireUploadsAgain)
{
    StreamUploads   store;
    const StreamKey key = vertexKey(&model_buffer_a, 7, 0, 36);

    // Two frames name it (the second one aliases), and then nothing does.
    store.beginFrame(1U, 1U);
    (void)store.acquireVertex(key, scalarsOf(floats(36)));
    store.beginFrame(2U, 1U);
    (void)store.acquireVertex(key, scalarsOf(floats(36)));
    EXPECT_EQ(store.live(), 1U);

    store.beginFrame(3U, 1U);
    EXPECT_EQ(store.releaseUnseen(), 0U) << "named one frame ago: inside the window";
    EXPECT_EQ(store.live(), 1U);

    store.beginFrame(4U, 1U);
    EXPECT_EQ(store.releaseUnseen(), 1U) << "a frame past the window and no frame has named it";
    EXPECT_EQ(store.unused(), 1U);
    EXPECT_EQ(store.live(), 0U);
    EXPECT_EQ(store.objects(), 0U) << "the bind went with the entry";

    const auto again = store.acquireVertex(key, scalarsOf(floats(36)));
    EXPECT_EQ(again.action, StreamUploads::Action::Uploaded) << "the bytes are uploaded again";
    EXPECT_EQ(store.uploads(), 2U);
    EXPECT_TRUE(store.agreesWithRegistry());
}

TEST(StreamUploadsTest, ACapacityEvictionDropsTheBindAndTheTwoMapsStayInStep)
{
    StreamUploads   store(2);
    const StreamKey first  = vertexKey(&model_buffer_a, 1, 0, 12);
    const StreamKey second = vertexKey(&model_buffer_a, 1, 12, 12);
    const StreamKey third  = vertexKey(&model_buffer_a, 1, 24, 12);

    store.beginFrame(1U, 1U);
    (void)store.acquireVertex(first, scalarsOf(floats(12)));
    (void)store.acquireVertex(second, scalarsOf(floats(12)));
    const auto evicting = store.acquireVertex(third, scalarsOf(floats(12)));

    EXPECT_EQ(evicting.action, StreamUploads::Action::Uploaded);
    EXPECT_EQ(store.evictions(), 1U);
    EXPECT_EQ(store.live(), 2U);
    EXPECT_EQ(store.objects(), 2U) << "the evicted entry's bind went with it";
    EXPECT_TRUE(store.agreesWithRegistry());

    const auto again = store.acquireVertex(first, scalarsOf(floats(12)));
    EXPECT_EQ(again.action, StreamUploads::Action::Uploaded) << "its lookup left the map";
    EXPECT_EQ(store.objects(), 2U) << "the store still holds one bind per entry";
    EXPECT_TRUE(store.agreesWithRegistry());
}

// 键里的 buffer 是**地址**：一个没人持有的 buffer 会把地址交还给分配器，而新建的 buffer 全是同一个 revision，
// 于是"同地址 + 同 revision + 同切片"的新 buffer 会被喂上**另一个几何上传过的字节**（画面里是别人的顶点，没有
// 任何拒绝）。所以条目必须持有它键名所指的那个 buffer —— 地址在键还能被查到之前不能被回收。
TEST(StreamUploadsTest, AnEntryHoldsTheBufferItsKeyNames)
{
    StreamUploads store;
    store.beginFrame(1U, 1U);

    auto            buffer = vn::intrusive_ptr<vn::Buffer<float>>(new vn::Buffer<float>(std::vector<float>(36, 0.0F)));
    const StreamKey key    = vertexKey(buffer.get(), buffer->revision(), 0, 36);

    const auto acquired = store.acquireVertex(key, buffer->view(), buffer);
    ASSERT_EQ(acquired.action, StreamUploads::Action::Uploaded);
    ASSERT_NE(acquired.bind, nullptr);

    auto watcher = buffer; // a handle to observe the counts with
    buffer.reset();        // the model lets its own reference go
    EXPECT_EQ(watcher->useCount(), 3U) << "the entry holds the buffer, AND the array the bind reads aliases it through a "
                                          "view - which is exactly what \"no copy\" means";

    // An object allocated NOW cannot take that address (a live object keeps it).
    const auto later = vn::intrusive_ptr<vn::Buffer<float>>(new vn::Buffer<float>(std::vector<float>(36, 0.0F)));
    EXPECT_NE(later.get(), watcher.get());

    // The sweep lets the entry - and with it the reservation - go.
    store.beginFrame(4U, 1U);
    EXPECT_EQ(store.releaseUnseen(), 1U);
    EXPECT_EQ(store.objects(), 0U) << "the bind went with the entry";
    EXPECT_EQ(watcher->useCount(), 2U) << "the entry released its share; the bind still in hand is what holds the bytes, "
                                         "which is the file note's \"a dropped entry never frees bytes a frame may still read\"";
}

TEST(StreamUploadsTest, TheUploadedVertexArrayAliasesTheModelsOwnBytes)
{
    StreamUploads store;
    // One arena of 24 floats; the channel keeps the SECOND half of it, as GeometryFacts hands a sliced channel over.
    auto arena = vn::intrusive_ptr<vn::Buffer<float>>(new vn::Buffer<float>(std::vector<float>(24U, 1.5F)));

    const auto acquired = store.acquireVertex(vertexKey(arena.get(), arena->revision(), 12, 12), arena->view().subspan(12U, 12U), arena);
    ASSERT_EQ(acquired.action, StreamUploads::Action::Uploaded);
    ASSERT_NE(acquired.bind, nullptr);
    ASSERT_EQ(acquired.bind->arrays.size(), 1U);

    const ::vsg::ref_ptr<::vsg::Data> array = acquired.bind->arrays.front()->data;
    ASSERT_NE(array, nullptr);
    EXPECT_EQ(array->dataPointer(), static_cast<const void*>(arena->data() + 12U))
        << "the array reads the model's own bytes at the channel's offset: a snapshot would live at another address";
    EXPECT_EQ(array->valueCount(), 12U) << "the channel is the slice, not the whole arena";
    EXPECT_EQ(array->properties.stride, sizeof(float)) << "one scalar per element, exactly as a copied floatArray states";
}

TEST(StreamUploadsTest, TheUploadedIndexArrayAliasesTheWholeModelBuffer)
{
    StreamUploads store;
    auto          arena = vn::intrusive_ptr<vn::Buffer<std::uint32_t>>(new vn::Buffer<std::uint32_t>(std::vector<std::uint32_t>(48U, 7U)));

    const auto acquired = store.acquireIndex(indexKey(arena.get(), arena->revision(), 36, 12), arena->view(), arena);
    ASSERT_EQ(acquired.action, StreamUploads::Action::Uploaded);
    ASSERT_NE(acquired.bind, nullptr);
    ASSERT_NE(acquired.bind->indices, nullptr);

    const ::vsg::ref_ptr<::vsg::Data> array = acquired.bind->indices->data;
    ASSERT_NE(array, nullptr);
    EXPECT_EQ(array->dataPointer(), static_cast<const void*>(arena->data()))
        << "the bind covers the whole buffer and the DRAW states the span, so the array starts where the buffer does";
    EXPECT_EQ(array->valueCount(), 48U) << "a span inside the arena may not shrink what the bind aliases";
}
