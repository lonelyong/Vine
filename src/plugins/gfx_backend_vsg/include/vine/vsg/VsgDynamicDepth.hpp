#pragma once

#include "vsg_global.hpp"

#include <cstdint>

#include <vsg/core/ref_ptr.h>
#include <vsg/state/DynamicState.h>
#include <vsg/state/StateCommand.h>
#include <vsg/utils/ShaderSet.h>

V_VSG_NS_BEGIN

namespace detail
{

/**
 * @brief The state slot (StateCommand::slot) this backend's depth command occupies.
 *
 * A slot is the identity of a STATE STACK, not a priority: vsg pushes every command of a state group onto
 * the stack named by its slot and records only that stack's top (StateStack::record), so two commands in
 * one group that share a slot shadow one another — the earlier one is never recorded at all. vsg's own
 * allocation is 0 for pipeline binds (see GraphicsPipeline.cpp), `1 + firstSet` for descriptor binds (see
 * BindDescriptorSets) and 2 for the view-dependent state and push constants. The first version of this
 * command took the default slot 0, which replaced the group's pipeline bind: the validation layer reported
 * "a valid pipeline must be bound" at the following draw and the driver crashed on it.
 *
 * The LOW free slot is not usable here: `1 + firstSet` means a program that binds descriptor set 2 or
 * higher would land on 3 or higher, and how many sets a program declares is the program's choice. So the
 * value is the top of vsg's fixed-size state stack instead — above every descriptor-set index a layout can
 * plausibly occupy, and still inside STATESTACK_SIZE (16).
 *
 * That leaves one requirement, which is why it is spelled out here: State::stateStacks is sized from
 * CollectResourceRequirements, which grows maxSlots.state from the slot of every StateCommand it VISITS
 * (StateGroup::traverse visits them). Slots above the collected maximum are never recorded and are indexed
 * out of range, so this command must be reachable from the compiled command graph as a state command of a
 * state group — which is exactly where a variant puts it (SceneBridgePipeline's buildStateGroup). Measured
 * on the self-test with a probe in record(): stateStacks.size() == 16 and maxSlots.state == 15, i.e. the
 * collected maximum is this command's own slot.
 */
inline constexpr uint32_t kDepthStateSlot = 15u;

/**
 * @brief The depth state of the NEXT draws, as a Vulkan 1.3 dynamic state.
 *
 * `depthTestEnable` / `depthWriteEnable` / `depthCompareOp` are pipeline-creation state in core 1.1 —
 * which is why this backend has three content ShaderSets (depth on / test-only / off) that a pass'
 * depth policy selects between, and why changing that policy has to drop and rebuild a drawable's state
 * wrapper (see SceneBridge::invalidateState). `VK_EXT_extended_dynamic_state`, promoted to
 * VK_VERSION_1_3, moves all three out of the pipeline, so one pipeline can serve every depth policy and
 * a policy change becomes one command per draw instead of a rebuild — the same shape as the viewport,
 * which this backend already delivers dynamically. It asks for nothing in the device-feature chain:
 * the promotion did not include the feature struct, so core 1.3 has these states and no bit to enable
 * (the 1.3 floor this backend refuses to run below is what backs it — see kDynamicDepth).
 *
 * The command carries the value that WOULD have been baked (see SceneBridge' variant build: it reads
 * the same mapped DepthStencilState), so enabling it is behaviour-neutral by construction; that is what
 * makes it possible to introduce the plumbing first and collapse the three sets afterwards.
 *
 * It derives from vsg::StateCommand — not from vsg::Command, the way vsg's own SetLineWidth does — because
 * a variant carries it in its StateGroup's stateCommands (alongside the pipeline and descriptor binds), and
 * that list holds StateCommands.
 *
 * A driver IGNORES this command for a pipeline that did not declare the three states dynamic — the
 * values such a pipeline was created with stay in force. That cannot happen for a variant that emits
 * it, because declaresDynamicDepth() is what decides whether it is emitted at all.
 */
class SetDepthState : public ::vsg::Inherit<::vsg::StateCommand, SetDepthState>
{
  public:
    SetDepthState() = default;

    /// @brief The depth state every following draw uses.
    explicit SetDepthState(VkBool32 in_depth_test_enable, VkBool32 in_depth_write_enable, VkCompareOp in_compare_op) :
        Inherit(kDepthStateSlot),
        depth_test_enable(in_depth_test_enable),
        depth_write_enable(in_depth_write_enable),
        compare_op(in_compare_op)
    {
    }

    VkBool32   depth_test_enable  = VK_FALSE;
    VkBool32   depth_write_enable = VK_FALSE;
    VkCompareOp compare_op        = VK_COMPARE_OP_ALWAYS;

    /// @brief Applies the state to @p commandBuffer (vkCmdSetDepthTestEnable & friends).
    void record(::vsg::CommandBuffer& commandBuffer) const override;
};

/**
 * @brief Whether the content sets of a session declare the depth states dynamic — the ONE decision.
 *
 * A constant rather than a runtime query because the functionality is core Vulkan 1.3 with no feature bit
 * to enable — the registry's entry for VK_EXT_extended_dynamic_state says "Feature struct is not promoted",
 * so nothing in the device-feature chain (and nothing in the device-creation outcome) can vary here. What
 * backs it is the version floor: this backend refuses to run on a device below detail::kRequiredVulkanVersion
 * (= 1.3), see VsgRenderer::initialize.
 * It exists as a NAME so the three places that build content sets (VsgRenderer's window sets,
 * VsgTargetBookkeeping's per-target sets, VsgContentSlot's lazily built one) cannot drift apart.
 */
inline constexpr bool kDynamicDepth = true;

/**
 * @brief The dynamic-state declaration a pipeline needs to be driven by SetDepthState.
 *
 * Declared through the ShaderSet's pipeline states, so it is a property of the SET and reaches every
 * pipeline built from it (vsg::DynamicState::apply writes VkPipelineDynamicStateCreateInfo — see
 * DynamicState.cpp). One object per set: the declaration is the same for all of them.
 *
 * @return The DynamicState naming the three depth states.
 */
[[nodiscard]] ::vsg::ref_ptr<::vsg::DynamicState> makeDynamicDepthState();

/**
 * @brief Whether @p shader_set declares the three depth states dynamic (see makeDynamicDepthState).
 *
 * The question a bridge asks ONCE per injected set: a set that declares them must be driven by a
 * SetDepthState per variant (the baked DepthStencilState values are then ignored by the driver), and a
 * set that does not would make that command invalid. Asking the set — rather than a session-wide flag —
 * is what keeps "who declared this" in one place: the set carries its own pipeline states, so it is the
 * only object that can answer.
 *
 * The question is about the UNION of the set's DynamicState objects, because that is what reaches the
 * pipeline: vsg merges a pipeline's states by type (mergeGraphicsPipelineStates) and unions the lists of
 * two DynamicState objects, so a set that spread the three states over several objects declares all three
 * just the same. All three have to be there — a set that declared two still bakes the third.
 *
 * @param shader_set Set to inspect.
 * @return true when the set declares all three states dynamic.
 */
[[nodiscard]] bool declaresDynamicDepth(const ::vsg::ShaderSet& shader_set);

} // namespace detail

V_VSG_NS_END

// SetDepthState lives in vine::vsg::detail, so its type_name<>() specialization has to be written at global
// scope with the qualified name (vsg's own VSG_type_name macro assumes the vsg namespace).
EVSG_type_name(vine::vsg::detail::SetDepthState);
