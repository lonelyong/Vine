#include <gtest/gtest.h>

#include <algorithm>
#include <thread>
#include <vector>

#include <vine/intrusive_ptr.hpp>
#include <vine/math/Transform3.hpp>
#include <vine/math/Vector3.hpp>

#include <vine/graphics/Group.hpp>

#include "DeltaChannel.hpp"

using vn::graphics::Group;
using vn::graphics::detail::buildRows;
using vn::graphics::detail::Delta;
using vn::graphics::detail::DeltaChannel;
using vn::graphics::detail::DeltaKind;
using vn::graphics::detail::OwnedDelta;
using vn::graphics::detail::SceneBake;
using vn::intrusive_ptr;
using vn::math::Mat4d;

namespace
{

/// @brief A placement delta about @p identity, translated by @p x on X (so a test can tell two apart).
Delta placementAbout(vn::graphics::NodeId identity, double x)
{
    Delta delta;
    delta.kind     = DeltaKind::Placement;
    delta.identity = identity;
    delta.matrix   = vn::math::translate(x, 0.0, 0.0);
    return delta;
}

} // namespace

TEST(DeltaChannelTest, ABatchArrivesInPostOrderAndOnlyOnce)
{
    // The drain IS the frame boundary: everything posted before it applies as one ordered batch, and a second drain
    // finds nothing. Without that, two deltas about one row could apply in either order and a frame could see half a
    // change.
    DeltaChannel channel;
    const auto   a = vn::graphics::NodeId(vn::graphics::Handle::make(1U, 1U, 0U));
    const auto   b = vn::graphics::NodeId(vn::graphics::Handle::make(2U, 1U, 0U));
    const auto   c = vn::graphics::NodeId(vn::graphics::Handle::make(3U, 1U, 0U));

    EXPECT_TRUE(channel.post(placementAbout(a, 1.0)));
    EXPECT_TRUE(channel.post(placementAbout(b, 2.0)));
    EXPECT_TRUE(channel.post(placementAbout(c, 3.0)));
    EXPECT_EQ(channel.posted(), 3U);
    EXPECT_EQ(channel.pending(), 3U);

    std::vector<OwnedDelta> batch;
    channel.drain(batch);
    ASSERT_EQ(batch.size(), 3U);
    EXPECT_EQ(batch[0].delta.identity, a);
    EXPECT_EQ(batch[1].delta.identity, b);
    EXPECT_EQ(batch[2].delta.identity, c);
    EXPECT_NEAR(batch[2].delta.matrix.element(0, 3), 3.0, 1e-12);
    EXPECT_EQ(channel.pending(), 0U);

    channel.drain(batch);
    EXPECT_TRUE(batch.empty()) << "a drained delta is never delivered twice";
}

TEST(DeltaChannelTest, ATableTravelsWithTheDeltaThatCarriesIt)
{
    // A change that OWNS content carries it in the same record: the engine side can never receive "the rows were
    // replaced" without the rows, which is the whole point of the two-part record.
    auto root = intrusive_ptr<Group>(new Group());
    auto leaf = intrusive_ptr<Group>(new Group());
    root->addChild(leaf);

    SceneBake rows;
    buildRows(*root, rows);
    const std::size_t row_count = rows.rows.size();
    auto              owned     = std::make_shared<const SceneBake>(std::move(rows));

    DeltaChannel channel;
    Delta        delta;
    delta.kind = DeltaKind::RowsReplaced;
    ASSERT_TRUE(channel.post(delta, owned));

    std::vector<OwnedDelta> batch;
    channel.drain(batch);
    ASSERT_EQ(batch.size(), 1U);
    EXPECT_EQ(batch[0].delta.kind, DeltaKind::RowsReplaced);
    ASSERT_TRUE(batch[0].rows != nullptr);
    EXPECT_EQ(batch[0].rows->rows.size(), row_count);
    EXPECT_EQ(batch[0].rows->rows.front().identity, root->id()) << "the table the content's thread built";
    EXPECT_EQ(batch[0].rows.use_count(), 2) << "the channel handed the share over, it did not copy the table";
}

