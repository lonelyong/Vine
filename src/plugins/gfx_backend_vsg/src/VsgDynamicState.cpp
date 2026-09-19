#include <vine/vsg/VsgDynamicState.hpp>

#include <vsg/core/compare.h>
#include <algorithm>
#include <array>

#include <vsg/vk/CommandBuffer.h>

V_VSG_NS_BEGIN

namespace detail
{

int SetDynamicState::compare(const ::vsg::Object& rhs_object) const
{
    int result = StateCommand::compare(rhs_object);
    if (result != 0) {
        return result;
    }
    const auto& rhs = static_cast<const SetDynamicState&>(rhs_object);
    if ((result = ::vsg::compare_value(depth_test_enable, rhs.depth_test_enable)) != 0) return result;
    if ((result = ::vsg::compare_value(depth_write_enable, rhs.depth_write_enable)) != 0) return result;
    if ((result = ::vsg::compare_value(compare_op, rhs.compare_op)) != 0) return result;
    if ((result = ::vsg::compare_value(cull_mode, rhs.cull_mode)) != 0) return result;
    if ((result = ::vsg::compare_value(front_face, rhs.front_face)) != 0) return result;
    if ((result = ::vsg::compare_value(polygon_mode, rhs.polygon_mode)) != 0) return result;
    if ((result = ::vsg::compare_value(topology, rhs.topology)) != 0) return result;
    if ((result = ::vsg::compare_value(color_attachment_count, rhs.color_attachment_count)) != 0) return result;
    // Per field, not per struct: the attachment struct has no comparison of its own, and the write mask is
    // deliberately left out — the command does not deliver it (vkCmdSetColorBlendEquationEXT reads the six
    // factors and ops only), so two commands that differ there are still the same state.
    for (std::size_t i = 0; i < blend.size(); ++i) {
        if ((result = ::vsg::compare_value(blend[i].blendEnable, rhs.blend[i].blendEnable)) != 0) return result;
        if ((result = ::vsg::compare_value(blend[i].srcColorBlendFactor, rhs.blend[i].srcColorBlendFactor)) != 0)
            return result;
        if ((result = ::vsg::compare_value(blend[i].dstColorBlendFactor, rhs.blend[i].dstColorBlendFactor)) != 0)
            return result;
        if ((result = ::vsg::compare_value(blend[i].colorBlendOp, rhs.blend[i].colorBlendOp)) != 0) return result;
        if ((result = ::vsg::compare_value(blend[i].srcAlphaBlendFactor, rhs.blend[i].srcAlphaBlendFactor)) != 0)
            return result;
        if ((result = ::vsg::compare_value(blend[i].dstAlphaBlendFactor, rhs.blend[i].dstAlphaBlendFactor)) != 0)
            return result;
        if ((result = ::vsg::compare_value(blend[i].alphaBlendOp, rhs.blend[i].alphaBlendOp)) != 0) return result;
    }
    // The entry points are compared too: they belong to one device, so two commands of different sessions
    // must never be pooled even if their values agree (the pointers are identical within a session, so this
    // costs nothing where it matters). Compared as ADDRESSES rather than through compare_value on the
    // pointers: ordered comparison of function pointers is not defined by the language (clang warns about it),
    // and pointer identity is the whole question anyway.
    const auto as_address = [](auto fn) { return static_cast<std::uintptr_t>(reinterpret_cast<std::uintptr_t>(fn)); };
    if ((result = ::vsg::compare_value(as_address(entry_points.set_polygon_mode),
                                       as_address(rhs.entry_points.set_polygon_mode))) != 0)
        return result;
    if ((result = ::vsg::compare_value(as_address(entry_points.set_color_blend_enable),
                                       as_address(rhs.entry_points.set_color_blend_enable))) != 0)
        return result;
    return ::vsg::compare_value(as_address(entry_points.set_color_blend_equation),
                                as_address(rhs.entry_points.set_color_blend_equation));
}

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

    // The three calls that need an entry point (see DynamicStateEntryPoints). Guarded on the pointer rather
    // than on a flag: a session refuses a device without them, so they are present in every real session.
    if (entry_points.set_polygon_mode != nullptr) {
        entry_points.set_polygon_mode(commandBuffer, polygon_mode);
    }
    const auto count = std::min(color_attachment_count, kMaxDynamicAttachments);
    if (count != 0u && entry_points.set_color_blend_enable != nullptr && entry_points.set_color_blend_equation != nullptr) {
        // The enables are read out of the same entries the equations come from, so there is one truth for
        // both calls (an entry's blendEnable IS its enable).
        std::array<VkBool32, kMaxDynamicAttachments> enables{};
        std::array<VkColorBlendEquationEXT, kMaxDynamicAttachments> equations{};
        for (std::uint32_t i = 0; i < count; ++i) {
            enables[i] = blend[i].blendEnable;
            equations[i].srcColorBlendFactor = blend[i].srcColorBlendFactor;
            equations[i].dstColorBlendFactor = blend[i].dstColorBlendFactor;
            equations[i].colorBlendOp        = blend[i].colorBlendOp;
            equations[i].srcAlphaBlendFactor = blend[i].srcAlphaBlendFactor;
            equations[i].dstAlphaBlendFactor = blend[i].dstAlphaBlendFactor;
            equations[i].alphaBlendOp        = blend[i].alphaBlendOp;
        }
        entry_points.set_color_blend_enable(commandBuffer, 0u, count, enables.data());
        entry_points.set_color_blend_equation(commandBuffer, 0u, count, equations.data());
    }
}

DynamicStateEntryPoints fetchDynamicStateEntryPoints(const ::vsg::Device& device)
{
    DynamicStateEntryPoints entry_points;
    device.getProcAddr(entry_points.set_polygon_mode, "vkCmdSetPolygonModeEXT");
    device.getProcAddr(entry_points.set_color_blend_enable, "vkCmdSetColorBlendEnableEXT");
    device.getProcAddr(entry_points.set_color_blend_equation, "vkCmdSetColorBlendEquationEXT");
    return entry_points;
}

::vsg::ref_ptr<::vsg::DynamicState> makeDynamicStateDeclaration()
{
    return ::vsg::DynamicState::create(VK_DYNAMIC_STATE_DEPTH_TEST_ENABLE, VK_DYNAMIC_STATE_DEPTH_WRITE_ENABLE,
                                       VK_DYNAMIC_STATE_DEPTH_COMPARE_OP, VK_DYNAMIC_STATE_CULL_MODE,
                                       VK_DYNAMIC_STATE_FRONT_FACE, VK_DYNAMIC_STATE_POLYGON_MODE_EXT,
                                       VK_DYNAMIC_STATE_PRIMITIVE_TOPOLOGY, VK_DYNAMIC_STATE_COLOR_BLEND_ENABLE_EXT,
                                       VK_DYNAMIC_STATE_COLOR_BLEND_EQUATION_EXT);
}

} // namespace detail

V_VSG_NS_END
