/**
 * @brief Full-screen program depth-binding tests (device-free).
 *
 * The full-screen program ABI (see makeFullscreenProgramNode) gives a source's
 * colour attachments binding indices 0..N-1 and its depth the binding index N, so
 * whether a program samples the depth is decidable from the fragment stage's
 * declared bindings alone — which is what programSamplesDepth() answers, and what
 * the promotion-revoke cascade uses to tell a slot that has a depth descriptor to
 * lose from a colour-only one that keeps drawing.
 *
 * The invariants pinned here are the ones an off-by-one would silently break:
 *
 *  - the depth index FOLLOWS the colour count, so the same binding is a colour
 *    attachment of a wider source;
 *  - only the fragment stage decides (a vertex stage cannot sample, and the ABI's
 *    bindings live in set 0);
 *  - a program that declares nothing (or no binding at all) binds no depth.
 */

#include <gtest/gtest.h>

#include <vine/graphics/ShaderProgram.hpp>

#include <vine/vsg/VsgBackendUtility.hpp>
#include <vine/vsg/VsgPipelineFactory.hpp>

using vine::vsg::detail::programImportsDefine;
using vine::vsg::detail::programSamplesDepth;

namespace
{

/** @brief Builds a single-stage program with the given GLSL body.
 *
 * @param type    Stage type to create.
 * @param source  GLSL source of that stage.
 * @return The program, owning the stage.
 */
vine::intrusive_ptr<const vine::graphics::ShaderProgram> makeProgram(vine::graphics::ShaderStageType type,
                                                                    const char8_t*               source)
{
    auto program = vine::intrusive_ptr<vine::graphics::ShaderProgram>(new vine::graphics::ShaderProgram());
    vine::graphics::ShaderStage stage;
    stage.type   = type;
    stage.source = vine::String(source);
    program->addStage(stage);
    return program;
}

const char8_t* const kFragmentColourOnly = u8R"(
#version 450
layout(location = 0) out vec4 outColor;
layout(binding = 0) uniform sampler2D gbuffer0;
void main() { outColor = vec4(texture(gbuffer0, vec2(0.5)).rgb, 1.0); }
)";

const char8_t* const kFragmentDepthOneColour = u8R"(
#version 450
layout(location = 0) out vec4 outColor;
layout(binding = 0) uniform sampler2D gbuffer0;
layout(binding = 1) uniform sampler2D sourceDepth;
void main() { outColor = vec4(vec3(texture(sourceDepth, vec2(0.5)).r), 1.0); }
)";

} // namespace

TEST(ProgramSamplingTest, ColourOnlyFragmentBindsNoDepth)
{
    const auto program = makeProgram(vine::graphics::ShaderStageType::Fragment, kFragmentColourOnly);
    EXPECT_FALSE(programSamplesDepth(program.get(), /*color_count*/ 1));
    EXPECT_FALSE(programSamplesDepth(program.get(), /*color_count*/ 2));
}

TEST(ProgramSamplingTest, DepthBindingFollowsTheColourCount)
{
    const auto program = makeProgram(vine::graphics::ShaderStageType::Fragment, kFragmentDepthOneColour);
    // Declares binding 1: the DEPTH of a one-colour source, but the second COLOUR
    // attachment of a two-colour one.
    EXPECT_TRUE(programSamplesDepth(program.get(), /*color_count*/ 1));
    EXPECT_FALSE(programSamplesDepth(program.get(), /*color_count*/ 2));
}

TEST(ProgramSamplingTest, OnlyTheFragmentStageDecides)
{
    // A vertex stage's declaration is not the ABI's sampler; the shader that
    // samples runs in the fragment stage.
    const auto program = makeProgram(vine::graphics::ShaderStageType::Vertex, kFragmentDepthOneColour);
    EXPECT_FALSE(programSamplesDepth(program.get(), /*color_count*/ 1));
}

TEST(ProgramSamplingTest, NoProgramBindsNoDepth)
{
    EXPECT_FALSE(programSamplesDepth(nullptr, /*color_count*/ 1));
}

TEST(ProgramDefineImportTest, ThePragmaListDecidesWhichDefinesAProgramAsksFor)
{
    // The backend only sets a define a source NAMES on its import line (vsg drops the rest silently), so
    // "does this program opt into the texcoord-width sampler rule (and its kind guard)" is a question
    // about this list - and a wrong answer would either skip the guard (an invalid descriptor) or apply
    // it to a program that declares its own sampler (its picture replaced with a texture it never asked
    // for).
    const auto asks_cube = [](const char8_t* source) {
        return programImportsDefine(makeProgram(vine::graphics::ShaderStageType::Vertex, source).get(),
                                    "VINE_TEXCOORD_CUBE");
    };

    EXPECT_TRUE(asks_cube(u8"#version 450\n#pragma import_defines (VINE_DIFFUSE_MAP, VINE_TEXCOORD_CUBE)\nvoid main(){}\n"));
    EXPECT_TRUE(asks_cube(u8"#version 450\n#pragma import_defines (VINE_TEXCOORD_CUBE)\nvoid main(){}\n"));
    // A whole ENTRY has to match: a longer name containing the define is a different define.
    EXPECT_FALSE(asks_cube(u8"#version 450\n#pragma import_defines (VINE_TEXCOORD_CUBE_EXTRA)\nvoid main(){}\n"));
    EXPECT_FALSE(asks_cube(u8"#version 450\n#pragma import_defines (VINE_DIFFUSE_MAP)\nvoid main(){}\n"));
    EXPECT_FALSE(asks_cube(u8"#version 450\nvoid main(){}\n"));

    // Any stage counts (the pragma is per stage, and the backend looks at them all), and neither a null
    // program nor an empty name matches anything.
    const auto fragment = makeProgram(vine::graphics::ShaderStageType::Fragment,
                                      u8"#version 450\n#pragma import_defines (VINE_DIFFUSE_MAP)\nvoid main(){}\n");
    EXPECT_TRUE(programImportsDefine(fragment.get(), "VINE_DIFFUSE_MAP"));
    EXPECT_FALSE(programImportsDefine(nullptr, "VINE_DIFFUSE_MAP"));
    EXPECT_FALSE(programImportsDefine(fragment.get(), ""));
}
