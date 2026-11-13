/**
 * @brief The engine's program switches, as data: which variant a drawable needs, and the names a compile receives.
 *
 * WHAT THIS PINS, and why it is device-free. The engine's own stages branch on names they declare in a
 * `#pragma import_defines` line (`VN_TEXTURED`, `VN_VERTEX_COLOR`, `VN_TEXCOORD_UV`, `VN_TEXCOORD_CUBE`,
 * `VN_INSTANCED`). The RULES that pick those names from a drawable are the engine's (a material with any texture
 * slot is textured; the texcoord WIDTH is the kind; a fed colour is an authored one; an instance stream is the
 * command's), so they are asserted here as a table - before any device exists, exactly like the barrier and state
 * tables. `compileStage` is glslang alone, so "the names reach the text" is also device-free.
 */

#include <gtest/gtest.h>

#include <array>
#include <string>
#include <vector>

#include <vine/Buffer.hpp>
#include <vine/graphics/BuiltinShaders.hpp>
#include <vine/graphics/Geometry.hpp>
#include <vine/graphics/Material.hpp>
#include <vine/graphics/Primitive.hpp>
#include <vine/graphics/ShaderAbi.hpp>
#include <vine/graphics/ShaderProgram.hpp>
#include <vine/graphics/Texture.hpp>
#include <vine/imaging/PixelFormat.hpp>

#include <vine/backend_vk/internal/ProgramVariant.hpp>
#include <vine/backend_vk/internal/ShaderCompiler.hpp>

using vn::backend_vk::compileStage;
using vn::backend_vk::ProgramVariant;
using vn::backend_vk::SpirvModule;
using vn::backend_vk::variantOf;
using vn::graphics::Geometry;
using vn::graphics::Material;
using vn::graphics::ShaderStage;
using vn::graphics::ShaderStageType;
using vn::graphics::Texture2D;
using vn::graphics::TextureSlot;
using vn::intrusive_ptr;

namespace
{

/// @brief A triangle with the channels @p with_colour / @p texcoord_components ask for (normals always fed).
///
/// The engine's builtin vertex stages read positions (0) and normals (1) unconditionally; the colour and texcoord
/// channels are what the variant switches look at, so they are the knobs here.
[[nodiscard]] intrusive_ptr<Geometry> makeGeometry(bool with_colour, int texcoord_components)
{
    auto geometry = intrusive_ptr<Geometry>(new Geometry());
    geometry->setPositions(intrusive_ptr<const vn::Buffer<float>>(new vn::Buffer<float>(std::vector<float>{
        -0.75F, -0.75F, 0.5F,
        0.75F,  -0.75F, 0.5F,
        0.0F,   0.75F,  0.5F,
    })));
    geometry->setNormals(intrusive_ptr<const vn::Buffer<float>>(new vn::Buffer<float>(std::vector<float>{
        0.0F, 0.0F, 1.0F,
        0.0F, 0.0F, 1.0F,
        0.0F, 0.0F, 1.0F,
    })));
    if (with_colour) {
        geometry->setColors(intrusive_ptr<const vn::Buffer<float>>(new vn::Buffer<float>(std::vector<float>{
            1.0F, 0.0F, 0.0F, 1.0F,
            1.0F, 0.0F, 0.0F, 1.0F,
            1.0F, 0.0F, 0.0F, 1.0F,
        })));
    }
    if (texcoord_components == 3) {
        geometry->setTexcoords3(intrusive_ptr<const vn::Buffer<float>>(new vn::Buffer<float>(std::vector<float>{
            0.0F, 0.0F, 1.0F,
            0.0F, 0.0F, 1.0F,
            0.0F, 0.0F, 1.0F,
        })));
    }
    else if (texcoord_components == 2) {
        geometry->setTexcoords2(intrusive_ptr<const vn::Buffer<float>>(new vn::Buffer<float>(std::vector<float>{
            0.0F, 0.0F,
            1.0F, 0.0F,
            0.5F, 1.0F,
        })));
    }
    geometry->bumpRevision();
    return geometry;
}

/// @brief A white material, with a 1x1 map in its base-colour slot when @p with_texture.
[[nodiscard]] intrusive_ptr<Material> makeMaterial(bool with_texture)
{
    auto material = intrusive_ptr<Material>(new Material());
    material->setBaseColor(vn::Colorf(1.0F, 1.0F, 1.0F, 1.0F));
    if (with_texture) {
        auto texture = intrusive_ptr<Texture2D>(new Texture2D(1, 1, vn::imaging::PixelFormat::Rgba8Unorm));
        material->setTexture(TextureSlot::BaseColor, texture);
    }
    return material;
}

/// @brief The vertex stage of @p program's text (the one the variant's attribute names live in).
[[nodiscard]] const ShaderStage* vertexStageOf(const vn::graphics::ShaderProgramDescription& program)
{
    for (const ShaderStage& stage : program.stages()) {
        if (stage.type == ShaderStageType::Vertex) {
            return &stage;
        }
    }
    return nullptr;
}

} // namespace

