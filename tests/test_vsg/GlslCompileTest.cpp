#include <gtest/gtest.h>

#include <vector>

#include <vine/graphics/BuiltinShaders.hpp>
#include <vine/graphics/ShaderProgram.hpp>

#include <vsg/state/ShaderStage.h>
#include <vsg/utils/ShaderCompiler.h>

namespace
{

/**
 * @brief Maps an SDK stage kind onto the matching Vulkan stage flag.
 *
 * @param type SDK stage kind.
 * @return Vulkan shader-stage flag.
 */
VkShaderStageFlagBits toVkStage(vn::graphics::ShaderStageType type)
{
    switch (type) {
    case vn::graphics::ShaderStageType::Fragment: return VK_SHADER_STAGE_FRAGMENT_BIT;
    case vn::graphics::ShaderStageType::Compute: return VK_SHADER_STAGE_COMPUTE_BIT;
    case vn::graphics::ShaderStageType::Vertex: return VK_SHADER_STAGE_VERTEX_BIT;
    }
    return VK_SHADER_STAGE_VERTEX_BIT;
}

TEST(GlslCompileTest, ShaderCompilerIsSupportedAfterGlslangIntegration)
{
    // Proves this vsg build actually carries glslang: runtime GLSL -> SPIR-V
    // is available (VSG_SUPPORTS_ShaderCompiler == 1).
    vsg::ShaderCompiler compiler;
    EXPECT_TRUE(compiler.supported());
}

TEST(GlslCompileTest, CompilesShaderProgramStagesToSpirv)
{
    vn::graphics::ShaderProgram program;
    program.setName(u8"flat-red");

    vn::graphics::ShaderStage vs;
    vs.type   = vn::graphics::ShaderStageType::Vertex;
    vs.source = u8"#version 450\n"
                u8"void main() { gl_Position = vec4(0.0, 0.0, 0.0, 1.0); }\n";
    program.addStage(vs);

    vn::graphics::ShaderStage fs;
    fs.type   = vn::graphics::ShaderStageType::Fragment;
    fs.source = u8"#version 450\n"
                u8"layout(location = 0) out vec4 outColor;\n"
                u8"void main() { outColor = vec4(1.0, 0.0, 0.0, 1.0); }\n";
    program.addStage(fs);

    vsg::ShaderCompiler compiler;
    ASSERT_TRUE(compiler.supported());

    std::size_t compiled = 0;
    for (const auto& stage : program.stages()) {
        const std::string source = stage.source.as_std_str();
        auto              vstage = vsg::ShaderStage::create(toVkStage(stage.type), stage.entryPoint.as_std_str(), source);
        ASSERT_TRUE(compiler.compile(vstage));
        ASSERT_NE(vstage->module, nullptr);
        if (vstage->module != nullptr && !vstage->module->code.empty()) {
            ++compiled;
        }
    }
    // Both the vertex and the fragment stage produced SPIR-V.
    EXPECT_EQ(compiled, 2u);
}

TEST(GlslCompileTest, EveryBuiltinForwardProgramCompilesToSpirv)
{
    // The three "same text, one define" forward programs (see BuiltinShaders): a define that turns a
    // preprocessor branch into GLSL the driver cannot compile would ship silently - the backend treats "no
    // compiled stage" as "this preset has no SDK program" and shades with its fallback, which is exactly how the
    // flat preset first came out lit-but-wrong (see withDefine's note). Compiling the real texts here is what
    // makes adding a mode a compile-time fact instead of a hope.
    const std::vector<vn::intrusive_ptr<vn::graphics::ShaderProgram>> programs = {
        vn::graphics::forwardProgram(),
        vn::graphics::flatForwardProgram(),
        vn::graphics::unlitForwardProgram(),
        vn::graphics::normalsForwardProgram(),
    };

    vsg::ShaderCompiler compiler;
    ASSERT_TRUE(compiler.supported());

    for (const auto& program : programs) {
        ASSERT_NE(program, nullptr);
        ASSERT_FALSE(program->stages().empty());
        for (const auto& stage : program->stages()) {
            const std::string source = stage.source.as_std_str();
            auto              vstage = vsg::ShaderStage::create(toVkStage(stage.type), stage.entryPoint.as_std_str(), source);
            EXPECT_TRUE(compiler.compile(vstage)) << "program '" << program->name().as_std_str() << "' stage failed";
            ASSERT_NE(vstage->module, nullptr);
            EXPECT_FALSE(vstage->module->code.empty());
        }
    }
}

} // namespace
