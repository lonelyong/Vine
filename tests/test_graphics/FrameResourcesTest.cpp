#include <gtest/gtest.h>

#include <cstddef>
#include <thread>

#include <vine/intrusive_ptr.hpp>

#include <vine/graphics/RenderPass.hpp>
#include <vine/graphics/RenderTarget.hpp>
#include <vine/graphics/Target.hpp>

#include "FrameResources.hpp"

using vn::graphics::RenderPass;
using vn::graphics::RenderTarget;
using vn::graphics::TransientShape;
using vn::graphics::detail::FrameResources;
using vn::intrusive_ptr;

namespace
{

/// @brief A colour target at @p width x @p height, with no device behind it (the tables need none).
intrusive_ptr<RenderTarget> colourTarget(int width, int height)
{
    auto target = intrusive_ptr<RenderTarget>(new RenderTarget());
    target->attachColor(RenderTarget::ColorFormat::RGBA8);
    target->setSize(width, height);
    return target;
}

} // namespace

TEST(FrameResourcesTest, TheFrameAnswersTheBindingsItsHeadTook)
{
    // C23: the head freezes the HOST's side. A publish landing while a frame is being built answers on the next frame,
    // so the rows, the seeding and the wiring checks of one frame cannot disagree about "what does this name serve" -
    // and the host's own query still answers what it just published.
    FrameResources resources;
    auto           first  = colourTarget(64, 64);
    auto           second = colourTarget(32, 32);

    resources.publish(u8"Shadow", first);
    resources.beginFrame();
    resources.seedFromHostBindings();

    // THE OTHER THREAD'S PUBLISH, AT THE WORST MOMENT: after the head, before the frame resolves anything.
    resources.publish(u8"Shadow", second);

    EXPECT_EQ(resources.resolveForFrame(u8"Shadow"), first.get()) << "the frame answers the set its head took";
    EXPECT_TRUE(resources.wasProduced(first.get())) << "and the binding it seeded is the one the frame produced";
    EXPECT_FALSE(resources.wasProduced(second.get())) << "a binding published mid-compile is not this frame's";
    EXPECT_EQ(resources.resolve(u8"Shadow"), second.get()) << "the host's query answers what it just published";

    resources.beginFrame();
    EXPECT_EQ(resources.resolveForFrame(u8"Shadow"), second.get()) << "the next frame takes the new binding";
    EXPECT_FALSE(resources.wasProduced(first.get())) << "and starts from an empty production table";
}

TEST(FrameResourcesTest, AHostSwappingBindingsWhileAFrameCompilesNeverTearsTheTables)
{
    // THE DOORS THAT MISSED THE RULE (review 19 Z1): a host publishes, withdraws and asks for a name - and reads the
    // transients - while a frame is being built. Every table such a verb can reach is locked, so a missing lock is a
    // torn container or a lost update here; under `scripts/tsan_check.sh` it is also the report that says so.
    FrameResources resources;
    auto           target_a = colourTarget(64, 64);
    auto           target_b = colourTarget(48, 48);
    // The tables are keyed by a pass' OUTPUT NAME and the named case belongs to the compile (PlanTest); this one is
    // about the storage, so the pass may publish nothing under a name nobody asks for.
    auto pass = intrusive_ptr<RenderPass>(new RenderPass());

    constexpr int kRounds = 2000;

    std::thread host([&] {
        for (int round = 0; round < kRounds; ++round) {
            // A host has no frame hook: it swaps what it offers whenever it has something new.
            resources.publish(u8"Host", ((round % 2) == 0) ? target_a : target_b);
            (void)resources.resolve(u8"Host");
            (void)resources.resolve(u8"Shadow");
            (void)resources.transientCount();
            (void)resources.transient(u8"gbuffer");
            resources.unpublish(u8"Host");
        }
    });

    std::thread compile([&] {
        for (int round = 0; round < kRounds; ++round) {
            resources.beginFrame();
            resources.seedFromHostBindings();
            (void)resources.recordPublication(pass.get(), target_a.get());
            resources.beginTransientCompile();
            // The shape moves every other round (one replacement) and is left undeclared every seventh (one
            // retirement), so the table is written, swept and read from both sides.
            if ((round % 7) != 0) {
                const int extent = ((round % 2) == 0) ? 64 : 48;
                (void)resources.resolveTransient(u8"gbuffer", TransientShape{}, extent, extent, nullptr);
            }
            resources.retireUndeclared(nullptr);
            (void)resources.resolveForFrame(u8"Host");
            (void)resources.wasProduced(target_a.get());
        }
    });

    host.join();
    compile.join();

    EXPECT_LE(resources.transientCount(), std::size_t{ 1 }) << "one declared identity is one entry, whatever the churn";
    EXPECT_EQ(resources.resolve(u8"Host"), nullptr) << "the last publish was withdrawn";
    EXPECT_EQ(resources.hostBindingNames().size(), std::size_t{ 0 }) << "and nothing of the host's is left behind";
}
