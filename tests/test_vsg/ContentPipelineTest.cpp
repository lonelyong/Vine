/**
 * @brief Content pipelines: one `vsg::GraphicsPipeline` per identity, with the editable half declared
 * dynamic (see `.ai/design/vsg-reimplementation.md` D3 and milestone M2c-2b-2).
 *
 * What these cases pin:
 *
 *   * one identity, one pipeline object - a second acquire of the same key hands back the SAME pipeline
 *     (the pool decides, this layer only builds what it asks for);
 *   * the DYNAMIC half is declared on the pipeline (depth test/write/compare, cull, front face, polygon
 *     mode, topology, blend enable/equation, and the viewport and scissor) - so a `StateNode` edit or a
 *     resize can never need a new pipeline, which is the failure family this layer exists to remove;
 *   * the layout binds the block set and the ABI's push budget - nothing else, because the blocks arrive
 *     through dynamic offsets;
 *   * an evicted identity costs this layer its object too (no keep-alive that nothing can look up);
 *   * a shader pair that does not compile yields no layer at all, rather than a pipeline that cannot shade.
 *
 * No device is created here: a `vsg::GraphicsPipeline` is a create-info until a `vsg::Context` compiles it
 * as part of a command graph, and GLSL compiles to SPIR-V in process. The GPU-side compilation is the
 * recording phase's business.
 */

#include <gtest/gtest.h>

#include <algorithm>
#include <cstdint>
#include <memory>

#include <vsg/state/ColorBlendState.h>
#include <vsg/state/DepthStencilState.h>
#include <vsg/state/DescriptorSetLayout.h>
#include <vsg/state/DynamicState.h>
#include <vsg/state/GraphicsPipeline.h>
#include <vsg/state/InputAssemblyState.h>
#include <vsg/state/MultisampleState.h>
#include <vsg/state/RasterizationState.h>
#include <vsg/state/VertexInputState.h>
#include <vsg/state/ViewportState.h>

#include <vine/graphics/BuiltinShaders.hpp>
#include <vine/graphics/ShaderProgram.hpp>

#include <vine/vsg/api/ContentPipeline.hpp>
#include <vine/vsg/api/ContentSources.hpp>
#include <vine/vsg/api/ProgramAbi.hpp>
#include <vine/vsg/core/StateRegistry.hpp>
#include <vine/vsg/core/VariantPool.hpp>

using vine::vsg::buildScreenProgramFacts;
using vine::vsg::ContentPipeline;
using vine::vsg::FactMiss;
using vine::vsg::ProgramFacts;
using vine::vsg::core::DynamicState;
using vine::vsg::core::PipelineKey;
using vine::vsg::core::StateRegistry;
using vine::vsg::core::VariantPool;

namespace
{

/// @brief A shader pair the compiler accepts: one vec3 position in, a red fragment out.
ContentPipeline::Shaders contentShaders()
{
    ContentPipeline::Shaders shaders;
    shaders.vertex = "#version 450\n"
                     "layout(location = 0) in vec3 position;\n"
                     "layout(push_constant) uniform pc { mat4 projection; mat4 modelView; } pc_data;\n"
                     "void main() { gl_Position = pc_data.projection * pc_data.modelView * vec4(position, 1.0); }\n";
    shaders.fragment = "#version 450\n"
                       "layout(location = 0) out vec4 outColor;\n"
                       "void main() { outColor = vec4(1.0, 0.0, 0.0, 1.0); }\n";
    return shaders;
}

/// @brief The one vertex stream the shader reads.
ContentPipeline::VertexBinding positionBinding()
{
    return { 0U, sizeof(float) * 3U, false };
}

ContentPipeline::VertexAttribute positionAttribute()
{
    return { 0U, 0U, VK_FORMAT_R32G32B32_SFLOAT, 0U };
}

/// @brief An identity with all identity-layer inputs filled in.
PipelineKey contentKey(std::uint64_t revision)
{
    static int program = 0;
    PipelineKey key;
    key.program                      = &program;
    key.revision                     = revision;
    key.vertex_layout.canonical_mask = 0x1U;
    key.compatibility.samples        = 1U;
    return key;
}

/// @brief The bindings and push ranges @p shaders declare (the layer is built from these, not from a set).
vine::vsg::ProgramAbi abiOf(const ContentPipeline::Shaders& shaders)
{
    vine::vsg::ProgramAbi abi;
    EXPECT_EQ(vine::vsg::scanProgramAbi(shaders.vertex, shaders.fragment, {}, abi), vine::vsg::FactMiss::None);
    return abi;
}

/// @brief The layer under test: the one-stream shader pair, built from its own declarations.
std::unique_ptr<ContentPipeline> makeLayer()
{
    // The declared layout is copied into the pipeline's own state objects, so these locals may die here.
    const ContentPipeline::Shaders         shaders   = contentShaders();
    const ContentPipeline::VertexBinding   binding   = positionBinding();
    const ContentPipeline::VertexAttribute attribute = positionAttribute();
    return ContentPipeline::create(abiOf(shaders), std::span<const ContentPipeline::VertexBinding>(&binding, 1),
                                   std::span<const ContentPipeline::VertexAttribute>(&attribute, 1), shaders);
}

/// @brief The layer under test with caller-chosen shader text.
std::unique_ptr<ContentPipeline> makeLayerWith(const ContentPipeline::Shaders& shaders)
{
    const ContentPipeline::VertexBinding   binding   = positionBinding();
    const ContentPipeline::VertexAttribute attribute = positionAttribute();
    return ContentPipeline::create(abiOf(shaders), std::span<const ContentPipeline::VertexBinding>(&binding, 1),
                                   std::span<const ContentPipeline::VertexAttribute>(&attribute, 1), shaders);
}

/// @brief The layer under test built with explicit settings (what the identity says beyond the text).
std::unique_ptr<ContentPipeline> makeLayerWithSettings(const ContentPipeline::Settings& settings)
{
    const ContentPipeline::Shaders         shaders   = contentShaders();
    const ContentPipeline::VertexBinding   binding   = positionBinding();
    const ContentPipeline::VertexAttribute attribute = positionAttribute();
    return ContentPipeline::create(abiOf(shaders), std::span<const ContentPipeline::VertexBinding>(&binding, 1),
                                   std::span<const ContentPipeline::VertexAttribute>(&attribute, 1), shaders,
                                   settings);
}

/// @brief Finds the state of type @p T in a pipeline's list.
template <typename T>
const T* stateOf(const ::vsg::GraphicsPipeline& pipeline)
{
    for (const auto& state : pipeline.pipelineStates) {
        if (const auto* found = dynamic_cast<const T*>(state.get())) {
            return found;
        }
    }
    return nullptr;
}

/// @brief The full-screen ABI's shader pair: the generated triangle plus a fragment stage reading a sampler.
ContentPipeline::Shaders screenShaders()
{
    ContentPipeline::Shaders shaders;
    shaders.vertex = "#version 450\n"
                     "layout(location = 0) out vec2 vine_uv;\n"
                     "void main() { vine_uv = vec2(float((gl_VertexIndex << 1) & 2), float(gl_VertexIndex & 2));\n"
                     "              gl_Position = vec4(vine_uv * 2.0 - 1.0, 0.0, 1.0); }\n";
    shaders.fragment = "#version 450\n"
                       "layout(location = 0) in vec2 vine_uv;\n"
                       "layout(location = 0) out vec4 out_color;\n"
                       "layout(binding = 0) uniform sampler2D picture;\n"
                       "void main() { out_color = texture(picture, vine_uv); }\n";
    return shaders;
}

/// @brief The layer a full-screen pair declares: the abi is scanned from the same two texts (the engine's
///        table entry does exactly that - see api/ContentSources), so the layout the layer builds IS what
///        the text says.
std::unique_ptr<ContentPipeline> screenLayer(const ContentPipeline::Shaders& shaders)
{
    vine::vsg::ProgramAbi abi;
    if (vine::vsg::scanProgramAbi(shaders.vertex, shaders.fragment, {}, abi) != FactMiss::None) {
        return nullptr;
    }
    return ContentPipeline::createScreen(abi, shaders);
}

/// @brief An identity of the full-screen kind (kind = Screen: the other descriptor ABI).
PipelineKey screenKey(std::uint64_t revision, std::uint32_t sampled_colors = 1U)
{
    static int program = 0;
    PipelineKey key;
    key.kind                     = vine::vsg::core::DrawKind::Screen;
    key.program                  = &program;
    key.revision                 = revision;
    key.compatibility.samples    = 1U;
    key.sampled_color_count      = sampled_colors;
    return key;
}

}  // namespace

