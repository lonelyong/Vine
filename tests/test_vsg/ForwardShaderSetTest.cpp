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

#include <vine/graphics/BuiltinShaders.hpp>
#include <vine/graphics/Geometry.hpp>
#include <vine/graphics/Material.hpp>
#include <vine/graphics/RenderCommand.hpp>
#include <vine/graphics/ShaderAbi.hpp>
#include <vine/graphics/ShaderPreset.hpp>
#include <vine/vsg/SceneBridge.hpp>
#include <vine/vsg/VsgPipelineFactory.hpp>

#include <vsg/core/Data.h>
#include <vsg/nodes/Group.h>
#include <vsg/nodes/StateGroup.h>
#include <vsg/state/ColorBlendState.h>
#include <vsg/state/DepthStencilState.h>
#include <vsg/state/DescriptorSetLayout.h>
#include <vsg/state/GraphicsPipeline.h>
#include <vsg/state/PipelineLayout.h>
#include <vsg/state/VertexInputState.h>
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

/**
 * @brief Builds a triangle that authors only positions and normals.
 *
 * No loc2 colour and no UVs is exactly the geometry our forward set can shade with the variants that do not
 * declare vsg_Color / vsg_TexCoord0. The data builder still materialises the white opacity carrier and the
 * zero UVs for the built-in path, which is what these tests look past.
 *
 * @return New triangle geometry.
 */
vine::graphics::GeometryPtr makeBareTriangle()
{
    auto geometry = vine::graphics::GeometryPtr(new vine::graphics::Geometry());
    vine::geometry::Vec3fArray positions;
    positions.emplace_back(0.0f, 0.0f, 0.0f);
    positions.emplace_back(1.0f, 0.0f, 0.0f);
    positions.emplace_back(0.0f, 1.0f, 0.0f);
    geometry->setPositions(vine::graphics::packAttribute(positions));
    vine::geometry::Vec3fArray normals;
    normals.emplace_back(0.0f, 0.0f, 1.0f);
    normals.emplace_back(0.0f, 0.0f, 1.0f);
    normals.emplace_back(0.0f, 0.0f, 1.0f);
    geometry->setNormals(vine::graphics::packAttribute(normals));
    return geometry;
}

/**
 * @brief Finds the graphics-pipeline bind a retained subtree holds.
 *
 * @param node Root of the subtree to walk.
 * @return The bind command, or null when the subtree holds none.
 */
const ::vsg::BindGraphicsPipeline* findGraphicsPipeline(const ::vsg::Node* node)
{
    if (node == nullptr) {
        return nullptr;
    }
    if (const auto* state_group = node->cast<::vsg::StateGroup>()) {
        for (const auto& command : state_group->stateCommands) {
            if (const auto* bind = command->cast<::vsg::BindGraphicsPipeline>()) {
                return bind;
            }
        }
    }
    if (const auto* group = node->cast<::vsg::Group>()) {
        for (const auto& child : group->children) {
            if (const auto* hit = findGraphicsPipeline(child.get())) {
                return hit;
            }
        }
    }
    return nullptr;
}

/**
 * @brief The pipeline's vertex input state, or null when it has none.
 *
 * @param pipeline Pipeline to inspect.
 * @return The vertex input state, or null.
 */
const ::vsg::VertexInputState* findVertexInputState(const ::vsg::GraphicsPipeline& pipeline)
{
    for (const auto& state : pipeline.pipelineStates) {
        if (const auto* vertex_input = state->cast<::vsg::VertexInputState>()) {
            return vertex_input;
        }
    }
    return nullptr;
}

/**
 * @brief Whether any descriptor set layout of the pipeline binds a sampled image.
 *
 * @param pipeline Pipeline to inspect.
 * @return true when a combined image sampler is declared.
 */
bool pipelineSamplesATexture(const ::vsg::GraphicsPipeline& pipeline)
{
    if (pipeline.layout == nullptr) {
        return false;
    }
    for (const auto& set_layout : pipeline.layout->setLayouts) {
        if (set_layout == nullptr) {
            continue;
        }
        for (const auto& binding : set_layout->bindings) {
            if (binding.descriptorType == VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER) {
                return true;
            }
        }
    }
    return false;
}

/**
 * @brief Builds a retained state group for one bare triangle through @p shader_set.
 *
 * @param shader_set Content set the bridge renders the triangle with.
 * @param root       Receives the retained root that keeps the built group alive.
 * @param opacity    The drawable's effective opacity.
 * @return The pipeline bind the state group holds, or null.
 */
