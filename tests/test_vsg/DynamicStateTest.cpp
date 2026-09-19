/**
 * @brief The state a StateNode configures, delivered dynamically: the declaration, the values, the slot.
 *
 * Depth test / write / compare, cull mode, front face and primitive topology are pipeline-creation state, so
 * a backend that bakes them needs one pipeline per combination. Core Vulkan 1.3 makes all four dynamic with
 * nothing to enable, so one command carries them instead (see VsgDynamicState.hpp) and there is no session
 * flag: the declaration is part of every content set the backend builds and every variant emits the command.
 * Polygon mode and the colour blend state are deliberately NOT in it (they are not core-1.3 state — see
 * VsgDynamicState.hpp), which is pinned below so the boundary moves only on purpose.
 *
 * Three things can only go wrong silently here, so they are pinned device-free:
 *
 *  * a state the command WRITES but the DECLARATION omits is a value the driver keeps taking from the
 *    create-info — half the command would do nothing, with no error anywhere;
 *  * a field of the command that the mapping (makeDynamicState) forgets has the same effect, which is why
 *    the test below compares the command against the mapped objects field by field;
 *  * a command whose slot is shared with the group's other state commands is never recorded at all (see
 *    kDynamicStateSlot), which is how the first version of this command replaced the pipeline bind.
 */

#include <algorithm>
#include <cstdint>
#include <iterator>
#include <vector>

#include <gtest/gtest.h>

#include <vsg/state/DynamicState.h>

#include <vine/graphics/RenderCommand.hpp>
#include <vine/graphics/StateNode.hpp>

#include <vine/vsg/RenderStateMapper.hpp>
#include <vine/vsg/VsgDynamicState.hpp>

#include "TestContentSet.hpp"

using vine::vsg::detail::kDynamicStateSlot;
using vine::vsg::detail::makeDynamicStateDeclaration;
using vine::vsg::detail::SetDynamicState;
using vine::vsg::makeDynamicState;
using vine::vsg::makeRenderStateObjects;

namespace
{

/// @brief The states the command's record() writes, in one place (see the test that pins them).
constexpr VkDynamicState kRecordedStates[] = {
    VK_DYNAMIC_STATE_DEPTH_TEST_ENABLE, VK_DYNAMIC_STATE_DEPTH_WRITE_ENABLE, VK_DYNAMIC_STATE_DEPTH_COMPARE_OP,
    VK_DYNAMIC_STATE_CULL_MODE,         VK_DYNAMIC_STATE_FRONT_FACE,         VK_DYNAMIC_STATE_PRIMITIVE_TOPOLOGY};

/// @brief Whether @p states names @p name among its dynamic states.
bool declares(const ::vsg::DynamicState& states, VkDynamicState name)
{
    return std::find(states.dynamicStates.begin(), states.dynamicStates.end(), name) != states.dynamicStates.end();
}

/// @brief Whether @p shader_set declares every state the command writes.
///
/// The question is about the UNION of the set's DynamicState objects, which is what reaches the pipeline:
/// vsg merges a pipeline's states by type and unions the lists of two DynamicState objects
/// (mergeGraphicsPipelineStates). Asking one object would refuse a set the driver honours.
bool setDeclaresTheLayer(const ::vsg::ShaderSet& shader_set)
{
    bool seen[std::size(kRecordedStates)] = {};
    for (const auto& state : shader_set.defaultGraphicsPipelineStates) {
        const auto dynamic = state.cast<::vsg::DynamicState>();
        if (dynamic == nullptr) {
            continue;
        }
        for (std::size_t i = 0; i < std::size(kRecordedStates); ++i) {
            seen[i] = seen[i] || declares(*dynamic, kRecordedStates[i]);
        }
    }
    return std::all_of(std::begin(seen), std::end(seen), [](bool found) { return found; });
}

} // namespace

TEST(DynamicStateTest, TheDeclarationNamesExactlyTheStatesTheCommandRecords)
{
    const auto declaration = makeDynamicStateDeclaration();
    ASSERT_NE(declaration, nullptr);

    // One name per call record() makes (see VsgDynamicState.cpp). A missing name means the driver keeps
    // that state's create-info value, i.e. the command silently loses one of the values it was given.
    for (const VkDynamicState name : kRecordedStates) {
        EXPECT_TRUE(declares(*declaration, name));
    }
    EXPECT_EQ(declaration->dynamicStates.size(), std::size(kRecordedStates)) << "nothing else should be declared";

    // The two states left baked on purpose (they are not core-1.3 state: see VsgDynamicState.hpp). If one
    // of them ever joins the command, its declaration has to join this list too — which is what this pins.
    EXPECT_FALSE(declares(*declaration, VK_DYNAMIC_STATE_POLYGON_MODE_EXT));
    EXPECT_FALSE(declares(*declaration, VK_DYNAMIC_STATE_COLOR_BLEND_ENABLE_EXT));
    EXPECT_FALSE(declares(*declaration, VK_DYNAMIC_STATE_COLOR_BLEND_EQUATION_EXT));
}