TEST(ContentPipelineTest, TheSameIdentityIsBuiltOnce)
{
    auto layer = makeLayer();
    ASSERT_NE(layer, nullptr) << "the shader pair must compile";

    VariantPool      pool;
    const PipelineKey key = contentKey(1);

    const auto first  = layer->acquire(pool, key);
    const auto second = layer->acquire(pool, key);

    EXPECT_EQ(first.action, VariantPool::Action::Created);
    EXPECT_EQ(second.action, VariantPool::Action::Reused);
    ASSERT_NE(first.pipeline, nullptr);
    EXPECT_EQ(first.pipeline, second.pipeline) << "one identity is one pipeline OBJECT, not one per frame";
    EXPECT_EQ(first.id, second.id);
    EXPECT_EQ(pool.created(), 1U);
    EXPECT_EQ(layer->compiles(), 1U);
    EXPECT_EQ(layer->pipelines(), 1U);
    EXPECT_TRUE(layer->agreesWithPool(pool));
}

TEST(ContentPipelineTest, AnIdentityChangeIsANewPipeline)
{
    auto layer = makeLayer();
    ASSERT_NE(layer, nullptr);

    VariantPool pool;
    const auto  first  = layer->acquire(pool, contentKey(1));
    const auto  second = layer->acquire(pool, contentKey(2));  // the program's revision moved

    EXPECT_EQ(second.action, VariantPool::Action::Created);
    EXPECT_NE(first.pipeline, second.pipeline);
    EXPECT_NE(first.id, second.id);
    EXPECT_EQ(layer->compiles(), 2U);
    EXPECT_EQ(layer->pipelines(), 2U);
    EXPECT_TRUE(layer->agreesWithPool(pool));
}

TEST(ContentPipelineTest, TheTopologyTheSettingsNameIsWhatThePipelineBakes)
{
    // The API restricts a dynamically set topology to the CLASS its pipeline was created with
    // (VUID-vkCmdSetPrimitiveTopology-... unless the implementation reports
    // dynamicPrimitiveTopologyUnrestricted), so a point cloud drawn through a triangle-baked pipeline is
    // undefined behaviour rather than a picture. The class is therefore identity, and the create-info has to
    // state the one the key names - a default layer keeps the engine's own default.
    ContentPipeline::Settings points;
    points.topology = vine::graphics::Topology::Points;
    auto point_layer = makeLayerWithSettings(points);
    ASSERT_NE(point_layer, nullptr);

    VariantPool pool;
    const auto  point_result = point_layer->acquire(pool, contentKey(1));
    ASSERT_NE(point_result.pipeline, nullptr);
    const auto* point_assembly = stateOf<::vsg::InputAssemblyState>(*point_result.pipeline);
    ASSERT_NE(point_assembly, nullptr);
    EXPECT_EQ(point_assembly->topology, VK_PRIMITIVE_TOPOLOGY_POINT_LIST);

    auto default_layer = makeLayer();
    ASSERT_NE(default_layer, nullptr);
    const auto default_result = default_layer->acquire(pool, contentKey(2));
    ASSERT_NE(default_result.pipeline, nullptr);
    const auto* default_assembly = stateOf<::vsg::InputAssemblyState>(*default_result.pipeline);
    ASSERT_NE(default_assembly, nullptr);
    EXPECT_EQ(default_assembly->topology, VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST)
        << "an unstated topology is the engine's own default, not whatever the last layer baked";
}

