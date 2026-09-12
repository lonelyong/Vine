/**
 * @brief Our own forward shader set: the interface contract the renderer relies on (P0 of the custom-shader plan).
 *
 * `buildVineShaderSet` replaces the vendored vsg phong set for scene geometry, so the things it declares are the
 * ABI the rest of the backend already assumes:
 *
 *  * the four canonical attributes at the CUSTOM-PROGRAM locations (colour 2, texcoord 8) in the binding order
 *    the data node binds them (positions, normals, texcoords, colours), with the two optional ones carrying
 *    their gate define — that define is what lets geometry without authored colour or textures draw the
 *    variant without them instead of being padded;
 *  * the material at set 0 / binding 0 as the same PhongMaterialValue the material manager produces (so both
 *    shading paths share one material value), the resolved diffuse map at binding 1, and the per-view lights
 *    at binding 2 (a UBO: the push-constant range belongs to the camera matrices);
 *  * the `pc` push range vsg's matrix stacks fill;
 *  * default pipeline states identical to the built-in set's (a difference would show up as a different
 *    picture rather than as an error).
 *
 * The two stages are also checked for gate parity: a define that gates an interface variable in one stage but
 * not the other links into undefined inputs (Vulkan does not reject it at pipeline creation).
 */

#include <gtest/gtest.h>

#include <vine/graphics/ShaderPreset.hpp>
#include <vine/vsg/EmbeddedShaders.hpp>
#include <vine/vsg/VsgPipelineFactory.hpp>

#include <vsg/core/Data.h>
#include <vsg/state/ColorBlendState.h>
#include <vsg/state/DepthStencilState.h>
#include <vsg/state/ViewportState.h>
#include <vsg/utils/ShaderSet.h>

#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <string>
#include <string_view>
#include <vector>

using namespace vine::vsg;
using namespace vine::vsg::detail;

