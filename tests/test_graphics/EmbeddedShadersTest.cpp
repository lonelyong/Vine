/**
 * @brief The shaders are embedded into the binary as whole GLSL sources (see cmake/VineShaders.cmake).
 *
 * The SDK's programs are no longer C++ string literals: each stage is a real .glsl file
 * under src/viz/graphics/shaders/, turned into string constants at build time. That
 * indirection is invisible to the renderer, so what these tests pin is that the
 * generated table is intact and is what the default programs actually use:
 *
 *  * every entry is a complete GLSL source (prologue, a main, closing brace) and not a
 *    truncated or line-ending-mangled copy;
 *  * the bookkeeping (name / hash / byte count) agrees with the text, so a corrupt
 *    embedding cannot pass unnoticed;
 *  * the default forward / G-buffer / deferred-light programs compile from exactly
 *    these constants, which is what makes editing the .glsl file the only way to
 *    change them.
 *
 * scripts/vine_shader_check.sh adds the two things a unit test cannot do: it compiles
 * every shader with glslangValidator and verifies the embedded hashes against the
 * files on disk.
 */

#include <gtest/gtest.h>

#include <vine/graphics/BuiltinShaders.hpp>
#include <vine/graphics/EmbeddedShaders.hpp>
#include <vine/graphics/RenderPipelineBuilder.hpp>
#include <vine/graphics/ShaderAbi.hpp>
#include <vine/graphics/ShaderProgram.hpp>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <iterator>
#include <string>
#include <string_view>
#include <vector>

using namespace vine::graphics;
using namespace vine::graphics::shaders;