TEST(ContentPipelineTest, TheEditableHalfIsDeclaredDynamic)
{
    auto layer = makeLayer();
    ASSERT_NE(layer, nullptr);

    VariantPool pool;
    const auto  result = layer->acquire(pool, contentKey(1));
    ASSERT_NE(result.pipeline, nullptr);

    const auto* dynamic = stateOf<::vsg::DynamicState>(*result.pipeline);
    ASSERT_NE(dynamic, nullptr) << "every content pipeline declares its dynamic states";
    const auto declares = [dynamic](VkDynamicState state) {
        return std::find(dynamic->dynamicStates.begin(), dynamic->dynamicStates.end(), state) !=
               dynamic->dynamicStates.end();
    };
    EXPECT_TRUE(declares(VK_DYNAMIC_STATE_DEPTH_TEST_ENABLE));
    EXPECT_TRUE(declares(VK_DYNAMIC_STATE_DEPTH_WRITE_ENABLE));
    EXPECT_TRUE(declares(VK_DYNAMIC_STATE_DEPTH_COMPARE_OP));
    EXPECT_TRUE(declares(VK_DYNAMIC_STATE_CULL_MODE));
    EXPECT_TRUE(declares(VK_DYNAMIC_STATE_FRONT_FACE));
    EXPECT_TRUE(declares(VK_DYNAMIC_STATE_POLYGON_MODE_EXT));
    EXPECT_TRUE(declares(VK_DYNAMIC_STATE_PRIMITIVE_TOPOLOGY));
    EXPECT_TRUE(declares(VK_DYNAMIC_STATE_COLOR_BLEND_ENABLE_EXT));
    EXPECT_TRUE(declares(VK_DYNAMIC_STATE_COLOR_BLEND_EQUATION_EXT));
    EXPECT_TRUE(declares(VK_DYNAMIC_STATE_VIEWPORT)) << "an extent is a command, never a compile";
    EXPECT_TRUE(declares(VK_DYNAMIC_STATE_SCISSOR));

    // The create-info still carries the structs Vulkan requires; the vertex layout is the declared one.
    const auto* vertex_input = stateOf<::vsg::VertexInputState>(*result.pipeline);
    ASSERT_NE(vertex_input, nullptr);
    ASSERT_EQ(vertex_input->vertexBindingDescriptions.size(), 1U);
    EXPECT_EQ(vertex_input->vertexBindingDescriptions[0].stride, 12U);
    ASSERT_EQ(vertex_input->vertexAttributeDescriptions.size(), 1U);
    EXPECT_EQ(vertex_input->vertexAttributeDescriptions[0].format, VK_FORMAT_R32G32B32_SFLOAT);
    EXPECT_NE(stateOf<::vsg::RasterizationState>(*result.pipeline), nullptr);
    EXPECT_NE(stateOf<::vsg::DepthStencilState>(*result.pipeline), nullptr);
    EXPECT_NE(stateOf<::vsg::InputAssemblyState>(*result.pipeline), nullptr);
    EXPECT_NE(stateOf<::vsg::MultisampleState>(*result.pipeline), nullptr);
    EXPECT_NE(stateOf<::vsg::ViewportState>(*result.pipeline), nullptr);
    const auto* blend = stateOf<::vsg::ColorBlendState>(*result.pipeline);
    ASSERT_NE(blend, nullptr);
    EXPECT_EQ(blend->attachments.size(), 1U) << "the blend state follows the colour attachment count";
}

TEST(ContentPipelineTest, TheLayoutIsExactlyWhatTheTextDeclares)
{
    auto layer = makeLayer();
    ASSERT_NE(layer, nullptr);

    VariantPool pool;
    const auto  result = layer->acquire(pool, contentKey(1));
    ASSERT_NE(result.pipeline, nullptr);
    ASSERT_NE(result.pipeline->layout, nullptr);

    // This program declares ONE thing: the camera matrices as a push block. So the layout has no descriptor
    // sets at all and one push range - the range the TEXT names (128 bytes, read by the vertex stage), not a
    // constant this backend would have guessed.
    EXPECT_TRUE(result.pipeline->layout->setLayouts.empty()) << "the text declares no blocks and no samplers";
    ASSERT_EQ(result.pipeline->layout->pushConstantRanges.size(), 1U);
    const VkPushConstantRange& push = result.pipeline->layout->pushConstantRanges.front();
    EXPECT_EQ(push.stageFlags, VK_SHADER_STAGE_VERTEX_BIT);
    EXPECT_EQ(push.offset, 0U);
    EXPECT_EQ(push.size, 128U) << "the two matrices the text declares (see ShaderAbi.hpp)";
    ASSERT_EQ(layer->stages().size(), 2U);
    EXPECT_NE(layer->stages()[0]->module, nullptr) << "both stages are SPIR-V modules";
    EXPECT_NE(layer->stages()[1]->module, nullptr);
}