namespace
{

/**
 * @brief Finds a declared attribute binding by name.
 *
 * @param shader_set Set to inspect.
 * @param name Attribute binding name, e.g. "vsg_Color".
 * @return Pointer to the binding, or null when the set does not declare it.
 */
const ::vsg::AttributeBinding* findAttribute(const ::vsg::ShaderSet& shader_set, const std::string& name)
{
    const auto it = std::find_if(shader_set.attributeBindings.begin(), shader_set.attributeBindings.end(),
                                 [&name](const ::vsg::AttributeBinding& binding) { return binding.name == name; });
    return it == shader_set.attributeBindings.end() ? nullptr : &*it;
}

/**
 * @brief Finds a declared descriptor binding by name.
 *
 * @param shader_set Set to inspect.
 * @param name Descriptor binding name, e.g. "vine_lights".
 * @return Pointer to the binding, or null when the set does not declare it.
 */
const ::vsg::DescriptorBinding* findDescriptor(const ::vsg::ShaderSet& shader_set, const std::string& name)
{
    const auto it = std::find_if(shader_set.descriptorBindings.begin(), shader_set.descriptorBindings.end(),
                                 [&name](const ::vsg::DescriptorBinding& binding) { return binding.name == name; });
    return it == shader_set.descriptorBindings.end() ? nullptr : &*it;
}

/**
 * @brief Builds the forward set for a 640x360 colour+depth target.
 *
 * @return The set, or null when the stages could not be compiled.
 */
::vsg::ref_ptr<::vsg::ShaderSet> makeForwardSet()
{
    return buildVineShaderSet(vine::graphics::ShaderPreset::StandardPhong, VkExtent2D{ 640, 360 }, true, true, 1);
}

TEST(ForwardShaderSetTest, OnlyPresetsWithStagesGetASet)
{
    // A preset without its own stages must be refused rather than shaded as phong:
    // a silently wrong shading model is worse than falling back to the built-in set.
    EXPECT_EQ(buildVineShaderSet(vine::graphics::ShaderPreset::FlatShaded, VkExtent2D{ 640, 360 }, true, true, 1), nullptr);
    EXPECT_EQ(buildVineShaderSet(vine::graphics::ShaderPreset::Pbr, VkExtent2D{ 640, 360 }, true, true, 1), nullptr);
    EXPECT_NE(makeForwardSet(), nullptr);
}

TEST(ForwardShaderSetTest, DeclaresTheCanonicalAttributesWithTheirGates)
{
    const auto shader_set = makeForwardSet();
    ASSERT_NE(shader_set, nullptr);

    // Location AND binding order matter: locations are the custom-program contract,
    // and vsg numbers a vertex binding by the order assignArray() accepts them.
    const auto* positions = findAttribute(*shader_set, "vsg_Vertex");
    const auto* normals   = findAttribute(*shader_set, "vsg_Normal");
    const auto* uv        = findAttribute(*shader_set, "vsg_TexCoord0");
    const auto* color     = findAttribute(*shader_set, "vsg_Color");
    ASSERT_NE(positions, nullptr);
    ASSERT_NE(normals, nullptr);
    ASSERT_NE(uv, nullptr);
    ASSERT_NE(color, nullptr);
    EXPECT_EQ(positions->location, 0u);
    EXPECT_EQ(normals->location, 1u);
    EXPECT_EQ(color->location, 2u);
    EXPECT_EQ(uv->location, 8u);

    // The optional attributes carry their gate; the required ones must not.
    EXPECT_TRUE(positions->define.empty());
    EXPECT_TRUE(normals->define.empty());
    EXPECT_EQ(color->define, "VINE_VERTEX_COLOR");
    EXPECT_EQ(uv->define, "VINE_DIFFUSE_MAP");
}

TEST(ForwardShaderSetTest, DeclaresMaterialLightsAndTheColourMatrixPushRange)
{
    const auto shader_set = makeForwardSet();
    ASSERT_NE(shader_set, nullptr);

    const auto* material = findDescriptor(*shader_set, "material");
    const auto* diffuse  = findDescriptor(*shader_set, "diffuseMap");
    const auto* lights   = findDescriptor(*shader_set, "vine_lights");
    ASSERT_NE(material, nullptr);
    ASSERT_NE(diffuse, nullptr);
    ASSERT_NE(lights, nullptr);
    EXPECT_EQ(material->set, 0u);
    EXPECT_EQ(material->binding, 0u);
    EXPECT_EQ(material->descriptorType, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER);
    EXPECT_EQ(diffuse->set, 0u);
    EXPECT_EQ(diffuse->binding, 1u);
    EXPECT_EQ(diffuse->descriptorType, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER);
    EXPECT_EQ(diffuse->define, "VINE_DIFFUSE_MAP");
    EXPECT_EQ(lights->set, 0u);
    EXPECT_EQ(lights->binding, 2u);
    EXPECT_EQ(lights->descriptorType, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER);
    // The lights block is bound by the host, never assigned through the
    // configurator: vsg must not try to build a descriptor for it from our sample.
    EXPECT_TRUE(lights->define.empty());
    // The uniform block is exactly VineLightsBlock: the shader reads what we write.
    ASSERT_NE(lights->data, nullptr);
    EXPECT_EQ(lights->data->dataSize(), sizeof(VineLightsBlock));

    ASSERT_FALSE(shader_set->pushConstantRanges.empty());
    const auto& push = shader_set->pushConstantRanges.front();
    EXPECT_EQ(push.range.stageFlags, VK_SHADER_STAGE_VERTEX_BIT);
    EXPECT_EQ(push.range.offset, 0u);
    EXPECT_EQ(push.range.size, 128u);
}

TEST(ForwardShaderSetTest, InheritsTheScenePipelineStates)
{
    const auto shader_set = makeForwardSet();
    ASSERT_NE(shader_set, nullptr);
    // One colour attachment, depth test+write on: the states must match the built-in
    // set's, otherwise the same pass would render differently depending on the path.
    const auto& states = shader_set->defaultGraphicsPipelineStates;
    const auto  depth  = std::find_if(states.begin(), states.end(), [](const auto& state) {
        return dynamic_cast<const ::vsg::DepthStencilState*>(state.get()) != nullptr;
    });
    const auto viewport = std::find_if(states.begin(), states.end(), [](const auto& state) {
        return dynamic_cast<const ::vsg::ViewportState*>(state.get()) != nullptr;
    });
    ASSERT_NE(depth, states.end());
    ASSERT_NE(viewport, states.end());
    EXPECT_EQ(static_cast<const ::vsg::DepthStencilState*>(depth->get())->depthTestEnable, VK_TRUE);
    EXPECT_EQ(static_cast<const ::vsg::DepthStencilState*>(depth->get())->depthWriteEnable, VK_TRUE);

    const auto built_in = buildShaderSet(vine::graphics::ShaderPreset::StandardPhong, VkExtent2D{ 640, 360 }, true, true, 1);
    ASSERT_NE(built_in, nullptr);
    EXPECT_EQ(states.size(), built_in->defaultGraphicsPipelineStates.size());
    for (std::size_t i = 0; i < states.size(); ++i) {
        // Captured first: typeid on a dereferenced expression evaluates it, and the
        // compiler warns when that expression may have side effects.
        const auto* mine   = states[i].get();
        const auto* theirs = built_in->defaultGraphicsPipelineStates[i].get();
        EXPECT_EQ(typeid(*mine).hash_code(), typeid(*theirs).hash_code()) << i;
    }
}

TEST(ForwardShaderSetTest, BothStagesGateTheSameInterfaceVariables)
{
    const std::u8string_view vs = shaders::kVineForwardVert;
    const std::u8string_view fs = shaders::kVineForwardFrag;
    for (const char8_t* define : { u8"VINE_VERTEX_COLOR", u8"VINE_DIFFUSE_MAP" }) {
        const bool in_vs = vs.find(define) != std::u8string_view::npos;
        const bool in_fs = fs.find(define) != std::u8string_view::npos;
        EXPECT_TRUE(in_vs) << reinterpret_cast<const char*>(define);
        EXPECT_EQ(in_vs, in_fs) << reinterpret_cast<const char*>(define);
    }
}

TEST(ForwardShaderSetTest, StagesCompileToSpirv)
{
    // The set only exists when glslang accepted both stages; an empty stage list
    // would make buildVineShaderSet return null (covered above), so this pins that
    // the embedded sources are what compiles.
    const auto shader_set = makeForwardSet();
    ASSERT_NE(shader_set, nullptr);
    ASSERT_EQ(shader_set->stages.size(), 2u);
    for (const auto& stage : shader_set->stages) {
        ASSERT_NE(stage->module, nullptr);
        EXPECT_FALSE(stage->module->code.empty());
    }
}

TEST(ForwardShaderSetTest, ContentSetsAreNeverUnshaded)
{
    // makeContentShaderSet is the one entry point every content set is built from
    // (the window's three depth-mode sets and each off-screen target's). Whatever
    // the switch says, EVERY preset must come back with a usable set: a null here
    // would leave a pass with no pipeline at all.
    const VkExtent2D extent{ 640, 360 };
    for (const auto preset : { vine::graphics::ShaderPreset::StandardPhong, vine::graphics::ShaderPreset::FlatShaded,
                               vine::graphics::ShaderPreset::Pbr, vine::graphics::ShaderPreset::ShadowedPhong }) {
        for (const bool depth_test : { true, false }) {
            for (const bool depth_write : { true, false }) {
                for (const int color_count : { 0, 1, 3 }) {
                    EXPECT_NE(makeContentShaderSet(preset, extent, depth_test, depth_write, color_count), nullptr)
                        << "preset " << static_cast<int>(preset) << " depth_test " << depth_test << " color_count "
                        << color_count;
                }
            }
        }
    }
}

TEST(ForwardShaderSetTest, TheForwardSwitchIsOffByDefault)
{
    // VINE_VSG_FORWARD is a session decision read once; with it unset (the case in
    // this process, and in the shipped configuration) content must go through the
    // built-in set, whose layout has no vine_lights binding at all. If this ever
    // flips, the self-test evidence baseline changes with it — this test says the
    // flip was not accidental.
    ASSERT_EQ(std::getenv("VINE_VSG_FORWARD"), nullptr);
    EXPECT_FALSE(vineForwardShaderEnabled());
    const auto set = makeContentShaderSet(vine::graphics::ShaderPreset::StandardPhong, VkExtent2D{ 640, 360 }, true, true, 1);
    ASSERT_NE(set, nullptr);
    // getDescriptorBinding reports "not declared" through its bool conversion.
    EXPECT_FALSE(static_cast<bool>(set->getDescriptorBinding("vine_lights")));
}

}  // namespace
