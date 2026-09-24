#include <vine/vsg/VsgSceneRules.hpp>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>
#include <span>
#include <string>

#include <vsg/core/Array.h>
#include <vsg/maths/vec3.h>
#include <vsg/state/ColorBlendState.h>
#include <vsg/utils/ShaderSet.h>

#include <vine/imaging/Image.hpp>
#include <vine/vsg/VsgUtils.hpp>

VN_VSG_NS_BEGIN

// The bridge's device-free rules: what a custom vertex channel may be, which colour attachments a
// shader set declares, what an opaque multi-attachment pipeline writes, and the cache-key hashing.
// They are declared and explained in VsgSceneRules.hpp; this unit exists so a test can call them
// without a device.

namespace detail
{

ChannelShape channelShape(const vn::graphics::AttributeChannel& attr, std::size_t vertex_count)
{
    if (attr.components < 1u || attr.components > 4u) {
        return ChannelShape::Components;
    }
    if (attr.floatCount() % attr.components != 0u) {
        return ChannelShape::NotDivisible;
    }
    if (attr.vertexCount() != vertex_count) {
        return ChannelShape::VertexCount;
    }
    return ChannelShape::Ok;
}

vn::String ignoredChannelMessage(std::uint32_t location, const vn::graphics::AttributeChannel& attr,
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
                                location, attr.floatCount(), attr.components);
    case ChannelShape::VertexCount:
        return formatDiagnostic(u8"loc%u custom channel has %zu vertices, expected %zu; "
                                u8"channel ignored",
                                location, attr.vertexCount(), vertex_count);
    case ChannelShape::Ok:
        break;
    }
    return vn::String();
}

