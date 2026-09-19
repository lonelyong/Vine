#include <vine/vsg/VsgDynamicDepth.hpp>

#include <vsg/vk/CommandBuffer.h>

V_VSG_NS_BEGIN

namespace detail
{

void SetDepthState::record(::vsg::CommandBuffer& commandBuffer) const
{
    // The three calls the declaration in makeDynamicDepthState() covers. Order is irrelevant: each one
    // sets an independent piece of the dynamic depth state, and the pipeline is bound by the time a
    // variant records (the BindGraphicsPipeline precedes it in the same state group).
    vkCmdSetDepthTestEnable(commandBuffer, depth_test_enable);
    vkCmdSetDepthWriteEnable(commandBuffer, depth_write_enable);
    vkCmdSetDepthCompareOp(commandBuffer, compare_op);
}

::vsg::ref_ptr<::vsg::DynamicState> makeDynamicDepthState()
{
    return ::vsg::DynamicState::create(VK_DYNAMIC_STATE_DEPTH_TEST_ENABLE, VK_DYNAMIC_STATE_DEPTH_WRITE_ENABLE,
                                       VK_DYNAMIC_STATE_DEPTH_COMPARE_OP);
}

bool declaresDynamicDepth(const ::vsg::ShaderSet& shader_set)
{
    // The declaration is the UNION of the set's DynamicState objects, not the content of a single one:
    // vsg merges a pipeline's states by type in mergeGraphicsPipelineStates, and for DynamicState that
    // merge is a union of the state lists (a differing list becomes a new DynamicState holding both — see
    // the original_DynamicState branch there). A set built from several objects therefore still declares
    // all three to the pipeline this backend drives, and asking only one object would refuse a set the
    // driver would have honoured.
    std::uint32_t found = 0u;
    for (const auto& state : shader_set.defaultGraphicsPipelineStates) {
        const auto dynamic = state.cast<::vsg::DynamicState>();
        if (dynamic == nullptr) {
            continue;
        }
        for (const VkDynamicState name : dynamic->dynamicStates) {
            if (name == VK_DYNAMIC_STATE_DEPTH_TEST_ENABLE) {
                found |= 1u;
            } else if (name == VK_DYNAMIC_STATE_DEPTH_WRITE_ENABLE) {
                found |= 2u;
            } else if (name == VK_DYNAMIC_STATE_DEPTH_COMPARE_OP) {
                found |= 4u;
            }
        }
    }
    // All three, not any of them: a set that declared only some of them still bakes the others, and
    // SetDepthState writes all three — so a partial declaration is NOT a set this backend may drive (the
    // command would silently have no effect on whichever state the set did not declare).
    return found == 7u;
}

} // namespace detail

V_VSG_NS_END
