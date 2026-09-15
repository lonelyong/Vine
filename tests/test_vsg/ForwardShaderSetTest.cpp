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
#include <vine/vsg/SceneBridge.hpp>
#include <vine/vsg/VsgPipelineFactory.hpp>

#include <vsg/core/Data.h>
#include <vsg/nodes/Group.h>
#include <vsg/nodes/StateGroup.h>
#include <vsg/state/ColorBlendState.h>
#include <vsg/state/ImageInfo.h>
#include <vsg/state/DepthStencilState.h>
#include <vsg/state/DescriptorSetLayout.h>
#include <vsg/state/GraphicsPipeline.h>
#include <vsg/state/PipelineLayout.h>
#include <vsg/state/ShaderStage.h>
#include <vsg/state/VertexInputState.h>
#include <vsg/state/ViewportState.h>
#include <vsg/utils/ShaderCompiler.h>
#include <vsg/utils/ShaderSet.h>

#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <initializer_list>
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
 * @param name Attribute binding name, e.g. "vine_Color".
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
    return buildVineShaderSet(vine::graphics::forwardProgram(), VkExtent2D{ 640, 360 }, true, true, 1);
}

/**
 * @brief Builds a triangle that authors only positions and normals.
 *
 * No loc2 colour and no UVs is exactly the geometry our forward set can shade with the variants that do not
 * declare vine_Color / vine_TexCoord0. The data builder still materialises the white opacity carrier and the
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
 * @brief Whether the pipeline's layout binds a sampled image at @p set / @p binding.
 *
 * By BINDING, not "a texture somewhere": the content set declares the shadow ABI unconditionally
 * (set 0 / binding 3 is the shadow map, compiled into every forward pipeline because the shadow term
 * samples it under a runtime switch — see ShaderAbi.hpp), so "this pipeline has a sampler" is true of
 * every variant now. A test about the DIFFUSE map (set 0 / binding 1, gated by the UV attribute) has to
 * ask about that binding.
 *
 * @param pipeline Pipeline to inspect.
 * @param set      Descriptor set to look in.
 * @param binding  Binding within that set.
 * @return true when that binding is a combined image sampler.
 */
