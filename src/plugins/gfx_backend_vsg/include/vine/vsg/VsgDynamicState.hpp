#pragma once

#include "vsg_global.hpp"

#include <array>
#include <cstdint>

#include <vsg/core/ref_ptr.h>
#include <vsg/state/DynamicState.h>
#include <vsg/state/StateCommand.h>

V_VSG_NS_BEGIN

namespace detail
{

/**
 * @brief The state slot (StateCommand::slot) this backend's dynamic-state command occupies.
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
inline constexpr uint32_t kDynamicStateSlot = 15u;

/**
 * @brief The pipeline state of the NEXT draws, as Vulkan dynamic state.
 *
 * The state a vine::graphics::StateNode configures is pipeline-creation state, so without this every state
 * change (a depth policy, a culling side, a wireframe toggle, a blend factor, a topology) has to become a
 * new pipeline — the reason this backend carries one content ShaderSet per depth policy and keys its variant
 * cache on the resolved state. Vulkan 1.3 delivers all of it dynamically, so the state moves into the
 * command stream instead and the pipeline stops being a function of it. It is not something a session can
 * turn off: the declarations are part of every content set this backend builds, and every variant emits
 * this command (see makeDynamicStateDeclaration).
 *
 * The values are read from the SAME mapped render-state objects the pipeline create-info is given (see
 * makeDynamicState in RenderStateMapper.hpp), so today the command says exactly what the pipeline baked:
 * enabling the layer is behaviour-neutral by construction, and whether a driver takes the baked value (a
 * pipeline that did not declare the state dynamic) or the dynamic one (ours), the picture is the same.
 * That is what made it possible to introduce the plumbing first and drop the state from the variant
 * identity afterwards.
 *
 * WHAT IT CARRIES: the whole of what a StateNode can move that core 1.3 makes dynamic with nothing to
 * enable — depth test / write / compare, cull mode, front face and primitive topology. All four come from
 * VK_EXT_extended_dynamic_state, promoted to 1.3 with its feature struct NOT promoted: core 1.3 has these
 * states and no bit for them, which is what the 1.3 version floor backs. Nothing is requested in the
 * device-feature chain for any of them.
 *
 * WHAT IT DOES NOT CARRY, and why — the two items of ResolvedRenderState that are NOT core-1.3 state:
 *  * polygon mode (VK_DYNAMIC_STATE_POLYGON_MODE_EXT): VK_EXT_extended_dynamic_state2 was promoted to 1.3
 *    without it ("Feature struct and optional state are not promoted"), so it needs the extension's
 *    feature bit; LINE/POINT also need fillModeNonSolid (requested).
 *  * colour blend enable / factors (VK_DYNAMIC_STATE_COLOR_BLEND_ENABLE_EXT, ..._EQUATION_EXT):
 *    VK_EXT_extended_dynamic_state3 is not promoted at all, so these need its two feature bits.
 * Each of those means three things at once: an optional feature bit a 1.3 device may not have, the
 * extension enabled at device creation, and — the trap that settled it — ENTRY POINTS THE LOADER DOES NOT
 * EXPORT. `vkCmdSetPolygonModeEXT` and `vkCmdSetColorBlendEnableEXT`/`...EquationEXT` have no symbol in
 * libvulkan.so (unlike the promoted names, which do), so calling them directly links on Windows and fails
 * to link on Linux; using them properly means fetching them with vkGetDeviceProcAddr and carrying a
 * per-device pointer table. That is a mechanism of its own for two states that change rarely, so they stay
 * in the pipeline: see the variant identity in SceneBridgePipeline (milestone B keeps them there
 * deliberately).
 *
 * It derives from vsg::StateCommand — not from vsg::Command — because a variant carries it in its
 * StateGroup's stateCommands (alongside the pipeline and descriptor binds), and that list holds
 * StateCommands.
 *
 * A driver IGNORES the part of this command a pipeline did not declare dynamic — the values such a pipeline
 * was created with stay in force. For every set this backend builds that cannot happen (the declaration
 * above is part of it); for a foreign set it means the mapped values that were baked apply instead, which
 * is the same value by construction (see the paragraph on the invariant above).
 */
class SetDynamicState : public ::vsg::Inherit<::vsg::StateCommand, SetDynamicState>
{
  public:
    /// @brief Constructs the command in the slot this backend's state commands occupy.
    ///
    /// Explicit rather than `= default`: StateCommand's default slot is 0, which is where the pipeline bind
    /// lives, so a defaulted constructor would silently replace the group's pipeline bind (see
    /// kDynamicStateSlot — that is exactly what the first version of this command did).
    SetDynamicState() :
        Inherit(kDynamicStateSlot)
    {
    }

    VkBool32            depth_test_enable  = VK_TRUE;
    VkBool32            depth_write_enable = VK_TRUE;
    VkCompareOp         compare_op         = VK_COMPARE_OP_GREATER;
    VkCullModeFlags     cull_mode          = VK_CULL_MODE_NONE;
    VkFrontFace         front_face         = VK_FRONT_FACE_CLOCKWISE;
    VkPrimitiveTopology topology           = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;

    /// @brief Applies the state to @p commandBuffer (vkCmdSetDepthTestEnable & friends).
    void record(::vsg::CommandBuffer& commandBuffer) const override;
};

/**
 * @brief The dynamic-state declaration every content pipeline needs to be driven by SetDynamicState.
 *
 * Declared through the ShaderSet's pipeline states, so it is a property of the SET and reaches every
 * pipeline built from it (vsg::DynamicState::apply writes VkPipelineDynamicStateCreateInfo — see
 * DynamicState.cpp). One object per set: the declaration is the same for all of them, and it must name
 * exactly the states SetDynamicState::record writes — a state the command writes but the declaration
 * omits is a value the driver takes from the create-info instead (i.e. a silently ignored half of the
 * command), which is what the tests pin.
 *
 * @return The DynamicState naming every state this backend delivers dynamically.
 */
[[nodiscard]] ::vsg::ref_ptr<::vsg::DynamicState> makeDynamicStateDeclaration();

} // namespace detail

V_VSG_NS_END

// SetDynamicState lives in vine::vsg::detail, so its type_name<>() specialization has to be written at
// global scope with the qualified name (vsg's own VSG_type_name macro assumes the vsg namespace).
EVSG_type_name(vine::vsg::detail::SetDynamicState);
