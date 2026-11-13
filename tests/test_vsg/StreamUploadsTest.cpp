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

#include <atomic>
#include <cstdint>
#include <thread>
#include <unordered_map>
#include <vector>

#include <vsg/commands/BindIndexBuffer.h>
#include <vsg/commands/BindVertexBuffers.h>
#include <vsg/core/Array.h>
#include <vsg/core/Data.h>
#include <vsg/state/BufferInfo.h>

#include <vine/Buffer.hpp>
#include <vine/graphics/ShaderAbi.hpp>
#include <vine/graphics/backend/FrameTimeline.hpp>
#include <vine/graphics/backend/RetirementQueue.hpp>
#include <vine/graphics/backend/Streams.hpp>
#include <vine/intrusive_ptr.hpp>
#include <vine/vsg/internal/ContentFacts.hpp>
#include <vine/vsg/internal/StreamUploads.hpp>

using vn::graphics::backend::FrameTimeline;
using vn::graphics::backend::RetirementQueue;
using vn::graphics::backend::StreamKey;
using vn::graphics::backend::StreamKind;
using vn::vsg::ContentDeclarationIndexes;
using vn::vsg::ContentDeclarations;
using vn::vsg::ContentFacts;
using vn::vsg::StreamUploads;

namespace
{

/// @brief A canonical vertex stream: positions at their shader location, read from a model buffer.
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

/// @brief What the content store publishes: the buffers its live rows declare (and nothing else).
///
/// The store keeps this index beside its rows (see ContentDeclarations), so a test only has to say which
/// addresses are declared and hand the facts over.
class Declarations {
  public:
    void add(const void* buffer)
    {
        ++buffers_[buffer];
    }

    void remove(const void* buffer)
    {
        const auto found = buffers_.find(buffer);
        if (found != buffers_.end() && --found->second == 0U) {
            buffers_.erase(found);
        }
    }

    [[nodiscard]] ContentFacts facts() const
    {
        ContentFacts out;
        out.declarations = ContentDeclarations{ ContentDeclarationIndexes{ .buffers = &buffers_ } };
        return out;
    }

  private:
    std::unordered_map<const void*, std::uint32_t> buffers_;
};

} // namespace

TEST(StreamUploadsTest, TheSecondDrawableOfOneStreamAliasesTheUpload)
{
    StreamUploads store;
    auto          buffer = vn::intrusive_ptr<vn::Buffer<float>>(new vn::Buffer<float>(std::vector<float>(36U, 0.0F)));
    const StreamKey key  = vertexKey(buffer.get(), buffer->revision(), 0, 36);

    const auto first  = store.acquireVertex(key, buffer->view(), buffer);
    const auto second = store.acquireVertex(key, buffer->view(), buffer);

    EXPECT_EQ(first.action, StreamUploads::Action::Uploaded);
    EXPECT_EQ(second.action, StreamUploads::Action::Aliased) << "the same stream is one bind, and therefore one device buffer and one upload";
    ASSERT_NE(first.bind, nullptr);
    EXPECT_EQ(first.bind, second.bind) << "the SAME bind object is what makes the bytes exist once";
    EXPECT_EQ(store.uploads(), 1U);
    EXPECT_EQ(store.aliases(), 1U);
    EXPECT_EQ(store.live(), 1U);
}

TEST(StreamUploadsTest, ASliceThatMovedIsANewUpload)
{
    StreamUploads store;
    // One arena; two geometries keep different halves of it, as GeometryFacts hands the slices over.
    auto            arena = vn::intrusive_ptr<vn::Buffer<float>>(new vn::Buffer<float>(std::vector<float>(24U, 0.0F)));
    const StreamKey first = vertexKey(arena.get(), arena->revision(), 0, 12);
    const StreamKey next  = vertexKey(arena.get(), arena->revision(), 12, 12);

    const auto a = store.acquireVertex(first, arena->view().subspan(0U, 12U), arena);
    const auto b = store.acquireVertex(next, arena->view().subspan(12U, 12U), arena);

    EXPECT_EQ(a.action, StreamUploads::Action::Uploaded);
    EXPECT_EQ(b.action, StreamUploads::Action::Uploaded) << "the next geometry in the arena reads other bytes";
    EXPECT_NE(a.bind, b.bind);
    EXPECT_EQ(store.uploads(), 2U);
}

