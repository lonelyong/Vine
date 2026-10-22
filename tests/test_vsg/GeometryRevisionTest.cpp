#include <gtest/gtest.h>

#include <cstdint>
#include <vector>

#include <vine/graphics/Geometry.hpp>
#include <vine/graphics/ShaderAbi.hpp>
#include <vine/intrusive_ptr.hpp>

#include <vine/vsg/internal/FactResult.hpp>
#include <vine/vsg/internal/GeometryFacts.hpp>

using vn::graphics::Geometry;
using vn::vsg::buildGeometryFacts;
using vn::vsg::ChannelFacts;
using vn::vsg::FactMiss;
using vn::vsg::GeometryFacts;

namespace
{

/// @brief One channel's identity inside a finished entry, in binding order (a copy: the entry is rebuilt).
struct ChannelKeys
{
    std::vector<vn::graphics::backend::StreamKey> keys;
};

/// @brief A geometry with two channels, each reading its OWN buffer, so a bump can be aimed at one of them.
///
/// It is deliberately small: positions at location 0 and texcoords at the ABI's reserved slot, no indices, and
/// the two buffers are separate objects with separate revisions - which is what makes "one channel moved"
/// tellable from "the geometry moved".
struct TwoChannel
{
    vn::intrusive_ptr<Geometry>          geometry;
    vn::intrusive_ptr<vn::Buffer<float>> positions;
    vn::intrusive_ptr<vn::Buffer<float>> texcoords;
};

TwoChannel twoChannels()
{
    TwoChannel out;
    out.geometry = vn::intrusive_ptr<Geometry>(new Geometry());
    out.positions =
        vn::intrusive_ptr<vn::Buffer<float>>(new vn::Buffer<float>(
            std::vector<float>{ -1.0F, -1.0F, 0.0F, 1.0F, -1.0F, 0.0F, 1.0F, 1.0F, 0.0F, -1.0F, 1.0F, 0.0F }));
    out.texcoords =
        vn::intrusive_ptr<vn::Buffer<float>>(new vn::Buffer<float>(
            std::vector<float>{ 0.0F, 0.0F, 1.0F, 0.0F, 1.0F, 1.0F, 0.0F, 1.0F }));
    out.geometry->setPositions(out.positions);
    out.geometry->setTexcoords2(out.texcoords);
    out.geometry->setRevision(1U);
    return out;
}

/// @brief Builds an entry for @p geometry into caller-owned storage and hands back its channel keys.
ChannelKeys describe(const Geometry& geometry, GeometryFacts& facts, std::vector<ChannelFacts>& storage)
{
    EXPECT_EQ(buildGeometryFacts(geometry, facts, storage), FactMiss::None);
    ChannelKeys out;
    out.keys.reserve(facts.channels.size());
    for (const ChannelFacts& channel : facts.channels) {
        out.keys.push_back(channel.key);
    }
    return out;
}

}  // namespace

/*
 * @brief The FINE announcement: a buffer edited in place and announced on the buffer re-keys only its channel.
 *
 * This is the half of the two-level rule the backend used to ignore - the host bumped Buffer::setRevision, the
 * facts row kept its old revision (it was keyed by the geometry's), and the draw kept reading the first bytes
 * ever uploaded. Nothing failed; the picture was simply old.
 */
TEST(GeometryRevisionTest, ABufferAnnouncementRekeysOnlyTheChannelThatReadsIt)
{
    const TwoChannel two = twoChannels();

    GeometryFacts             before{};
    std::vector<ChannelFacts> before_storage;
    const ChannelKeys         before_keys = describe(*two.geometry, before, before_storage);
    ASSERT_EQ(before_keys.keys.size(), 2U);

    // The host edits the positions buffer in place and announces it the way Buffer's contract says.
    two.positions->setRevision(two.positions->revision() + 1U);

    GeometryFacts             after{};
    std::vector<ChannelFacts> after_storage;
    const ChannelKeys         after_keys = describe(*two.geometry, after, after_storage);
    ASSERT_EQ(after_keys.keys.size(), 2U);

    EXPECT_NE(after.revision, before.revision) << "the row describes other bytes now, so its revision must move";
    EXPECT_EQ(after.revision, two.geometry->contentRevision()) << "and it IS the content version";

    // Positions: only the buffer's own revision moved.
    EXPECT_EQ(after_keys.keys[0].buffer, two.positions.get());
    EXPECT_NE(after_keys.keys[0].buffer_revision, before_keys.keys[0].buffer_revision)
        << "the channel that reads the bumped buffer has to be re-uploaded";
    EXPECT_EQ(after_keys.keys[0].revision, before_keys.keys[0].revision) << "the geometry's own revision did not move";

    // Texcoords: byte-for-byte the same key, so the upload it names stays in the sharing table.
    EXPECT_EQ(after_keys.keys[1], before_keys.keys[1]) << "a channel nobody announced must not be re-uploaded";
}