const ::vsg::BindGraphicsPipeline* buildBareTriangleState(const ::vsg::ref_ptr<::vsg::ShaderSet>& shader_set,
                                                          ::vsg::ref_ptr<::vsg::Group>&            root,
                                                          float                                    opacity = 1.0f)
{
    vine::vsg::SceneBridge bridge;
    bridge.setShaderSet(shader_set);
    root          = ::vsg::Group::create();
    auto material = vine::graphics::MaterialPtr(new vine::graphics::Material());

    vine::graphics::RenderCommand command(makeBareTriangle(), material, vine::math::Mat4d());
    command.opacity = opacity;
    std::vector<vine::graphics::RenderCommand> commands{ std::move(command) };
    std::vector<::vsg::ref_ptr<::vsg::Node>>   created;
    bridge.syncRenderCommands(commands, root.get(), &created);
    return findGraphicsPipeline(root.get());
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

TEST(ForwardShaderSetTest, ThePushRangeRealizesTheL1ViewAndDrawBlocks)
{
    // The vsg backend's push is the L2 realization of the SDK's L1 camera blocks
    // (ShaderAbi.hpp): it carries VineViewBlock.proj and
    // VineViewBlock.view * VineDrawBlock.model. The full view block is larger than the
    // push range, which is why the push is an IMPLEMENTATION of the L1 pair rather
    // than the contract itself.
    EXPECT_GT(sizeof(vine::graphics::VineViewBlock), 128u);

    const auto program = vine::graphics::builtinProgram(vine::graphics::ShaderPreset::StandardPhong);
    ASSERT_NE(program, nullptr);
    ASSERT_EQ(program->stageCount(), 2u);
    const auto* vs_stage = program->stage(0);
    ASSERT_NE(vs_stage, nullptr);
    const std::string vs = vs_stage->source.stdstr();
    // The block instance is named "pc" because that is the name vsg's matrix stacks
    // fill; the two members are the L1 subset named above.
    EXPECT_NE(vs.find("layout(push_constant) uniform PushConstants"), std::string::npos);
    EXPECT_NE(vs.find("mat4 projection;"), std::string::npos);
    EXPECT_NE(vs.find("mat4 modelView;"), std::string::npos);
    EXPECT_NE(vs.find("} pc;"), std::string::npos);
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
    // The stages are the SDK's built-in program for the preset (BuiltinShaders.hpp):
    // this checks the engine's own shader text, not a backend copy of it.
    const auto program = vine::graphics::builtinProgram(vine::graphics::ShaderPreset::StandardPhong);
    ASSERT_NE(program, nullptr);
    ASSERT_EQ(program->stageCount(), 2u);
    const auto* vs_stage = program->stage(0);
    const auto* fs_stage = program->stage(1);
    ASSERT_NE(vs_stage, nullptr);
    ASSERT_NE(fs_stage, nullptr);
    const std::string vs = vs_stage->source.stdstr();
    const std::string fs = fs_stage->source.stdstr();
    for (const char* define : { "VINE_VERTEX_COLOR", "VINE_DIFFUSE_MAP" }) {
        const bool in_vs = vs.find(define) != std::string::npos;
        const bool in_fs = fs.find(define) != std::string::npos;
        EXPECT_TRUE(in_vs) << define;
        EXPECT_EQ(in_vs, in_fs) << define;
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

TEST(ForwardShaderSetTest, TheForwardSwitchIsOnByDefault)
{
    // VINE_VSG_BUILTIN is a session decision read once; with it unset (the case in
    // this process, and in the shipped configuration) content must go through our
    // own forward set, whose layout declares the vine_lights binding. If this ever
    // flips back, the self-test evidence baseline changes with it — this test says
    // the flip was not accidental.
    ASSERT_EQ(std::getenv("VINE_VSG_BUILTIN"), nullptr);
    EXPECT_TRUE(vineForwardShaderEnabled());
    const auto set = makeContentShaderSet(vine::graphics::ShaderPreset::StandardPhong, VkExtent2D{ 640, 360 }, true, true, 1);
    ASSERT_NE(set, nullptr);
    // getDescriptorBinding reports "not declared" through its bool conversion.
    EXPECT_TRUE(static_cast<bool>(set->getDescriptorBinding("vine_lights")));
}

TEST(ForwardShaderSetTest, ForwardSetDropsDerivedColourAndUvs)
{
    // Our forward set declares vsg_Color / vsg_TexCoord0 behind defines, so a geometry that authors neither
    // and whose material samples no texture must take the variant WITHOUT them: the white opacity carrier and
    // the zero UVs the data node builds for the built-in path are simply not assigned. That is two fewer
    // vertex bindings and no diffuse sampler — the whole point of gating the attributes.
    auto forward = makeForwardSet();
    ASSERT_NE(forward, nullptr);

    ::vsg::ref_ptr<::vsg::Group> root;
    const auto*                  bind = buildBareTriangleState(forward, root);
    ASSERT_NE(bind, nullptr);
    ASSERT_NE(bind->pipeline, nullptr);
    const auto* vertex_input = findVertexInputState(*bind->pipeline);
    ASSERT_NE(vertex_input, nullptr);
    EXPECT_EQ(vertex_input->vertexBindingDescriptions.size(), 2u); // positions + normals only
    EXPECT_FALSE(pipelineSamplesATexture(*bind->pipeline));
}

TEST(ForwardShaderSetTest, OpacityIsNotPartOfTheVariantIdentity)
{
    // Opacity is a per-drawable VALUE, delivered in the `vine_draw` block — so a
    // translucent drawable takes exactly the same pipeline as an opaque one, and changing
    // the opacity never rebuilds the state wrapper. Before this, the opacity rode the
    // derived colour carrier's alpha, which forced a translucent drawable to keep that
    // attribute bound (and a rebuild whenever the opacity crossed 1).
    auto forward = makeForwardSet();
    ASSERT_NE(forward, nullptr);

    ::vsg::ref_ptr<::vsg::Group> root;
    const auto* opaque_bind = buildBareTriangleState(forward, root, 1.0f);
    ASSERT_NE(opaque_bind, nullptr);
    ASSERT_NE(opaque_bind->pipeline, nullptr);

    ::vsg::ref_ptr<::vsg::Group> half_root;
    const auto* half_bind = buildBareTriangleState(forward, half_root, 0.5f);
    ASSERT_NE(half_bind, nullptr);
    ASSERT_NE(half_bind->pipeline, nullptr);

    const auto* opaque_input = findVertexInputState(*opaque_bind->pipeline);
    const auto* half_input   = findVertexInputState(*half_bind->pipeline);
    ASSERT_NE(opaque_input, nullptr);
    ASSERT_NE(half_input, nullptr);
    EXPECT_EQ(opaque_input->vertexBindingDescriptions.size(), 2u); // positions + normals only, both ways
    EXPECT_EQ(half_input->vertexBindingDescriptions.size(), 2u);
    EXPECT_FALSE(pipelineSamplesATexture(*half_bind->pipeline));
}

TEST(ForwardShaderSetTest, TheFragmentStageScalesAlphaByTheDrawBlock)
{
    // The other half of the contract above: the value has to be READ. The fragment stage
    // multiplies the material's alpha by the block's params.x, and deliberately does NOT
    // take an opacity from the vertex colour's alpha (that would make one drawable's
    // opacity leak into every geometry sharing the vertex stream).
    const auto program = vine::graphics::builtinProgram(vine::graphics::ShaderPreset::StandardPhong);
    ASSERT_NE(program, nullptr);
    ASSERT_EQ(program->stageCount(), 2u);
    const auto* fs_stage = program->stage(1);
    ASSERT_NE(fs_stage, nullptr);
    const std::string fragment = fs_stage->source.stdstr();
    EXPECT_NE(fragment.find("material.diffuse.a * draw.params.x"), std::string::npos);
    EXPECT_EQ(fragment.find("alpha *= v_color.a;"), std::string::npos);
    // The block itself is declared at the L1 binding the backend assigns.
    EXPECT_NE(fragment.find("uniform VineDrawBlock"), std::string::npos);
    EXPECT_NE(fragment.find("binding = 3"), std::string::npos);
}

TEST(ForwardShaderSetTest, BuiltInSetKeepsTheFullCanonicalPrefix)
{
    // The built-in phong set declares the canonical attributes unconditionally, so the derived white carrier
    // and zero UVs stay bound there: the dropping above must not leak into the fallback path.
    ::vsg::ref_ptr<::vsg::Group> root;
    const auto*                  bind = buildBareTriangleState(::vsg::createPhongShaderSet(), root);
    ASSERT_NE(bind, nullptr);
    ASSERT_NE(bind->pipeline, nullptr);
    const auto* vertex_input = findVertexInputState(*bind->pipeline);
    ASSERT_NE(vertex_input, nullptr);
    EXPECT_EQ(vertex_input->vertexBindingDescriptions.size(), 4u);
}

}  // namespace
