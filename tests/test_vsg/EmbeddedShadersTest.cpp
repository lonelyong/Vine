/**
 * @brief The backend's shaders are embedded into the plugin as whole GLSL sources (see cmake/VineShaders.cmake).
 *
 * The overlay stages a PiP / blit pass needs are files under
 * src/plugins/gfx_backend_vsg/shaders/ turned into string constants at build time, so
 * nothing is copied next to the plugin DLL and nothing is committed as pre-compiled
 * SPIR-V. What these tests pin:
 *
 *  * the embedded table is intact (complete GLSL, bookkeeping agreeing with the text);
 *  * `fullscreenVertexSource()` really serves the embedded file, so the constant and the
 *    factory cannot drift apart;
 *  * the screen-texture fragment still declares the ABI the node builder wires
 *    (`screen_tex` at binding 0, `v_uv` in, `out_color` out) — the interface a device-free
 *    test can check, since building the node itself needs a Vulkan device.
 */

#include <gtest/gtest.h>

#include <vine/vsg/EmbeddedShaders.hpp>
#include <vine/vsg/VsgPipelineFactory.hpp>
#include <vine/vsg/VsgUtils.hpp>

#include <algorithm>
#include <cstddef>
#include <iterator>
#include <string>
#include <string_view>
#include <vector>

using namespace vine::vsg;

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
 * @brief Finds the embedded entry generated from @p file_name.
 *
 * @param file_name Source file name, e.g. "screen_texture.frag".
 * @return Pointer to the entry, or null when it was not embedded.
 */
const shaders::Entry* findEntry(std::string_view file_name)
{
    const auto it = std::find_if(std::begin(shaders::kAll), std::end(shaders::kAll),
                                 [file_name](const shaders::Entry& entry) { return entry.name == file_name; });
    return it == std::end(shaders::kAll) ? nullptr : &*it;
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

TEST(EmbeddedShadersTest, EveryShaderIsAWholeGlslSource)
{
    ASSERT_GT(shaders::kCount, 0u);
    for (const shaders::Entry& entry : shaders::kAll) {
        ASSERT_FALSE(entry.name.empty());
        expectWholeGlslSource(std::string(entry.name), entry.source);
    }
}

TEST(EmbeddedShadersTest, BookkeepingAgreesWithTheText)
{
    std::vector<std::string> names;
    for (const shaders::Entry& entry : shaders::kAll) {
        ASSERT_EQ(entry.bytes, entry.source.size()) << entry.name;
        ASSERT_TRUE(isAbbreviatedSha256(entry.hash)) << entry.name << ": " << entry.hash;
        names.emplace_back(entry.name);
    }
    std::sort(names.begin(), names.end());
    ASSERT_EQ(std::adjacent_find(names.begin(), names.end()), names.end());
}

TEST(EmbeddedShadersTest, TheFactoryServesTheEmbeddedFullscreenStage)
{
    const std::string& source = detail::fullscreenVertexSource();
    ASSERT_FALSE(source.empty());
    EXPECT_EQ(std::string_view(source), std::string_view(asShaderSource(shaders::kFullscreenVert)));
}

TEST(EmbeddedShadersTest, TheScreenTextureStageDeclaresTheOverlayAbi)
{
    const shaders::Entry* entry = findEntry("screen_texture.frag");
    ASSERT_NE(entry, nullptr);
    const std::u8string_view text = entry->source;
    // The node builder binds the pass input as "screen_tex" at binding 0 and the
    // fullscreen vertex stage writes v_uv as location 0.
    EXPECT_NE(text.find(u8"layout(binding = 0) uniform sampler2D screen_tex;"), std::u8string_view::npos);
    EXPECT_NE(text.find(u8"layout(location = 0) in vec2 v_uv;"), std::u8string_view::npos);
    EXPECT_NE(text.find(u8"layout(location = 0) out vec4 out_color;"), std::u8string_view::npos);
    EXPECT_NE(text.find(u8"texture(screen_tex, v_uv)"), std::u8string_view::npos);
}

TEST(EmbeddedShadersTest, TheForwardShadingIsNotABackendStage)
{
    // The built-in forward shading is the SDK's (BuiltinShaders.hpp): this table holds
    // only the backend's own plumbing stages, so the engine's shader cannot be
    // silently forked by a backend copy.
    EXPECT_EQ(findEntry("vine_forward.vert"), nullptr);
    EXPECT_EQ(findEntry("vine_forward.frag"), nullptr);
}

}  // namespace
