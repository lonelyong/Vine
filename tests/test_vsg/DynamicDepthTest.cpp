/**
 * @brief The depth states as DYNAMIC state: what a set declares, and the slot it is delivered through.
 *
 * Depth test / write / compare are pipeline-creation state in core 1.1, which is why this backend has one
 * content ShaderSet per depth policy and has to rebuild a drawable's wrapper when the policy changes. The
 * dynamic-state form (core 1.3) moves all three out of the pipeline, so one pipeline can serve every
 * policy — and the whole of that plumbing rests on two things that are pure values, testable with no
 * device and no session:
 *
 *  * the DECLARATION a set carries (`makeDynamicDepthState`, asked back through `declaresDynamicDepth`).
 *    A bridge drives its variants with SetDepthState only when the set asked for it: a pipeline that did
 *    not declare the states dynamic keeps the values it was created with, so driving it with the command
 *    would be a command the driver silently ignores. `declaresDynamicDepth` therefore has to answer
 *    "all three", not "any of them" — a set that declared two would still bake the third, and the command
 *    writes all three.
 *  * the SLOT the command occupies (see VsgDynamicDepth.hpp). A slot is a state-stack identity: two
 *    commands of one state group that share a slot shadow each other, and the earlier one is never
 *    recorded. The first cut of this command took the default slot and silently replaced the group's
 *    pipeline bind, so the slot is pinned here as a value rather than left to a default.
 */

#include <algorithm>
#include <cstdint>

#include <gtest/gtest.h>

#include <vsg/core/ref_ptr.h>
#include <vsg/state/DynamicState.h>
#include <vsg/utils/ShaderSet.h>

#include <vine/vsg/VsgDynamicDepth.hpp>

using vine::vsg::detail::declaresDynamicDepth;
using vine::vsg::detail::kDepthStateSlot;
using vine::vsg::detail::kDynamicDepth;
using vine::vsg::detail::makeDynamicDepthState;
using vine::vsg::detail::SetDepthState;

namespace
{

/// @brief Whether @p states names @p name among its dynamic states.
bool declares(const ::vsg::DynamicState& states, VkDynamicState name)
{
    return std::find(states.dynamicStates.begin(), states.dynamicStates.end(), name) != states.dynamicStates.end();
}

} // namespace

TEST(DynamicDepthTest, TheDeclarationCoversExactlyTheThreeStatesTheCommandWrites)
{
    const auto state = makeDynamicDepthState();
    ASSERT_NE(state, nullptr);

    // The command writes all three (see SetDepthState::record), so the declaration must name all three: a
    // declaration that missed one would leave that state baked while the command set it, i.e. the command
    // would be ignored for exactly one of the three values.
    EXPECT_TRUE(declares(*state, VK_DYNAMIC_STATE_DEPTH_TEST_ENABLE));
    EXPECT_TRUE(declares(*state, VK_DYNAMIC_STATE_DEPTH_WRITE_ENABLE));
    EXPECT_TRUE(declares(*state, VK_DYNAMIC_STATE_DEPTH_COMPARE_OP));
    EXPECT_EQ(state->dynamicStates.size(), 3u) << "the declaration should ask for nothing else";
}

TEST(DynamicDepthTest, ADeclarationInASetIsFoundRegardlessOfWhatElseTheSetDeclares)
{
    auto with_depth = ::vsg::ShaderSet::create();
    auto with_other = ::vsg::ShaderSet::create();
    auto without    = ::vsg::ShaderSet::create();

    with_depth->defaultGraphicsPipelineStates.push_back(makeDynamicDepthState());
    with_other->defaultGraphicsPipelineStates.push_back(::vsg::DynamicState::create(VK_DYNAMIC_STATE_LINE_WIDTH));
    without->defaultGraphicsPipelineStates.push_back(::vsg::DynamicState::create(VK_DYNAMIC_STATE_VIEWPORT));

    EXPECT_TRUE(declaresDynamicDepth(*with_depth));
    // Other dynamic state is not this question: the answer is about the depth states.
    EXPECT_FALSE(declaresDynamicDepth(*with_other));
    EXPECT_FALSE(declaresDynamicDepth(*without));
    // A set with no dynamic state at all — the state of every set this backend built before this layer:
    // the bridge must not emit the command for it, because its pipelines keep the baked values.
    EXPECT_FALSE(declaresDynamicDepth(*::vsg::ShaderSet::create()));
}

