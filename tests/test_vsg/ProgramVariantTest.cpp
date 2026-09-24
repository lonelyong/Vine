/**
 * @brief Device-free tests of the PROGRAM VARIANT: the defines that change what a text means.
 *
 * The engine's own stages gate declarations and code on names in a `#pragma import_defines` line, so one
 * program text is several programs - several ABIs and several pictures. This suite pins the three
 * switches' NAMES, the engine's rule for picking them from a drawable (the texcoord channel's width, the
 * material's texture, the geometry's authored colour), and the fact that the engine's own forward program
 * really does describe itself differently per variant: a layout scanned for the wrong variant is a
 * pipeline the driver refuses at best, and the wrong picture at worst.
 */

#include <cstdint>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include <vine/graphics/BuiltinShaders.hpp>
#include <vine/graphics/ShaderAbi.hpp>
#include <vine/graphics/Texture.hpp>

#include <vine/vsg/api/ContentSources.hpp>
#include <vine/vsg/api/GeometryFacts.hpp>
#include <vine/vsg/api/ProgramVariant.hpp>

using vn::graphics::attributeLocation;
using vn::graphics::VertexAttribute;
using vn::graphics::Texture2D;
using vn::imaging::PixelFormat;
using vn::vsg::AbiBinding;
using vn::vsg::AbiDescriptorKind;
using vn::vsg::buildProgramFacts;
using vn::vsg::ChannelFacts;
using vn::vsg::FactMiss;
using vn::vsg::GeometryFacts;
using vn::vsg::MaterialFacts;
using vn::vsg::ProgramFacts;
using vn::vsg::ProgramVariant;
using vn::vsg::variantOf;

namespace
{

/// @brief A non-null buffer identity: a channel that is AUTHORED rather than derived by the backend.
const void* const kAuthoredBuffer = reinterpret_cast<const void*>(0x11U);

/// @brief One vertex channel of a geometry, as the facts table would carry it.
ChannelFacts channelAt(std::uint32_t location, std::uint32_t components, const void* buffer = kAuthoredBuffer)
{
    ChannelFacts facts;
    facts.key.kind       = vn::vsg::core::StreamKind::Vertex;
    facts.key.location   = location;
    facts.key.components = components;
    facts.key.buffer     = buffer;
    facts.key.count      = 3U;
    return facts;
}

/// @brief A geometry facts entry over a fixed channel list.
GeometryFacts geometryWith(std::vector<ChannelFacts>& storage)
{
    GeometryFacts facts;
    facts.geometry = reinterpret_cast<const void*>(0x21U);
    facts.revision = 1U;
    facts.channels = std::span<const ChannelFacts>(storage);
    return facts;
}

}  // namespace

TEST(ProgramVariantTest, TheDefinesAreTheEnginesOwnNames)
{
    // Exactly one KIND is always named: the engine's stages `#error` when a sampled texcoord slot has no
    // kind, so "no kind" is not a variant that builds - it is a program that does not sample the slot.
    const ProgramVariant flat;
    EXPECT_FALSE(flat.cube_texcoord);
    const std::vector<std::string> flat_names = flat.defines();
    ASSERT_EQ(flat_names.size(), 1U);
    EXPECT_EQ(flat_names[0], "VINE_TEXCOORD_UV");

    ProgramVariant cube;
    cube.cube_texcoord = true;
    cube.diffuse_map   = true;
    cube.vertex_color  = true;
    const std::vector<std::string> cube_names = cube.defines();
    ASSERT_EQ(cube_names.size(), 3U);
    EXPECT_EQ(cube_names[0], "VINE_TEXCOORD_CUBE");
    EXPECT_EQ(cube_names[1], "VINE_DIFFUSE_MAP");
    EXPECT_EQ(cube_names[2], "VINE_VERTEX_COLOR");

    // The number the pipeline key carries must tell the variants apart - all of them.
    std::vector<std::uint32_t> seen;
    for (int mask = 0; mask < 8; ++mask) {
        ProgramVariant variant;
        variant.diffuse_map   = (mask & 1) != 0;
        variant.vertex_color  = (mask & 2) != 0;
        variant.cube_texcoord = (mask & 4) != 0;
        const std::uint32_t bits = variant.bits();
        EXPECT_EQ(std::find(seen.begin(), seen.end(), bits), seen.end()) << "two variants share a key number";
        seen.push_back(bits);
    }
    EXPECT_EQ(seen.size(), 8U);
}