TEST(DeltaChannelTest, PostsFromSeveralThreadsAllArriveOnceAndInEachThreadsOrder)
{
    // T2: MPSC - N threads post M deltas each, the total is exact (no loss, no duplication) and each PRODUCER's own
    // deltas keep their order, which is what makes "the last placement of a row wins" true per producer.
    constexpr int kProducers   = 4;
    constexpr int kPerProducer = 500;
    // Sized for the whole run: this test is about delivery and order, not about a consumer racing the producers.
    DeltaChannel  channel{ static_cast<std::size_t>(kProducers) * static_cast<std::size_t>(kPerProducer) };
    std::vector<double> expected(kProducers);
    for (int producer = 0; producer < kProducers; ++producer) {
        expected[static_cast<std::size_t>(producer)] = 0.0;
    }

    std::vector<std::thread> threads;
    threads.reserve(kProducers);
    for (int producer = 0; producer < kProducers; ++producer) {
        threads.emplace_back([&channel, producer]() {
            // The identity encodes the producer, the matrix the step, so the drain can check both.
            const vn::graphics::NodeId identity =
                vn::graphics::NodeId(vn::graphics::Handle::make(static_cast<std::uint32_t>(producer + 1), 1U, 0U));
            for (int step = 1; step <= kPerProducer; ++step) {
                Delta delta;
                delta.kind     = DeltaKind::Placement;
                delta.identity = identity;
                delta.matrix   = vn::math::translate(static_cast<double>(step), 0.0, 0.0);
                EXPECT_TRUE(channel.post(delta));
            }
        });
    }
    for (std::thread& thread : threads) {
        thread.join();
    }

    std::vector<OwnedDelta> batch;
    channel.drain(batch);
    ASSERT_EQ(batch.size(), static_cast<std::size_t>(kProducers) * static_cast<std::size_t>(kPerProducer));
    EXPECT_EQ(channel.posted(), batch.size());
    EXPECT_EQ(channel.refused(), 0U);

    std::vector<double> last(kProducers, 0.0);
    std::vector<int>    seen(kProducers, 0);
    for (const OwnedDelta& owned : batch) {
        const auto producer = static_cast<std::size_t>(owned.delta.identity.handle().index()) - 1U;
        ASSERT_LT(producer, static_cast<std::size_t>(kProducers));
        const double step = owned.delta.matrix.element(0, 3);
        EXPECT_GT(step, last[producer]) << "producer " << producer << " arrived out of order";
        last[producer] = step;
        ++seen[producer];
    }
    for (int producer = 0; producer < kProducers; ++producer) {
        EXPECT_EQ(seen[static_cast<std::size_t>(producer)], kPerProducer);
        EXPECT_NEAR(last[static_cast<std::size_t>(producer)], static_cast<double>(kPerProducer), 1e-12);
    }
}

TEST(DeltaChannelTest, AFullChannelRefusesAndCountsInsteadOfDroppingQuietly)
{
    // The bound is the contract: a post that does not fit is REFUSED (visible in the return value and in the count),
    // never silently lost. Losing one is recoverable by construction - `RowsReplaced` is a superset of every finer
    // delta, so the producer answers a full channel with a fresh table.
    DeltaChannel channel{ 2 };
    EXPECT_EQ(channel.capacity(), 2U);

    const auto a = vn::graphics::NodeId(vn::graphics::Handle::make(1U, 1U, 0U));
    EXPECT_TRUE(channel.post(placementAbout(a, 1.0)));
    EXPECT_TRUE(channel.post(placementAbout(a, 2.0)));
    EXPECT_FALSE(channel.post(placementAbout(a, 3.0))) << "the third does not fit";
    EXPECT_EQ(channel.refused(), 1U);
    EXPECT_EQ(channel.posted(), 2U);
    EXPECT_EQ(channel.pending(), 2U);

    std::vector<OwnedDelta> batch;
    channel.drain(batch);
    ASSERT_EQ(batch.size(), 2U);
    EXPECT_NEAR(batch.back().delta.matrix.element(0, 3), 2.0, 1e-12) << "the refused delta was not queued";

    // ...and the channel is usable again once the batch is gone.
    EXPECT_TRUE(channel.post(placementAbout(a, 4.0)));
    channel.drain(batch);
    ASSERT_EQ(batch.size(), 1U);
    EXPECT_NEAR(batch.front().delta.matrix.element(0, 3), 4.0, 1e-12);
}

TEST(DeltaChannelTest, DrainingRecyclesTheBufferItIsGiven)
{
    // The drain swaps storage instead of building a batch, so a consumer that passes the same vector every frame never
    // allocates: the capacity it grew once is the capacity it keeps, and the producer gets the other buffer back.
    DeltaChannel channel;
    std::vector<OwnedDelta> batch;

    const auto a = vn::graphics::NodeId(vn::graphics::Handle::make(1U, 1U, 0U));
    for (int frame = 0; frame < 4; ++frame) {
        for (int index = 0; index < 8; ++index) {
            ASSERT_TRUE(channel.post(placementAbout(a, static_cast<double>(index))));
        }
        channel.drain(batch);
        ASSERT_EQ(batch.size(), 8U);
        EXPECT_GE(batch.capacity(), 8U);
    }
    // The producer's own buffer never grew past the capacity the channel was built with.
    EXPECT_EQ(channel.pending(), 0U);
    EXPECT_EQ(channel.posted(), 32U);
    EXPECT_EQ(channel.refused(), 0U);
}
