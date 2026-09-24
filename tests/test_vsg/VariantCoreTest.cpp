/**
 * @brief The identity layer's device-free half: the variant pool and the per-pass state registry
 * (see `.ai/design/vsg-reimplementation.md` D3 and milestone M2b).
 *
 * What these cases pin:
 *
 *   * one identity, one compile: two lookups that agree on program, revision, layout and compatibility
 *     share a variant, and the caller is told which of the two happened;
 *   * DYNAMIC state never reaches the pool - the churn phases may change cull, polygon, blend, depth policy
 *     and topology as often as they like, and the compiled variant count does not move (the two historical
 *     failure families in this backend are exactly this mistake in two directions);
 *   * a pass' registry answers two costs separately - "bind another variant" and "issue the dynamic block" -
 *     so a phase can tell a compile from a set command after the fact;
 *   * variants are shared BETWEEN passes while each pass keeps its own binding, which is what the API's
 *     per-view pipeline compilation forces;
 *   * eviction drops a LOOKUP, not an object, and variant ids are never reused (a stale id can never alias a
 *     newer variant).
 *
 * Every case is device-free by construction: nothing here dereferences a GPU object or includes `vsg::`.
 */

#include <gtest/gtest.h>

#include <cstdint>

#include <vine/graphics/DepthMode.hpp>
#include <vine/graphics/RenderTarget.hpp>
#include <vine/graphics/StateNode.hpp>
#include <vine/vsg/core/Keys.hpp>
#include <vine/vsg/core/StateRegistry.hpp>
#include <vine/vsg/core/VariantPool.hpp>

using vn::graphics::BlendState;
using vn::graphics::CullMode;
using vn::graphics::DepthMode;
using vn::graphics::PolygonMode;
using vn::graphics::RenderTarget;
using vn::graphics::Topology;
using vn::vsg::core::DynamicState;
using vn::vsg::core::PipelineKey;
using vn::vsg::core::StateRegistry;
using vn::vsg::core::VariantPool;

namespace
{

int program_a = 0;
int program_b = 0;
int program_c = 0;

/// @brief A content identity with all three layers' worth of inputs filled in.
PipelineKey contentKey(const void* program, std::uint64_t revision)
{
    PipelineKey key;
    key.program                        = program;
    key.revision                       = revision;
    key.vertex_layout.canonical_mask   = 0xFU;
    key.vertex_layout.custom_locations = {3U};
    key.compatibility.color_formats    = {RenderTarget::ColorFormat::RGBA8};
    key.compatibility.depth_format     = RenderTarget::DepthFormat::D24;
    key.compatibility.samples          = 1U;
    return key;
}

}  // namespace

// --- the pool -------------------------------------------------------------------------------------------

TEST(CoreVariantPoolTest, TheSameIdentityIsCompiledOnce)
{
    VariantPool pool;
    const PipelineKey key = contentKey(&program_a, 1);

    const VariantPool::Lookup first  = pool.acquire(key);
    const VariantPool::Lookup second = pool.acquire(key);

    EXPECT_EQ(first.action, VariantPool::Action::Created) << "nothing had this identity";
    EXPECT_EQ(second.action, VariantPool::Action::Reused) << "the same identity is one variant, not two";
    EXPECT_EQ(first.id, second.id);
    EXPECT_EQ(pool.created(), 1U);
    EXPECT_EQ(pool.reused(), 1U);
    EXPECT_EQ(pool.variants(), 1U);
}

TEST(CoreVariantPoolTest, ARevisionChangeIsANewVariantAndTheScopeEndsWithClear)
{
    VariantPool pool;
    const VariantPool::Lookup first  = pool.acquire(contentKey(&program_a, 1));
    const VariantPool::Lookup second = pool.acquire(contentKey(&program_a, 2));

    EXPECT_EQ(second.action, VariantPool::Action::Created) << "the program's revision is identity";
    EXPECT_NE(first.id, second.id);
    EXPECT_EQ(pool.variants(), 2U);

    pool.clear();
    EXPECT_EQ(pool.variants(), 0U) << "the scope died and every variant went with it";
    EXPECT_FALSE(pool.contains(first.id));
    EXPECT_EQ(pool.created(), 2U) << "the counters record what happened, not what is left";
}

TEST(CoreVariantPoolTest, TheCompatibilityHalfOfTheKeyMatters)
{
    VariantPool       pool;
    const PipelineKey base = contentKey(&program_a, 1);

    PipelineKey other_format = base;
    other_format.compatibility.color_formats = {RenderTarget::ColorFormat::RGBA16F};
    PipelineKey other_depth = base;
    other_depth.compatibility.depth_format = RenderTarget::DepthFormat::D32F;
    PipelineKey sampled_depth = base;
    sampled_depth.depth_sampleable = true;
    PipelineKey sampled_depth_count = base;
    sampled_depth_count.sampled_depth_count = 1U;
    PipelineKey layout = base;
    layout.vertex_layout.canonical_mask = 0x7U;

    EXPECT_EQ(pool.acquire(base).action, VariantPool::Action::Created);
    EXPECT_EQ(pool.acquire(other_format).action, VariantPool::Action::Created);
    EXPECT_EQ(pool.acquire(other_depth).action, VariantPool::Action::Created);
    EXPECT_EQ(pool.acquire(sampled_depth).action, VariantPool::Action::Created);
    EXPECT_EQ(pool.acquire(sampled_depth_count).action, VariantPool::Action::Created);
    EXPECT_EQ(pool.acquire(layout).action, VariantPool::Action::Created);
    EXPECT_EQ(pool.variants(), 6U) << "every compatibility change is a different pipeline";

    EXPECT_EQ(pool.acquire(other_format).action, VariantPool::Action::Reused)
        << "and each of them is still one variant";
}