TEST(StreamUploadsTest, ARefilledBufferIsANewUpload)
{
    StreamUploads store;
    auto          buffer = vn::intrusive_ptr<vn::Buffer<float>>(new vn::Buffer<float>(std::vector<float>(36U, 0.0F)));
    const auto    first  = store.acquireVertex(vertexKey(buffer.get(), buffer->revision(), 0, 36), buffer->view(), buffer);

    buffer->setRevision(buffer->revision() + 1U);
    const auto filled = store.acquireVertex(vertexKey(buffer.get(), buffer->revision(), 0, 36), buffer->view(), buffer);

    EXPECT_EQ(filled.action, StreamUploads::Action::Uploaded) << "the revision moved: serving the old bind would draw the previous bytes";
    EXPECT_NE(first.bind, filled.bind);
}

TEST(StreamUploadsTest, AStreamWithoutItsBufferHandleIsRefused)
{
    StreamUploads store;
    auto          buffer = vn::intrusive_ptr<vn::Buffer<float>>(new vn::Buffer<float>(std::vector<float>(36U, 0.0F)));

    // A borrowed identity has nothing that reserves the address its key names; a snapshot served to whoever lands
    // there next is exactly the silent-wrong-picture case the entry's hold exists to prevent.
    const auto refused = store.acquireVertex(vertexKey(buffer.get(), buffer->revision(), 0, 36), buffer->view());

    EXPECT_EQ(refused.action, StreamUploads::Action::Refused);
    EXPECT_EQ(refused.bind, nullptr);
    EXPECT_EQ(store.refusals(), 1U);
    EXPECT_EQ(store.live(), 0U) << "a refusal changes no state: the caller builds its own bind";
}

TEST(StreamUploadsTest, TheIndexBindAliasesTheWholeBufferSoTwoSpansShareIt)
{
    StreamUploads store;
    // Two geometries slicing ONE index arena: same buffer, same revision, different spans.
    auto            arena = vn::intrusive_ptr<vn::Buffer<std::uint32_t>>(new vn::Buffer<std::uint32_t>(std::vector<std::uint32_t>(48U, 1U)));
    const StreamKey first  = indexKey(arena.get(), arena->revision(), 0, 36);
    const StreamKey second = indexKey(arena.get(), arena->revision(), 36, 12);

    const auto a = store.acquireIndex(first, arena->view(), arena);
    const auto b = store.acquireIndex(second, arena->view(), arena);

    EXPECT_EQ(a.action, StreamUploads::Action::Uploaded);
    EXPECT_EQ(b.action, StreamUploads::Action::Aliased) << "the bind covers the whole buffer and the DRAW states the span, so both read one upload";
    EXPECT_EQ(a.bind, b.bind);
    EXPECT_EQ(store.uploads(), 1U);
    EXPECT_EQ(store.live(), 1U) << "the bind covers the whole buffer, so both spans are ONE entry";
}

