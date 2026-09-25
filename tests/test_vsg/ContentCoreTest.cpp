/**
 * @brief The content layer's device-free half: stream identity, the plan that reacts to an edit, the geometry
 * aliasing registry, the material arena and the per-frame ring (see `.ai/design/vsg-reimplementation.md`
 * D3/D7 and milestone M2).
 *
 * What these cases pin, in the order the design promises them:
 *
 *   * a stream's identity is its SLICE and its revision, so two segments of one arena are two streams, and a
 *     refilled buffer is a new one - never a stale copy;
 *   * an edit is answered by a pure decision: nothing, an in-place refresh of named streams, or a rebuild -
 *     and an announcement no stream can account for refuses shared uploads instead of trusting them;
 *   * the same stream offered twice uploads once, and the registry drops an entry when its last reader
 *     leaves without ever owning the bytes;
 *   * a steady frame writes no material bytes, a revision change writes exactly one block, and the
 *     in-flight copies rotate - so a write can never land on a copy an in-flight frame reads;
 *   * the per-frame ring never grows: a frame past its budget is refused and counted, because relocating a
 *     buffer a submitted command buffer still names is not a recovery.
 *
 * Every case is device-free by construction: nothing here dereferences a GPU object or includes `vsg::`.
 */

#include <gtest/gtest.h>

#include <cstdint>
#include <vector>

#include <vine/vsg/core/FrameRing.hpp>
#include <vine/vsg/core/MaterialArena.hpp>
#include <vine/vsg/core/Streams.hpp>

using vn::vsg::core::FrameRing;
using vn::vsg::core::kAssumedInFlightSlots;
using vn::vsg::core::MaterialArena;
using vn::vsg::core::perFrameCopies;
using vn::vsg::core::SharedStreams;
using vn::vsg::core::StreamKey;
using vn::vsg::core::StreamKind;

namespace
{

/// @brief A vertex channel key with the given slice, on a stand-in buffer.
StreamKey vertexKey(std::uint32_t location, std::uint32_t components, const void* buffer, std::uint64_t revision,
                    std::uint64_t offset, std::uint64_t count)
{
    StreamKey key;
    key.kind       = StreamKind::Vertex;
    key.location   = location;
    key.components = components;
    key.buffer     = buffer;
    key.revision   = revision;
    key.offset     = offset;
    key.count      = count;
    return key;
}

}  // namespace

// --- stream identity -----------------------------------------------------------------------------------

TEST(CoreStreamsTest, StreamIdentityIsTheSliceAndTheRevision)
{
    static int buffer_a = 0;
    static int buffer_b = 0;

    const StreamKey first  = vertexKey(0, 3, &buffer_a, 7, 0, 12);
    const StreamKey again  = vertexKey(0, 3, &buffer_a, 7, 0, 12);
    const StreamKey moved  = vertexKey(0, 3, &buffer_a, 7, 12, 12);   // the next segment of the same arena
    const StreamKey filled = vertexKey(0, 3, &buffer_a, 8, 0, 12);    // refilled: the revision moved
    const StreamKey other  = vertexKey(0, 3, &buffer_b, 7, 0, 12);

    // The key IS the identity the alias registry looks up (CoreSharedStreamsTest observes the same rules
    // through the live registry): the slice and the revision both matter, and a refilled buffer is a
    // different stream rather than a stale copy of the earlier one.
    EXPECT_TRUE(first == again) << "the same slice at the same revision is one stream";
    EXPECT_FALSE(first == moved) << "two segments of one buffer are two streams";
    EXPECT_FALSE(first == filled) << "a refilled buffer is a new stream, not a stale one";
    EXPECT_FALSE(first == other);

    // A derived channel has no buffer to alias, which is exactly what makes it unshareable.
    EXPECT_TRUE(vertexKey(2, 4, nullptr, 0, 0, 12).derived());
    EXPECT_FALSE(first.derived());
}

// --- the alias registry --------------------------------------------------------------------------------