namespace
{

/**
 * @brief Checks that @p text is one complete GLSL source.
 *
 * @param name Entry name, used in the failure messages.
 * @param text Source text to inspect.
 */
void expectWholeGlslSource(std::string_view name, std::u8string_view text)
{
    ASSERT_FALSE(text.empty()) << name;
    ASSERT_TRUE(text.starts_with(u8"#version 450\n")) << name;
    ASSERT_NE(text.find(u8"void main("), std::u8string_view::npos) << name;
    ASSERT_TRUE(text.ends_with(u8"}\n")) << name;
}

/**
 * @brief Checks that @p text is 16 lowercase hexadecimal digits.
 *
 * @param hash Hash field to inspect.
 * @return true when the field could be an abbreviated SHA-256.
 */
bool isAbbreviatedSha256(std::string_view hash)
{
    return hash.size() == 16 &&
           std::all_of(hash.begin(), hash.end(), [](char c) {
               return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f');
           });
}

/**
 * @brief Finds the embedded entry generated from @p file_name.
 *
 * @param file_name Source file name, e.g. "gbuffer_geometry.vert".
 * @return Pointer to the entry, or null when it was not embedded.
 */
const Entry* findEntry(std::string_view file_name)
{
    const auto it = std::find_if(std::begin(kAll), std::end(kAll),
                                 [file_name](const Entry& entry) { return entry.name == file_name; });
    return it == std::end(kAll) ? nullptr : &*it;
}

TEST(EmbeddedShadersTest, EveryShaderIsAWholeGlslSource)
{
    ASSERT_GT(kCount, 0u);
    for (const Entry& entry : kAll) {
        ASSERT_FALSE(entry.name.empty());
        expectWholeGlslSource(std::string(entry.name), entry.source);
    }
}

TEST(EmbeddedShadersTest, BookkeepingAgreesWithTheText)
{
    std::vector<std::string> names;
    for (const Entry& entry : kAll) {
        ASSERT_EQ(entry.bytes, entry.source.size()) << entry.name;
        ASSERT_TRUE(isAbbreviatedSha256(entry.hash)) << entry.name << ": " << entry.hash;
        names.emplace_back(entry.name);
    }
    std::sort(names.begin(), names.end());
    ASSERT_EQ(std::adjacent_find(names.begin(), names.end()), names.end());
}

/** @brief Reinterprets UTF-8 shader text as a searchable byte string (defined below). */
std::string asByteString(std::u8string_view text);

TEST(EmbeddedShadersTest, TheDeferredProgramsUseTheEmbeddedSources)
{
    const auto gbuffer = RenderPipelineBuilder::defaultGbufferGeometryProgram();
    ASSERT_NE(gbuffer, nullptr);
    ASSERT_EQ(gbuffer->stageCount(), 2u);
    const ShaderStage* geometry_vs = gbuffer->stage(0);
    const ShaderStage* geometry_fs = gbuffer->stage(1);
    ASSERT_NE(geometry_vs, nullptr);
    ASSERT_NE(geometry_fs, nullptr);
    EXPECT_EQ(geometry_vs->type, ShaderStageType::Vertex);
    EXPECT_EQ(geometry_fs->type, ShaderStageType::Fragment);
    EXPECT_EQ(geometry_vs->source, vine::String(kGbufferGeometryVert));
    EXPECT_EQ(geometry_fs->source, vine::String(kGbufferGeometryFrag));

    const auto light = RenderPipelineBuilder::defaultDeferredLightProgram();
    ASSERT_NE(light, nullptr);
    ASSERT_EQ(light->stageCount(), 1u);
    const ShaderStage* light_fs = light->stage(0);
    ASSERT_NE(light_fs, nullptr);
    EXPECT_EQ(light_fs->type, ShaderStageType::Fragment);
    // The lighting program is the embedded source with its two insertion markers taken out (the
    // shadowed variant is the same source with text put in their place): the FILE is what ships, and
    // a program that is not derived from it is a second copy that drifts.
    const std::string shipped  = asByteString(kDeferredLightFrag);
    const std::string markers  = "// VINE_SHADOW_BINDINGS\n";
    const std::string markers2 = "// VINE_SHADOW_TERM\n";
    std::string       expected = shipped;
    for (const std::string& marker : { markers, markers2 }) {
        const std::size_t at = expected.find(marker);
        ASSERT_NE(at, std::string::npos) << "the shipped source carries the insertion markers";
        expected.erase(at, marker.size());
    }
    EXPECT_EQ(light_fs->source.stdstr(), expected);
}

TEST(EmbeddedShadersTest, TheBuiltinForwardProgramsUseTheEmbeddedSources)
{
    // Scene shading is an SDK program like the deferred ones: its stages must be exactly the embedded
    // forward GLSL, so the program the engine hands out and the file on disk cannot drift apart.
    const auto forward = forwardProgram();
    ASSERT_NE(forward, nullptr);
    ASSERT_EQ(forward->stageCount(), 2u);
    const ShaderStage* forward_vs = forward->stage(0);
    const ShaderStage* forward_fs = forward->stage(1);
    ASSERT_NE(forward_vs, nullptr);
    ASSERT_NE(forward_fs, nullptr);
    EXPECT_EQ(forward_vs->type, ShaderStageType::Vertex);
    EXPECT_EQ(forward_fs->type, ShaderStageType::Fragment);
    EXPECT_EQ(forward_vs->source, vine::String(kStdForwardVert));
    EXPECT_EQ(forward_fs->source, vine::String(kStdForwardFrag));

    // The flat program is the SAME stages with one define injected into the fragment source — which is
    // what makes it a different program (a different text), not a mode of another one. The vertex stage
    // is the same object's text, so the two can only differ where the define is read.
    const auto flat = flatForwardProgram();
    ASSERT_NE(flat, nullptr);
    ASSERT_EQ(flat->stageCount(), 2u);
    const ShaderStage* flat_vs = flat->stage(0);
    const ShaderStage* flat_fs = flat->stage(1);
    ASSERT_NE(flat_vs, nullptr);
    ASSERT_NE(flat_fs, nullptr);
    EXPECT_EQ(flat_vs->source, vine::String(kStdForwardVert));
    EXPECT_NE(flat_fs->source, vine::String(kStdForwardFrag));
    EXPECT_NE(flat_fs->source.stdstr().find("#define VINE_FLAT 1"), std::string::npos);
}

/**
 * @brief The "layout(location = N) in" text an attribute declaration starts with.
 *
 * @param location Shader location.
 * @return The declaration prefix.
 */
std::string attributeLayout(std::uint32_t location)
{
    return "layout(location = " + std::to_string(location) + ") in";
}

/**
 * @brief Reinterprets UTF-8 shader text as a searchable byte string.
 *
 * @param text Shader source.
 * @return The same bytes as a std::string.
 */
std::string asByteString(std::u8string_view text)
{
    return std::string(reinterpret_cast<const char*>(text.data()), text.size());
}

TEST(EmbeddedShadersTest, TheAbiLocationsMatchTheShaderText)
{
    // The attribute location table is the engine's ABI (ShaderAbi.hpp); the shader
    // text must declare each role where the table says. A location changed in one
    // place only is exactly the drift this pins.
    EXPECT_EQ(attributeLocation(VertexAttribute::Position), 0u);
    EXPECT_EQ(attributeLocation(VertexAttribute::Normal), 1u);
    EXPECT_EQ(attributeLocation(VertexAttribute::Color), 2u);
    EXPECT_EQ(attributeLocation(VertexAttribute::TexCoord0), 8u);

    const std::string forward_vs = asByteString(kStdForwardVert);
    for (const auto role : { VertexAttribute::Position, VertexAttribute::Normal, VertexAttribute::Color,
                             VertexAttribute::TexCoord0 }) {
        EXPECT_NE(forward_vs.find(attributeLayout(attributeLocation(role))), std::string::npos)
            << "vertex attribute at location " << attributeLocation(role);
    }
}

TEST(EmbeddedShadersTest, TheShadowedLightingProgramIsBuiltFromMarkersTheSourceCarries)
{
    // The shadowed variant of the lighting program is the shipped source with two insertions, and
    // its bindings are part of the shadow ABI (see BuiltinShaders::deferredLightProgram): the map
    // at binding 5 and its block at 6, right after the canonical G-buffer's four colours and the
    // depth slot. A source edit that moves or renames a marker must fail HERE — the alternative is
    // a program that builds without complaint and whose shadow terms are whatever used to follow.
    const std::string source = asByteString(kDeferredLightFrag);
    EXPECT_NE(source.find("// VINE_SHADOW_BINDINGS"), std::string::npos) << "the declarations' marker";
    EXPECT_NE(source.find("// VINE_SHADOW_TERM"), std::string::npos) << "the term's marker";

    const auto plain = deferredLightProgram(/*with_shadow*/ false);
    ASSERT_NE(plain, nullptr);
    const std::string plain_text = plain->stage(0)->source.stdstr();
    EXPECT_EQ(plain_text.find("VINE_SHADOW_"), std::string::npos)
        << "the unshadowed program must ask the pass for no shadow binding at all";
    EXPECT_EQ(plain_text.find("shadow_map"), std::string::npos);

    const auto shadowed = deferredLightProgram(/*with_shadow*/ true);
    ASSERT_NE(shadowed, nullptr);
    const std::string shadowed_text = shadowed->stage(0)->source.stdstr();
    EXPECT_NE(shadowed_text.find("layout(binding = 5) uniform sampler2D shadow_map;"), std::string::npos);
    EXPECT_NE(shadowed_text.find("layout(binding = 6, std140) uniform VineShadowBlock"), std::string::npos)
        << "the block's GLSL type name is the L1 name (ShaderAbi.hpp)";
    EXPECT_NE(shadowed_text.find("shadow.viewToLight"), std::string::npos);
    EXPECT_EQ(shadowed_text.find("VINE_SHADOW_BINDINGS"), std::string::npos)
        << "the markers are scaffolding: the program text must not carry them";
    EXPECT_EQ(shadowed_text.find("VINE_SHADOW_TERM"), std::string::npos);
}

TEST(EmbeddedShadersTest, TheNamedConstantsAreInTheTable)
{
    ASSERT_NE(findEntry("std_forward.vert"), nullptr);
    ASSERT_NE(findEntry("std_forward.frag"), nullptr);
    ASSERT_NE(findEntry("gbuffer_geometry.vert"), nullptr);
    ASSERT_NE(findEntry("gbuffer_geometry.frag"), nullptr);
    ASSERT_NE(findEntry("deferred_light.frag"), nullptr);
    EXPECT_EQ(findEntry("std_forward.vert")->source, kStdForwardVert);
    EXPECT_EQ(findEntry("std_forward.frag")->source, kStdForwardFrag);
    EXPECT_EQ(findEntry("gbuffer_geometry.vert")->source, kGbufferGeometryVert);
    EXPECT_EQ(findEntry("gbuffer_geometry.frag")->source, kGbufferGeometryFrag);
    EXPECT_EQ(findEntry("deferred_light.frag")->source, kDeferredLightFrag);
}

}  // namespace