TEST(StreamUploadsTest, AnIndexEntryLeavesWhenItsBufferIsNoLongerDeclared)
{
    StreamUploads store;
    auto          arena = vn::intrusive_ptr<vn::Buffer<std::uint32_t>>(new vn::Buffer<std::uint32_t>(std::vector<std::uint32_t>(48U, 1U)));
    const StreamKey key  = indexKey(arena.get(), arena->revision(), 0, 36);

    (void)store.acquireIndex(key, arena->view(), arena);

    // One live row declares the index buffer.
    Declarations declared;
    declared.add(arena.get());
    FrameTimeline   timeline;
    RetirementQueue retirement(1U);
    EXPECT_EQ(store.beginFrame(declared.facts(), timeline, retirement), 0U);

    // The row goes (its geometry left the tree): nothing declares the buffer any more.
    declared.remove(arena.get());
    EXPECT_EQ(store.beginFrame(declared.facts(), timeline, retirement), 1U);
    EXPECT_EQ(store.live(), 0U);
    EXPECT_EQ(store.retired(), 1U);

    const auto again = store.acquireIndex(key, arena->view(), arena);
    EXPECT_EQ(again.action, StreamUploads::Action::Uploaded) << "nothing declares those bytes any more";
    EXPECT_EQ(store.uploads(), 2U);
}

TEST(StreamUploadsTest, ADerivedChannelIsRefusedAndCounted)
{
    StreamUploads store;
    auto          buffer  = vn::intrusive_ptr<vn::Buffer<float>>(new vn::Buffer<float>(std::vector<float>(36U, 0.0F)));
    StreamKey     derived = vertexKey(nullptr, 0, 0, 36); // what the builder makes for a geometry that authors none

    const auto refused = store.acquireVertex(derived, buffer->view(), buffer);

    EXPECT_EQ(refused.action, StreamUploads::Action::Refused);
    EXPECT_EQ(refused.bind, nullptr);
    EXPECT_EQ(store.refusals(), 1U);
    EXPECT_EQ(store.live(), 0U) << "a refusal changes no state: the caller builds its own bind";
}