TEST(ContentPipelineTest, AScreenLayerBindsItsSamplersAtSetZeroAndHasNoBlocks)
{
    // The engine's TWO descriptor ABIs, and the whole difference between them: a content layer puts the blocks
    // at set 0 and the sampled inputs at set 1; a full-screen layer puts the samplers at set 0 and nothing
    // else, because that is where the engine's own screen programs declare theirs (`layout(binding = 0) uniform
    // sampler2D`, no set qualifier). A layer that mixed the two would compile a pipeline the draw cannot bind.
    auto layer = screenLayer(screenShaders());
    ASSERT_NE(layer, nullptr) << "the full-screen pair must compile";
    EXPECT_EQ(layer->kind(), vine::vsg::core::DrawKind::Screen);

    // A CUBE declaration is refused HERE, unlike a content layer's: the full-screen ABI binds the SOURCE's
    // attachments and depth - 2D views, one per binding - so a text declaring a cube asks for an image this
    // ABI has nowhere to take from (a cube map is a content drawable's own map, see api/ContentImages).
    ContentPipeline::Shaders cube_screen = screenShaders();
    cube_screen.fragment                = "#version 450\n"
                                          "layout(location = 0) out vec4 out_color;\n"
                                          "layout(set = 0, binding = 0) uniform samplerCube sky;\n"
                                          "void main() { out_color = texture(sky, vec3(0.0, 0.0, 1.0)); }\n";
    EXPECT_EQ(screenLayer(cube_screen), nullptr);

    // The text declares `layout(binding = 0) uniform sampler2D picture;` - so the SET IS THE TEXT'S: a pass
    // whose source offers no attachment still binds binding 0 (the picture could be the map, or something a
    // text of its own declares), and `layoutFor(0, 0)` is that set plus the push range.
    const auto declared_zero = layer->sampledSetLayout(0, 0U);
    ASSERT_NE(declared_zero, nullptr) << "a text that declares a sampler has a set even with no source texture";
    ASSERT_EQ(declared_zero->bindings.size(), 1U);
    EXPECT_EQ(declared_zero->bindings[0].binding, 0U);
    EXPECT_NE(layer->layoutFor(0, 0U), nullptr) << "the layout a pass with no source textures binds";
    ASSERT_EQ(layer->layoutFor(0, 0U)->setLayouts.size(), 1U) << "the text's own set, and nothing else";

    const auto samplers = layer->sampledSetLayout(1, 0U);
    ASSERT_NE(samplers, nullptr);
    ASSERT_EQ(samplers->bindings.size(), 1U);
    EXPECT_EQ(samplers->bindings[0].binding, 0U) << "binding i is attachment i, from 0";
    EXPECT_EQ(samplers->bindings[0].descriptorType, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER);
    EXPECT_EQ(layer->sampledSetLayout(1, 0U), samplers) << "built once and kept, like the content layer's";

    const auto layout_one = layer->layoutFor(1, 0U);
    ASSERT_NE(layout_one, nullptr);
    ASSERT_EQ(layout_one->setLayouts.size(), 1U) << "the samplers are the only set - no block set, no set 1";
    EXPECT_EQ(layout_one->setLayouts[0], samplers);

    // The push budget is the full-screen ABI's 128 bytes, and the FRAGMENT stage reads them: the engine's
    // canonical vertex stage declares no constants, while a screen program's light block is the shader's.
    ASSERT_EQ(layout_one->pushConstantRanges.size(), 1U);
    EXPECT_EQ(layout_one->pushConstantRanges.front().stageFlags, VK_SHADER_STAGE_FRAGMENT_BIT);
    EXPECT_EQ(layout_one->pushConstantRanges.front().size, 128U);

    VariantPool pool;
    const auto  screen = layer->acquire(pool, screenKey(1U));
    ASSERT_NE(screen.pipeline, nullptr) << "a key of this layer's kind is compiled";
    EXPECT_EQ(screen.pipeline->layout, layout_one);
    EXPECT_NE(screen.pipeline->layout, layer->layout()) << "and never against the content layout";

    // The key carries the kind, so the same identity of the OTHER kind is refused rather than compiled: the
    // layer cannot build a pipeline for a descriptor ABI its draws do not bind.
    const std::uint64_t failures_before = layer->failures();
    PipelineKey         content_kind    = screenKey(1U);
    content_kind.kind                   = vine::vsg::core::DrawKind::Content;
    const auto refused                  = layer->acquire(pool, content_kind);
    EXPECT_EQ(refused.pipeline, nullptr);
    EXPECT_EQ(layer->failures(), failures_before + 1U) << "a silently compiled wrong-ABI pipeline is the failure";
}

