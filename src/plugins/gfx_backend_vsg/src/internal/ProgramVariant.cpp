#include <vine/vsg/internal/ProgramVariant.hpp>

#include <vine/graphics/ShaderAbi.hpp>

#include <vine/vsg/internal/ContentFacts.hpp>
#include <vine/vsg/internal/GeometryFacts.hpp>

VN_VSG_NS_BEGIN

std::uint32_t ProgramVariant::bits() const noexcept
{
    // One bit per switch, and the bits ARE the spelling `core::PipelineKey::variant` carries: a key that
    // compared two variants by anything else would have to know this mapping, which is exactly what the
    // core does not (see the class note).
    return (textured ? 1U : 0U) | (vertex_color ? 2U : 0U) | (cube_texcoord ? 4U : 0U) |
           (instanced ? 8U : 0U);
}

std::vector<std::string> ProgramVariant::defines() const
{
    std::vector<std::string> names;
    names.reserve(4U);
    // The kind is NOT optional (see the declaration): it is the one name every variant states, because a
    // stage that samples the slot without it refuses to build.
    names.emplace_back(cube_texcoord ? "VN_TEXCOORD_CUBE" : "VN_TEXCOORD_UV");
    if (textured) {
        names.emplace_back("VN_TEXTURED");
    }
    if (vertex_color) {
        names.emplace_back("VN_VERTEX_COLOR");
    }
    if (instanced) {
        names.emplace_back("VN_INSTANCED");
    }
    return names;
}

std::string ProgramVariant::describe() const
{
    std::string text = cube_texcoord ? "VN_TEXCOORD_CUBE" : "VN_TEXCOORD_UV";
    if (textured) {
        text += ", VN_TEXTURED";
    }
    if (vertex_color) {
        text += ", VN_VERTEX_COLOR";
    }
    if (instanced) {
        text += ", VN_INSTANCED";
    }
    return text;
}

bool operator==(const ProgramVariant& left, const ProgramVariant& right) noexcept
{
    return left.textured == right.textured && left.vertex_color == right.vertex_color &&
           left.cube_texcoord == right.cube_texcoord && left.instanced == right.instanced;
}

bool operator!=(const ProgramVariant& left, const ProgramVariant& right) noexcept
{
    return !(left == right);
}

ProgramVariant variantOf(const MaterialFacts& material, const GeometryFacts& geometry) noexcept
{
    using vn::graphics::attributeLocation;
    using vn::graphics::VertexAttribute;

    ProgramVariant variant;
    // ANY present slot asks for the textured variant: the variant is "has a texture set", not "which slots"
    // (five per-slot bits would multiply the pipeline cache by 32, graphics-material.md §2.5).
    for (const MaterialSlotFacts& slot : material.slots) {
        variant.textured = variant.textured || slot.texture != nullptr;
    }

    for (const ChannelFacts& channel : geometry.channels) {
        if (channel.key.kind != core::StreamKind::Vertex) {
            continue;
        }
        if (channel.key.location == attributeLocation(VertexAttribute::TexCoord0)) {
            // The WIDTH is the kind: three scalars per vertex is a direction (see the declaration).
            variant.cube_texcoord = channel.key.components == 3U;
        } else if (channel.key.location == attributeLocation(VertexAttribute::Color)) {
            // An AUTHORED colour only: a derived channel is the backend's own white carrier, and shading
            // it with a multiply that cannot change the picture is what the define would add.
            variant.vertex_color = !channel.key.derived();
        }
    }
    return variant;
}

VN_VSG_NS_END
