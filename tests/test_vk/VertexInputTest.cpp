/**
 * @brief What a compiled vertex stage DECLARES as its vertex inputs, and why the pipeline is built from that.
 *
 * WHAT THIS PINS, and why it is device-free. `compileStage` is glslang alone, so the SPIR-V a stage produces can be
 * read back without a device. The backend builds a content pipeline's vertex input from this answer instead of from
 * the geometry's channel list: a geometry carries what its data has, a program reads what it declares, and a pipeline
 * that declares the rest describes a vertex input nobody wrote - which validation reports ("attribute at location N
 * not consumed by vertex shader") and which makes a draw fetch what it never uses.
 *
 * The answers that matter are here:
 *   - the engine's content stage declares position, normal and the texcoord slot it samples;
 *   - instanced, it owns location 12 as a MATRIX, i.e. locations 12-15;
 *   - the sky program declares position and its direction channel, and never the normal;
 *   - a module nobody can read answers UNKNOWN, which callers treat as "declares everything" - narrowing on a guess
 *     would drop real data.
 */

#include <gtest/gtest.h>

#include <cstdint>
#include <string>
#include <vector>

#include <vine/graphics/BuiltinShaders.hpp>
#include <vine/graphics/ShaderProgram.hpp>

#include <vine/backend_vk/internal/ShaderCompiler.hpp>

using vn::backend_vk::compileStage;
using vn::backend_vk::SpirvModule;
using vn::graphics::ShaderProgram;
using vn::graphics::ShaderStage;
using vn::graphics::ShaderStageType;

namespace
{

/// @brief The vertex stage of @p program (the one its text declares its inputs in), or null when it has none.
[[nodiscard]] const ShaderStage* vertexStageOf(const ShaderProgram& program)
{
    for (const ShaderStage& stage : program.description()->stages()) {
        if (stage.type == ShaderStageType::Vertex) {
            return &stage;
        }
    }
    return nullptr;
}

/// @brief Compiles @p stage (with @p defines) and answers the locations it declares.
[[nodiscard]] std::vector<std::uint32_t> declaredBy(const ShaderStage& stage, const std::vector<std::string>& defines = {})
{
    SpirvModule module;
    vn::String  why;
    const bool  compiled = compileStage(stage, defines, module, why);
    EXPECT_TRUE(compiled) << why.as_std_str();
    EXPECT_TRUE(module.input_locations_known) << "a module glslang produced must be readable";
    return module.input_locations;
}

} // namespace

// A stage that reads one position and one pair of coordinates declares exactly those two locations - the ones the
// pipeline may name. Anything the geometry also carries (a normal, say) is not this program's input.
TEST(VkVertexInputTest, AStageDeclaresTheLocationsItReads)
{
    ShaderStage stage;
    stage.type   = ShaderStageType::Vertex;
    stage.source = vn::String(u8R"(#version 450
layout(location = 0) in vec3 vn_Vertex;
layout(location = 8) in vec2 vn_Uv;
void main()
{
    gl_Position = vec4(vn_Vertex, 1.0) + vec4(vn_Uv, 0.0, 0.0);
}
)");
    EXPECT_EQ(declaredBy(stage), (std::vector<std::uint32_t>{ 0U, 8U }));
}

// The engine's content stage, textured: position (0), normal (1) and the texcoord slot (8). A shadow pass drawn with it
// therefore declares every channel the geometry carries - which is why it has nothing to warn about.
TEST(VkVertexInputTest, TheContentStageDeclaresPositionNormalAndItsTexcoordSlot)
{
    const vn::intrusive_ptr<ShaderProgram> program = vn::graphics::forwardProgram();
    const ShaderStage* const                stage   = vertexStageOf(*program);
    ASSERT_NE(stage, nullptr);

    EXPECT_EQ(declaredBy(*stage, { "VN_TEXTURED", "VN_TEXCOORD_UV" }), (std::vector<std::uint32_t>{ 0U, 1U, 8U }));
}

// Instanced, the same stage owns location 12 as a MATRIX: one location per column, 12-15. A reader that stopped at the
// decoration (12) would drop columns 13-15, and the draw would then read the wrong rows of the instance matrix.
TEST(VkVertexInputTest, AnInstancedStageOwnsTheWholeMatrixRange)
{
    const vn::intrusive_ptr<ShaderProgram> program = vn::graphics::forwardProgram();
    const ShaderStage* const                stage   = vertexStageOf(*program);
    ASSERT_NE(stage, nullptr);

    EXPECT_EQ(declaredBy(*stage, { "VN_INSTANCED" }), (std::vector<std::uint32_t>{ 0U, 1U, 12U, 13U, 14U, 15U }));
}

// The sky program derives its direction from the authored one, so it reads position and that channel - a geometry that
// also carries normals has one channel this program does not declare.
TEST(VkVertexInputTest, TheSkyStageDoesNotDeclareANormalItNeverReads)
{
    const vn::intrusive_ptr<ShaderProgram> program = vn::graphics::skyboxProgram();
    const ShaderStage* const                stage   = vertexStageOf(*program);
    ASSERT_NE(stage, nullptr);

    EXPECT_EQ(declaredBy(*stage, { "VN_TEXCOORD_CUBE" }), (std::vector<std::uint32_t>{ 0U, 8U }));
}

// UNKNOWN MEANS YES. Words that are not SPIR-V must leave every channel declared, because the alternative - narrowing
// by an empty answer - would silently strip a stage's real inputs.
TEST(VkVertexInputTest, AModuleThatCannotBeReadDeclaresEverything)
{
    SpirvModule module;
    module.words = { 0xDEADBEEFU, 0x00010000U };
    EXPECT_FALSE(module.input_locations_known);
    EXPECT_TRUE(module.declaresInput(0U));
    EXPECT_TRUE(module.declaresInput(15U));
}