TEST(DynamicDepthTest, APartialDeclarationIsNotAccepted)
{
    // Two of the three: SetDepthState writes all three, so such a set is not one this backend may drive —
    // the missing state would still be baked and the command would have no effect on it.
    auto partial = ::vsg::ShaderSet::create();
    partial->defaultGraphicsPipelineStates.push_back(
        ::vsg::DynamicState::create(VK_DYNAMIC_STATE_DEPTH_TEST_ENABLE, VK_DYNAMIC_STATE_DEPTH_WRITE_ENABLE));
    EXPECT_FALSE(declaresDynamicDepth(*partial));

    // The two are spread over two DynamicState objects, which is how a set assembled by hand from
    // elsewhere could present them: still partial, still refused.
    auto split = ::vsg::ShaderSet::create();
    split->defaultGraphicsPipelineStates.push_back(::vsg::DynamicState::create(VK_DYNAMIC_STATE_DEPTH_TEST_ENABLE));
    split->defaultGraphicsPipelineStates.push_back(::vsg::DynamicState::create(VK_DYNAMIC_STATE_DEPTH_WRITE_ENABLE));
    EXPECT_FALSE(declaresDynamicDepth(*split));

    // The same declaration spread over two objects IS complete: what reaches the pipeline is the union
    // of a set's DynamicState objects (vsg's mergeGraphicsPipelineStates unions the two lists), so asking
    // only one object would refuse a set the driver would have honoured.
    auto complete = ::vsg::ShaderSet::create();
    complete->defaultGraphicsPipelineStates.push_back(
        ::vsg::DynamicState::create(VK_DYNAMIC_STATE_DEPTH_TEST_ENABLE, VK_DYNAMIC_STATE_DEPTH_WRITE_ENABLE));
    complete->defaultGraphicsPipelineStates.push_back(::vsg::DynamicState::create(VK_DYNAMIC_STATE_DEPTH_COMPARE_OP));
    EXPECT_TRUE(declaresDynamicDepth(*complete));

    // ... and unrelated dynamic state alongside does not disturb the answer: the union still names all
    // three (vsg declares the dynamic viewport this way, next to a set's own list).
    auto together = ::vsg::ShaderSet::create();
    together->defaultGraphicsPipelineStates.push_back(makeDynamicDepthState());
    together->defaultGraphicsPipelineStates.push_back(
        ::vsg::DynamicState::create(VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR));
    EXPECT_TRUE(declaresDynamicDepth(*together));
}

TEST(DynamicDepthTest, TheSetsTheBackendBuildsAreTheOnesThatDeclareIt)
{
    // The three places that build content sets (VsgRenderer's window sets, VsgTargetBookkeeping's
    // per-target sets, VsgContentSlot's lazily built one) all pass kDynamicDepth; the constant is what
    // keeps them from drifting apart.
    EXPECT_TRUE(kDynamicDepth);

    auto set = ::vsg::ShaderSet::create();
    if (kDynamicDepth) {
        set->defaultGraphicsPipelineStates.push_back(makeDynamicDepthState());
    }
    EXPECT_EQ(declaresDynamicDepth(*set), kDynamicDepth);
}

TEST(DynamicDepthTest, TheCommandCarriesTheValuesItWasGiven)
{
    const auto test_only = SetDepthState::create(VK_TRUE, VK_FALSE, VK_COMPARE_OP_LESS_OR_EQUAL);
    ASSERT_NE(test_only, nullptr);

    // One command, three independent states: "test but do not write" (a shadow caster) and "write"
    // (opaque geometry) are the same pipeline under different values, which is the point of the layer.
    EXPECT_EQ(test_only->depth_test_enable, VK_TRUE);
    EXPECT_EQ(test_only->depth_write_enable, VK_FALSE);
    EXPECT_EQ(test_only->compare_op, VK_COMPARE_OP_LESS_OR_EQUAL);

    const auto no_depth = SetDepthState::create(VK_FALSE, VK_FALSE, VK_COMPARE_OP_ALWAYS);
    EXPECT_EQ(no_depth->depth_test_enable, VK_FALSE);
    EXPECT_EQ(no_depth->depth_write_enable, VK_FALSE);
    EXPECT_EQ(no_depth->compare_op, VK_COMPARE_OP_ALWAYS);
}

TEST(DynamicDepthTest, TheCommandHasItsOwnStateSlotAtTheTopOfTheStack)
{
    // Slot 0 is the pipeline bind (GraphicsPipeline.cpp), `1 + firstSet` are the descriptor binds
    // (BindDescriptorSets) and 2 is vsg's own view-dependent state / push constants. The free slot above
    // every descriptor-set index a layout can bind into is the top of vsg's fixed-size state stack
    // (STATESTACK_SIZE, 16) — see the constant's documentation for why a low free slot would not do.
    EXPECT_EQ(kDepthStateSlot, 15u);

    const auto command = SetDepthState::create(VK_TRUE, VK_TRUE, VK_COMPARE_OP_LESS);
    EXPECT_EQ(command->slot, kDepthStateSlot);

    // Not the default (StateCommand's default slot is 0, which the pipeline bind occupies): a command
    // that took that slot would silently replace the state group's pipeline bind, which is what the first
    // cut of this command did.
    EXPECT_NE(command->slot, 0u) << "the default slot belongs to the pipeline bind";
}
