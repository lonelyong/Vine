#include <vine/vsg/VsgSceneRules.hpp>

#include <algorithm>
#include <cstdint>

#include <vsg/state/ColorBlendState.h>
#include <vsg/utils/ShaderSet.h>

#include <vine/vsg/VsgUtils.hpp>

V_VSG_NS_BEGIN

// The bridge's device-free rules: what a custom vertex channel may be, which colour attachments a
// shader set declares, what an opaque multi-attachment pipeline writes, and the cache-key hashing.
// They are declared and explained in VsgSceneRules.hpp; this unit exists so a test can call them
// without a device.

namespace detail
{

ChannelShape channelShape(const vine::graphics::AttributeBuffer& attr, std::size_t vertex_count)
{
    if (attr.components < 1u || attr.components > 4u) {
        return ChannelShape::Components;
    }
    if (attr.data->size() % attr.components != 0u) {
        return ChannelShape::NotDivisible;
    }
    if (attr.data->size() / attr.components != vertex_count) {
        return ChannelShape::VertexCount;
    }
    return ChannelShape::Ok;
}

vine::String ignoredChannelMessage(std::uint32_t location, const vine::graphics::AttributeBuffer& attr,
                                   std::size_t vertex_count, ChannelShape shape)
{
    switch (shape) {
    case ChannelShape::Components:
        return formatDiagnostic(u8"loc%u custom channel has components=%u (1..4 "
                                u8"required); channel ignored",
                                location, attr.components);
    case ChannelShape::NotDivisible:
        return formatDiagnostic(u8"loc%u custom channel holds %zu floats, not divisible "
                                u8"by components=%u; channel ignored",
                                location, attr.data->size(), attr.components);
    case ChannelShape::VertexCount:
        return formatDiagnostic(u8"loc%u custom channel has %zu vertices, expected %zu; "
                                u8"channel ignored",
                                location, attr.data->size() / attr.components, vertex_count);
    case ChannelShape::Ok:
        break;
    }
    return vine::String();
}

std::uint64_t hashStateVariant(const vine::graphics::ShaderProgram* program, const vine::graphics::Material* material,
                               const vine::graphics::ResolvedRenderState& state, std::uint64_t layout)
{
    std::uint64_t h = kHashSeed;
    const auto    mix_ptr = [&](const void* p) {
        h = hashCombine(h, static_cast<std::uint64_t>(reinterpret_cast<std::uintptr_t>(p)));
    };
    mix_ptr(program);
    if (program != nullptr) {
        // Program content is part of the variant identity: editing a retained
        // program's GLSL bumps its revision, which yields a new template key
        // and a fresh pipeline (D10).
        h = hashCombine(h, program->revision());
    }
    mix_ptr(material);
    h = hashCombine(h, layout);
    h = hashCombine(h, static_cast<std::uint64_t>(state.depth.test));
    h = hashCombine(h, static_cast<std::uint64_t>(state.depth.write));
    h = hashCombine(h, static_cast<std::uint64_t>(state.depth.compare));
    h = hashCombine(h, static_cast<std::uint64_t>(state.cullMode));
    h = hashCombine(h, static_cast<std::uint64_t>(state.blend.enabled));
    h = hashCombine(h, static_cast<std::uint64_t>(state.blend.src));
    h = hashCombine(h, static_cast<std::uint64_t>(state.blend.dst));
    h = hashCombine(h, static_cast<std::uint64_t>(state.polygonMode));
    h = hashCombine(h, static_cast<std::uint64_t>(state.topology));
    return h;
}

int colourAttachmentCount(const ::vsg::ref_ptr<::vsg::ShaderSet>& shader_set)
{
    if (shader_set != nullptr) {
        for (const auto& state : shader_set->defaultGraphicsPipelineStates) {
            if (auto blend = state.cast<::vsg::ColorBlendState>()) {
                return std::max(1, static_cast<int>(blend->attachments.size()));
            }
        }
    }
    return 1;
}

void applyOpaqueBlendForAttachments(RenderStateObjects& states, int colour_count)
{
    VkPipelineColorBlendAttachmentState opaque{};
    opaque.blendEnable         = VK_FALSE;
    opaque.srcColorBlendFactor = VK_BLEND_FACTOR_ONE;
    opaque.dstColorBlendFactor = VK_BLEND_FACTOR_ZERO;
    opaque.colorBlendOp        = VK_BLEND_OP_ADD;
    opaque.srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
    opaque.dstAlphaBlendFactor = VK_BLEND_FACTOR_ZERO;
    opaque.alphaBlendOp        = VK_BLEND_OP_ADD;
    opaque.colorWriteMask      = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT | VK_COLOR_COMPONENT_B_BIT |
                            VK_COLOR_COMPONENT_A_BIT;
    states.colorBlend->attachments.clear();
    for (int i = 0; i < colour_count; ++i) {
        states.colorBlend->attachments.push_back(opaque);
    }
}

} // namespace detail

V_VSG_NS_END