TEST(CoreSharedStreamsTest, TheSecondReaderOfOneStreamAliasesTheFirstUpload)
{
    SharedStreams   registry;
    const StreamKey key = vertexKey(0, 3, reinterpret_cast<const void*>(1), 5, 0, 12);

    const SharedStreams::Decision first  = registry.acquire(key, 1U);
    const SharedStreams::Decision second = registry.acquire(key, 1U);

    EXPECT_EQ(first.action, SharedStreams::Action::Upload) << "the first reader uploads";
    EXPECT_EQ(second.action, SharedStreams::Action::Alias) << "the second reads the same upload";
    EXPECT_EQ(registry.uploads(), 1U);
    EXPECT_EQ(registry.aliases(), 1U);
    EXPECT_EQ(registry.live(), 1U);
}

TEST(CoreSharedStreamsTest, ASliceThatMovedIsAUploadNotAnAlias)
{
    SharedStreams   registry;
    const StreamKey first = vertexKey(0, 3, reinterpret_cast<const void*>(1), 5, 0, 12);
    const StreamKey next  = vertexKey(0, 3, reinterpret_cast<const void*>(1), 5, 12, 12);

    (void)registry.acquire(first, 1U);
    const SharedStreams::Decision decision = registry.acquire(next, 1U);

    EXPECT_EQ(decision.action, SharedStreams::Action::Upload)
        << "the next geometry in the same arena reads different bytes";
    EXPECT_EQ(registry.uploads(), 2U);
}

TEST(CoreSharedStreamsTest, ARefilledSliceIsANewStreamSoItUploadsAgain)
{
    // THE RULE, through the LIVE path: the revision is part of the identity, so a buffer the host refilled
    // (and announced) can never alias the earlier upload - what is on the device is the old bytes. The
    // unwired `planGeometry` used to be where this was pinned; it had no production caller and is gone
    // (design log §11.16cr), so the evidence moved onto the registry the frame path really uses.
    SharedStreams   registry;
    const StreamKey before   = vertexKey(0, 3, reinterpret_cast<const void*>(1), 5, 0, 12);
    const StreamKey refilled = vertexKey(0, 3, reinterpret_cast<const void*>(1), 6, 0, 12);

    EXPECT_EQ(registry.acquire(before, 1U).action, SharedStreams::Action::Upload);
    EXPECT_EQ(registry.acquire(before, 2U).action, SharedStreams::Action::Alias)
        << "the same identity named again is served by the upload it already has";
    EXPECT_EQ(registry.acquire(refilled, 3U).action, SharedStreams::Action::Upload)
        << "a refilled buffer is a different stream: it uploads again instead of aliasing";
    EXPECT_EQ(registry.uploads(), 2U);
    EXPECT_EQ(registry.aliases(), 1U);
}

TEST(CoreSharedStreamsTest, AnEntryNoFrameNamesForTheGraceWindowLeavesAndTheNextAcquireUploadsAgain)
{
    // THE LIFETIME RULE, and what replaced the reader count (see SharedStreams's note: the count could only
    // ever go up, so "the last reader let go" was unreachable and `release()` had no caller).
    SharedStreams   registry;
    const StreamKey key = vertexKey(0, 3, reinterpret_cast<const void*>(1), 5, 0, 12);
    (void)registry.acquire(key, 1U);
    (void)registry.acquire(key, 1U);

    std::vector<StreamKey> dropped;
    EXPECT_EQ(registry.releaseUnseen(2U, 3U, dropped), 0U) << "named one frame ago: inside the window";
    EXPECT_EQ(registry.releaseUnseen(4U, 3U, dropped), 0U) << "named three frames ago: still inside it";
    EXPECT_EQ(registry.releaseUnseen(5U, 3U, dropped), 1U) << "one frame past the window: it leaves";
    ASSERT_EQ(dropped.size(), 1U);
    EXPECT_TRUE(dropped.front() == key) << "the caller is told WHICH key left, so its objects can follow";
    EXPECT_EQ(registry.unused(), 1U);
    EXPECT_EQ(registry.live(), 0U);
    EXPECT_EQ(registry.releaseUnseen(9U, 3U, dropped), 0U) << "and it leaves exactly once";

    const SharedStreams::Decision again = registry.acquire(key, 9U);
    EXPECT_EQ(again.action, SharedStreams::Action::Upload)
        << "nothing names those bytes any more, so they have to be uploaded again";
    EXPECT_EQ(registry.uploads(), 2U);
    EXPECT_EQ(registry.evictions(), 0U) << "the capacity was never the reason";
}