TEST(ContentPipelineTest, TheEnginesShadowedLightingDeclaresItsOwnShadowSlots)
{
    // The engine's own screen lighting program (`BuiltinShaders::shadowedDeferredLightProgram`) writes its
    // shadow ABI's bindings down by hand: the map at 5 (the source's four colours take 0..3 and its depth
    // would take 4) and the block at 6. The layer's set is what the TEXT declares, so those numbers are the
    // text's - and the layer has to serve them.
    const vine::intrusive_ptr<vine::graphics::ShaderProgram> program =
        vine::graphics::shadowedDeferredLightProgram();
    ASSERT_NE(program, nullptr);
    ProgramFacts facts;
    ASSERT_EQ(buildScreenProgramFacts(*program, facts), FactMiss::None);
    auto layer = ContentPipeline::createScreen(facts.abi, facts.shaders);
    ASSERT_NE(layer, nullptr) << "the engine's shadowed lighting program must be servable";

    EXPECT_TRUE(layer->declaredSets().empty()) << "a full-screen set is the PASS' to build";
    const auto samplers = layer->samplerBindings(0U);
    ASSERT_EQ(samplers.size(), 5U) << "the source's four attachments and the map";
    EXPECT_EQ(samplers[0], 0U);
    EXPECT_EQ(samplers[1], 1U);
    EXPECT_EQ(samplers[2], 2U);
    EXPECT_EQ(samplers[3], 3U);
    EXPECT_EQ(samplers[4], 5U) << "the map's binding is the text's own number";
    const auto shape = layer->blockShape(0U);
    ASSERT_EQ(shape.size(), 1U);
    EXPECT_EQ(shape[0].binding, 6U);
    EXPECT_EQ(shape[0].role, vine::vsg::AbiBlockRole::ShadowBlock);

    // The set a pass binds when its source offers FOUR colours and NO sampleable depth: 0..3 for the colours,
    // the map at 5 and the block at 6 - binding 4 is NOT in it, which is exactly where an arrangement that
    // just appended the textures would have put the map (and the engine's text would then read undefined
    // data: it hard-codes 5).
    const auto set = layer->sampledSetLayout(4U, 0U);
    ASSERT_NE(set, nullptr);
    std::vector<std::uint32_t> bindings;
    for (const auto& entry : set->bindings) { bindings.push_back(entry.binding); }
    ASSERT_EQ(bindings.size(), 6U);
    EXPECT_EQ(bindings[0], 0U);
    EXPECT_EQ(bindings[3], 3U);
    EXPECT_EQ(bindings[4], 5U) << "the map takes its declared binding";
    EXPECT_EQ(bindings[5], 6U) << "and the block takes its own";
    EXPECT_EQ(std::find(bindings.begin(), bindings.end(), 4U), bindings.end())
        << "nothing lives at 4: the source offers no depth";
    for (const auto& entry : set->bindings) {
        if (entry.binding == 6U) {
            // DYNAMIC, and that is the point: the block's offset moves every frame (the arena writes one block
            // per call per frame), so a descriptor that baked it would make the pass rebuild its set every
            // frame - and a freshly written set that a pending command buffer still names is exactly what the
            // pending-state rule refuses (VUID-vkUpdateDescriptorSets-None-03047). The set names the buffer,
            // the bind carries the offset.
            EXPECT_EQ(entry.descriptorType, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC)
                << "a full-screen call's block travels as a dynamic offset, not as a baked descriptor offset";
        }
    }

    // ... and with a sampleable source depth the arrangement gains binding 4 while the map STAYS at 5.
    const auto with_depth = layer->sampledSetLayout(4U, 1U);
    ASSERT_NE(with_depth, nullptr);
    std::vector<std::uint32_t> with_depth_bindings;
    for (const auto& entry : with_depth->bindings) { with_depth_bindings.push_back(entry.binding); }
    ASSERT_EQ(with_depth_bindings.size(), 7U);
    EXPECT_EQ(with_depth_bindings[4], 4U) << "the source's own depth";
    EXPECT_EQ(with_depth_bindings[5], 5U) << "the map did not move";
    EXPECT_EQ(with_depth_bindings[6], 6U);

    // The full-screen ABI's rules, each refused where the layer is built: a declaration outside set 0 (there
    // is no second set to bind), and any block that is not the shadow's (a screen program's lights travel in
    // its push).
    const auto refusing = [](const std::string& declaration) {
        ContentPipeline::Shaders shaders;
        shaders.vertex   = "#version 450\n"
                           "layout(location = 0) out vec2 vine_uv;\n"
                           "void main() { vine_uv = vec2(0.0); gl_Position = vec4(0.0, 0.0, 0.0, 1.0); }\n";
        shaders.fragment = "#version 450\n"
                           "layout(location = 0) in vec2 vine_uv;\n"
                           "layout(location = 0) out vec4 out_color;\n" +
                           declaration + "\nvoid main() { out_color = vec4(vine_uv, 0.0, 1.0); }\n";
        vine::vsg::ProgramAbi abi;
        if (vine::vsg::scanProgramAbi(shaders.vertex, shaders.fragment, {}, abi) != FactMiss::None) {
            return std::unique_ptr<ContentPipeline>{};
        }
        return ContentPipeline::createScreen(abi, shaders);
    };
    EXPECT_EQ(refusing("layout(set = 1, binding = 0) uniform sampler2D picture;"), nullptr)
        << "the full-screen ABI is set 0 and nothing else";
    EXPECT_EQ(refusing("layout(binding = 7, std140) uniform VineLightsBlock { vec4 ambient; } lights;"), nullptr)
        << "a screen program's lights travel in its push block";
    EXPECT_NE(refusing("layout(binding = 5) uniform sampler2D shadow_map;"), nullptr)
        << "while the shadow's pair is exactly what a screen text may declare";
}

TEST(ContentPipelineTest, AScreenPipelineBakesTheLegacyFullscreenShape)
{
    // The create-info's states ARE the dynamic declaration's starting point, and for a full-screen draw they
    // are the previous implementation's overlay shape: no culling (the triangle's winding is the engine's own,
    // and cutting it out loses the whole picture) and no depth test (the draw composites on top). The plan's
    // resolved state is what a draw commands; these are the values nothing may silently inherit.
    auto layer = screenLayer(screenShaders());
    ASSERT_NE(layer, nullptr);

    VariantPool pool;
    const auto  screen = layer->acquire(pool, screenKey(1U));
    ASSERT_NE(screen.pipeline, nullptr);

    const auto* vertex_input = stateOf<::vsg::VertexInputState>(*screen.pipeline);
    ASSERT_NE(vertex_input, nullptr);
    EXPECT_TRUE(vertex_input->vertexBindingDescriptions.empty())
        << "the vertices are generated, so no stream is declared";
    EXPECT_TRUE(vertex_input->vertexAttributeDescriptions.empty());

    const auto* raster = stateOf<::vsg::RasterizationState>(*screen.pipeline);
    ASSERT_NE(raster, nullptr);
    EXPECT_EQ(raster->cullMode, VK_CULL_MODE_NONE);

    const auto* depth = stateOf<::vsg::DepthStencilState>(*screen.pipeline);
    ASSERT_NE(depth, nullptr);
    EXPECT_EQ(depth->depthTestEnable, VK_FALSE);
    EXPECT_EQ(depth->depthWriteEnable, VK_FALSE);
}