TEST(CoreVariantPoolTest, TheCapacityBoundEvictsTheOldestLookupAndStaleIdsStayStale)
{
    VariantPool pool(2);
    const VariantPool::Lookup first  = pool.acquire(contentKey(&program_a, 1));
    const VariantPool::Lookup second = pool.acquire(contentKey(&program_b, 1));
    const VariantPool::Lookup third  = pool.acquire(contentKey(&program_c, 1));

    EXPECT_EQ(pool.evictions(), 1U) << "the bound was reached and the oldest lookup left";
    EXPECT_EQ(pool.variants(), 2U);
    EXPECT_FALSE(pool.contains(first.id))
        << "the lookup left the map; whether the holder's object may die is the retirement queue's question";
    EXPECT_TRUE(pool.contains(second.id));
    EXPECT_TRUE(pool.contains(third.id));

    const VariantPool::Lookup again = pool.acquire(contentKey(&program_a, 1));
    EXPECT_EQ(again.action, VariantPool::Action::Created) << "the pool has to compile it again";
    EXPECT_NE(again.id, first.id) << "ids are never reused, so a stale id cannot alias a newer variant";
}

TEST(CoreVariantPoolTest, AnEntryKeepsItsKeyAfterTheCallersKeyIsGone)
{
    VariantPool pool;
    std::uint64_t id = 0;
    {
        PipelineKey key = contentKey(&program_a, 9);
        key.vertex_layout.custom_locations = {3U, 7U};
        id = pool.acquire(key).id;
    }

    const PipelineKey* kept = pool.keyOf(id);
    ASSERT_NE(kept, nullptr);
    EXPECT_EQ(kept->program, static_cast<const void*>(&program_a));
    EXPECT_EQ(kept->revision, 9U);
    ASSERT_EQ(kept->vertex_layout.custom_locations.size(), 2U);
    EXPECT_EQ(kept->vertex_layout.custom_locations[1], 7U);
    EXPECT_EQ(kept->compatibility.depth_format, RenderTarget::DepthFormat::D24);
    EXPECT_EQ(pool.keyOf(9999U), nullptr) << "an id the pool does not know has no key";
}

// --- the per-pass registry ------------------------------------------------------------------------------

TEST(CoreStateRegistryTest, TheFirstResolutionCompilesAndIssuesEverything)
{
    VariantPool   pool;
    StateRegistry registry(pool);

    const StateRegistry::Resolution resolution = registry.resolve(contentKey(&program_a, 1), DynamicState{});

    EXPECT_EQ(pool.created(), 1U);
    EXPECT_TRUE(resolution.variant_switched) << "the pass has bound nothing yet";
    EXPECT_TRUE(resolution.dynamic_issued) << "and has issued no dynamic values in this recording";
    EXPECT_EQ(registry.dynamic_issued(), 1U);
    EXPECT_EQ(registry.dynamic_skipped(), 0U);
    EXPECT_EQ(registry.currentVariant(), resolution.variant);
}

TEST(CoreStateRegistryTest, DynamicStateChurnNeverCompilesAgain)
{
    VariantPool   pool;
    StateRegistry registry(pool);
    const PipelineKey key = contentKey(&program_a, 1);

    (void)registry.resolve(key, DynamicState{});
    for (int step = 0; step < 20; ++step) {
        DynamicState state;
        state.cull_mode    = (step % 2 == 0) ? CullMode::Back : CullMode::Front;
        state.polygon_mode = (step % 3 == 0) ? PolygonMode::Line : PolygonMode::Fill;
        state.topology     = (step % 5 == 0) ? Topology::Points : Topology::Triangles;
        state.depth        = (step % 2 == 0) ? DepthMode::TestOnly : DepthMode::TestAndWrite;
        state.blend.enabled = (step % 2) == 0;

        const StateRegistry::Resolution resolution = registry.resolve(key, state);
        EXPECT_FALSE(resolution.variant_switched) << "step " << step << " changed only the dynamic half";
        EXPECT_TRUE(resolution.dynamic_issued) << "step " << step << " does have to issue its values";
    }

    EXPECT_EQ(pool.created(), 1U) << "the identity never moved: one compile for the whole churn";
    EXPECT_EQ(pool.variants(), 1U);
    EXPECT_EQ(registry.variant_switches(), 1U);
    EXPECT_EQ(registry.dynamic_issued(), 21U) << "every churn costs a set command, never a compile";
}