/*
 * @brief The COARSE announcement: "this geometry changed, I did not say which part" re-keys every channel.
 *
 * Both levels are honoured, and this is the one every host in the repository uses today - it has to keep meaning
 * what it always meant, or a host that edits through a raw pointer (the ordinary case) loses its bytes.
 */
TEST(GeometryRevisionTest, AGeometryAnnouncementRekeysEveryChannel)
{
    const TwoChannel two = twoChannels();

    GeometryFacts             before{};
    std::vector<ChannelFacts> before_storage;
    const ChannelKeys         before_keys = describe(*two.geometry, before, before_storage);

    two.geometry->bumpRevision();

    GeometryFacts             after{};
    std::vector<ChannelFacts> after_storage;
    const ChannelKeys         after_keys = describe(*two.geometry, after, after_storage);

    ASSERT_EQ(after_keys.keys.size(), before_keys.keys.size());
    for (std::size_t channel = 0U; channel < after_keys.keys.size(); ++channel) {
        EXPECT_NE(after_keys.keys[channel].revision, before_keys.keys[channel].revision)
            << "channel " << channel << " belongs to the geometry that announced, so its key moves";
        EXPECT_EQ(after_keys.keys[channel].buffer_revision, before_keys.keys[channel].buffer_revision)
            << "the buffers did not change - only the coarse announcement did";
    }
}

/*
 * @brief The alias range does not silently move: two geometries over one buffer segment still share an upload
 * exactly when their own revisions agree, which is what they did before the fine level was honoured.
 *
 * Stated as a case because the tempting "share by bytes alone" would break the coarse announcement: a host that
 * edits through a pointer and announces on the geometry changes no buffer revision, so a byte-keyed stream
 * would serve stale bytes to everybody reading that buffer.
 */
TEST(GeometryRevisionTest, TwoGeometriesShareAnUploadOnTheirOwnRevisionNotOnTheBytesAlone)
{
    const TwoChannel first = twoChannels();

    // A second geometry over the SAME buffers (the arena case), with the same coarse revision.
    const auto second = vn::intrusive_ptr<Geometry>(new Geometry());
    second->setPositions(first.positions);
    second->setTexcoords2(first.texcoords);
    second->setRevision(first.geometry->revision());

    GeometryFacts             first_facts{};
    std::vector<ChannelFacts> first_storage;
    const ChannelKeys         first_keys = describe(*first.geometry, first_facts, first_storage);
    GeometryFacts             second_facts{};
    std::vector<ChannelFacts> second_storage;
    const ChannelKeys         second_keys = describe(*second, second_facts, second_storage);

    ASSERT_EQ(first_keys.keys.size(), second_keys.keys.size());
    for (std::size_t channel = 0U; channel < first_keys.keys.size(); ++channel) {
        EXPECT_EQ(first_keys.keys[channel], second_keys.keys[channel])
            << "same bytes, same announcements, one upload";
    }

    // One of them announces a coarse change: its channels are re-read, the other geometry's are untouched.
    first.geometry->bumpRevision();
    GeometryFacts             bumped{};
    std::vector<ChannelFacts> bumped_storage;
    const ChannelKeys         bumped_keys = describe(*first.geometry, bumped, bumped_storage);
    for (std::size_t channel = 0U; channel < bumped_keys.keys.size(); ++channel) {
        EXPECT_NE(bumped_keys.keys[channel], second_keys.keys[channel])
            << "the geometry that announced re-reads its own channels";
    }
}