XyzUnpack unpackXyz(const vn::graphics::AttributeChannel& attr, vn::geometry::Vec3fArray& out)
{
    const auto             comps = attr.components;
    const std::span<const float> data = attr.scalars();
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

vn::String ignoredNormalChannelMessage(const vn::graphics::AttributeChannel& attr, XyzUnpack reason)
{
    if (reason == XyzUnpack::NotXyzStride) {
        return formatDiagnostic(u8"loc1 normal has components=%u (3 or 4 required); "
                                u8"normals will be derived",
                                attr.components);
    }
    return formatDiagnostic(u8"loc1 normal holds %zu floats, not divisible by its "
                            u8"components=%u stride; normals will be derived",
                            attr.floatCount(), attr.components);
}

vn::math::Vec3f faceNormal(const vn::math::Vec3f& a, const vn::math::Vec3f& b, const vn::math::Vec3f& c)
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

::vsg::ref_ptr<::vsg::vec2Array> makeZeroTexcoords(std::size_t count)
{
    auto texcoords = ::vsg::vec2Array::create(static_cast<uint32_t>(count));
    for (auto& uv : *texcoords) {
        uv = ::vsg::vec2(0.0f, 0.0f);
    }
    return texcoords;
}

::vsg::ref_ptr<::vsg::Data> texCoordArray(const vn::graphics::AttributeChannel& attr, std::size_t vertex_count)
{
    const auto comps = attr.components;
    if (attr.empty() || attr.floatCount() != vertex_count * comps) {
        return {};
    }
    if (comps == 3u) {
        auto directions                = aliasArray<::vsg::vec3Array, float>(attr.values, vertex_count, attr.offset);
        directions->properties.format  = VK_FORMAT_R32G32B32_SFLOAT;
        return directions;
    }
    if (comps == 2u) {
        auto uv                = aliasArray<::vsg::vec2Array, float>(attr.values, vertex_count, attr.offset);
        uv->properties.format  = VK_FORMAT_R32G32_SFLOAT;
        return uv;
    }
    return {};
}

bool isThreeScalarTexcoord(const ::vsg::Data& array) noexcept
{
    return array.properties.format == VK_FORMAT_R32G32B32_SFLOAT;
}

VkFormat vkFormatFor(vn::imaging::PixelFormat format) noexcept
{
    using vn::imaging::PixelFormat;

    switch (format) {
        case PixelFormat::R8Unorm:
            return VK_FORMAT_R8_UNORM;
        case PixelFormat::R8Srgb:
            return VK_FORMAT_R8_SRGB;
        case PixelFormat::Rg8Unorm:
            return VK_FORMAT_R8G8_UNORM;
        case PixelFormat::Rgba8Unorm:
            return VK_FORMAT_R8G8B8A8_UNORM;
        case PixelFormat::Rgba8Srgb:
            return VK_FORMAT_R8G8B8A8_SRGB;
        case PixelFormat::Bgra8Unorm:
            return VK_FORMAT_B8G8R8A8_UNORM;
        case PixelFormat::Bgra8Srgb:
            return VK_FORMAT_B8G8R8A8_SRGB;

        case PixelFormat::R16Float:
            return VK_FORMAT_R16_SFLOAT;
        case PixelFormat::Rg16Float:
            return VK_FORMAT_R16G16_SFLOAT;
        case PixelFormat::Rgba16Float:
            return VK_FORMAT_R16G16B16A16_SFLOAT;
        case PixelFormat::R32Float:
            return VK_FORMAT_R32_SFLOAT;
        case PixelFormat::Rg32Float:
            return VK_FORMAT_R32G32_SFLOAT;
        case PixelFormat::Rgba32Float:
            return VK_FORMAT_R32G32B32A32_SFLOAT;

        // Depth has a sampleable format, so a depth image is a legal texture (a program that
        // reconstructs positions from it is why ImageRef has a Depth kind).
        case PixelFormat::D16Unorm:
            return VK_FORMAT_D16_UNORM;
        case PixelFormat::D24UnormS8Uint:
            return VK_FORMAT_D24_UNORM_S8_UINT;
        case PixelFormat::D32Float:
            return VK_FORMAT_D32_SFLOAT;

        // No 24-bit format exists, and Unknown is not a layout.
        case PixelFormat::Rgb8Unorm:
        case PixelFormat::Rgb8Srgb:
        case PixelFormat::Unknown:
            return VK_FORMAT_UNDEFINED;
    }

    return VK_FORMAT_UNDEFINED; // Out-of-range cast.
}

namespace
{

/**
 * @brief Counts how many of a texture's faces have been filled.
 *
 * @param texture Texture to inspect.
 * @return The number of faces holding a source image.
 */
int filledFaceCount(const vn::graphics::Texture& texture) noexcept
{
    int filled = 0;
    for (int face = 0; face < texture.faceCount(); ++face) {
        if (texture.hasSource(face)) {
            ++filled;
        }
    }
    return filled;
}

} // namespace

TextureReject classifyTexture(const vn::graphics::Texture* texture) noexcept
{
    using vn::graphics::Texture;

    if (texture == nullptr) {
        return TextureReject::Absent;
    }
    // Both shapes this backend knows are uploaded: a cube is six 2D layers read through a cube view, so it
    // goes down the same path as a 2D texture. The switch is exhaustive with NO default, which is what makes
    // adding a shape a compile-time decision (-Wswitch) instead of a silent upload that treats it as one
    // layer. The reject value and its diagnostic come back with the first shape that needs them, together
    // with the test that exercises them.
    switch (texture->kind()) {
        case Texture::Kind::D2:
        case Texture::Kind::Cube:
            break;
    }
    if (!texture->isComplete()) {
        return TextureReject::Incomplete;
    }
    if (vkFormatFor(texture->format()) == VK_FORMAT_UNDEFINED) {
        return TextureReject::UnsupportedFormat;
    }
    // Last, because it is the only rule that touches the pixels: a texture that is still being filled is a
    // normal intermediate state, while one whose data already disagrees with its extent is a caller's bug
    // that the upload would otherwise act on (a copy past the staging buffer — see the rule's own notes).
    if (!textureDataMatchesExtent(*texture)) {
        return TextureReject::Inconsistent;
    }

    return TextureReject::Ok;
}

std::uint32_t levelExtent(int size, std::size_t level) noexcept
{
    // A level beyond the bit width has collapsed to 1 already; shifting by that much would be undefined.
    if (level >= 32u) {
        return 1u;
    }

    const auto shifted = static_cast<unsigned>(size) >> level;
    return (shifted == 0u) ? 1u : shifted;
}

bool textureDataMatchesExtent(const vn::graphics::Texture& texture) noexcept
{
    const auto bytes_per_texel = static_cast<std::size_t>(vn::imaging::bytesPerPixel(texture.format()));
    const auto level_count     = static_cast<std::size_t>(texture.mipCount());
    const auto layer_count     = static_cast<std::size_t>(texture.layerCount());
    if (bytes_per_texel == 0u || level_count == 0u || layer_count == 0u) {
        return false;
    }
    // The offsets are the running sum of the levels' sizes, and vsg carries one in a 32-bit field (see the
    // declaration): the check that the chain fits is what keeps that field from wrapping.
    constexpr std::size_t kMaxOffset = static_cast<std::size_t>(std::numeric_limits<std::uint32_t>::max());
    std::size_t           offset     = 0u;
    for (std::size_t level = 0; level < level_count; ++level) {
        const auto width  = static_cast<std::size_t>(levelExtent(texture.width(), level));
        const auto height = static_cast<std::size_t>(levelExtent(texture.height(), level));
        const auto span   = bytes_per_texel * width * height; // one layer of this level
        for (std::size_t layer = 0; layer < layer_count; ++layer) {
            const auto* source = texture.layer(static_cast<int>(layer));
            if (source == nullptr || source->mipData(static_cast<int>(level)).size() != span) {
                return false;
            }
        }
        offset += span * layer_count;
        if (offset > kMaxOffset) {
            return false;
        }
    }
    return true;
}

float anisotropyFor(float device_limit) noexcept
{
    // The device may offer more than is worth paying for, and it may offer nothing (Vulkan's floor is 1.0).
    constexpr float kCeiling = 16.0f;

    if (device_limit < 1.0f) {
        return 1.0f;
    }

    return (device_limit > kCeiling) ? kCeiling : device_limit;
}

vn::String textureRejectMessage(TextureReject reason, const vn::graphics::Texture& texture)
{
    switch (reason) {
        case TextureReject::Incomplete:
            return formatDiagnostic(u8"texture is incomplete (%d of %d face(s) filled); "
                                    u8"the material renders untextured",
                                    filledFaceCount(texture), texture.faceCount());

        case TextureReject::UnsupportedFormat:
            return formatDiagnostic(u8"texture pixel layout '%s' has no Vulkan format; "
                                    u8"the material renders untextured",
                                    vn::imaging::formatName(texture.format()));

        case TextureReject::Inconsistent:
            return formatDiagnostic(u8"texture pixel data does not account for its description "
                                    u8"(%dx%d, %d level(s), %d layer(s), layout '%s'); "
                                    u8"the material renders untextured",
                                    texture.width(), texture.height(), texture.mipCount(), texture.layerCount(),
                                    vn::imaging::formatName(texture.format()));

        // Absent is the normal "this material has no texture" case and is not a diagnostic; Ok is never
        // reported.
        case TextureReject::Ok:
        case TextureReject::Absent:
            break;
    }

    return vn::String();
}

::vsg::ref_ptr<::vsg::vec3Array> makeNormals(std::span<const vn::math::Vec3f> positions,
                                            std::span<const vn::math::Vec3f> meshNormals)
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
        const vn::math::Vec3f n0 = faceNormal(positions[i], positions[i + 1], positions[i + 2]);
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

::vsg::ref_ptr<::vsg::vec3Array> makeIndexedNormals(std::span<const vn::math::Vec3f> positions,
                                                   std::span<const vn::math::Vec3f> meshNormals,
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
        const vn::math::Vec3f n = faceNormal(positions[ia], positions[ib], positions[ic]);
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

::vsg::ref_ptr<::vsg::Data> aliasTypedVertexData(std::uint32_t components,
                                                intrusive_ptr<const vn::Buffer<float>> values,
                                                std::size_t vertex_count, std::size_t offset_scalars)
{
    switch (components) {
    case 1u:
        return aliasArray<::vsg::floatArray, float>(std::move(values), vertex_count, offset_scalars);
    case 2u:
        return aliasArray<::vsg::vec2Array, float>(std::move(values), vertex_count, offset_scalars);
    case 3u:
        return aliasArray<::vsg::vec3Array, float>(std::move(values), vertex_count, offset_scalars);
    default:
        return aliasArray<::vsg::vec4Array, float>(std::move(values), vertex_count, offset_scalars);
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

VkShaderStageFlagBits stageFlag(vn::graphics::ShaderStageType type)
{
    switch (type) {
    case vn::graphics::ShaderStageType::Fragment: return VK_SHADER_STAGE_FRAGMENT_BIT;
    case vn::graphics::ShaderStageType::Compute: return VK_SHADER_STAGE_COMPUTE_BIT;
    case vn::graphics::ShaderStageType::Vertex: return VK_SHADER_STAGE_VERTEX_BIT;
    }
    return VK_SHADER_STAGE_VERTEX_BIT;
}

std::uint64_t hashStateVariant(const vn::graphics::ShaderProgram* program, const vn::graphics::Material* material,
                               const void* texture_resource, const vn::graphics::ResolvedRenderState& state,
                               std::uint64_t layout)
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
    // The resolved texture resource, so two materials sharing one Phong value but sampling different
    // images never share a descriptor bind.
    mix_ptr(texture_resource);
    h = hashCombine(h, layout);
    // NO part of the resolved state is mixed in: every item of it is delivered per drawable now (see
    // VsgDynamicState.hpp), so the state a drawable resolves to must not move it between variants — one
    // pipeline serves them all, which is the whole point of the layer. What is left is what a pipeline is
    // really a function of: the program (and its revision), the material, the bound texture resource and the
    // geometry's vertex layout.
    (void)state;
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

VN_VSG_NS_END