TEST(ContentPipelineTest, ASampledInputCountGetsItsOwnSetLayoutAndItsOwnPipeline)
{
    // How many colour textures a pass samples is identity, not runtime state: the pipeline is compiled against
    // one descriptor set layout, so the count has to reach the layout - and the layer keeps one layout per
    // count, so a second pass with another shape does not disturb the first.
    auto layer = makeLayer();
    ASSERT_NE(layer, nullptr);

    EXPECT_EQ(layer->sampledSetLayout(0, 0U), nullptr) << "a pass with no sampled inputs has no set 1 at all";
    EXPECT_EQ(layer->layoutFor(0, 0U), layer->layout()) << "the no-inputs layout is the one the layer was built with";

    const auto one = layer->sampledSetLayout(1, 0U);
    ASSERT_NE(one, nullptr);
    ASSERT_EQ(one->bindings.size(), 1U);
    EXPECT_EQ(one->bindings[0].binding, 0U);
    EXPECT_EQ(one->bindings[0].descriptorType, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER);
    EXPECT_EQ(one->bindings[0].descriptorCount, 1U);
    EXPECT_EQ(one->bindings[0].stageFlags, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT)
        << "which stage samples the picture is the shader's business";
    EXPECT_EQ(layer->sampledSetLayout(1, 0U), one) << "the layout is built once and kept";

    const auto two = layer->sampledSetLayout(2, 0U);
    ASSERT_NE(two, nullptr);
    ASSERT_EQ(two->bindings.size(), 2U);
    EXPECT_NE(two, one) << "two sampled textures are another shape, so another layout";

    const auto layout_one = layer->layoutFor(1, 0U);
    ASSERT_NE(layout_one, nullptr);
    ASSERT_EQ(layout_one->setLayouts.size(), 2U) << "the set before the samplers (empty here) and the sampled set";
    EXPECT_NE(layout_one->setLayouts[0], nullptr) << "a gap is a set with no bindings, not a missing one";
    EXPECT_TRUE(layout_one->setLayouts[0]->bindings.empty());
    EXPECT_EQ(layout_one->setLayouts[1], one);
    EXPECT_EQ(layer->layoutFor(1, 0U), layout_one);

    // The count is part of the identity: two counts are two pipelines, and each was compiled against its own
    // layout.
    VariantPool pool;
    PipelineKey plain       = contentKey(1);
    PipelineKey sampling    = contentKey(1);
    sampling.sampled_color_count = 1U;

    const auto without = layer->acquire(pool, plain);
    const auto with    = layer->acquire(pool, sampling);
    ASSERT_NE(without.pipeline, nullptr);
    ASSERT_NE(with.pipeline, nullptr);
    EXPECT_NE(without.pipeline, with.pipeline);
    EXPECT_EQ(pool.created(), 2U);
    EXPECT_EQ(without.pipeline->layout->setLayouts.size(), 0U)
        << "the text declares no blocks: a pass with no inputs binds nothing at all";
    EXPECT_EQ(with.pipeline->layout->setLayouts.size(), 2U)
        << "the (empty) set before the samplers and the sampled set itself";
    EXPECT_EQ(layer->acquire(pool, sampling).action, VariantPool::Action::Reused);
}

TEST(ContentPipelineTest, DynamicStateChurnNeverReachesThePoolOrThisLayer)
{
    auto layer = makeLayer();
    ASSERT_NE(layer, nullptr);

    VariantPool   pool;
    StateRegistry registry(pool);
    const PipelineKey key = contentKey(1);

    (void)registry.resolve(key, DynamicState{});
    for (int step = 0; step < 20; ++step) {
        DynamicState state;
        state.cull_mode     = (step % 2 == 0) ? vine::graphics::CullMode::Back : vine::graphics::CullMode::Front;
        state.polygon_mode  = (step % 3 == 0) ? vine::graphics::PolygonMode::Line : vine::graphics::PolygonMode::Fill;
        state.blend.enabled = (step % 2) == 0;
        (void)registry.resolve(key, state);
        EXPECT_EQ(layer->acquire(pool, key).action, VariantPool::Action::Reused);
    }

    EXPECT_EQ(pool.created(), 1U) << "the identity never moved";
    EXPECT_EQ(layer->compiles(), 1U) << "so exactly one pipeline object exists";
    EXPECT_EQ(registry.dynamic_issued(), 21U) << "every churn was a set command";
}

TEST(ContentPipelineTest, AnEvictedIdentityCostsThisLayerItsObject)
{
    auto layer = makeLayer();
    ASSERT_NE(layer, nullptr);

    VariantPool pool(1);  // one identity at a time: the second acquire evicts the first
    const auto  first  = layer->acquire(pool, contentKey(1));
    const auto  second = layer->acquire(pool, contentKey(2));

    EXPECT_FALSE(pool.contains(first.id)) << "the pool evicted the first identity";
    EXPECT_EQ(layer->pipelines(), 1U) << "and this layer dropped its object: nothing can look it up again";
    EXPECT_TRUE(layer->agreesWithPool(pool));
    EXPECT_TRUE(pool.contains(second.id));

    const auto again = layer->acquire(pool, contentKey(1));
    EXPECT_EQ(again.action, VariantPool::Action::Created) << "the pool has to compile it again";
    EXPECT_NE(again.id, first.id);
    EXPECT_EQ(layer->pipelines(), 1U) << "the second identity was evicted in its turn";
    EXPECT_TRUE(layer->agreesWithPool(pool));
}

TEST(ContentPipelineTest, AShaderPairThatDoesNotCompileYieldsNoLayer)
{
    ContentPipeline::Shaders broken = contentShaders();
    broken.fragment = "#version 450\nvoid main() { this is not glsl; }\n";

    auto layer = makeLayerWith(broken);
    EXPECT_EQ(layer, nullptr) << "a layer that can never build a pipeline must say so before anything is drawn";
}

