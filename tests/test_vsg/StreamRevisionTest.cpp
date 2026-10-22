#include <gtest/gtest.h>

#include <cstdint>
#include <vector>

#include <vine/Buffer.hpp>
#include <vine/graphics/Geometry.hpp>
#include <vine/graphics/ShaderAbi.hpp>
#include <vine/intrusive_ptr.hpp>

#include <vine/graphics/backend/Streams.hpp>
#include <vine/vsg/internal/ContentFacts.hpp>
#include <vine/vsg/internal/FactResult.hpp>
#include <vine/vsg/internal/GeometryFacts.hpp>

using vn::graphics::Geometry;
using vn::graphics::backend::SharedStreams;
using vn::graphics::backend::StreamKey;
using vn::vsg::buildGeometryFacts;
using vn::vsg::ChannelFacts;
using vn::vsg::FactMiss;
using vn::vsg::GeometryFacts;

namespace
{

/// @brief A geometry whose two channels read two DIFFERENT buffers, so an announcement can be aimed at one.
///
/// Positions at location 0 and texcoords at the ABI's reserved slot, no indices: the smallest geometry that
/// can tell "this channel moved" from "the geometry changed".
struct TwoChannel
{
    vn::intrusive_ptr<Geometry>          geometry;
    vn::intrusive_ptr<vn::Buffer<float>> positions;
    vn::intrusive_ptr<vn::Buffer<float>> texcoords;

    /// @brief What the table says about it right now (built fresh: entries borrow the caller's storage).
    [[nodiscard]] GeometryFacts describe(std::vector<ChannelFacts>& storage) const
    {
        GeometryFacts facts;
        EXPECT_EQ(buildGeometryFacts(*geometry, facts, storage), FactMiss::None);
        return facts;
    }
};

TwoChannel twoChannels()
{
    TwoChannel out;
    out.geometry = vn::intrusive_ptr<Geometry>(new Geometry());
    out.positions =
        vn::intrusive_ptr<vn::Buffer<float>>(new vn::Buffer<float>(
            std::vector<float>{ -1.0F, -1.0F, 0.0F, 1.0F, -1.0F, 0.0F, 1.0F, 1.0F, 0.0F, -1.0F, 1.0F, 0.0F }));
    out.texcoords = vn::intrusive_ptr<vn::Buffer<float>>(
        new vn::Buffer<float>(std::vector<float>{ 0.0F, 0.0F, 1.0F, 0.0F, 1.0F, 1.0F, 0.0F, 1.0F }));
    out.geometry->setPositions(out.positions);
    out.geometry->setTexcoords2(out.texcoords);
    out.geometry->setRevision(1U);
    return out;
}

}  // namespace

/*
 * @brief The ONLY way an edit re-uploads a stream: the host announced it on the buffer that holds the bytes.
 */
TEST(StreamRevisionTest, ABufferAnnouncementRekeysOnlyTheChannelThatReadsIt)
{
    const TwoChannel two = twoChannels();

    std::vector<ChannelFacts> storage;
    const GeometryFacts       facts = two.describe(storage);
    ASSERT_EQ(facts.channels.size(), 2U);

    const StreamKey positions_before = facts.channels[0].streamKey();
    const StreamKey texcoords_before = facts.channels[1].streamKey();

    // The host refills the positions buffer in place and announces it the way Buffer's contract says.
    two.positions->setRevision(two.positions->revision() + 1U);

    const StreamKey positions_after = facts.channels[0].streamKey();
    const StreamKey texcoords_after = facts.channels[1].streamKey();

    EXPECT_NE(positions_after, positions_before) << "the channel that reads the announced buffer is re-uploaded";
    EXPECT_EQ(texcoords_after, texcoords_before) << "a channel nobody announced keeps its upload";
}

/*
 * @brief There is NO fallback, and this case is what says so: a geometry-level announcement is about the
 * geometry's SHAPE, so it does not re-key the streams - bytes nobody announced are treated as unchanged.
 *
 * Stated as evidence because the opposite (letting the geometry's revision stand in for a buffer's) looks
 * kind and costs every host a full re-upload whenever it touches one channel.
 */
TEST(StreamRevisionTest, AGeometryAnnouncementDoesNotRekeyTheStreams)
{
    const TwoChannel two = twoChannels();

    std::vector<ChannelFacts> storage;
    const GeometryFacts       facts = two.describe(storage);

    const StreamKey before_positions = facts.channels[0].streamKey();
    const StreamKey before_texcoords = facts.channels[1].streamKey();

    two.geometry->bumpRevision();

    EXPECT_EQ(facts.channels[0].streamKey(), before_positions) << "the buffers said nothing, so nothing moved";
    EXPECT_EQ(facts.channels[1].streamKey(), before_texcoords);

    // What it DOES move is the description's revision: the number a table is rebuilt on (layout and counts).
    std::vector<ChannelFacts> again;
    const GeometryFacts       rebuilt = two.describe(again);
    EXPECT_NE(rebuilt.revision, facts.revision) << "the geometry announced, so its description is a new one";
}

