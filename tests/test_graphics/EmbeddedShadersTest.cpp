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
 *  * the default G-buffer and deferred-light programs compile from exactly these
 *    constants, which is what makes editing the .glsl file the only way to change them.
 *
 * scripts/vine_shader_check.sh adds the two things a unit test cannot do: it compiles
 * every shader with glslangValidator and verifies the embedded hashes against the
 * files on disk.
 */

#include <gtest/gtest.h>

#include <vine/graphics/EmbeddedShaders.hpp>
#include <vine/graphics/RenderPipelineBuilder.hpp>
#include <vine/graphics/ShaderProgram.hpp>

#include <algorithm>
#include <cstddef>
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
    EXPECT_EQ(light_fs->source, vine::String(kDeferredLightFrag));
}

TEST(EmbeddedShadersTest, TheNamedConstantsAreInTheTable)
{
    ASSERT_NE(findEntry("gbuffer_geometry.vert"), nullptr);
    ASSERT_NE(findEntry("gbuffer_geometry.frag"), nullptr);
    ASSERT_NE(findEntry("deferred_light.frag"), nullptr);
    EXPECT_EQ(findEntry("gbuffer_geometry.vert")->source, kGbufferGeometryVert);
    EXPECT_EQ(findEntry("gbuffer_geometry.frag")->source, kGbufferGeometryFrag);
    EXPECT_EQ(findEntry("deferred_light.frag")->source, kDeferredLightFrag);
}

}  // namespace
