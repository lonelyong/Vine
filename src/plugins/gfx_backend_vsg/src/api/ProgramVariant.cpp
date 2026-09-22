#include <vine/vsg/api/ProgramVariant.hpp>

#include <vine/graphics/ShaderAbi.hpp>

#include <vine/vsg/api/ContentFacts.hpp>
#include <vine/vsg/api/GeometryFacts.hpp>

V_VSG_NS_BEGIN

std::uint32_t ProgramVariant::bits() const noexcept
{
    // One bit per switch, and the bits ARE the spelling `core::PipelineKey::variant` carries: a key that
    // compared two variants by anything else would have to know this mapping, which is exactly what the
    // core does not (see the class note).
    return (diffuse_map ? 1U : 0U) | (vertex_color ? 2U : 0U) | (cube_texcoord ? 4U : 0U);
}

std::vector<std::string> ProgramVariant::defines() const
{
    std::vector<std::string> names;
    names.reserve(3U);
    // The kind is NOT optional (see the declaration): it is the one name every variant states, because a
    // stage that samples the slot without it refuses to build.
    names.emplace_back(cube_texcoord ? "VINE_TEXCOORD_CUBE" : "VINE_TEXCOORD_UV");
    if (diffuse_map) {
        names.emplace_back("VINE_DIFFUSE_MAP");
    }
    if (vertex_color) {
        names.emplace_back("VINE_VERTEX_COLOR");
    }
    return names;
}

std::string ProgramVariant::describe() const
{
    std::string text = cube_texcoord ? "VINE_TEXCOORD_CUBE" : "VINE_TEXCOORD_UV";
    if (diffuse_map) {
        text += ", VINE_DIFFUSE_MAP";
    }
    if (vertex_color) {
        text += ", VINE_VERTEX_COLOR";
    }
    return text;
}

bool operator==(const ProgramVariant& left, const ProgramVariant& right) noexcept
{
    return left.diffuse_map == right.diffuse_map && left.vertex_color == right.vertex_color &&
           left.cube_texcoord == right.cube_texcoord;
}

bool operator!=(const ProgramVariant& left, const ProgramVariant& right) noexcept
{
    return !(left == right);
}

ProgramVariant variantOf(const MaterialFacts& material, const GeometryFacts& geometry) noexcept
{
    using vine::graphics::attributeLocation;
    using vine::graphics::VertexAttribute;

    ProgramVariant variant;
    variant.diffuse_map = material.texture != nullptr;

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

V_VSG_NS_END
