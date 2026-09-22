/**
 * @brief The state a StateNode configures, delivered dynamically: the declaration, the values, the slot.
 *
 * Everything a StateNode can set — depth test / write / compare, cull mode, front face, polygon mode,
 * primitive topology and the colour blend enable/factors — is pipeline-creation state, so a backend that
 * bakes any of it needs one pipeline per combination. This backend delivers all of it dynamically, so one
 * command carries it instead (see VsgDynamicState.hpp) and there is no session flag: the declaration is part
 * of every content set the backend builds and every variant emits the command. Four of the nine states are
 * core 1.3 with nothing to enable; polygon mode and the blend pair need two extensions and their feature
 * bits, which makeWindowTraits requests.
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
#include <vine/vsg/api/StateCommands.hpp>
#include <vine/vsg/core/Keys.hpp>
#include <vine/vsg/VsgDynamicState.hpp>
#include <vine/vsg/VsgSceneRules.hpp>

#include "TestContentSet.hpp"

using vine::vsg::detail::applyOpaqueBlendForAttachments;
using vine::vsg::detail::DynamicStateEntryPoints;
using vine::vsg::detail::kDynamicStateSlot;
using vine::vsg::detail::kBakedBlendEnable;
using vine::vsg::detail::kBakedBlendFactor;
using vine::vsg::detail::kBakedCompareOp;
using vine::vsg::detail::kBakedCullMode;
using vine::vsg::detail::kBakedDepthTestEnable;
using vine::vsg::detail::kBakedDepthWriteEnable;
using vine::vsg::detail::kBakedFrontFace;
using vine::vsg::detail::kBakedPolygonMode;
using vine::vsg::detail::kBakedTopology;
using vine::vsg::detail::kMaxDynamicAttachments;
using vine::vsg::detail::makeDynamicStateDeclaration;
using vine::vsg::detail::SetDynamicState;
using vine::vsg::makeDynamicStateCommand;
using vine::vsg::makeDynamicState;
using vine::vsg::makePipelineStateObjects;
using vine::vsg::makeRenderStateObjects;

namespace
{

/// @brief The states the command's record() writes, in one place (see the test that pins them).
constexpr VkDynamicState kRecordedStates[] = {
    VK_DYNAMIC_STATE_DEPTH_TEST_ENABLE,        VK_DYNAMIC_STATE_DEPTH_WRITE_ENABLE, VK_DYNAMIC_STATE_DEPTH_COMPARE_OP,
    VK_DYNAMIC_STATE_CULL_MODE,                VK_DYNAMIC_STATE_FRONT_FACE,         VK_DYNAMIC_STATE_POLYGON_MODE_EXT,
    VK_DYNAMIC_STATE_PRIMITIVE_TOPOLOGY,       VK_DYNAMIC_STATE_COLOR_BLEND_ENABLE_EXT,
    VK_DYNAMIC_STATE_COLOR_BLEND_EQUATION_EXT};

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

    // Nothing of the resolved state is left baked, so nothing else should be declared either — the list
    // above is the whole surface a StateNode can move.
}

TEST(DynamicStateTest, EveryContentSetTheBackendBuildsDeclaresTheLayer)
{
    // The mandatory half of "no session flag": whatever shape a set is built for, it declares the state the
    // command delivers. Built here for the three attachment shapes the backend asks for — a single colour
    // attachment, a depth-only pass and an MRT pass — through the same factory the renderer uses.
    for (const int color_count : { 1, 0, 3 }) {
        const auto set =
            vine::vsg::detail::makeContentShaderSet(vine::graphics::forwardProgram(), VkExtent2D{ 640, 360 }, color_count);
        ASSERT_NE(set, nullptr) << color_count;
        EXPECT_TRUE(setDeclaresTheLayer(*set)) << color_count;
    }
}

TEST(DynamicStateTest, EveryFieldOfTheMappedStateReachesTheCommand)
{
    // A state that differs from the defaults in every movable item at once, so a field the mapping forgot
    // shows up as a mismatch rather than as "the defaults happened to agree".
    vine::graphics::ResolvedRenderState state;
    state.depth       = vine::graphics::DepthState{ /*enabled*/ false, /*write*/ false, vine::graphics::CompareOp::Never };
    state.cullMode    = vine::graphics::CullMode::Front;
    state.polygonMode = vine::graphics::PolygonMode::Line;
    state.topology    = vine::graphics::Topology::Points;
    state.blend       = vine::graphics::BlendState{ /*enabled*/ true, vine::graphics::BlendFactor::One,
                                                    vine::graphics::BlendFactor::Zero };

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
    EXPECT_EQ(command->polygon_mode, objects.rasterization->polygonMode);

    EXPECT_EQ(command->topology, objects.inputAssembly->topology);

    ASSERT_EQ(objects.colorBlend->attachments.size(), 1u);
    EXPECT_EQ(command->color_attachment_count, 1u);
    EXPECT_EQ(command->blend[0].blendEnable, objects.colorBlend->attachments[0].blendEnable);
    EXPECT_EQ(command->blend[0].srcColorBlendFactor, objects.colorBlend->attachments[0].srcColorBlendFactor);
    EXPECT_EQ(command->blend[0].dstColorBlendFactor, objects.colorBlend->attachments[0].dstColorBlendFactor);
    EXPECT_EQ(command->blend[0].colorBlendOp, objects.colorBlend->attachments[0].colorBlendOp);

    // The concrete values, so the mapping is checked against Vulkan rather than against itself: the SDK
    // compares distance (Less = the nearer fragment wins) and this backend renders reverse-Z, so "closer"
    // is a LARGER depth.
    EXPECT_EQ(command->depth_test_enable, VK_FALSE);
    EXPECT_EQ(command->depth_write_enable, VK_FALSE);
    EXPECT_EQ(command->compare_op, VK_COMPARE_OP_NEVER);
    EXPECT_EQ(command->cull_mode, VK_CULL_MODE_FRONT_BIT);
    EXPECT_EQ(command->front_face, VK_FRONT_FACE_CLOCKWISE);
    EXPECT_EQ(command->polygon_mode, VK_POLYGON_MODE_LINE);
    EXPECT_EQ(command->topology, VK_PRIMITIVE_TOPOLOGY_POINT_LIST);
    EXPECT_EQ(command->blend[0].blendEnable, VK_TRUE);
    EXPECT_EQ(command->blend[0].srcColorBlendFactor, VK_BLEND_FACTOR_ONE);
    EXPECT_EQ(command->blend[0].dstColorBlendFactor, VK_BLEND_FACTOR_ZERO);
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
    EXPECT_EQ(command->polygon_mode, VK_POLYGON_MODE_FILL);
    EXPECT_EQ(command->topology, VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST);
    EXPECT_EQ(command->color_attachment_count, 1u);
    EXPECT_EQ(command->blend[0].blendEnable, VK_TRUE) << "this backend always blends, for the per-vertex opacity";
    EXPECT_EQ(command->blend[0].srcColorBlendFactor, VK_BLEND_FACTOR_SRC_ALPHA);
    EXPECT_EQ(command->blend[0].dstColorBlendFactor, VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA);
}

