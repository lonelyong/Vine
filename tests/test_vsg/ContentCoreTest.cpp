/**
 * @brief The content layer's device-free half: stream identity, the plan that reacts to an edit, the geometry
 * aliasing registry, the material arena and the per-frame ring (see `.ai/design/vsg-backend.md`
 * D3/D7 and milestone M2).
 *
 * What these cases pin, in the order the design promises them:
 *
 *   * a stream's identity is its SLICE and its revision, so two segments of one arena are two streams, and a
 *     refilled buffer is a new one - never a stale copy;
 *   * an edit is answered by a pure decision: nothing, an in-place refresh of named streams, or a rebuild -
 *     and an announcement no stream can account for refuses shared uploads instead of trusting them;
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

#include <vine/graphics/backend/FrameRing.hpp>
#include <vine/graphics/backend/MaterialArena.hpp>
#include <vine/graphics/backend/Streams.hpp>

using vn::graphics::backend::FrameRing;
using vn::graphics::backend::kAssumedInFlightSlots;
using vn::graphics::backend::MaterialArena;
using vn::graphics::backend::perFrameCopies;
using vn::graphics::backend::StreamKey;
using vn::graphics::backend::StreamKind;

namespace
{

/// @brief A vertex channel key with the given slice, on a stand-in buffer.
StreamKey vertexKey(std::uint32_t location, std::uint32_t components, const void* buffer, std::uint64_t revision, std::uint64_t offset, std::uint64_t count)
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

} // namespace

// --- stream identity -----------------------------------------------------------------------------------

TEST(CoreStreamsTest, StreamIdentityIsTheSliceAndTheRevision)
{
    static int buffer_a = 0;
    static int buffer_b = 0;

    const StreamKey first  = vertexKey(0, 3, &buffer_a, 7, 0, 12);
    const StreamKey again  = vertexKey(0, 3, &buffer_a, 7, 0, 12);
    const StreamKey moved  = vertexKey(0, 3, &buffer_a, 7, 12, 12); // the next segment of the same arena
    const StreamKey filled = vertexKey(0, 3, &buffer_a, 8, 0, 12);  // refilled: the revision moved
    const StreamKey other  = vertexKey(0, 3, &buffer_b, 7, 0, 12);

    // The key IS the identity the sharing table looks up (StreamUploads observes the same rules through the
    // live table): the slice and the revision both matter, and a refilled buffer is a different stream
    // rather than a stale copy of the earlier one.
    EXPECT_TRUE(first == again) << "the same slice at the same revision is one stream";
    EXPECT_FALSE(first == moved) << "two segments of one buffer are two streams";
    EXPECT_FALSE(first == filled) << "a refilled buffer is a new stream, not a stale one";
    EXPECT_FALSE(first == other);

    // A derived channel has no buffer to alias, which is exactly what makes it unshareable.
    EXPECT_TRUE(vertexKey(2, 4, nullptr, 0, 0, 12).derived());
    EXPECT_FALSE(first.derived());
}

// --- the material arena --------------------------------------------------------------------------------

/// @brief A material identity a case can name without building an object: the arena keys by id, never by address.
vn::graphics::MaterialId materialId(int index)
{
    return vn::graphics::MaterialId(vn::graphics::Handle::make(static_cast<std::uint32_t>(index + 1), 1U, 0U));
}

TEST(CoreMaterialArenaTest, TheFirstNoteAllocatesAndWritesOneBlock)
{
    MaterialArena arena({});
    arena.beginFrame();

    const auto                 material = materialId(0);
    const MaterialArena::Write write    = arena.note(material, 1);

    EXPECT_EQ(write.kind, MaterialArena::WriteKind::Allocated);
    EXPECT_EQ(write.bytes, 64U) << "one VineMaterialBlock";
    EXPECT_EQ(write.slot, 0U);
    EXPECT_EQ(write.copy, 0U);
    EXPECT_EQ(arena.writes(), 1U);
    EXPECT_EQ(arena.allocations(), 1U);
    EXPECT_EQ(arena.live(), 1U);
    EXPECT_EQ(arena.capacityBytes(), 64U * arena.copies() * 256U) << "block x copies x slots is what the API layer allocates";
    EXPECT_EQ(arena.copies(), perFrameCopies(kAssumedInFlightSlots))
        << "one copy MORE than the frames in flight: the frame being written lands on the copy the OLDEST frame "
           "still allowed to be in flight reads (see core::perFrameCopies)";
}

TEST(CoreMaterialArenaTest, ASteadyFrameWritesNothing)
{
    MaterialArena arena({});
    const auto    material = materialId(0);
    arena.beginFrame();
    (void)arena.note(material, 7);

    for (int frame = 0; frame < 10; ++frame) {
        arena.beginFrame();
        const MaterialArena::Write write = arena.note(material, 7);
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
    const auto    material = materialId(0);
    arena.beginFrame();
    (void)arena.note(material, 1);

    arena.beginFrame();
    const MaterialArena::Write first  = arena.note(material, 2);
    const MaterialArena::Write second = arena.note(material, 3);
    const MaterialArena::Write third  = arena.note(material, 3);

    EXPECT_EQ(first.kind, MaterialArena::WriteKind::Rewritten) << "the revision moved";
    EXPECT_EQ(first.bytes, 64U);
    EXPECT_EQ(second.kind, MaterialArena::WriteKind::Unchanged) << "an application that edited twice still gives the GPU one version of the value";
    EXPECT_EQ(third.kind, MaterialArena::WriteKind::Unchanged);
    EXPECT_EQ(arena.writes(), 2U);
    EXPECT_EQ(arena.bytesWritten(), 128U);
}

TEST(CoreMaterialArenaTest, TheCopyRotationSpansTheInFlightFrames)
{
    MaterialArena arena({});
    const auto    material = materialId(0);

    // ONE MORE frame than the arena owns copies: a copy is only free once the frame that read it is proved
    // finished, and the framework proves that when the slot is recycled - after this frame's write. So the
    // rotation spans `copies` frames, and the first copy comes back on the frame AFTER the last in flight.
    std::vector<std::uint32_t> copies;
    for (std::uint64_t frame = 0; frame <= arena.copies(); ++frame) {
        arena.beginFrame();
        EXPECT_EQ(arena.frame(), frame);
        const MaterialArena::Write write = arena.note(material, frame + 1);
        ASSERT_EQ(write.kind, frame == 0 ? MaterialArena::WriteKind::Allocated : MaterialArena::WriteKind::Rewritten);
        copies.push_back(write.copy);
    }

    ASSERT_EQ(copies.size(), static_cast<std::size_t>(arena.copies()) + 1U);
    for (std::uint32_t copy = 0; copy < arena.copies(); ++copy) {
        EXPECT_EQ(copies[copy], copy) << "every frame of a full rotation writes its own copy";
    }
    EXPECT_EQ(copies[arena.copies()], 0U) << "the first copy is free again only once the frame that read it is out of flight, which is one frame "
                                             "later than the number of frames in flight";
    EXPECT_NE(arena.offsetOf(0U, copies[0]), arena.offsetOf(0U, copies[1])) << "copies are distinct blocks";
}

TEST(CoreMaterialArenaTest, AMaterialThatDoesNotFitIsRefusedNotEvicted)
{
    MaterialArena arena({ 64U, 3U, 2U });
    arena.beginFrame();
    const auto first  = materialId(0);
    const auto second = materialId(1);
    const auto third  = materialId(2);

    const MaterialArena::Write first_write = arena.note(first, 1);
    (void)arena.note(second, 1);
    const MaterialArena::Write refused = arena.note(third, 1);

    // NO EVICTION: the material that does not fit is refused, and the two live ones keep their slots.
    EXPECT_EQ(refused.kind, MaterialArena::WriteKind::Refused);
    EXPECT_EQ(arena.refusals(), 1U);
    EXPECT_EQ(arena.live(), 2U);
    EXPECT_EQ(arena.capacityNeed(), 3U) << "one capacity would have served this frame";
    EXPECT_EQ(arena.note(first, 1).kind, MaterialArena::WriteKind::Unchanged) << "the existing material still hits: nothing was reused under it";

    // A release is what makes room - the host's decision, not the arena's.
    EXPECT_TRUE(arena.release(first));
    const MaterialArena::Write fitted = arena.note(third, 1);
    EXPECT_EQ(fitted.kind, MaterialArena::WriteKind::Allocated);
    EXPECT_EQ(fitted.slot, first_write.slot) << "the released slot is the one reused";
    EXPECT_EQ(arena.refusals(), 1U) << "a release is not a refusal";
}

TEST(CoreMaterialArenaTest, TheCapacityNeedCountsDistinctRefusedMaterialsPerFrame)
{
    MaterialArena arena({ 64U, 3U, 1U });
    arena.beginFrame();
    const auto held    = materialId(0);
    const auto refused = materialId(1);

    (void)arena.note(held, 1);
    (void)arena.note(refused, 1);
    (void)arena.note(refused, 1); // the same material twice in one frame is one slot short, not two
    EXPECT_EQ(arena.refusals(), 2U) << "both notes were answered Refused";
    EXPECT_EQ(arena.capacityNeed(), 2U) << "live (1) + distinct refused (1)";

    arena.beginFrame();
    (void)arena.note(refused, 1);
    EXPECT_EQ(arena.capacityNeed(), 2U) << "the need is the frame's own";
}

TEST(CoreMaterialArenaTest, ThePinSweepReturnsTheSlotsNoLiveContentNames)
{
    // THE SLOT'S LIFETIME IS A PIN, NOT A WINDOW (ADR-2026-11-10, step 4): the arena asks once per resident slot
    // whether live content still names it, and a slot nobody names comes back the same call. A culled material keeps
    // its row (and therefore its slot), so the answer is stable for as long as the content is.
    MaterialArena arena({ 64U, 3U, 4U });
    arena.beginFrame();
    const auto pinned   = materialId(0);
    const auto unpinned = materialId(1);
    (void)arena.note(pinned, 1);
    (void)arena.note(unpinned, 1);

    std::size_t asked   = 0U;
    const auto  is_live = [&asked, pinned](vn::graphics::MaterialId material) {
        ++asked;
        return material == pinned;
    };
    EXPECT_EQ(arena.retireUnpinned(is_live), 1U);
    EXPECT_EQ(asked, 2U) << "one question per resident slot - never a walk over the content";
    EXPECT_EQ(arena.live(), 1U);
    EXPECT_EQ(arena.retired(), 1U);

    // The un-named material is a NEW allocation when content names it again, and the pinned one is untouched.
    EXPECT_EQ(arena.note(unpinned, 1).kind, MaterialArena::WriteKind::Allocated);
    EXPECT_EQ(arena.note(pinned, 1).kind, MaterialArena::WriteKind::Unchanged) << "a pin sweep never rewrites a live slot";
    EXPECT_EQ(arena.writes(), 3U) << "two allocations and the first material's allocation; no rewrite anywhere";
}

TEST(CoreMaterialArenaTest, AReleasedSlotIsReusedByTheNextMaterial)
{
    MaterialArena arena({ 64U, 3U, 2U });
    arena.beginFrame();
    const auto first  = materialId(0);
    const auto second = materialId(1);
    const auto third  = materialId(2);

    const MaterialArena::Write allocated = arena.note(first, 1);
    (void)arena.note(second, 1);
    EXPECT_TRUE(arena.release(first));

    const MaterialArena::Write reused = arena.note(third, 1);
    EXPECT_EQ(reused.kind, MaterialArena::WriteKind::Allocated);
    EXPECT_EQ(reused.slot, allocated.slot) << "the freed slot is what the next material takes";
    EXPECT_EQ(arena.live(), 2U);
    EXPECT_EQ(arena.refusals(), 0U) << "releasing first means nothing had to be refused";
}

// --- the per-frame ring --------------------------------------------------------------------------------

TEST(CoreFrameRingTest, ReservationsAreAlignedAndStayInsideTheSlab)
{
    FrameRing ring({ 80U, 256U, 2U, 2U });
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
    FrameRing ring({ 64U, 64U, 3U, 2U });
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
    FrameRing ring({ 64U, 64U, 2U, 4U });
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