TEST(CoreStateRegistryTest, AValueEqualToWhatWasIssuedIsNotReissued)
{
    VariantPool   pool;
    StateRegistry registry(pool);

    DynamicState state;
    state.cull_mode = CullMode::Back;

    const StateRegistry::Resolution first  = registry.resolve(contentKey(&program_a, 1), state);
    const StateRegistry::Resolution second = registry.resolve(contentKey(&program_a, 1), state);

    EXPECT_TRUE(first.dynamic_issued);
    EXPECT_FALSE(second.dynamic_issued) << "the recording already holds these values";
    EXPECT_FALSE(second.variant_switched);
    EXPECT_EQ(registry.dynamic_issued(), 1U);
    EXPECT_EQ(registry.dynamic_skipped(), 1U);
    EXPECT_EQ(first.variant, second.variant);
}

TEST(CoreStateRegistryTest, AVariantSwitchDoesNotReissueAnUnchangedDynamicBlock)
{
    VariantPool   pool;
    StateRegistry registry(pool);
    const DynamicState state;

    (void)registry.resolve(contentKey(&program_a, 1), state);
    const StateRegistry::Resolution switched = registry.resolve(contentKey(&program_b, 1), state);

    EXPECT_TRUE(switched.variant_switched) << "a different program is a different variant";
    EXPECT_FALSE(switched.dynamic_issued) << "the dynamic values in this recording did not change";
    EXPECT_EQ(pool.created(), 2U);
}

TEST(CoreStateRegistryTest, TwoPassesShareVariantsAndKeepTheirOwnBindings)
{
    VariantPool   pool;
    StateRegistry first_pass(pool);
    StateRegistry second_pass(pool);
    const PipelineKey key = contentKey(&program_a, 3);

    const StateRegistry::Resolution first  = first_pass.resolve(key, DynamicState{});
    const StateRegistry::Resolution second = second_pass.resolve(key, DynamicState{});

    EXPECT_EQ(pool.created(), 1U) << "the identity is the same, so the compile happens once";
    EXPECT_EQ(pool.reused(), 1U);
    EXPECT_EQ(first.variant, second.variant);
    EXPECT_TRUE(second.variant_switched) << "the second pass has bound nothing yet";
    EXPECT_TRUE(second.dynamic_issued) << "and its own recording holds no values";
}

TEST(CoreStateRegistryTest, TheSampledInputSetIsBoundOncePerPass)
{
    // The pass' inputs are a property of the PASS, so the set that carries them costs one bind however many
    // draws read it - while a different set (a new pass, a rebuilt frame) has to be bound again.
    VariantPool   pool;
    StateRegistry registry(pool);
    const PipelineKey key = contentKey(&program_a, 1);

    int first_set  = 0;
    int second_set = 0;

    const StateRegistry::Resolution first = registry.resolve(key, DynamicState{}, &first_set);
    EXPECT_TRUE(first.inputs_issued) << "the pass has bound no sampled-input set yet";
    EXPECT_TRUE(first.variant_switched) << "and the input set does not replace the pipeline bind";

    const StateRegistry::Resolution repeated = registry.resolve(key, DynamicState{}, &first_set);
    EXPECT_FALSE(repeated.inputs_issued) << "the same set is still the one bound";
    EXPECT_EQ(registry.inputs_issued(), 1U);
    EXPECT_EQ(registry.inputs_skipped(), 1U);

    const StateRegistry::Resolution another = registry.resolve(key, DynamicState{}, &second_set);
    EXPECT_TRUE(another.inputs_issued) << "a different set has to reach the recording";

    const StateRegistry::Resolution none = registry.resolve(key, DynamicState{}, nullptr);
    EXPECT_FALSE(none.inputs_issued) << "a pass with no inputs binds nothing - there is nothing to rebind";

    // A fresh recording holds no state at all: the next resolution issues the set again.
    registry.reset();
    const StateRegistry::Resolution after_reset = registry.resolve(key, DynamicState{}, &first_set);
    EXPECT_TRUE(after_reset.inputs_issued);
    EXPECT_TRUE(after_reset.variant_switched);
    EXPECT_EQ(registry.inputs_issued(), 3U);
}

TEST(CoreStateRegistryTest, ANewRecordingIssuesEverythingAgain)
{
    VariantPool   pool;
    StateRegistry registry(pool);
    const PipelineKey key = contentKey(&program_a, 1);

    (void)registry.resolve(key, DynamicState{});
    registry.reset();
    const StateRegistry::Resolution after = registry.resolve(key, DynamicState{});

    EXPECT_TRUE(after.variant_switched) << "nothing is bound in the new recording";
    EXPECT_TRUE(after.dynamic_issued) << "and it holds no values either";
    EXPECT_EQ(registry.dynamic_issued(), 2U);
    EXPECT_EQ(registry.currentVariant(), after.variant);
}