TEST(DynamicStateTest, TheAttachmentCountFollowsTheMappedBlendStateAndTheCarriedEntriesAreBounded)
{
    // The blend half is delivered per attachment, and the COUNT must be the pipeline's: a pipeline declares
    // one entry per colour attachment of its subpass (VUID-VkGraphicsPipelineCreateInfo-renderPass-07609), so
    // the command sets exactly those.
    const auto              one = makeDynamicState(makeRenderStateObjects(vine::graphics::ResolvedRenderState{}));
    vine::graphics::ResolvedRenderState base;
    ASSERT_EQ(one->color_attachment_count, 1u);

    // An MRT pass re-maps the list to one entry per attachment (all opaque, see
    // applyOpaqueBlendForAttachments): the count follows and every entry carries the opaque values.
    auto mrt_objects = makeRenderStateObjects(base);
    applyOpaqueBlendForAttachments(mrt_objects, 3);
    const auto mrt = makeDynamicState(mrt_objects);
    EXPECT_EQ(mrt->color_attachment_count, 3u);
    for (std::uint32_t i = 0; i < 3u; ++i) {
        EXPECT_EQ(mrt->blend[i].blendEnable, VK_FALSE) << i;
        EXPECT_EQ(mrt->blend[i].srcColorBlendFactor, VK_BLEND_FACTOR_ONE) << i;
        EXPECT_EQ(mrt->blend[i].dstColorBlendFactor, VK_BLEND_FACTOR_ZERO) << i;
    }

    // Past what the command can carry, the entries stop being copied but the COUNT stays honest: record()
    // emits min(count, kMaxDynamicAttachments), and the attachments beyond it keep the pipeline's constants
    // — which decide nothing anyway (the pipeline's baked values are constants).
    auto many_objects = makeRenderStateObjects(base);
    applyOpaqueBlendForAttachments(many_objects, static_cast<int>(kMaxDynamicAttachments) + 2);
    const auto many = makeDynamicState(many_objects);
    EXPECT_EQ(many->color_attachment_count, kMaxDynamicAttachments + 2u);
    EXPECT_EQ(many->blend[kMaxDynamicAttachments - 1].blendEnable, VK_FALSE);
}