/*
 * @brief A STRUCTURAL change is a new stream by IDENTITY, so it uploads with no revision involved: replacing a
 * buffer or re-slicing the range changes the key's own fields.
 */
TEST(StreamRevisionTest, AStructuralChangeIsANewStreamByIdentity)
{
    const TwoChannel two = twoChannels();

    std::vector<ChannelFacts> storage;
    const GeometryFacts       facts = two.describe(storage);
    const StreamKey           before = facts.channels[0].streamKey();

    // The same bytes in a DIFFERENT buffer (a replacement is how a host swaps a channel's source).
    auto replacement = vn::intrusive_ptr<vn::Buffer<float>>(
        new vn::Buffer<float>(std::vector<float>{ -1.0F, -1.0F, 0.0F, 1.0F, -1.0F, 0.0F, 1.0F, 1.0F, 0.0F,
                                                  -1.0F, 1.0F, 0.0F }));
    two.geometry->setPositions(replacement);
    two.geometry->bumpRevision();
    ASSERT_EQ(replacement->revision(), 0U) << "a fresh buffer starts at 0: the identity, not a revision, is what moved";

    std::vector<ChannelFacts> replaced_storage;
    const GeometryFacts       replaced = two.describe(replaced_storage);
    const StreamKey           after    = replaced.channels[0].streamKey();
    EXPECT_NE(after.buffer, before.buffer) << "a different buffer is a different stream";
    EXPECT_NE(after, before);

    // ...and so is the same buffer read from a different place: the slice is part of the identity.
    two.geometry->setPositions(replacement, 0U, 2U);
    two.geometry->bumpRevision();
    std::vector<ChannelFacts> sliced_storage;
    const GeometryFacts       sliced = two.describe(sliced_storage);
    EXPECT_NE(sliced.channels[0].streamKey(), after) << "the slice's own fields are the identity";
}

/*
 * @brief The bytes are read when the upload happens, not when the table is built: an edit is visible to the very
 * next frame without rebuilding anything.
 *
 * This is the half that makes the buffer announcement work at all - a table holding a COPY would upload the old
 * bytes under a new key, which is worse than not re-uploading.
 */
TEST(StreamRevisionTest, TheBytesComeFromTheBufferAndNotFromTheTable)
{
    const TwoChannel two = twoChannels();

    std::vector<ChannelFacts> storage;
    const GeometryFacts       facts = two.describe(storage);

    const std::span<const float> before = facts.channels[0].scalars();
    ASSERT_EQ(before.size(), 12U);

    // In place, behind the SDK's back - exactly the edit a host makes through a raw pointer.
    (*two.positions)[0] = 7.0F;

    const std::span<const float> after = facts.channels[0].scalars();
    ASSERT_EQ(after.size(), 12U);
    EXPECT_EQ(after[0], 7.0F) << "the table must not have snapshotted the scalars";
}

/*
 * @brief What the whole mechanism is FOR: the sharing table uploads exactly the streams whose key moved.
 *
 * The keys are the composed ones (streamKey), which is the identity `StreamUploads` caches by, so this case
 * measures the cost a frame pays.
 */
TEST(StreamRevisionTest, TheSharingTableUploadsOnlyTheStreamWhoseAnnouncementMoved)
{
    const TwoChannel two = twoChannels();

    std::vector<ChannelFacts> storage;
    const GeometryFacts       facts = two.describe(storage);

    SharedStreams table(8U);
    const auto    acquire = [&table](const StreamKey& key, std::uint64_t frame) {
        return table.acquire(key, frame).action;
    };

    EXPECT_EQ(acquire(facts.channels[0].streamKey(), 1U), SharedStreams::Action::Upload);
    EXPECT_EQ(acquire(facts.channels[1].streamKey(), 1U), SharedStreams::Action::Upload);
    EXPECT_EQ(table.uploads(), 2U);

    // The editor touched positions only.
    two.positions->setRevision(two.positions->revision() + 1U);
    EXPECT_EQ(acquire(facts.channels[0].streamKey(), 2U), SharedStreams::Action::Upload)
        << "the edited channel is uploaded again";
    EXPECT_EQ(acquire(facts.channels[1].streamKey(), 2U), SharedStreams::Action::Alias)
        << "and the untouched one is not";

    // A geometry announcement is about shape: it re-uploads nothing, because no buffer spoke.
    two.geometry->bumpRevision();
    EXPECT_EQ(acquire(facts.channels[0].streamKey(), 3U), SharedStreams::Action::Alias);
    EXPECT_EQ(acquire(facts.channels[1].streamKey(), 3U), SharedStreams::Action::Alias);
    EXPECT_EQ(table.uploads(), 3U) << "2 first, 1 after the buffer announcement, none for the geometry's";
}