TEST(ProgramVariantTest, TheRuleFollowsTheChannelWidthTheTextureAndTheAuthoredColour)
{
    // Nothing to sample and no authored colour: the variant the engine's stages describe with no optional
    // declaration at all - and the UV kind, which every variant states.
    std::vector<ChannelFacts> bare{ channelAt(attributeLocation(VertexAttribute::Position), 3U) };
    {
        GeometryFacts geometry = geometryWith(bare);
        MaterialFacts material;
        const ProgramVariant variant = variantOf(material, geometry);
        EXPECT_FALSE(variant.diffuse_map);
        EXPECT_FALSE(variant.vertex_color);
        EXPECT_FALSE(variant.cube_texcoord);
    }

    // A material with a texture asks for the sampler...
    std::vector<ChannelFacts> uvs{ channelAt(attributeLocation(VertexAttribute::Position), 3U),
                                   channelAt(attributeLocation(VertexAttribute::TexCoord0), 2U) };
    {
        GeometryFacts geometry = geometryWith(uvs);
        const vn::intrusive_ptr<Texture2D> texture(new Texture2D(2, 2, PixelFormat::Rgba8Unorm));
        MaterialFacts material;
        material.texture = texture.get();
        const ProgramVariant variant = variantOf(material, geometry);
        EXPECT_TRUE(variant.diffuse_map);
        EXPECT_FALSE(variant.cube_texcoord) << "two scalars are a UV pair";
    }

    // ... and the WIDTH of the slot it samples decides which sampler the text asks for: three scalars are
    // a direction.
    std::vector<ChannelFacts> directions{ channelAt(attributeLocation(VertexAttribute::Position), 3U),
                                          channelAt(attributeLocation(VertexAttribute::TexCoord0), 3U) };
    {
        GeometryFacts geometry = geometryWith(directions);
        MaterialFacts material;
        material.texture = reinterpret_cast<const vn::graphics::Texture*>(kAuthoredBuffer);
        EXPECT_TRUE(variantOf(material, geometry).cube_texcoord);
    }

    // The colour define follows an AUTHORED colour channel: a derived one (the backend's own white
    // carrier) is not a colour the geometry wrote, so shading it with a multiply would only cost.
    std::vector<ChannelFacts> authored{ channelAt(attributeLocation(VertexAttribute::Position), 3U),
                                        channelAt(attributeLocation(VertexAttribute::Color), 4U) };
    std::vector<ChannelFacts> derived{ channelAt(attributeLocation(VertexAttribute::Position), 3U),
                                       channelAt(attributeLocation(VertexAttribute::Color), 4U, /*buffer*/ nullptr) };
    {
        GeometryFacts authored_geometry = geometryWith(authored);
        GeometryFacts derived_geometry  = geometryWith(derived);
        MaterialFacts material;
        EXPECT_TRUE(variantOf(material, authored_geometry).vertex_color);
        EXPECT_FALSE(variantOf(material, derived_geometry).vertex_color);
    }
}

TEST(ProgramVariantTest, TheEnginesOwnForwardProgramDescribesItselfPerVariant)
{
    const vn::intrusive_ptr<vn::graphics::ShaderProgram> program = vn::graphics::forwardProgram();
    ASSERT_NE(program, nullptr);

    const auto samplerAt = [](const ProgramFacts& facts, std::uint32_t set, std::uint32_t binding) {
        for (const AbiBinding& declared : facts.abi.bindings) {
            if (declared.set == set && declared.binding == binding) {
                return declared.kind;
            }
        }
        return AbiDescriptorKind::UniformBlock;  // "absent": the block kind stands for nothing declared
    };

    // WITHOUT the map's define the engine's forward stage declares no diffuse sampler at all - the
    // declaration sits behind `#ifdef VINE_DIFFUSE_MAP` - and its ABI is the material block, the lights
    // and the rest.
    ProgramFacts without;
    ASSERT_EQ(buildProgramFacts(*program, ProgramVariant{}, without), FactMiss::None);
    EXPECT_EQ(samplerAt(without, 0U, 1U), AbiDescriptorKind::UniformBlock)
        << "an untextured variant declares no sampler at the map's binding";
    ASSERT_EQ(without.shaders.defines.size(), 1U);
    EXPECT_EQ(without.shaders.defines[0], "VINE_TEXCOORD_UV");

    // WITH it, the same text describes a sampler at (0,1) - and its KIND follows the variant: a UV pair
    // asks for `sampler2D`, a three-scalar slot for `samplerCube`. A layout scanned for the wrong one is
    // a descriptor the shader's sampler type does not match.
    ProgramVariant textured;
    textured.diffuse_map = true;

    ProgramVariant uv_variant = textured;
    ProgramFacts   uv_facts;
    ASSERT_EQ(buildProgramFacts(*program, uv_variant, uv_facts), FactMiss::None);
    EXPECT_EQ(samplerAt(uv_facts, 0U, 1U), AbiDescriptorKind::Sampler2D);

    ProgramVariant cube_variant = textured;
    cube_variant.cube_texcoord  = true;
    ProgramFacts cube_facts;
    ASSERT_EQ(buildProgramFacts(*program, cube_variant, cube_facts), FactMiss::None);
    EXPECT_EQ(samplerAt(cube_facts, 0U, 1U), AbiDescriptorKind::SamplerCube);

    // Both variants come from ONE text: the ABI differs, the source does not.
    EXPECT_EQ(uv_facts.shaders.fragment, cube_facts.shaders.fragment);
    EXPECT_NE(uv_facts.shaders.defines, cube_facts.shaders.defines);
}