TEST(DynamicStateTest, TheBakedPipelineStateIsTheConstantsAndTheCommandStartsFromThem)
{
    // Two couplings that have to hold together, or the collapse is unsound:
    //  * what the pipeline create-info carries IS the named constant (makePipelineStateObjects), so every
    //    drawable of a set gets one content-identical pipeline;
    //  * the command's own field initializers are those same constants, so "the value the pipeline bakes"
    //    is one named thing rather than a literal repeated in two files.
    // They are NOT the resolved state's values: after the collapse a pipeline deliberately bakes constants,
    // and every set this backend builds declares the states dynamic, so the delivered value is the one that
    // decides. (A foreign set that declared none would draw with these constants — which is why they are the
    // engine's DEFAULT state and not something arbitrary.)
    const SetDynamicState defaults;
    const auto             baked = makePipelineStateObjects(makeRenderStateObjects(vine::graphics::ResolvedRenderState{}));

    EXPECT_EQ(baked.depthStencil->depthTestEnable, kBakedDepthTestEnable);
    EXPECT_EQ(baked.depthStencil->depthWriteEnable, kBakedDepthWriteEnable);
    EXPECT_EQ(baked.depthStencil->depthCompareOp, kBakedCompareOp);
    EXPECT_EQ(baked.rasterization->cullMode, kBakedCullMode);
    EXPECT_EQ(baked.rasterization->frontFace, kBakedFrontFace);
    EXPECT_EQ(baked.rasterization->polygonMode, kBakedPolygonMode);
    EXPECT_EQ(baked.inputAssembly->topology, kBakedTopology);

    EXPECT_EQ(defaults.depth_test_enable, kBakedDepthTestEnable);
    EXPECT_EQ(defaults.depth_write_enable, kBakedDepthWriteEnable);
    EXPECT_EQ(defaults.compare_op, kBakedCompareOp);
    EXPECT_EQ(defaults.cull_mode, kBakedCullMode);
    EXPECT_EQ(defaults.front_face, kBakedFrontFace);
    EXPECT_EQ(defaults.polygon_mode, kBakedPolygonMode);
    EXPECT_EQ(defaults.topology, kBakedTopology);

    // The blend half: the pipeline declares one entry per attachment (the count its subpass needs) holding
    // the opaque constant, while what a drawable DELIVERS for the default state is this backend's
    // always-blend pair for the per-vertex opacity — so the two differ, and the delivered one is the one
    // that reaches the GPU (the state is declared dynamic).
    ASSERT_EQ(baked.colorBlend->attachments.size(), 1u);
    EXPECT_EQ(baked.colorBlend->attachments[0].blendEnable, VK_FALSE) << "the bake is the constant";
    EXPECT_EQ(baked.colorBlend->attachments[0].srcColorBlendFactor, VK_BLEND_FACTOR_ONE);

    const auto command = makeDynamicState(makeRenderStateObjects(vine::graphics::ResolvedRenderState{}));
    ASSERT_NE(command, nullptr);
    EXPECT_EQ(command->blend[0].blendEnable, VK_TRUE) << "the delivered value is the resolved one";
    EXPECT_EQ(command->blend[0].srcColorBlendFactor, VK_BLEND_FACTOR_SRC_ALPHA);
}