TEST(ContentPipelineTest, TheLayoutFollowsTheDeclarationsWhereverTheyPutTheBlocks)
{
    // A program whose blocks are NOT where this backend's own programs put them: the material at binding 3 of
    // set 0 and the per-drawable block in set 1's binding 0 (the ENGINE's programs put the material at 0 and
    // the draw block in set 1 - a third arrangement again). All of them are served the same way: the layout is
    // read out of the text, and the shape a caller must build its set from is answered by the same facts.
    ContentPipeline::Shaders shaders;
    shaders.vertex = "#version 450\n"
                     "layout(location = 0) in vec3 position;\n"
                     "layout(set = 1, binding = 0, std140) uniform VineDrawBlock { mat4 model; vec4 params; } draw;\n"
                     "void main() { gl_Position = draw.model * vec4(position, 1.0); }\n";
    shaders.fragment = "#version 450\n"
                       "layout(location = 0) out vec4 outColor;\n"
                       "layout(set = 0, binding = 3, std140) uniform VineMaterialBlock\n"
                       "{\n"
                       "    vec4 ambient; vec4 diffuse; vec4 specular; float shininess;\n"
                       "} material;\n"
                       "void main() { outColor = material.diffuse; }\n";

    auto layer = makeLayerWith(shaders);
    ASSERT_NE(layer, nullptr);

    VariantPool pool;
    const auto  result = layer->acquire(pool, contentKey(1));
    ASSERT_NE(result.pipeline, nullptr);
    ASSERT_NE(result.pipeline->layout, nullptr);

    const ::vsg::DescriptorSetLayouts& sets = result.pipeline->layout->setLayouts;
    ASSERT_EQ(sets.size(), 2U) << "the text declares a block in each of two sets";
    ASSERT_EQ(sets[0]->bindings.size(), 1U);
    EXPECT_EQ(sets[0]->bindings[0].binding, 3U) << "the material's binding is the TEXT's, not this backend's";
    EXPECT_EQ(sets[0]->bindings[0].descriptorType, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC)
        << "the blocks arrive through dynamic offsets, whatever binding they sit at";
    ASSERT_EQ(sets[1]->bindings.size(), 1U);
    EXPECT_EQ(sets[1]->bindings[0].binding, 0U);

    ASSERT_EQ(layer->declaredSets().size(), 2U) << "the sets a caller has to build block sets for";
    EXPECT_EQ(layer->declaredSets()[0], 0U);
    EXPECT_EQ(layer->declaredSets()[1], 1U);
    const std::span<const vine::vsg::BlockDescriptors::Binding> shape = layer->blockShape(0U);
    ASSERT_EQ(shape.size(), 1U);
    EXPECT_EQ(shape[0].binding, 3U);
    EXPECT_EQ(shape[0].role, vine::vsg::AbiBlockRole::Material);
    EXPECT_TRUE(layer->blockShape(2U).empty()) << "a set the text says nothing about declares nothing";

    // A block in set 2 ALONE: the sets before it are part of the layout all the same - the API wants a
    // contiguous range from 0 - and the ones in between are REAL, EMPTY descriptor set layouts, because a
    // null entry in a pipeline layout is not "no set", it is an invalid handle.
    ContentPipeline::Shaders gapped;
    gapped.vertex   = "#version 450\n"
                      "layout(location = 0) in vec3 position;\n"
                      "void main() { gl_Position = vec4(position, 1.0); }\n";
    gapped.fragment = "#version 450\n"
                      "layout(location = 0) out vec4 outColor;\n"
                      "layout(set = 2, binding = 0, std140) uniform VineLightsBlock { vec4 light0; } lights;\n"
                      "void main() { outColor = lights.light0; }\n";
    auto gapped_layer = makeLayerWith(gapped);
    ASSERT_NE(gapped_layer, nullptr);
    ASSERT_EQ(gapped_layer->declaredSets().size(), 1U);
    EXPECT_EQ(gapped_layer->declaredSets()[0], 2U);
    const auto gapped_result = gapped_layer->acquire(pool, contentKey(1));
    ASSERT_NE(gapped_result.pipeline, nullptr);
    ASSERT_NE(gapped_result.pipeline->layout, nullptr);
    const ::vsg::DescriptorSetLayouts& gapped_sets = gapped_result.pipeline->layout->setLayouts;
    ASSERT_EQ(gapped_sets.size(), 3U);
    EXPECT_EQ(gapped_sets[0]->bindings.size(), 0U) << "a set the text does not mention is still a set: empty";
    EXPECT_EQ(gapped_sets[1]->bindings.size(), 0U);
    EXPECT_EQ(gapped_sets[2]->bindings.size(), 1U);
}

