#pragma once

#include "vsg_global.hpp"

#include <array>
#include <cstdint>

#include <vsg/core/ref_ptr.h>
#include <vsg/state/DynamicState.h>
#include <vsg/state/StateCommand.h>

#include <vine/vsg/VsgVulkanEntryPoints.hpp>

VN_VSG_NS_BEGIN

namespace detail
{

/**
 * @brief The values a pipeline BAKES for the states this backend delivers dynamically.
 *
 * A pipeline still has to carry valid values in its create-info (Vulkan needs the structs); with the states
 * declared dynamic the driver ignores them, so which values they are does not matter — as long as every
 * pipeline of a set carries the SAME ones. They are the command's own defaults, named here so the bake and
 * the command cannot drift: a pipeline built with anything else would be a pipeline that differs from its
 * neighbours for no reason, i.e. one VkPipeline per state combination again.
 */
inline constexpr VkBool32            kBakedDepthTestEnable  = VK_TRUE;
inline constexpr VkBool32            kBakedDepthWriteEnable = VK_TRUE;
inline constexpr VkCompareOp         kBakedCompareOp        = VK_COMPARE_OP_GREATER;
inline constexpr VkCullModeFlags     kBakedCullMode         = VK_CULL_MODE_NONE;
inline constexpr VkFrontFace         kBakedFrontFace        = VK_FRONT_FACE_CLOCKWISE;
inline constexpr VkPrimitiveTopology kBakedTopology         = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
inline constexpr VkPolygonMode       kBakedPolygonMode      = VK_POLYGON_MODE_FILL;
inline constexpr VkBool32            kBakedBlendEnable      = VK_FALSE;
inline constexpr VkBlendFactor       kBakedBlendFactor      = VK_BLEND_FACTOR_ONE;

/**
 * @brief The most colour attachments the blend half of this command can carry.
 *
 * `vkCmdSetColorBlendEnableEXT` / `vkCmdSetColorBlendEquationEXT` take an array, so the command holds one
 * entry per attachment (see SetDynamicState). Eight is what the desktop class guarantees and far above what
 * a pass in this engine uses (MRT passes here write three); a variant with more attachments keeps the ones
 * past this bound at the values the pipeline was created with — which are the same constants, because the
 * bake is constant too, so only the entries the command carries decide anything.
 */
inline constexpr std::uint32_t kMaxDynamicAttachments = 8u;

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
 * The state a vn::graphics::StateNode configures is pipeline-creation state, so without this every state
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
 * WHAT IT CARRIES — everything a StateNode can move, so nothing of the resolved state is pipeline state
 * any more:
 *  * depth test / write / compare, cull mode, front face, primitive topology: core 1.3 via
 *    VK_EXT_extended_dynamic_state, whose feature struct was NOT promoted — the states are core and there is
 *    no bit to enable. Nothing is requested in the device-feature chain for them.
 *  * polygon mode (VK_DYNAMIC_STATE_POLYGON_MODE_EXT) and colour blend enable + factors
 *    (VK_DYNAMIC_STATE_COLOR_BLEND_ENABLE_EXT / ..._EQUATION_EXT): all three belong to
 *    VK_EXT_extended_dynamic_state3, which is not promoted to any core version (1.4 does not absorb it
 *    either), so its feature bits are what make them available. ⚠ The polygon-mode enum is DEFINED in the
 *    VK_EXT_extended_dynamic_state2 block — which is why 1.3's promotion note says "Feature struct and
 *    optional state are not promoted" — but the bit that gates it is
 *    `extendedDynamicState3PolygonMode`, not `extendedDynamicState2`: the validator says so by name
 *    (VUID-VkGraphicsPipelineCreateInfo-extendedDynamicState3PolygonMode-07372), so that is what
 *    makeWindowTraits requests. LINE / POINT still need fillModeNonSolid, which this backend already
 *    requests. The blend OPS and the write mask stay baked: nothing in the engine can change them.
 * The extension is enabled and its three feature bits requested unconditionally in makeWindowTraits,
 * together with the optional core-1.0 features this backend already required: a device that cannot deliver
 * the state the engine configures is refused when the device is created rather than served on a subset that
 * would draw something else. The three COMMANDS that go with it are the only ones this layer cannot call by
 * name (see DynamicStateEntryPoints).
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

    VkBool32            depth_test_enable  = kBakedDepthTestEnable;
    VkBool32            depth_write_enable = kBakedDepthWriteEnable;
    VkCompareOp         compare_op         = kBakedCompareOp;
    VkCullModeFlags     cull_mode          = kBakedCullMode;
    VkFrontFace         front_face         = kBakedFrontFace;
    VkPolygonMode       polygon_mode       = kBakedPolygonMode;
    VkPrimitiveTopology topology           = kBakedTopology;
    /// The colour attachments of the draw the command precedes (0 = a depth-only pass: no blend at all).
    std::uint32_t color_attachment_count = 0u;
    /// Per-attachment blend state: only the entries below color_attachment_count are recorded.
    std::array<VkPipelineColorBlendAttachmentState, kMaxDynamicAttachments> blend{};
    /// The extension entry points record() calls through (see DynamicStateEntryPoints).
    DynamicStateEntryPoints entry_points;

    /// @brief Orders two commands by their values (content equality, what vsg::SharedObjects dedups by).
    ///
    /// Required, not cosmetic: the bridge shares these commands through its SharedObjects so that consecutive
    /// drawables of one state share one object and the state stack can skip re-recording it. Without a
    /// value-aware compare() every command would compare equal, and sharing would hand out the FIRST
    /// command's values for every state — a silently wrong picture.
    ///
    /// @param rhs_object Command to compare against.
    /// @return Negative, zero or positive, per vsg's compare convention.
    int compare(const ::vsg::Object& rhs_object) const override;

    /// @brief Applies the state to @p commandBuffer (vkCmdSetDepthTestEnable & friends).
    ///
    /// The polygon-mode and blend calls go through entry_points (see DynamicStateEntryPoints) — they are the
    /// three commands this layer cannot name directly. A session always has them (a device without the two
    /// extensions fails device creation), so a null entry point means a bridge that was never given them
    /// (device-free tests): the calls are then skipped rather than made through a null pointer.
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

VN_VSG_NS_END

// SetDynamicState lives in vn::vsg::detail, so its type_name<>() specialization has to be written at
// global scope with the qualified name (vsg's own VSG_type_name macro assumes the vsg namespace).
EVSG_type_name(vn::vsg::detail::SetDynamicState);