TEST(CoreSharedStreamsTest, AStreamAFrameStillNamesIsNeverDropped)
{
    // The other half of the rule, and the reason the sweep can run in the same frame that named the entry: an
    // age of zero is inside every window.
    SharedStreams          registry;
    const StreamKey        alive = vertexKey(0, 3, reinterpret_cast<const void*>(1), 5, 0, 12);
    const StreamKey        stale = vertexKey(1, 3, reinterpret_cast<const void*>(1), 5, 36, 12);
    std::vector<StreamKey> dropped;

    for (std::uint64_t frame = 1U; frame <= 8U; ++frame) {
        if (frame == 1U) {
            (void)registry.acquire(stale, frame);
        }
        (void)registry.acquire(alive, frame);
        const std::uint64_t released = registry.releaseUnseen(frame, 1U, dropped);
        // The stale stream was named in frame 1 only: its age is 1 in frame 2 (inside the window) and 2 in
        // frame 3, so it is frame 3 that lets it go - and the one this frame named is never touched.
        const std::uint64_t expected = frame == 3U ? 1U : 0U;
        EXPECT_EQ(released, expected) << "frame " << frame;
        EXPECT_EQ(registry.live(), frame <= 2U ? 2U : 1U) << "frame " << frame;
        EXPECT_EQ(registry.acquire(alive, frame).action, SharedStreams::Action::Alias)
            << "a stream a frame keeps naming stays exactly one entry (frame " << frame << ")";
    }
    EXPECT_EQ(registry.unused(), 1U);
}

TEST(CoreSharedStreamsTest, TheCapacityBoundEvictsTheStalestLookupWithoutReleasingItsBytes)
{
    SharedStreams   registry(2);
    const StreamKey first  = vertexKey(0, 3, reinterpret_cast<const void*>(1), 5, 0, 12);
    const StreamKey second = vertexKey(1, 3, reinterpret_cast<const void*>(1), 5, 36, 12);
    const StreamKey third  = vertexKey(2, 4, reinterpret_cast<const void*>(1), 5, 72, 12);

    (void)registry.acquire(first, 1U);
    (void)registry.acquire(second, 2U);
    (void)registry.acquire(third, 3U);

    EXPECT_EQ(registry.evictions(), 1U) << "the bound was reached and one lookup left";
    EXPECT_EQ(registry.live(), 2U);

    const SharedStreams::Decision again = registry.acquire(first, 4U);
    EXPECT_EQ(again.action, SharedStreams::Action::Upload)
        << "the lookup left the map; a reader that already bound the bytes is a different question";
    EXPECT_EQ(registry.uploads(), 4U);
}

TEST(CoreSharedStreamsTest, TheBoundTakesTheStalestEntryNotTheOldestInserted)
{
    // A scene that ROTATES its content re-names old streams, and there "oldest inserted" and "stalest" are
    // different rows - which is why the bound follows the stamp the lifetime rule already carries.
    SharedStreams   registry(2);
    const StreamKey early_but_renamed = vertexKey(0, 3, reinterpret_cast<const void*>(1), 5, 0, 12);
    const StreamKey middle            = vertexKey(1, 3, reinterpret_cast<const void*>(1), 5, 36, 12);
    const StreamKey newest            = vertexKey(2, 4, reinterpret_cast<const void*>(1), 5, 72, 12);

    (void)registry.acquire(early_but_renamed, 1U);
    (void)registry.acquire(middle, 2U);
    (void)registry.acquire(early_but_renamed, 3U);  // named again: it is now the freshest of the two
    const SharedStreams::Decision evicting = registry.acquire(newest, 4U);

    ASSERT_TRUE(evicting.evicted.has_value());
    EXPECT_TRUE(*evicting.evicted == middle)
        << "the stalest leaves: the entry inserted first is still the one a frame named last";
}

