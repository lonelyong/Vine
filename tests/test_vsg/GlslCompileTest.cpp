#include <gtest/gtest.h>

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
        case vn::graphics::ShaderStageType::Fragment:
            return VK_SHADER_STAGE_FRAGMENT_BIT;
        case vn::graphics::ShaderStageType::Compute:
            return VK_SHADER_STAGE_COMPUTE_BIT;
        case vn::graphics::ShaderStageType::Vertex:
            return VK_SHADER_STAGE_VERTEX_BIT;
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
    vs.type = vn::graphics::ShaderStageType::Vertex;
    vs.source = u8"#version 450\n"
                u8"void main() { gl_Position = vec4(0.0, 0.0, 0.0, 1.0); }\n";
    program.addStage(vs);

    vn::graphics::ShaderStage fs;
    fs.type = vn::graphics::ShaderStageType::Fragment;
    fs.source = u8"#version 450\n"
                u8"layout(location = 0) out vec4 outColor;\n"
                u8"void main() { outColor = vec4(1.0, 0.0, 0.0, 1.0); }\n";
    program.addStage(fs);

    vsg::ShaderCompiler compiler;
    ASSERT_TRUE(compiler.supported());

    std::size_t compiled = 0;
    for (const auto& stage : program.stages()) {
        const std::string source = stage.source.as_std_str();
        auto vstage = vsg::ShaderStage::create(toVkStage(stage.type),
                                               stage.entryPoint.as_std_str(), source);
        ASSERT_TRUE(compiler.compile(vstage));
        ASSERT_NE(vstage->module, nullptr);
        if (vstage->module != nullptr && !vstage->module->code.empty()) {
            ++compiled;
        }
    }
    // Both the vertex and the fragment stage produced SPIR-V.
    EXPECT_EQ(compiled, 2u);
}

}  // namespace
