#include <vine/backend_vk/internal/ProgramVariant.hpp>

#include <vine/graphics/ShaderAbi.hpp>

VN_BACKEND_VK_NS_BEGIN

std::uint32_t ProgramVariant::bits() const noexcept
{
    // One bit per switch, and the bits ARE the number the program cache and the pipeline key carry: a reader that
    // compared two variants by anything else would have to know this mapping, which is what the type exists to avoid.
    return (textured ? 1U : 0U) | (vertex_color ? 2U : 0U) | (cube_texcoord ? 4U : 0U) | (instanced ? 8U : 0U);
}

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
    return left.textured == right.textured && left.vertex_color == right.vertex_color && left.cube_texcoord == right.cube_texcoord &&
           left.instanced == right.instanced;
}

bool operator!=(const ProgramVariant& left, const ProgramVariant& right) noexcept
{
    return !(left == right);
}

ProgramVariant variantOf(const vn::graphics::GeometryDescription& geometry,
                         const vn::graphics::MaterialDescription*  material,
                         const bool                                instanced) noexcept
{
    using vn::graphics::attributeLocation;
    using vn::graphics::VertexAttribute;

    ProgramVariant variant;

    // ANY present slot asks for the textured variant: the switch is "has a texture set", not "which slots" (see the
    // struct note on why five per-slot bits would multiply the cache).
    if (material != nullptr) {
        for (const vn::graphics::TextureSlotBinding& slot : material->values().textures) {
            variant.textured = variant.textured || slot.texture != nullptr;
        }
    }

    for (const vn::graphics::GeometryChannel& channel : geometry.channels()) {
        if (channel.channel.empty()) {
            continue;
        }
        if (channel.location == attributeLocation(VertexAttribute::TexCoord0)) {
            // The WIDTH is the kind: three scalars per vertex is a direction (a cube sampler), two is a UV pair.
            variant.cube_texcoord = channel.channel.components == 3U;
        }
        else if (channel.location == attributeLocation(VertexAttribute::Color)) {
            variant.vertex_color = true; // this backend derives no channel, so a fed colour is an authored one
        }
    }

    variant.instanced = instanced;
    return variant;
}

VN_BACKEND_VK_NS_END