TEST(DynamicStateTest, EveryContentSetTheBackendBuildsDeclaresTheLayer)
{
    // The mandatory half of "no session flag": whatever shape a set is built for, it declares the state the
    // command delivers. Built here for the three shapes the backend asks for — a presenting target, a
    // depth-only pass and an MRT pass — through the same factory the renderer uses.
    struct Shape
    {
        bool depth_test;
        bool depth_write;
        int  color_count;
    };
    for (const Shape shape : { Shape{ true, true, 1 }, Shape{ true, false, 1 }, Shape{ false, false, 0 },
                               Shape{ true, true, 3 } }) {
        const auto set = vine::vsg::detail::makeContentShaderSet(vine::graphics::forwardProgram(),
                                                                 VkExtent2D{ 640, 360 }, shape.depth_test,
                                                                 shape.depth_write, shape.color_count);
        ASSERT_NE(set, nullptr) << shape.color_count;
        EXPECT_TRUE(setDeclaresTheLayer(*set)) << shape.color_count;
    }
}

TEST(DynamicStateTest, EveryFieldOfTheMappedStateReachesTheCommand)
{
    // A state that differs from the defaults in every movable item at once, so a field the mapping forgot
    // shows up as a mismatch rather than as "the defaults happened to agree".
    vine::graphics::ResolvedRenderState state;
    state.depth    = vine::graphics::DepthState{ /*enabled*/ false, /*write*/ false, vine::graphics::CompareOp::Never };
    state.cullMode = vine::graphics::CullMode::Front;
    state.topology = vine::graphics::Topology::Points;

    const auto objects = makeRenderStateObjects(state);
    const auto command = makeDynamicState(objects);
    ASSERT_NE(command, nullptr);

    // Compared against the mapped OBJECTS, not against the resolved state: those objects are what the
    // pipeline create-info gets, so agreement with them is what makes the command behaviour-neutral.
    EXPECT_EQ(command->depth_test_enable, objects.depthStencil->depthTestEnable);
    EXPECT_EQ(command->depth_write_enable, objects.depthStencil->depthWriteEnable);
    EXPECT_EQ(command->compare_op, objects.depthStencil->depthCompareOp);

    EXPECT_EQ(command->cull_mode, objects.rasterization->cullMode);
    EXPECT_EQ(command->front_face, objects.rasterization->frontFace);

    EXPECT_EQ(command->topology, objects.inputAssembly->topology);

    // The concrete values, so the mapping is checked against Vulkan rather than against itself: the SDK
    // compares distance (Less = the nearer fragment wins) and this backend renders reverse-Z, so "closer"
    // is a LARGER depth.
    EXPECT_EQ(command->depth_test_enable, VK_FALSE);
    EXPECT_EQ(command->depth_write_enable, VK_FALSE);
    EXPECT_EQ(command->compare_op, VK_COMPARE_OP_NEVER);
    EXPECT_EQ(command->cull_mode, VK_CULL_MODE_FRONT_BIT);
    EXPECT_EQ(command->front_face, VK_FRONT_FACE_CLOCKWISE);
    EXPECT_EQ(command->topology, VK_PRIMITIVE_TOPOLOGY_POINT_LIST);
}

TEST(DynamicStateTest, TheDefaultResolvedStateIsTheBackendsDefaultPipelineState)
{
    // A scene without StateNodes must reach the same pipeline state as before the layer existed: reverse-Z
    // depth test/write on, no culling, clockwise front faces, triangle list. Pinned as values because "the
    // defaults did not move" is the reason nothing about a plain scene changed.
    const auto command = makeDynamicState(makeRenderStateObjects(vine::graphics::ResolvedRenderState{}));
    ASSERT_NE(command, nullptr);

    EXPECT_EQ(command->depth_test_enable, VK_TRUE);
    EXPECT_EQ(command->depth_write_enable, VK_TRUE);
    EXPECT_EQ(command->compare_op, VK_COMPARE_OP_GREATER);
    EXPECT_EQ(command->cull_mode, VK_CULL_MODE_NONE);
    EXPECT_EQ(command->front_face, VK_FRONT_FACE_CLOCKWISE);
    EXPECT_EQ(command->topology, VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST);
}

TEST(DynamicStateTest, TheCommandHasItsOwnStateSlotAtTheTopOfTheStack)
{
    // Slot 0 is the pipeline bind (GraphicsPipeline.cpp), `1 + firstSet` are the descriptor binds
    // (BindDescriptorSets) and 2 is vsg's own view-dependent state / push constants. The free slot above
    // every descriptor-set index a layout can bind into is the top of vsg's fixed-size state stack
    // (STATESTACK_SIZE, 16) — see the constant's documentation for why a low free slot would not do.
    EXPECT_EQ(kDynamicStateSlot, 15u);

    const auto command = makeDynamicState(makeRenderStateObjects(vine::graphics::ResolvedRenderState{}));
    EXPECT_EQ(command->slot, kDynamicStateSlot);

    // Not the default (StateCommand's default slot is 0, which the pipeline bind occupies): a command that
    // took that slot would silently replace the state group's pipeline bind — which is what the first
    // version of this command did.
    EXPECT_NE(command->slot, 0u) << "the default slot belongs to the pipeline bind";
}