TEST(DynamicStateTest, TheExtensionBackedCallsNeedAnEntryPointPerDevice)
{
    // Three of the nine calls cannot be made by name: their entry points are the only ones the loader does
    // not export (see DynamicStateEntryPoints), so the command carries pointers fetched from its session's
    // device. complete() is what a session can check before trusting them, and it is deliberately strict:
    // one missing pointer is an incomplete set.
    DynamicStateEntryPoints none;
    EXPECT_FALSE(none.complete());

    DynamicStateEntryPoints partial;
    partial.set_polygon_mode = reinterpret_cast<PFN_vkCmdSetPolygonModeEXT>(0x1);
    EXPECT_FALSE(partial.complete());

    DynamicStateEntryPoints all;
    all.set_polygon_mode         = reinterpret_cast<PFN_vkCmdSetPolygonModeEXT>(0x1);
    all.set_color_blend_enable   = reinterpret_cast<PFN_vkCmdSetColorBlendEnableEXT>(0x2);
    all.set_color_blend_equation = reinterpret_cast<PFN_vkCmdSetColorBlendEquationEXT>(0x3);
    EXPECT_TRUE(all.complete());

    // A command built without them still compares/shares normally: the pointers are part of its content, so
    // a command of one device never pools with a command of another.
    const auto bare = makeDynamicState(makeRenderStateObjects(vine::graphics::ResolvedRenderState{}));
    EXPECT_FALSE(bare->entry_points.complete());
}

TEST(DynamicStateTest, SeveralColourAttachmentsAreDeliveredUnblended)
{
    // The blend ENABLE is delivered by this command, so the pipeline's baked constants no longer decide:
    // one colour attachment keeps the engine's opacity pair (a drawable's per-vertex alpha may drop
    // below 1 at any time without a rebuild), while a pass with SEVERAL writes DATA - a G-buffer's
    // normal rides with the material's shininess in its alpha - and this backend's rule for those
    // attachments is blend DISABLED (the same rule the L2 applies in applyOpaqueBlendForAttachments,
    // which was the measured fix for the same picture: a shininess of 32 attenuated every stored normal
    // to 12.5% of its value). The engine's own lighting case in ContentPassTest shades a G-buffer whose
    // normal attachment carries alpha 0.125 and saw exactly that attenuation while this end said "on".
    const vine::vsg::core::DynamicState state;
    const auto                       one =
        makeDynamicStateCommand(state, 1U, DynamicStateEntryPoints{});
    ASSERT_NE(one, nullptr);
    EXPECT_EQ(one->color_attachment_count, 1U);
    EXPECT_EQ(one->blend[0].blendEnable, VK_TRUE) << "one attachment: the opacity path";
    EXPECT_EQ(one->blend[0].srcColorBlendFactor, VK_BLEND_FACTOR_SRC_ALPHA);
    EXPECT_EQ(one->blend[0].dstColorBlendFactor, VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA);

    const auto four = makeDynamicStateCommand(state, 4U, DynamicStateEntryPoints{});
    ASSERT_NE(four, nullptr);
    EXPECT_EQ(four->color_attachment_count, 4U);
    for (std::uint32_t index = 0; index < 4U; ++index) {
        EXPECT_EQ(four->blend[index].blendEnable, VK_FALSE)
            << "attachment " << index << " carries data, not transparency";
        EXPECT_EQ(four->blend[index].srcColorBlendFactor, VK_BLEND_FACTOR_ONE);
        EXPECT_EQ(four->blend[index].dstColorBlendFactor, VK_BLEND_FACTOR_ZERO);
    }
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