TEST(CoreSharedStreamsTest, TheEvictedKeyIsReportedToTheCaller)
{
    SharedStreams   registry(2);
    const StreamKey first  = vertexKey(0, 3, reinterpret_cast<const void*>(1), 5, 0, 12);
    const StreamKey second = vertexKey(1, 3, reinterpret_cast<const void*>(1), 5, 36, 12);
    const StreamKey third  = vertexKey(2, 4, reinterpret_cast<const void*>(1), 5, 72, 12);

    EXPECT_FALSE(registry.acquire(first, 1U).evicted.has_value()) << "nothing had to make room yet";
    EXPECT_FALSE(registry.acquire(second, 2U).evicted.has_value());
    const SharedStreams::Decision evicting = registry.acquire(third, 3U);

    ASSERT_TRUE(evicting.evicted.has_value())
        << "the layer that owns one object per entry has to hear about this: this is the only moment the two"
           " can be reconciled";
    EXPECT_TRUE(*evicting.evicted == first) << "the stalest entry is the one that leaves";
    EXPECT_EQ(evicting.action, SharedStreams::Action::Upload);
}

// --- the material arena --------------------------------------------------------------------------------

TEST(CoreMaterialArenaTest, TheFirstNoteAllocatesAndWritesOneBlock)
{
    MaterialArena arena({});
    arena.beginFrame();

    const int                 material = 0;
    const MaterialArena::Write write = arena.note(&material, 1);

    EXPECT_EQ(write.kind, MaterialArena::WriteKind::Allocated);
    EXPECT_EQ(write.bytes, 64U) << "one VineMaterialBlock";
    EXPECT_EQ(write.slot, 0U);
    EXPECT_EQ(write.copy, 0U);
    EXPECT_EQ(arena.writes(), 1U);
    EXPECT_EQ(arena.allocations(), 1U);
    EXPECT_EQ(arena.live(), 1U);
    EXPECT_EQ(arena.capacityBytes(), 64U * arena.copies() * 256U)
        << "block x copies x slots is what the API layer allocates";
    EXPECT_EQ(arena.copies(), perFrameCopies(kAssumedInFlightSlots))
        << "one copy MORE than the frames in flight: the frame being written lands on the copy the OLDEST frame "
           "still allowed to be in flight reads (see core::perFrameCopies)";
}

TEST(CoreMaterialArenaTest, ASteadyFrameWritesNothing)
{
    MaterialArena arena({});
    const int     material = 0;
    arena.beginFrame();
    (void)arena.note(&material, 7);

    for (int frame = 0; frame < 10; ++frame) {
        arena.beginFrame();
        const MaterialArena::Write write = arena.note(&material, 7);
        EXPECT_EQ(write.kind, MaterialArena::WriteKind::Unchanged);
        EXPECT_EQ(write.bytes, 0U);
    }

    EXPECT_EQ(arena.writes(), 1U) << "the bytes did not change, so the frame transferred nothing for them";
    EXPECT_EQ(arena.hits(), 10U);
    EXPECT_EQ(arena.bytesWritten(), 64U);
}

TEST(CoreMaterialArenaTest, ARevisionChangeWritesExactlyOncePerFrame)
{
    MaterialArena arena({});
    const int     material = 0;
    arena.beginFrame();
    (void)arena.note(&material, 1);

    arena.beginFrame();
    const MaterialArena::Write first  = arena.note(&material, 2);
    const MaterialArena::Write second = arena.note(&material, 3);
    const MaterialArena::Write third  = arena.note(&material, 3);

    EXPECT_EQ(first.kind, MaterialArena::WriteKind::Rewritten) << "the revision moved";
    EXPECT_EQ(first.bytes, 64U);
    EXPECT_EQ(second.kind, MaterialArena::WriteKind::Unchanged)
        << "an application that edited twice still gives the GPU one version of the value";
    EXPECT_EQ(third.kind, MaterialArena::WriteKind::Unchanged);
    EXPECT_EQ(arena.writes(), 2U);
    EXPECT_EQ(arena.bytesWritten(), 128U);
}