bool pipelineSamplesAt(const ::vsg::GraphicsPipeline& pipeline, std::uint32_t set, std::uint32_t binding)
{
    if (pipeline.layout == nullptr || set >= pipeline.layout->setLayouts.size()) {
        return false;
    }
    const auto& set_layout = pipeline.layout->setLayouts[set];
    if (set_layout == nullptr) {
        return false;
    }
    for (const auto& declared : set_layout->bindings) {
        if (declared.binding == binding) {
            return declared.descriptorType == VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
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

TEST(ForwardShaderSetTest, OnlyProgramsWithUsableStagesGetASet)
{
    // A program the backend cannot compile into a set must be refused rather than shaded as phong:
    // a silently wrong shading model is worse than nothing drawn.
    EXPECT_NE(buildVineShaderSet(vine::graphics::forwardProgram(), VkExtent2D{ 640, 360 }, true, true, 1), nullptr);
    EXPECT_NE(buildVineShaderSet(vine::graphics::flatForwardProgram(), VkExtent2D{ 640, 360 }, true, true, 1), nullptr);
    EXPECT_EQ(buildVineShaderSet(nullptr, VkExtent2D{ 640, 360 }, true, true, 1), nullptr);
    // A program that exists but carries no stages (nothing to compile) is declined the same way:
    // the caller reports it and nothing is drawn, rather than a shading the host never named.
    auto stageless = vine::intrusive_ptr<vine::graphics::ShaderProgram>(new vine::graphics::ShaderProgram());
    EXPECT_EQ(buildVineShaderSet(stageless, VkExtent2D{ 640, 360 }, true, true, 1), nullptr);
    EXPECT_NE(makeForwardSet(), nullptr);
}

TEST(ForwardShaderSetTest, TheFlatProgramReusesTheForwardStagesWithItsDefine)
{
    // Flat shading is the forward program with ONE define, and the define has to land AFTER the
    // `#version` directive: GLSL requires the version first, and a source that fails to parse costs
    // the program its set — the slot then draws nothing and reports it, which is the honest outcome
    // but a picture nobody wanted (the first cut of this shipped vsg's flat set, drawing the
    // material colour with no lighting at all). Both halves are pinned here.
    const auto phong = vine::graphics::forwardProgram();
    const auto flat  = vine::graphics::flatForwardProgram();
    ASSERT_NE(phong, nullptr);
    ASSERT_NE(flat, nullptr);
    ASSERT_EQ(phong->stageCount(), 2u);
    ASSERT_EQ(flat->stageCount(), 2u);

    const auto* phong_vs = phong->stage(0);
    const auto* flat_vs  = flat->stage(0);
    ASSERT_NE(phong_vs, nullptr);
    ASSERT_NE(flat_vs, nullptr);
    EXPECT_EQ(flat_vs->source.stdstr(), phong_vs->source.stdstr()); // same vertex stage

    const auto* flat_fs = flat->stage(1);
    ASSERT_NE(flat_fs, nullptr);
    const std::string fragment = flat_fs->source.stdstr();
    // A define BEFORE the version directive is not valid GLSL, so the order is the contract.
    const auto version_at = fragment.find("#version");
    const auto define_at  = fragment.find("#define VINE_FLAT");
    ASSERT_NE(version_at, std::string::npos);
    ASSERT_NE(define_at, std::string::npos);
    EXPECT_LT(version_at, define_at);
    EXPECT_NE(fragment.find("cross(dFdy(vine_view_pos), dFdx(vine_view_pos))"), std::string::npos);
}

TEST(ForwardShaderSetTest, DeclaresTheCanonicalAttributesWithTheirGates)
{
    const auto shader_set = makeForwardSet();
    ASSERT_NE(shader_set, nullptr);

    // Location AND binding order matter: locations are the custom-program contract,
    // and vsg numbers a vertex binding by the order assignArray() accepts them.
    const auto* positions = findAttribute(*shader_set, "vine_Vertex");
    const auto* normals   = findAttribute(*shader_set, "vine_Normal");
    const auto* uv        = findAttribute(*shader_set, "vine_TexCoord0");
    const auto* color     = findAttribute(*shader_set, "vine_Color");
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

    const auto program = vine::graphics::forwardProgram();
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

    // The same parity check, against the engine's OTHER depth variant of the same
    // program: the two variants must carry the same state list (their difference is
    // the depth policy alone).
    const auto sibling = makeContentShaderSet(vine::graphics::forwardProgram(), VkExtent2D{ 640, 360 }, true, false, 1);
    ASSERT_NE(sibling, nullptr);
    EXPECT_EQ(states.size(), sibling->defaultGraphicsPipelineStates.size());
    for (std::size_t i = 0; i < states.size(); ++i) {
        // Captured first: typeid on a dereferenced expression evaluates it, and the
        // compiler warns when that expression may have side effects.
        const auto* mine   = states[i].get();
        const auto* theirs = sibling->defaultGraphicsPipelineStates[i].get();
        EXPECT_EQ(typeid(*mine).hash_code(), typeid(*theirs).hash_code()) << i;
    }
}

TEST(ForwardShaderSetTest, BothStagesGateTheSameInterfaceVariables)
{
    // The stages are the SDK's built-in program for the preset (BuiltinShaders.hpp):
    // this checks the engine's own shader text, not a backend copy of it.
    const auto program = vine::graphics::forwardProgram();
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

TEST(ForwardShaderSetTest, TheForwardStagesAskForEveryDefineTheBackendCanSet)
{
    // vsg assembles the source it hands glslang: it moves `#version` and every `#pragma import_defines` line
    // into a header and emits `#define <name>` ONLY for the names that pragma lists (ShaderCompiler.cpp,
    // combineSourceAndDefines). A define the compile settings carry but the source does not ask for is
    // dropped in SILENCE — no error from vsg, from glslang or from validation — and the branch that tests it
    // never compiles.
    //
    // That is not hypothetical: the texture and the vertex-colour branches of THIS shader were dead until the
    // pragma was added, and every assertion in this file stayed green through it (the names appear in the
    // source either way — see BothStagesGateTheSameInterfaceVariables above). The end-to-end proof is the
    // self-test's "built-in sampling" phase, which draws a textured quad and a cube-mapped quad through this
    // shader with NO program set.
    //
    // The names below are the ones the backend sets: VINE_VERTEX_COLOR / VINE_DIFFUSE_MAP through the set's
    // attribute and descriptor bindings (VsgPipelineFactory), and the texcoord KIND as a per-drawable
    // compile hint (SceneBridgePipeline) - exactly one of the two names, never neither.
    const char* const kBackendDefines[] = { "VINE_VERTEX_COLOR", "VINE_DIFFUSE_MAP", "VINE_TEXCOORD_UV",
                                            "VINE_TEXCOORD_CUBE" };

    for (const auto& program : { vine::graphics::forwardProgram(), vine::graphics::flatForwardProgram() }) {
        ASSERT_NE(program, nullptr);
        ASSERT_EQ(program->stageCount(), 2u);
        for (std::size_t i = 0; i < program->stageCount(); ++i) {
            const auto* stage = program->stage(i);
            ASSERT_NE(stage, nullptr);
            const std::string source = stage->source.stdstr();
            const auto        pragma = source.find("#pragma import_defines");
            ASSERT_NE(pragma, std::string::npos) << "stage " << i << " asks for no define at all";
            const auto        close = source.find(')', pragma);
            ASSERT_NE(close, std::string::npos);
            const std::string asked = source.substr(pragma, close - pragma);
            for (const char* define : kBackendDefines) {
                EXPECT_NE(asked.find(define), std::string::npos)
                    << define << " is set by the backend but not asked for in '" << asked << "'";
            }
        }
    }
}

TEST(ForwardShaderSetTest, ASampledTexcoordSlotMustStateItsKind)
{
    // The kind is NAMED, never defaulted: both stages fork on VINE_TEXCOORD_UV / VINE_TEXCOORD_CUBE, and the
    // sampled branch has no `#else` that quietly means one of them. A variant that samples the slot without
    // stating its kind is a build nobody asked for - the backend sets exactly one on every variant (see
    // SceneBridgePipeline) - so it has to FAIL instead of picking a sampler type on its own. glslang is the
    // only reader that can answer that, and it answers here.
    //
    // The no-define case is pinned too, and it is not decoration: VsgPipelineFactory compiles a program's
    // stages ONCE with no defines, before any drawable states its variant, so a source that cannot be built
    // that way would take the whole program (and every drawable using it) down with it.
    vsg::ShaderCompiler probe;
    if (!probe.supported()) GTEST_SKIP() << "this build has no glslang (see GlslCompileTest)";

    const auto program = vine::graphics::forwardProgram();
    ASSERT_NE(program, nullptr);
    ASSERT_EQ(program->stageCount(), 2u);

    const auto compiles = [](const std::string& stage_source, VkShaderStageFlagBits flag,
                             std::initializer_list<const char*> defines) {
        vsg::ShaderCompiler compiler;
        auto                 stage = vsg::ShaderStage::create(flag, "main", stage_source);
        // The defines travel on the stage's compile settings, and `create(flags, entry, source)` leaves
        // them UNSET (a null pointer, which the compiler replaces with its own defaults) — so they are
        // made here rather than assumed.
        if (stage->module->hints == nullptr) {
            stage->module->hints = vsg::ShaderCompileSettings::create();
        }
        for (const char* define : defines) {
            stage->module->hints->defines.insert(define);
        }
        return compiler.compile(stage);
    };

    for (std::size_t i = 0; i < program->stageCount(); ++i) {
        const auto* stage = program->stage(i);
        ASSERT_NE(stage, nullptr);
        const std::string source = stage->source.stdstr();
        const auto        flag   = (i == 0u) ? VK_SHADER_STAGE_VERTEX_BIT : VK_SHADER_STAGE_FRAGMENT_BIT;
        EXPECT_FALSE(compiles(source, flag, { "VINE_DIFFUSE_MAP" }))
            << "stage " << i << " samples a slot whose kind is unstated";
        EXPECT_TRUE(compiles(source, flag, { "VINE_DIFFUSE_MAP", "VINE_TEXCOORD_UV" })) << "stage " << i;
        EXPECT_TRUE(compiles(source, flag, { "VINE_DIFFUSE_MAP", "VINE_TEXCOORD_CUBE" })) << "stage " << i;
        EXPECT_TRUE(compiles(source, flag, {})) << "stage " << i << " is the program-level compile";
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

TEST(ForwardShaderSetTest, AProgramWithNoUsableStagesIsDeclinedNotSubstituted)
{
    // makeContentShaderSet is the one entry point every content set is built from (the window's
    // three depth-mode sets and each off-screen target's), and it answers with a set built from the
    // program it was handed — or with NOTHING.
    //
    // A program the backend cannot compile into a set (nothing at all, or one that carries no
    // stages) is declined rather than substituted: the caller reports it and draws nothing. Shading
    // it with another model (the engine's forward program, or a library's phong set) would show the
    // host a picture it did not ask for and cannot tell apart from the one it did — which is worse
    // than an empty frame whose reason it can read.
    const VkExtent2D extent{ 640, 360 };
    auto stageless = vine::intrusive_ptr<vine::graphics::ShaderProgram>(new vine::graphics::ShaderProgram());
    for (const bool depth_test : { true, false }) {
        for (const bool depth_write : { true, false }) {
            for (const int color_count : { 0, 1, 3 }) {
                for (const auto& program : { vine::intrusive_ptr<const vine::graphics::ShaderProgram>(
                                                  vine::graphics::forwardProgram()),
                                              vine::intrusive_ptr<const vine::graphics::ShaderProgram>(
                                                  vine::graphics::flatForwardProgram()) }) {
                    EXPECT_NE(makeContentShaderSet(program, extent, depth_test, depth_write, color_count), nullptr)
                        << "depth_test " << depth_test << " color_count " << color_count;
                }
                for (const auto& unusable : { vine::intrusive_ptr<const vine::graphics::ShaderProgram>(nullptr),
                                              vine::intrusive_ptr<const vine::graphics::ShaderProgram>(stageless) }) {
                    EXPECT_EQ(makeContentShaderSet(unusable, extent, depth_test, depth_write, color_count), nullptr)
                        << "a program the backend cannot compile into a set must be declined (the caller reports "
                        << "it and draws nothing)";
                }
            }
        }
    }
}

TEST(ForwardShaderSetTest, EveryContentSetIsTheEnginesOwn)
{
    // There is no switch: content shading is always the set of the program the caller named, whose
    // layout declares the vine_lights binding (vsg's built-in sets are not used at all — a set of
    // theirs carries their declarations, attribute locations and light source, a second shading ABI
    // to keep in step). There is no substitute for a program that cannot be used.
    const auto set = makeContentShaderSet(vine::graphics::forwardProgram(), VkExtent2D{ 640, 360 }, true, true, 1);
    ASSERT_NE(set, nullptr);
    // getDescriptorBinding reports "not declared" through its bool conversion.
    EXPECT_TRUE(static_cast<bool>(set->getDescriptorBinding("vine_lights")));

    // A program the backend cannot compile into a set (here: one with no stages) is DECLINED, so a
    // caller that only checks for null -- and draws nothing when it sees one -- never puts an
    // unexplained picture on screen. That a slot actually reports and skips is pinned in
    // SceneBridgeCacheOwnershipTest.
    auto stageless = vine::intrusive_ptr<vine::graphics::ShaderProgram>(new vine::graphics::ShaderProgram());
    EXPECT_EQ(makeContentShaderSet(stageless, VkExtent2D{ 640, 360 }, true, true, 1), nullptr)
        << "no usable stages means no set, not somebody else's";
}

TEST(ForwardShaderSetTest, ForwardSetDropsDerivedColourAndUvs)
{
    // Our forward set declares vine_Color / vine_TexCoord0 behind defines, so a geometry that authors neither
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
    EXPECT_FALSE(pipelineSamplesAt(*bind->pipeline, 0u, 1u)) << "the diffuse map's binding is the one the UV gate drops";
    EXPECT_TRUE(pipelineSamplesAt(*bind->pipeline, 0u, 3u))
        << "the shadow map is NOT gated: the content set declares it either way and the shader takes the "
           "unshadowed path at runtime (ShaderAbi.hpp)";
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
    EXPECT_FALSE(pipelineSamplesAt(*half_bind->pipeline, 0u, 1u));
}

TEST(ForwardShaderSetTest, TheFragmentStageScalesAlphaByTheDrawBlock)
{
    // The other half of the contract above: the value has to be READ, and it has to be the ONLY
    // transparency input. The fragment stage's alpha is the block's params.x — the drawable's
    // opacity — and deliberately does NOT read the material's alpha, the sampled texel's alpha or
    // the vertex colour's alpha:
    //   * the material is shared by every drawable that uses it, so a translucent material would
    //     make all of them translucent while the engine sorts by the per-drawable opacity;
    //   * the texel's alpha is discarded by the deferred path (its lighting program writes alpha 1),
    //     so reading it here would make one asset translucent in forward and opaque in deferred;
    //   * the vertex colour's alpha would leak one drawable's opacity into every geometry sharing
    //     the vertex stream.
    // Blending is on for every content pipeline, so an alpha that nobody classified as transparent
    // is not merely wrong in the buffer: it is blended in an order that was never computed.
    const auto program = vine::graphics::forwardProgram();
    ASSERT_NE(program, nullptr);
    ASSERT_EQ(program->stageCount(), 2u);
    const auto* fs_stage = program->stage(1);
    ASSERT_NE(fs_stage, nullptr);
    const std::string fragment = fs_stage->source.stdstr();
    EXPECT_NE(fragment.find("float alpha = draw.params.x;"), std::string::npos);
    EXPECT_EQ(fragment.find("material.diffuse.a"), std::string::npos);
    EXPECT_EQ(fragment.find("alpha *= texel.a;"), std::string::npos);
    EXPECT_EQ(fragment.find("alpha *= vine_color.a;"), std::string::npos);
    // The block itself is declared in its OWN set, because it is bound per drawable with a
    // dynamic offset (the scene shares one buffer and one descriptor set).
    EXPECT_NE(fragment.find("uniform VineDrawBlock"), std::string::npos);
    EXPECT_NE(fragment.find("layout(set = 1, binding = 0"), std::string::npos);
}

TEST(ForwardShaderSetTest, ThePerDrawBlockIsSetOneWithItsOwnBinding)
{
    // Set 1 is the per-draw block's, and it has to be declarable WITHOUT a device: the layout
    // comes from a custom descriptor-set binding (the bridge binds it per drawable, so the
    // bind command cannot be one shared command per variant), while the set RANGE comes from
    // the descriptor declaration vsg derives it from. Both are pinned here, because a shader
    // declaring a set the pipeline layout does not have is an invalid pipeline — and that is a
    // driver fault, not a diagnostic.
    const auto set = makeForwardSet();
    ASSERT_NE(set, nullptr);

    ASSERT_EQ(set->customDescriptorSetBindings.size(), 1u);
    const auto& custom = set->customDescriptorSetBindings.front();
    ASSERT_NE(custom, nullptr);
    EXPECT_EQ(custom->set, 1u);
    const auto draw_layout = custom->createDescriptorSetLayout();
    ASSERT_NE(draw_layout, nullptr);
    ASSERT_EQ(draw_layout->bindings.size(), 1u);
    EXPECT_EQ(draw_layout->bindings.front().binding, 0u);
    EXPECT_EQ(draw_layout->bindings.front().descriptorType, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC);
    // The declaration that carries the range must describe the same set, or the layout the
    // configurator builds for set 1 would disagree with the one the pipeline layout gets.
    const auto declared = set->getDescriptorBinding("vine_draw");
    ASSERT_TRUE(static_cast<bool>(declared));
    EXPECT_EQ(declared.set, 1u);
    EXPECT_EQ(declared.binding, 0u);
    EXPECT_EQ(declared.descriptorType, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC);
    EXPECT_EQ(declared.descriptorCount, 1u);

    const auto layout = set->createPipelineLayout({});
    ASSERT_NE(layout, nullptr);
    EXPECT_EQ(layout->setLayouts.size(), 2u); // set 0 (material/lights) + set 1 (per draw)
}

TEST(ForwardShaderSetTest, EveryProgramTheEngineBuildsReadsItsOwnLightBlock)
{
    // A slot feeds the light source its SHADER SET reads, so the set has to declare the block the slot
    // fills. The engine builds a set for every program the host names (see makeContentShaderSet) and
    // none of them may fall back to another library's light data: a slot whose set wants lights the
    // slot never feeds draws UNLIT, with nothing to see in the frame that says so. Pinned on the two
    // forward programs, checked the way the slot checks it — the declared binding.
    for (const auto& program :
         { vine::graphics::forwardProgram(), vine::graphics::flatForwardProgram() }) {
        const auto set = makeContentShaderSet(program, VkExtent2D{ 640, 360 }, true, true, 1);
        ASSERT_NE(set, nullptr) << "the engine's own programs get an engine set";
        const auto lights = set->getDescriptorBinding("vine_lights");
        ASSERT_TRUE(static_cast<bool>(lights)) << "without the block the slot's lights reach nothing";
        EXPECT_EQ(lights.set, 0u);
        EXPECT_EQ(lights.binding, 2u);
        EXPECT_EQ(lights.descriptorType, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER);
    }
}

TEST(ForwardShaderSetTest, ABridgeWithNoShaderSetReportsAndDrawsNothing)
{
    // A slot that was never given a set — and therefore also a slot whose program the backend
    // could not compile into one, which is declined (see makeContentShaderSet) — draws NOTHING and
    // says why. The
    // alternative (inventing a default, or borrowing another library's set) puts a picture on screen
    // that nobody can explain, and it hides the actual problem: the shading the host asked for does
    // not exist yet.
    SceneBridge bridge; // no setShaderSet() on purpose
    std::vector<vine::graphics::RenderDiagnostic> reported;
    bridge.setDiagnosticSink([&reported](const vine::graphics::RenderDiagnostic& diagnostic) {
        reported.push_back(diagnostic);
    });

    auto root     = vsg::Group::create();
    auto geometry = makeBareTriangle();
    auto material = vine::graphics::MaterialPtr(new vine::graphics::Material());
    std::vector<vine::graphics::RenderCommand> commands;
    for (int i = 0; i < 3; ++i) { // several drawables and several syncs: one report, not one per loop
        commands.clear();
        commands.emplace_back(geometry, material, vine::math::Mat4d());
        bridge.syncRenderCommands(commands, root.get(), nullptr);
    }

    EXPECT_TRUE(root->children.empty()) << "no set means nothing is drawn, not something shaded";
    ASSERT_EQ(reported.size(), 1u) << "the reason is reported once, not once per drawable or frame";
    EXPECT_EQ(reported.front().category, vine::graphics::DiagnosticCategory::ShaderFallback);
    EXPECT_EQ(reported.front().severity, vine::graphics::DiagnosticSeverity::Error);
    EXPECT_EQ(bridge.diagnosticCount(vine::graphics::DiagnosticCategory::ShaderFallback), 1u);

    // Injecting a set re-arms the report: a slot that loses its set again must say so again instead
    // of going quiet (the report is per SET, not per session).
    bridge.setShaderSet(makeForwardSet());
    commands.clear();
    commands.emplace_back(geometry, material, vine::math::Mat4d());
    bridge.syncRenderCommands(commands, root.get(), nullptr);
    EXPECT_EQ(root->children.size(), 1u) << "with a set the drawable is back";

    bridge.setShaderSet(nullptr);
    bridge.syncRenderCommands(commands, root.get(), nullptr);
    EXPECT_EQ(bridge.diagnosticCount(vine::graphics::DiagnosticCategory::ShaderFallback), 2u);
}

TEST(ForwardShaderSetTest, AProgramThatCannotShadeTheDeclaredShadowIsReported)
{
    // The content set declares the shadow ABI unconditionally (it is shared per (target, depth mode),
    // so a shadowed variant would double that cache — see buildVineShaderSet), which means a program
    // whose text never declares the map would be bound a map it never reads: shaded unshadowed, with
    // nothing in the frame saying why. That is the same complaint the pipeline builder makes about a
    // PATH that builds no shadow pass, one layer down, and this report is where a HOST's own content
    // program gets it.
    //
    // Reported where a VARIANT is built, so the scene says it once (a built variant is reused).
    std::vector<vine::graphics::RenderDiagnostic> reported;
    vine::vsg::SceneBridge                       bridge;
    bridge.setShaderSet(makeForwardSet());
    bridge.setDiagnosticSink([&reported](const vine::graphics::RenderDiagnostic& diagnostic) {
        reported.push_back(diagnostic);
    });
    // A shadow WAS declared for this slot (that is what the flag says): nothing here rasterises, so the
    // map only has to be a real binding target and the block a real block.
    bridge.setShadowMap(::vsg::ImageInfo::create(), /*declared*/ true);
    bridge.setShadowData(::vsg::ubyteArray::create(static_cast<std::uint32_t>(sizeof(vine::graphics::VineShadowBlock))));

    auto root     = ::vsg::Group::create();
    auto geometry = makeBareTriangle();
    auto material = vine::graphics::MaterialPtr(new vine::graphics::Material());

    // A HOST program: two stages of its own, shading its own way, declaring no shadow_map (the engine's
    // forward program does declare it, which the second half of this test checks). It has to compile —
    // the backend compiles what a program names — so it is minimal rather than absent.
    auto program = vine::intrusive_ptr<vine::graphics::ShaderProgram>(new vine::graphics::ShaderProgram());
    program->setName(u8"content_without_shadow");
    {
        vine::graphics::ShaderStage vertex;
        vertex.type   = vine::graphics::ShaderStageType::Vertex;
        vertex.source = vine::String(u8"#version 450\n"
                                     u8"layout(location = 0) in vec3 vine_Vertex;\n"
                                     u8"layout(location = 1) in vec3 vine_Normal;\n"
                                     u8"layout(push_constant) uniform pc { mat4 projection; mat4 modelView; };\n"
                                     u8"void main() { gl_Position = projection * modelView * vec4(vine_Vertex, 1.0); }\n");
        program->addStage(vertex);
        vine::graphics::ShaderStage fragment;
        fragment.type   = vine::graphics::ShaderStageType::Fragment;
        fragment.source = vine::String(u8"#version 450\n"
                                       u8"layout(location = 0) out vec4 out_color;\n"
                                       u8"void main() { out_color = vec4(1.0); }\n");
        program->addStage(fragment);
    }

    std::vector<vine::graphics::RenderCommand> commands;
    commands.emplace_back(geometry, material, vine::math::Mat4d());
    commands.front().program = program;
    for (int i = 0; i < 3; ++i) {   // several syncs: the variant is built once
        bridge.syncRenderCommands(commands, root.get(), nullptr);
    }
    ASSERT_EQ(root->children.size(), 1u) << "the drawable is still drawn: this is a diagnostic, not a refusal";
    ASSERT_EQ(reported.size(), 1u) << "once per built variant, not once per frame";
    EXPECT_EQ(reported.front().severity, vine::graphics::DiagnosticSeverity::Warning);
    EXPECT_EQ(reported.front().category, vine::graphics::DiagnosticCategory::UnsupportedRequest);
    EXPECT_NE(reported.front().message.find(u8"shadow_map"), vine::String::npos);

    // The engine's own forward program declares the map, so it says nothing: the report is about the
    // program that cannot use what its pass handed it, not about every forward draw.
    std::vector<vine::graphics::RenderDiagnostic> quiet;
    vine::vsg::SceneBridge                       declaring;
    declaring.setShaderSet(makeForwardSet());
    declaring.setDiagnosticSink([&quiet](const vine::graphics::RenderDiagnostic& diagnostic) {
        quiet.push_back(diagnostic);
    });
    declaring.setShadowMap(::vsg::ImageInfo::create(), /*declared*/ true);
    declaring.setShadowData(::vsg::ubyteArray::create(static_cast<std::uint32_t>(sizeof(vine::graphics::VineShadowBlock))));
    auto declaring_root = ::vsg::Group::create();
    commands.front().program = vine::graphics::forwardProgram();
    declaring.syncRenderCommands(commands, declaring_root.get(), nullptr);
    EXPECT_EQ(declaring_root->children.size(), 1u);
    EXPECT_TRUE(quiet.empty()) << "the engine's forward program declares the map its pass declared";
}

TEST(ForwardShaderSetTest, AForeignSetIsReportedInsteadOfQuietlyUnbound)
{
    // The engine's own sets declare `vine_Vertex` (and the other three canonical names), and the bridge
    // binds every array by those names. It used to also try the `vsg_*` spelling, so a set of another
    // library's could still receive the arrays; that path is gone — the engine never hands a slot a
    // foreign set, and keeping a second spelling alive meant keeping two ABIs in step for a case the
    // engine does not have. What that removes is a SUPPORTED input, so what replaces it is not silence:
    // a set that declares none of the names gets no vertex data at all, and the drawable would
    // degenerate (nothing drawn) while validation stays clean. Pinned with vsg's own phong set, which
    // is exactly such a set.
    std::vector<vine::graphics::RenderDiagnostic> reported;
    vine::vsg::SceneBridge                       bridge;
    bridge.setShaderSet(::vsg::createPhongShaderSet());
    bridge.setDiagnosticSink([&reported](const vine::graphics::RenderDiagnostic& diagnostic) {
        reported.push_back(diagnostic);
    });

    auto root     = ::vsg::Group::create();
    auto material = vine::graphics::MaterialPtr(new vine::graphics::Material());
    std::vector<vine::graphics::RenderCommand> commands;
    commands.emplace_back(makeBareTriangle(), material, vine::math::Mat4d());
    bridge.syncRenderCommands(commands, root.get(), nullptr);

    ASSERT_EQ(reported.size(), 1u) << "a set that declares none of the engine's attribute names must say so";
    EXPECT_EQ(reported.front().severity, vine::graphics::DiagnosticSeverity::Warning);
    EXPECT_EQ(reported.front().category, vine::graphics::DiagnosticCategory::ContentSkipped);
}

/**
 * @brief A minimal usable program: positions in the vertex stage, one colour out of the fragment one.
 *
 * The stage table is keyed by the program itself, so the bound test needs DISTINCT programs; what
 * they shade does not matter (no device is involved, and a set is assembled from the same ABI
 * declarations for all of them).
 *
 * @return The program.
 */
vine::intrusive_ptr<const vine::graphics::ShaderProgram> makeMinimalProgram()
{
    auto program = vine::intrusive_ptr<vine::graphics::ShaderProgram>(new vine::graphics::ShaderProgram());
    vine::graphics::ShaderStage vs;
    vs.type   = vine::graphics::ShaderStageType::Vertex;
    vs.source = u8"#version 450\n"
                u8"layout(location = 0) in vec3 vine_Vertex;\n"
                u8"void main() { gl_Position = vec4(vine_Vertex, 1.0); }\n";
    program->addStage(vs);
    vine::graphics::ShaderStage fs;
    fs.type   = vine::graphics::ShaderStageType::Fragment;
    fs.source = u8"#version 450\n"
                u8"layout(location = 0) out vec4 outColor;\n"
                u8"void main() { outColor = vec4(1.0, 1.0, 1.0, 1.0); }\n";
    program->addStage(fs);
    return program;
}

TEST(ForwardShaderSetTest, TheCompiledStageTableIsBoundedAndStillUsableAfterATrim)
{
    // The compiled stages of a program are remembered in a PROCESS-wide table: the programs a session
    // shades by default are the engine's own singletons, so a second session should not pay for
    // compiling them again — but nothing releases that table, and an entry holds the program and its
    // SPIR-V, so without a bound a host that churns its shaders (one revision per edit) would leave
    // one entry per revision for the life of the process. This pins both halves of the bargain: the
    // table stays within its bound, and a program whose entry was trimmed still builds a set (the
    // bound costs a recompile, never the answer).
    const VkExtent2D extent{ 640, 360 };
    const auto       first  = makeMinimalProgram();
    ASSERT_NE(makeContentShaderSet(first, extent, true, true, 1), nullptr);

    std::vector<vine::intrusive_ptr<const vine::graphics::ShaderProgram>> churn;
    churn.reserve(kMaxCompiledStageEntries + 2u);
    for (std::size_t i = 0; i < kMaxCompiledStageEntries + 2u; ++i) {
        churn.push_back(makeMinimalProgram());
        ASSERT_NE(makeContentShaderSet(churn.back(), extent, true, true, 1), nullptr) << "program " << i;
    }

    EXPECT_LE(compiledStageCacheCount(), kMaxCompiledStageEntries)
        << "the table grew past its bound: nothing releases it, so the bound is what keeps a shader "
           "editor from leaking one entry per edit for the life of the process";
    EXPECT_LT(compiledStageCacheCount(), churn.size())
        << "the table remembered every program it ever compiled, i.e. it never trimmed";

    // The trimmed program is asked for again: a trimmed entry must recompile rather than be answered
    // with a stale or empty stage list.
    EXPECT_NE(makeContentShaderSet(first, extent, true, true, 1), nullptr)
        << "a trimmed program must still build a set";
}

}  // namespace