TEST(ContentPipelineTest, ADeclarationThisBackendCannotFillIsRefused)
{
    const auto layerDeclaring = [](const std::string& declaration) {
        ContentPipeline::Shaders shaders;
        shaders.vertex   = "#version 450\n"
                           "layout(location = 0) in vec3 position;\n"
                           "void main() { gl_Position = vec4(position, 1.0); }\n";
        shaders.fragment = "#version 450\n"
                           "layout(location = 0) out vec4 outColor;\n" +
                           declaration + "\nvoid main() { outColor = vec4(1.0); }\n";
        return makeLayerWith(shaders);
    };

    // A uniform block that is none of the L1 names: nothing can fill it, so no pipeline is built.
    EXPECT_EQ(layerDeclaring("layout(set = 0, binding = 0, std140) uniform MyOwnBlock { vec4 value; } own;"), nullptr);
    // A material block that declares MORE bytes than the L1 struct: the bytes past it are not the ABI's.
    EXPECT_EQ(layerDeclaring("layout(set = 0, binding = 0, std140) uniform VineMaterialBlock\n"
                             "{ vec4 ambient; vec4 diffuse; vec4 specular; float shininess; vec4 extra; } material;"),
              nullptr);
    // A block whose layout is not std140: its size is the compiler's, and the bytes bound are std140's.
    EXPECT_EQ(layerDeclaring("layout(set = 0, binding = 0) uniform VineMaterialBlock\n"
                             "{ vec4 ambient; vec4 diffuse; vec4 specular; float shininess; } material;"),
              nullptr);
    // A sampler in the block set is FINE now: the engine's own set 0 carries the material block AND the
    // diffuse map, so the two kinds share a set (the caller supplies the image - see api/BlockDescriptors).
    auto map_in_a_block_set = layerDeclaring("layout(set = 0, binding = 1) uniform sampler2D diffuseMap;");
    ASSERT_NE(map_in_a_block_set, nullptr);
    EXPECT_EQ(map_in_a_block_set->samplerBindings(0U).size(), 1U);
    EXPECT_EQ(map_in_a_block_set->samplerBindings(0U)[0], 1U);
    EXPECT_TRUE(map_in_a_block_set->samplerBindings(1U).empty());

    // A sampler kind this backend has no view for (neither 2D nor cube).
    EXPECT_EQ(layerDeclaring("layout(set = 1, binding = 0) uniform sampler3D volume_tex;"), nullptr);
    // A CUBE declaration is SERVED: the engine's sky program samples the sky box's own cube map out of its
    // material, and the by-name policy hands the drawable's own texture to `skyMap` exactly as it does to
    // `diffuseMap` (api/ContentImages) - the cube view itself comes from api/MaterialImages.
    auto cube_map = layerDeclaring("layout(set = 0, binding = 1) uniform samplerCube skyMap;");
    ASSERT_NE(cube_map, nullptr);
    EXPECT_EQ(cube_map->samplerBindings(0U).size(), 1U);
    EXPECT_EQ(cube_map->samplerBindings(0U)[0], 1U);
    // ... and the 2D declaration of the same name is the pair branch the sky program also ships.
    auto flat_sky = layerDeclaring("layout(set = 0, binding = 1) uniform sampler2D skyMap;");
    ASSERT_NE(flat_sky, nullptr);

    // A block that reads a PREFIX of the L1 struct is fine: the range it is bound with covers what it reads.
    auto prefix = layerDeclaring("layout(set = 0, binding = 0, std140) uniform VineLightsBlock { vec4 light0; } lights;");
    ASSERT_NE(prefix, nullptr);
    const std::span<const vine::vsg::BlockDescriptors::Binding> shape = prefix->blockShape(0U);
    ASSERT_EQ(shape.size(), 1U);
    EXPECT_EQ(shape[0].role, vine::vsg::AbiBlockRole::Lights);

    // A declared input sampler is a fact of the program, but the KEY has to cover it: a pass that samples
    // nothing cannot bind an image at binding 0, so the pipeline is refused for that key and only that key.
    auto sampling = layerDeclaring("layout(set = 1, binding = 0) uniform sampler2D picture;");
    ASSERT_NE(sampling, nullptr);
    VariantPool      pool;
    const auto       no_inputs = sampling->acquire(pool, contentKey(1));
    EXPECT_EQ(no_inputs.pipeline, nullptr) << "a sampler the pass' input count cannot reach has no image to bind";
    PipelineKey      with_input = contentKey(1);
    with_input.sampled_color_count = 1U;
    EXPECT_NE(sampling->acquire(pool, with_input).pipeline, nullptr);
}

TEST(ContentPipelineTest, APushNobodyCouldFillIsRefusedWhereTheLayoutIsBuilt)
{
    // A push range is the one range whose bytes are ASSEMBLED rather than copied from an L1 struct
    // (api/ContentPush), so the layer that would compile against it also has to know that every member is one
    // the pass can fill: a program reading `pc.tint` would shade with zeros, and a `vec4 projection` would be
    // a projection written into bytes that are not its own. Both are refused where the layout is built, not
    // discovered when the picture is black.
    const auto layerPushing = [](const std::string& members) {
        ContentPipeline::Shaders shaders;
        shaders.vertex   = "#version 450\n"
                           "layout(location = 0) in vec3 position;\n"
                           "layout(push_constant) uniform PushConstants { " + members +
                           " } pc;\n"
                           "void main() { gl_Position = vec4(position, 1.0); }\n";
        shaders.fragment = "#version 450\n"
                           "layout(location = 0) out vec4 outColor;\n"
                           "void main() { outColor = vec4(1.0); }\n";
        return makeLayerWith(shaders);
    };

    // The engine's own pair: the two camera matrices, each a mat4 - fillable, and the layer keeps the range.
    auto engine_shaped = layerPushing("mat4 projection; mat4 modelView;");
    ASSERT_NE(engine_shaped, nullptr);
    VariantPool pool;
    const auto  compiled = engine_shaped->acquire(pool, contentKey(1));
    ASSERT_NE(compiled.pipeline, nullptr);
    ASSERT_NE(compiled.pipeline->layout, nullptr);
    ASSERT_EQ(compiled.pipeline->layout->pushConstantRanges.size(), 1U);
    EXPECT_EQ(compiled.pipeline->layout->pushConstantRanges.front().size, 128U);
    EXPECT_EQ(compiled.pipeline->layout->pushConstantRanges.front().stageFlags, VK_SHADER_STAGE_VERTEX_BIT);

    // A member nobody names: no pass can fill it, so there is no pipeline to draw with.
    EXPECT_EQ(layerPushing("mat4 projection; vec4 tint;"), nullptr);
    // A known name of the wrong size: `vec4 projection` is not a projection matrix.
    EXPECT_EQ(layerPushing("vec4 projection; mat4 modelView;"), nullptr);
    // A range that declares only PART of the pair is fine: the pass fills what is declared, and the range's
    // size is what the text declares (the same rule a block that reads a prefix of its L1 struct follows).
    auto projection_only = layerPushing("mat4 projection;");
    ASSERT_NE(projection_only, nullptr);
    const auto projection_result = projection_only->acquire(pool, contentKey(1));
    ASSERT_NE(projection_result.pipeline, nullptr);
    ASSERT_EQ(projection_result.pipeline->layout->pushConstantRanges.size(), 1U);
    EXPECT_EQ(projection_result.pipeline->layout->pushConstantRanges.front().size, 64U);
}