TEST(CoreMaterialArenaTest, TheCopyRotationSpansTheInFlightFrames)
{
    MaterialArena arena({});
    const int     material = 0;

    // ONE MORE frame than the arena owns copies: a copy is only free once the frame that read it is proved
    // finished, and the framework proves that when the slot is recycled - after this frame's write. So the
    // rotation spans `copies` frames, and the first copy comes back on the frame AFTER the last in flight.
    std::vector<std::uint32_t> copies;
    for (std::uint64_t frame = 0; frame <= arena.copies(); ++frame) {
        arena.beginFrame();
        EXPECT_EQ(arena.frame(), frame);
        const MaterialArena::Write write = arena.note(&material, frame + 1);
        ASSERT_EQ(write.kind, frame == 0 ? MaterialArena::WriteKind::Allocated : MaterialArena::WriteKind::Rewritten);
        copies.push_back(write.copy);
    }

    ASSERT_EQ(copies.size(), static_cast<std::size_t>(arena.copies()) + 1U);
    for (std::uint32_t copy = 0; copy < arena.copies(); ++copy) {
        EXPECT_EQ(copies[copy], copy) << "every frame of a full rotation writes its own copy";
    }
    EXPECT_EQ(copies[arena.copies()], 0U)
        << "the first copy is free again only once the frame that read it is out of flight, which is one frame "
           "later than the number of frames in flight";
    EXPECT_NE(arena.offsetOf(0U, copies[0]), arena.offsetOf(0U, copies[1])) << "copies are distinct blocks";
}

TEST(CoreMaterialArenaTest, TheCapacityBoundEvictsTheOldestMaterial)
{
    MaterialArena arena({64U, 3U, 2U});
    arena.beginFrame();
    const int first  = 0;
    const int second = 1;
    const int third  = 2;

    (void)arena.note(&first, 1);
    (void)arena.note(&second, 1);
    const MaterialArena::Write evicting = arena.note(&third, 1);

    EXPECT_EQ(evicting.kind, MaterialArena::WriteKind::Allocated);
    EXPECT_EQ(arena.evictions(), 1U) << "the bound was reached; the oldest material left";
    EXPECT_EQ(arena.live(), 2U);

    const MaterialArena::Write back = arena.note(&first, 1);
    EXPECT_EQ(back.kind, MaterialArena::WriteKind::Allocated) << "its slot left with it, so it allocates again";
    EXPECT_EQ(arena.allocations(), 4U);
}

// --- the per-frame ring --------------------------------------------------------------------------------

TEST(CoreFrameRingTest, ReservationsAreAlignedAndStayInsideTheSlab)
{
    FrameRing ring({80U, 256U, 2U, 2U});
    EXPECT_EQ(ring.stride(), 256U) << "the effective stride is the aligned one";
    EXPECT_EQ(ring.slabBytes(), 512U);
    EXPECT_EQ(ring.capacityBytes(), 1024U);

    ring.beginFrame();
    const FrameRing::Reservation first  = ring.reserve();
    const FrameRing::Reservation second = ring.reserve();
    ASSERT_TRUE(first.valid);
    ASSERT_TRUE(second.valid);
    EXPECT_EQ(first.offset, 0U);
    EXPECT_EQ(second.offset, 256U);

    ring.beginFrame();
    EXPECT_EQ(ring.slot(), 1U);
    EXPECT_EQ(ring.reserve().offset, 512U) << "the second slab belongs to the frame that just began";
}

TEST(CoreFrameRingTest, AFullFrameIsRefusedRatherThanGrown)
{
    FrameRing ring({64U, 64U, 3U, 2U});
    ring.beginFrame();
    EXPECT_TRUE(ring.reserve().valid);
    EXPECT_TRUE(ring.reserve().valid);

    const FrameRing::Reservation refused = ring.reserve();
    EXPECT_FALSE(refused.valid) << "relocating a buffer a submitted command buffer still names is not a recovery";
    EXPECT_EQ(ring.overflows(), 1U);
    EXPECT_EQ(ring.capacityBytes(), 64U * 2U * 3U) << "the layout did not change";
}

TEST(CoreFrameRingTest, TheCursorResetsEveryFrameAndTheHighWaterRemembers)
{
    FrameRing ring({64U, 64U, 2U, 4U});
    ring.beginFrame();
    (void)ring.reserve();
    (void)ring.reserve();
    (void)ring.reserve();
    EXPECT_EQ(ring.reserved(), 3U);
    EXPECT_EQ(ring.highWater(), 3U);

    ring.beginFrame();
    EXPECT_EQ(ring.reserved(), 0U);
    EXPECT_EQ(ring.slot(), 1U);
    (void)ring.reserve();
    EXPECT_EQ(ring.highWater(), 3U) << "the busiest frame is what a phase gates on";

    ring.beginFrame();
    EXPECT_EQ(ring.slot(), 0U) << "the rotation returns to the first slab once the frame is out of flight";
    EXPECT_EQ(ring.frames(), 2U);
}
