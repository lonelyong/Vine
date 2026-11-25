#include <vine/graphics/ProgramVariant.hpp>

#include <string>
#include <vector>

#include <vine/graphics/Geometry.hpp>
#include <vine/graphics/MaterialValues.hpp>
#include <vine/graphics/ShaderAbi.hpp>

VN_GRAPHICS_NS_BEGIN

std::vector<std::string> ProgramVariant::defines() const
{
    std::vector<std::string> names;
    names.reserve(4U);
    // The KIND is not optional (see the struct note): it is the one name every variant states, because an engine
    // stage that samples the texcoord slot refuses to build without it.
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

ProgramVariant variantOf(const GeometryDescription& geometry, const MaterialDescription* material, const bool instanced) noexcept
{
    using vn::graphics::attributeLocation;
    using vn::graphics::VertexAttribute;

    ProgramVariant variant;

    // ANY present slot asks for the textured variant: the switch is "has a texture set", not "which slots" (see the
    // struct note on why per-slot bits would multiply the cache).
    if (material != nullptr) {
        for (const TextureSlotBinding& slot : material->values().textures) {
            variant.textured = variant.textured || slot.texture != nullptr;
        }
    }

    for (const GeometryChannel& channel : geometry.channels()) {
        if (channel.channel.empty()) {
            continue;
        }
        if (channel.location == attributeLocation(VertexAttribute::TexCoord0)) {
            // The WIDTH is the kind: three scalars per vertex is a direction (a cube sampler), two is a UV pair.
            variant.cube_texcoord = channel.channel.components == 3U;
        }
        else if (channel.location == attributeLocation(VertexAttribute::Color)) {
            variant.vertex_color = true; // no backend derives a channel of its own, so a fed colour is an authored one
        }
    }

    variant.instanced = instanced;
    return variant;
}

VN_GRAPHICS_NS_END
