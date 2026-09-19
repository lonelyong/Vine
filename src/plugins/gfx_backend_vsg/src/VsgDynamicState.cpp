#include <vine/vsg/VsgDynamicState.hpp>

#include <vsg/vk/CommandBuffer.h>

V_VSG_NS_BEGIN

namespace detail
{

void SetDynamicState::record(::vsg::CommandBuffer& commandBuffer) const
{
    // One call per state named in makeDynamicStateDeclaration(). The order is irrelevant: each one sets an
    // independent piece of dynamic state, and the pipeline is bound by the time a variant records (the
    // BindGraphicsPipeline precedes it in the same state group).
    vkCmdSetDepthTestEnable(commandBuffer, depth_test_enable);
    vkCmdSetDepthWriteEnable(commandBuffer, depth_write_enable);
    vkCmdSetDepthCompareOp(commandBuffer, compare_op);
    vkCmdSetCullMode(commandBuffer, cull_mode);
    vkCmdSetFrontFace(commandBuffer, front_face);
    vkCmdSetPrimitiveTopology(commandBuffer, topology);
}

::vsg::ref_ptr<::vsg::DynamicState> makeDynamicStateDeclaration()
{
    return ::vsg::DynamicState::create(VK_DYNAMIC_STATE_DEPTH_TEST_ENABLE, VK_DYNAMIC_STATE_DEPTH_WRITE_ENABLE,
                                       VK_DYNAMIC_STATE_DEPTH_COMPARE_OP, VK_DYNAMIC_STATE_CULL_MODE,
                                       VK_DYNAMIC_STATE_FRONT_FACE, VK_DYNAMIC_STATE_PRIMITIVE_TOPOLOGY);
}

} // namespace detail

V_VSG_NS_END