TEST(StreamUploadsTest, ACustomChannelIsRefusedBecauseItsBindIsTheWholeLayout)
{
    StreamUploads store;
    auto          buffer = vn::intrusive_ptr<vn::Buffer<float>>(new vn::Buffer<float>(std::vector<float>(36U, 0.0F)));
    StreamKey     custom = vertexKey(buffer.get(), buffer->revision(), 0, 36);
    custom.location      = 5; // a forwarded custom channel (>= 3, never the reserved texcoord slot)

    const auto refused = store.acquireVertex(custom, buffer->view(), buffer);

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

TEST(StreamUploadsTest, ACulledStreamIsNeverReleasedOrReUploaded)
{
    StreamUploads store;
    auto          arena = vn::intrusive_ptr<vn::Buffer<float>>(new vn::Buffer<float>(std::vector<float>(36U, 0.0F)));
    const StreamKey key  = vertexKey(arena.get(), arena->revision(), 0, 36);

    Declarations declared;
    declared.add(arena.get());

    FrameTimeline   timeline;
    RetirementQueue retirement(1U);

    EXPECT_EQ(store.beginFrame(declared.facts(), timeline, retirement), 0U);
    (void)store.acquireVertex(key, arena->view(), arena);
    EXPECT_EQ(store.live(), 1U);

    // CULLING IS NOT LEAVING: the declaration stays for the whole stretch, so nothing leaves and nothing re-uploads.
    for (int frame = 0; frame < 100; ++frame) {
        EXPECT_EQ(store.beginFrame(declared.facts(), timeline, retirement), 0U) << "frame " << frame;
    }
    EXPECT_EQ(store.live(), 1U) << "the content still declares it, however long it is out of view";
    EXPECT_EQ(store.uploads(), 1U) << "and its upload was never thrown away";

    // LEAVING is the declaration going away: the entry is parked and the lookup stops answering.
    declared.remove(arena.get());
    EXPECT_EQ(store.beginFrame(declared.facts(), timeline, retirement), 1U);
    EXPECT_EQ(store.live(), 0U) << "the bind left the table (and is parked, see AnEntryHoldsTheBufferItsKeyNames)";
    EXPECT_EQ(store.retired(), 1U);

    const auto again = store.acquireVertex(key, arena->view(), arena);
    EXPECT_EQ(again.action, StreamUploads::Action::Uploaded) << "nothing declares those bytes any more";
    EXPECT_EQ(store.uploads(), 2U);
}

TEST(StreamUploadsTest, TwoGeometriesOfOneBufferShareOneUploadUntilBothLeave)
{
    StreamUploads store;
    auto          arena = vn::intrusive_ptr<vn::Buffer<float>>(new vn::Buffer<float>(std::vector<float>(36U, 0.0F)));
    const StreamKey key  = vertexKey(arena.get(), arena->revision(), 0, 36);

    const auto first  = store.acquireVertex(key, arena->view(), arena);
    const auto second = store.acquireVertex(key, arena->view(), arena);
    EXPECT_EQ(first.action, StreamUploads::Action::Uploaded);
    EXPECT_EQ(second.action, StreamUploads::Action::Aliased);
    EXPECT_EQ(store.uploads(), 1U) << "one buffer, one upload, however many geometries read it";

    // Two rows declare the buffer: the declaration index counts them (see GeometryTable).
    Declarations declared;
    declared.add(arena.get());
    declared.add(arena.get());

    FrameTimeline   timeline;
    RetirementQueue retirement(1U);
    EXPECT_EQ(store.beginFrame(declared.facts(), timeline, retirement), 0U);

    // One geometry leaves: the other still declares the bytes, so the shared upload stays.
    declared.remove(arena.get());
    EXPECT_EQ(store.beginFrame(declared.facts(), timeline, retirement), 0U);
    EXPECT_EQ(store.live(), 1U) << "a shared upload leaves only when the LAST declarer has";

    // Both gone: the entry goes.
    declared.remove(arena.get());
    EXPECT_EQ(store.beginFrame(declared.facts(), timeline, retirement), 1U);
    EXPECT_EQ(store.live(), 0U);
}

TEST(StreamUploadsTest, ABufferRevisionThatMovedRetiresTheEntryTheContentStillDeclares)
{
    StreamUploads store;
    auto          arena = vn::intrusive_ptr<vn::Buffer<float>>(new vn::Buffer<float>(std::vector<float>(36U, 0.0F)));
    const StreamKey before = vertexKey(arena.get(), arena->revision(), 0, 36);

    Declarations declared;
    declared.add(arena.get());

    FrameTimeline   timeline;
    RetirementQueue retirement(1U);
    (void)store.acquireVertex(before, arena->view(), arena);
    EXPECT_EQ(store.beginFrame(declared.facts(), timeline, retirement), 0U);
    EXPECT_EQ(store.live(), 1U);

    // The host refilled the buffer and said so ON THE BUFFER: no row moved, and the entry still knows.
    arena->setRevision(arena->revision() + 1U);
    EXPECT_EQ(store.beginFrame(declared.facts(), timeline, retirement), 1U)
        << "the entry compares its own handle's revision: the old upload is not what these bytes are";
    EXPECT_EQ(store.live(), 0U);

    const StreamKey after = vertexKey(arena.get(), arena->revision(), 0, 36);
    const auto      again = store.acquireVertex(after, arena->view(), arena);
    EXPECT_EQ(again.action, StreamUploads::Action::Uploaded) << "the new bytes were never uploaded under this key";
    EXPECT_EQ(store.uploads(), 2U);
}

// 键里的 buffer 是**地址**：一个没人持有的 buffer 会把地址交还给分配器，而新建的 buffer 全是同一个 revision，
// 于是"同地址 + 同 revision + 同切片"的新 buffer 会被喂上**另一个几何上传过的字节**（画面里是别人的顶点，没有
// 任何拒绝）。所以条目必须持有它键名所指的那个 buffer —— 地址在键还能被查到之前不能被回收。
TEST(StreamUploadsTest, AnEntryHoldsTheBufferItsKeyNames)
{
    StreamUploads store;

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

    // A frame that declares nothing parks the entry: the LOOKUP goes now, the objects when the parking window is
    // past (a submitted frame may still bind them).
    FrameTimeline   timeline;
    RetirementQueue retirement(1U);
    EXPECT_EQ(store.beginFrame(ContentFacts{}, timeline, retirement), 1U);
    EXPECT_EQ(store.live(), 0U) << "the bind left the table";
    EXPECT_EQ(watcher->useCount(), 3U) << "but it is PARKED, not destroyed: the queue holds the bind and its owner";

    timeline.completeUpTo(FrameTimeline::retirePoint(timeline.submittedFrame(), 1U));
    retirement.advance(timeline);
    EXPECT_EQ(watcher->useCount(), 2U) << "the park came due; the bind still in hand is what holds the bytes, "
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

/*
 * @brief T31: an edit on the HOST thread is swept like a serial one - and is a data race today (see the note).
 *
 * The sweep reads `entry.owner->revision()` (a plain `std::uint64_t`) on the frame's thread, while the host's
 * `Buffer::edit()` scope writes it on its own. The two accesses are unordered, and this case is the diagnostic
 * backlog T31 asks for: under ThreadSanitizer it reports exactly that pair. In an ordinary build it pins the
 * ANSWER the fix must preserve - the concurrent edit retires the old upload exactly like an edit made between
 * frames, and the next acquire uploads the new bytes.
 *
 * How to see the report (no device is created here, so it runs anywhere):
 *   cmake --build build-tsan --target test_vsg   # compiles; the LINK fails, see the note below
 *   ninja -C build-tsan -t commands test_vsg | tail -n 1 | sed 's#AllocationCounter.cpp.o##g' | bash
 *   TSAN_OPTIONS=halt_on_error=0 ./build-tsan/bin/test_vsg --gtest_filter='StreamUploadsTest.AnEditFromTheHostThread*'
 * The plain link fails today because `tests/test_vsg/AllocationCounter.cpp` replaces the global allocation
 * functions, which collide with libclang_rt.tsan_cxx's own definitions (backlog T34).
 * The unguarded case is deliberate: a sanitizer report here is the KNOWN residual documented in
 * `.ai/design/graphics-threading.md` 2.4, not a regression. Its fix is the declared `(buffer, revision)` value;
 * when that lands, this case must stay green under the sanitizer too.
 */
TEST(StreamUploadsTest, AnEditFromTheHostThreadRetiresTheSameUploadASerialOne)
{
    StreamUploads store;
    auto          arena = vn::intrusive_ptr<vn::Buffer<float>>(new vn::Buffer<float>(std::vector<float>(36U, 0.0F)));
    const StreamKey key  = vertexKey(arena.get(), arena->revision(), 0, 36);
    (void)store.acquireVertex(key, arena->view(), arena);

    Declarations declared;
    declared.add(arena.get());

    // The sweep owns the store (one thread, as on the engine side); the host owns the buffer. Nothing between the
    // two is synchronized - which is the point.
    std::atomic<bool> sweeping{ true };
    std::thread       sweep([&] {
        FrameTimeline   timeline;
        RetirementQueue retirement(1U);
        while (sweeping.load(std::memory_order_relaxed)) {
            (void)store.beginFrame(declared.facts(), timeline, retirement);
        }
    });

    {
        auto scope = arena->edit();
        scope[0]   = 1.0F;
    }
    sweeping.store(false, std::memory_order_relaxed);
    sweep.join();

    // The same answer a serialized edit gives (see ABufferRevisionThatMovedRetiresTheEntryTheContentStillDeclares):
    // the old upload is not what these bytes are, and the next acquire builds the new one.
    FrameTimeline   timeline;
    RetirementQueue retirement(1U);
    (void)store.beginFrame(declared.facts(), timeline, retirement);
    EXPECT_EQ(store.live(), 0U) << "the edited bytes are not what the old upload holds";
    EXPECT_EQ(store.retired(), 1U);

    const auto again = store.acquireVertex(vertexKey(arena.get(), arena->revision(), 0, 36), arena->view(), arena);
    EXPECT_EQ(again.action, StreamUploads::Action::Uploaded) << "the new bytes were never uploaded under this key";
}
