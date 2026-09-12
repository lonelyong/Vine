#include <vine/vsg/VsgSceneRules.hpp>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <string>

#include <vsg/core/Array.h>
#include <vsg/maths/vec3.h>
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

XyzUnpack unpackXyz(const vine::graphics::AttributeBuffer& attr, vine::geometry::Vec3fArray& out)
{
    const auto  comps = attr.components;
    const auto& data  = *attr.data;
    if (comps < 3u || comps > 4u) {
        return XyzUnpack::NotXyzStride;
    }
    if (data.size() % comps != 0u) {
        return XyzUnpack::NotDivisible;
    }
    const std::size_t count = data.size() / comps;
    out.clear();
    out.reserve(count);
    for (std::size_t v = 0; v < count; ++v) {
        const std::size_t b = v * comps;
        out.emplace_back(data[b], data[b + 1], data[b + 2]);
    }
    return XyzUnpack::Ok;
}

vine::String ignoredNormalChannelMessage(const vine::graphics::AttributeBuffer& attr, XyzUnpack reason)
{
    if (reason == XyzUnpack::NotXyzStride) {
        return formatDiagnostic(u8"loc1 normal has components=%u (3 or 4 required); "
                                u8"normals will be derived",
                                attr.components);
    }
    return formatDiagnostic(u8"loc1 normal holds %zu floats, not divisible by its "
                            u8"components=%u stride; normals will be derived",
                            attr.data->size(), attr.components);
}

vine::math::Vec3f faceNormal(const vine::math::Vec3f& a, const vine::math::Vec3f& b, const vine::math::Vec3f& c)
{
    return (b - a).cross(c - a);
}

::vsg::ref_ptr<::vsg::vec4Array> makeWhiteColors(std::size_t count)
{
    auto colors = ::vsg::vec4Array::create(static_cast<uint32_t>(count));
    for (auto& v : *colors) {
        v = ::vsg::vec4(1.0f, 1.0f, 1.0f, 1.0f);
    }
    return colors;
}

::vsg::ref_ptr<::vsg::vec3Array> makeNormals(const vine::geometry::Vec3fArray& positions,
                                             const vine::geometry::Vec3fArray& meshNormals)
{
    auto normals = ::vsg::vec3Array::create(static_cast<uint32_t>(positions.size()));
    if (meshNormals.size() == positions.size()) {
        for (std::size_t i = 0; i < positions.size(); ++i) {
            const auto& n = meshNormals[i];
            (*normals)[i] = ::vsg::vec3(n.x, n.y, n.z);
        }
        return normals;
    }
    for (std::size_t i = 0; i + 2 < positions.size(); i += 3) {
        const vine::math::Vec3f n0 = faceNormal(positions[i], positions[i + 1], positions[i + 2]);
        // A degenerate triangle has a zero-length cross product: leave its
        // normal zero instead of normalising NaN (see normalIsUsable).
        ::vsg::vec3 n{ 0.0f, 0.0f, 0.0f };
        const float len_sq = n0.x * n0.x + n0.y * n0.y + n0.z * n0.z;
        if (normalIsUsable(len_sq)) {
            // Scaled by the reciprocal here, and by ::vsg::normalize (a division) in
            // makeIndexedNormals. The two forms differ in the last bit, and the values reach pixels in
            // the self-test's evidence lines, so they are deliberately NOT unified (§39 tried and kept
            // them apart); only the DECISION (normalIsUsable) and the cross product (faceNormal) are shared.
            const float inv_len = 1.0f / std::sqrt(len_sq);
            n = ::vsg::vec3(n0.x * inv_len, n0.y * inv_len, n0.z * inv_len);
        }
        for (std::size_t k = 0; k < 3; ++k) {
            (*normals)[i + k] = n;
        }
    }
    return normals;
}