TEST(VkVariantTest, TheBitsAreTheEnginesOwnSwitches)
{
    // One bit per switch, in the order ProgramVariant states them: the number is what the program cache and the
    // pipeline key compare, so a mapping that drifted from the flags would silently share a compile.
    ProgramVariant variant;
    EXPECT_EQ(variant.bits(), 0U);

    variant.textured = true;
    EXPECT_EQ(variant.bits(), 1U);
    variant.textured     = false;
    variant.vertex_color = true;
    EXPECT_EQ(variant.bits(), 2U);
    variant.vertex_color = false;
    variant.cube_texcoord = true;
    EXPECT_EQ(variant.bits(), 4U);
    variant.cube_texcoord = false;
    variant.instanced     = true;
    EXPECT_EQ(variant.bits(), 8U);

    ProgramVariant everything;
    everything.textured      = true;
    everything.vertex_color  = true;
    everything.cube_texcoord = true;
    everything.instanced     = true;
    EXPECT_EQ(everything.bits(), 15U);
}

TEST(VkVariantTest, ExactlyOneTexcoordKindIsAlwaysNamed)
{
    // The engine's stages REFUSE a textured variant that does not say what the texcoord slot is, so the kind is not
    // an option: it is the one name every variant carries.
    EXPECT_EQ(ProgramVariant{}.defines(), (std::vector<std::string>{ "VN_TEXCOORD_UV" }));

    ProgramVariant cube;
    cube.cube_texcoord = true;
    EXPECT_EQ(cube.defines(), (std::vector<std::string>{ "VN_TEXCOORD_CUBE" }));

    ProgramVariant everything;
    everything.textured      = true;
    everything.vertex_color  = true;
    everything.cube_texcoord = true;
    everything.instanced     = true;
    EXPECT_EQ(everything.defines(),
              (std::vector<std::string>{ "VN_TEXCOORD_CUBE", "VN_TEXTURED", "VN_VERTEX_COLOR", "VN_INSTANCED" }));
}

TEST(VkVariantTest, TheMaterialsTextureSlotSelectsTheTexturedVariant)
{
    const auto plain    = makeGeometry(false, 0);
    const auto bare     = makeMaterial(false);
    const auto textured = makeMaterial(true);

    const ProgramVariant without_map = variantOf(*plain->description(), bare->description().get(), false);
    EXPECT_FALSE(without_map.textured) << "a material with no slot map asks for no sampler variant";

    const ProgramVariant with_map = variantOf(*plain->description(), textured->description().get(), false);
    EXPECT_TRUE(with_map.textured) << "ANY present slot asks for the textured variant";
    EXPECT_EQ(with_map.bits(), 1U);

    // A drawable with no material at all takes the block's defaults - no map, so no textured variant.
    EXPECT_FALSE(variantOf(*plain->description(), nullptr, false).textured);
}