::vsg::ref_ptr<::vsg::vec3Array> makeIndexedNormals(const vine::geometry::Vec3fArray& positions,
                                                    const vine::geometry::Vec3fArray& meshNormals,
                                                    const ::vsg::uintArray& indices)
{
    auto normals = ::vsg::vec3Array::create(static_cast<uint32_t>(positions.size()));
    if (meshNormals.size() == positions.size()) {
        for (std::size_t i = 0; i < positions.size(); ++i) {
            const auto& n = meshNormals[i];
            (*normals)[i] = ::vsg::vec3(n.x, n.y, n.z);
        }
        return normals;
    }
    const auto vertex_count = positions.size();
    // Accumulate face normals per vertex for a smoother result. Each triangle
    // is validated before use so a malformed index can never read OOB here.
    for (std::size_t tri = 0; tri + 2 < indices.size(); tri += 3) {
        const std::uint32_t ia = indices[tri];
        const std::uint32_t ib = indices[tri + 1];
        const std::uint32_t ic = indices[tri + 2];
        if (ia >= vertex_count || ib >= vertex_count || ic >= vertex_count) {
            continue; // defensive: rejected upstream; never index OOB.
        }
        const vine::math::Vec3f n = faceNormal(positions[ia], positions[ib], positions[ic]);
        (*normals)[ia] += ::vsg::vec3(n.x, n.y, n.z);
        (*normals)[ib] += ::vsg::vec3(n.x, n.y, n.z);
        (*normals)[ic] += ::vsg::vec3(n.x, n.y, n.z);
    }
    for (auto& n : *normals) {
        const float len_sq = n.x * n.x + n.y * n.y + n.z * n.z;
        if (normalIsUsable(len_sq)) {
            // ::vsg::normalize divides; makeNormals multiplies by the reciprocal. Keep the two as they
            // are (see the note there): the last bit reaches the self-test's pixel evidence.
            n = ::vsg::normalize(n);
        }
    }
    return normals;
}

::vsg::ref_ptr<::vsg::Data> makeTypedVertexData(std::uint32_t components, const std::vector<float>& data,
                                                std::size_t vertex_count)
{
    const auto n = static_cast<uint32_t>(vertex_count);
    switch (components) {
    case 1u: {
        auto arr = ::vsg::floatArray::create(n);
        for (std::size_t v = 0; v < vertex_count; ++v) {
            (*arr)[v] = data[v];
        }
        return arr;
    }
    case 2u: {
        auto arr = ::vsg::vec2Array::create(n);
        for (std::size_t v = 0; v < vertex_count; ++v) {
            (*arr)[v] = ::vsg::vec2(data[v * 2u], data[v * 2u + 1u]);
        }
        return arr;
    }
    case 3u: {
        auto arr = ::vsg::vec3Array::create(n);
        for (std::size_t v = 0; v < vertex_count; ++v) {
            (*arr)[v] = ::vsg::vec3(data[v * 3u], data[v * 3u + 1u], data[v * 3u + 2u]);
        }
        return arr;
    }
    default: {
        auto arr = ::vsg::vec4Array::create(n);
        for (std::size_t v = 0; v < vertex_count; ++v) {
            (*arr)[v] = ::vsg::vec4(data[v * 4u], data[v * 4u + 1u], data[v * 4u + 2u], data[v * 4u + 3u]);
        }
        return arr;
    }
    }
}

VkFormat formatForComponents(std::uint32_t components)
{
    switch (components) {
    case 1u: return VK_FORMAT_R32_SFLOAT;
    case 2u: return VK_FORMAT_R32G32_SFLOAT;
    case 3u: return VK_FORMAT_R32G32B32_SFLOAT;
    default: return VK_FORMAT_R32G32B32A32_SFLOAT;
    }
}

::vsg::ref_ptr<::vsg::Data> sampleVertexData(std::uint32_t components)
{
    switch (components) {
    case 1u: return ::vsg::floatArray::create(1);
    case 2u: return ::vsg::vec2Array::create(1);
    case 3u: return ::vsg::vec3Array::create(1);
    default: return ::vsg::vec4Array::create(1);
    }
}

std::string customAttributeName(std::uint32_t location)
{
    return "vine_Attribute" + std::to_string(location);
}

VkShaderStageFlagBits stageFlag(vine::graphics::ShaderStageType type)
{
    switch (type) {
    case vine::graphics::ShaderStageType::Fragment: return VK_SHADER_STAGE_FRAGMENT_BIT;
    case vine::graphics::ShaderStageType::Compute: return VK_SHADER_STAGE_COMPUTE_BIT;
    case vine::graphics::ShaderStageType::Vertex: return VK_SHADER_STAGE_VERTEX_BIT;
    }
    return VK_SHADER_STAGE_VERTEX_BIT;
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