TEST(VkVariantTest, TheGeometrysColourChannelSelectsItsVariant)
{
    const auto bare    = makeMaterial(false);
    const auto plain   = makeGeometry(false, 0);
    const auto coloured = makeGeometry(true, 0);

    EXPECT_FALSE(variantOf(*plain->description(), bare->description().get(), false).vertex_color);
    EXPECT_TRUE(variantOf(*coloured->description(), bare->description().get(), false).vertex_color)
        << "a geometry that feeds location 2 authors its colour";
}

TEST(VkVariantTest, TheTexcoordWidthIsTheKind)
{
    const auto    material = makeMaterial(true);
    const auto    uv       = makeGeometry(false, 2);
    const auto    cube     = makeGeometry(false, 3);
    const auto    no_slot  = makeGeometry(false, 0);

    EXPECT_FALSE(variantOf(*uv->description(), material->description().get(), false).cube_texcoord)
        << "two scalars per vertex is a UV pair";
    EXPECT_TRUE(variantOf(*cube->description(), material->description().get(), false).cube_texcoord)
        << "three scalars per vertex is a direction";
    EXPECT_FALSE(variantOf(*no_slot->description(), material->description().get(), false).cube_texcoord)
        << "a geometry without the slot still names UV (the variant that samples nothing never reads it)";
}

TEST(VkVariantTest, TheInstanceSwitchIsTheCommands)
{
    // The placement is a fact of the DRAWING CALL, not of the geometry or the material, which is why it is the one
    // input the caller states.
    const auto plain    = makeGeometry(false, 0);
    const auto material = makeMaterial(false);

    EXPECT_FALSE(variantOf(*plain->description(), material->description().get(), false).instanced);
    const ProgramVariant instanced = variantOf(*plain->description(), material->description().get(), true);
    EXPECT_TRUE(instanced.instanced);
    EXPECT_EQ(instanced.bits(), 8U);
}

TEST(VkVariantTest, TheCompilerDeliversTheNamesToTheEngineText)
{
    // THE ACCEPTANCE FOR THE DEFINE PATH, without a device: the engine's own vertex stage is compiled under the
    // variant's names. Without a texcoord kind a textured compile is REFUSED BY THE TEXT itself (`#error a sampled
    // texcoord slot needs one kind`), which is the proof that the names reached it.
    const auto program = vn::graphics::unlitForwardProgram();
    ASSERT_NE(program, nullptr);
    const auto description = program->description();
    ASSERT_NE(description, nullptr);
    const ShaderStage* const vertex = vertexStageOf(*description);
    ASSERT_NE(vertex, nullptr) << "the engine's forward program has a vertex stage";

    {
        SpirvModule module;
        vn::String  why;
        const std::vector<std::string> textured_without_kind{ "VN_TEXTURED" };
        EXPECT_FALSE(compileStage(*vertex, textured_without_kind, module, why))
            << "a textured compile without a texcoord kind must be refused by the stage's own #error";
        EXPECT_NE(why.as_std_str().find("one kind"), std::string::npos) << why.as_std_str();
    }

    SpirvModule uv;
    vn::String  uv_why;
    ASSERT_TRUE(compileStage(*vertex, std::vector<std::string>{ "VN_TEXTURED", "VN_TEXCOORD_UV" }, uv, uv_why)) << uv_why.as_std_str();

    SpirvModule cube;
    vn::String  cube_why;
    ASSERT_TRUE(compileStage(*vertex, std::vector<std::string>{ "VN_TEXTURED", "VN_TEXCOORD_CUBE" }, cube, cube_why))
        << cube_why.as_std_str();

    // The kind is a COMPILE-TIME difference: same text, same names minus one, different module.
    EXPECT_NE(uv.words, cube.words) << "the two texcoord kinds produced identical SPIR-V, so the define did not arrive";
}
