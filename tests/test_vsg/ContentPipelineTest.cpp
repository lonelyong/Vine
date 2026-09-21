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

#include <vine/vsg/api/ContentPipeline.hpp>
#include <vine/vsg/core/StateRegistry.hpp>
#include <vine/vsg/core/VariantPool.hpp>

using vine::vsg::ContentPipeline;
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

/// @brief The layer under test: the one-stream shader pair over @p block_set.
std::unique_ptr<ContentPipeline> makeLayer(const ::vsg::ref_ptr<::vsg::DescriptorSetLayout>& block_set)
{
    // The declared layout is copied into the pipeline's own state objects, so these locals may die here.
    const ContentPipeline::VertexBinding   binding   = positionBinding();
    const ContentPipeline::VertexAttribute attribute = positionAttribute();
    return ContentPipeline::create(block_set, std::span<const ContentPipeline::VertexBinding>(&binding, 1),
                                   std::span<const ContentPipeline::VertexAttribute>(&attribute, 1),
                                   contentShaders());
}

/// @brief The layer under test with caller-chosen shader text.
std::unique_ptr<ContentPipeline> makeLayerWith(const ::vsg::ref_ptr<::vsg::DescriptorSetLayout>& block_set,
                                               const ContentPipeline::Shaders&                shaders)
{
    const ContentPipeline::VertexBinding   binding   = positionBinding();
    const ContentPipeline::VertexAttribute attribute = positionAttribute();
    return ContentPipeline::create(block_set, std::span<const ContentPipeline::VertexBinding>(&binding, 1),
                                   std::span<const ContentPipeline::VertexAttribute>(&attribute, 1), shaders);
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

}  // namespace

TEST(ContentPipelineTest, TheSameIdentityIsBuiltOnce)
{
    const auto block_set = ::vsg::DescriptorSetLayout::create();
    auto       layer     = makeLayer(block_set);
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
    const auto block_set = ::vsg::DescriptorSetLayout::create();
    auto       layer     = makeLayer(block_set);
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

TEST(ContentPipelineTest, TheEditableHalfIsDeclaredDynamic)
{
    const auto block_set = ::vsg::DescriptorSetLayout::create();
    auto       layer     = makeLayer(block_set);
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

TEST(ContentPipelineTest, TheLayoutBindsTheBlockSetAndThePushBudget)
{
    const auto block_set = ::vsg::DescriptorSetLayout::create();
    auto       layer     = makeLayer(block_set);
    ASSERT_NE(layer, nullptr);

    VariantPool pool;
    const auto  result = layer->acquire(pool, contentKey(1));
    ASSERT_NE(result.pipeline, nullptr);
    ASSERT_NE(result.pipeline->layout, nullptr);

    ASSERT_EQ(result.pipeline->layout->setLayouts.size(), 1U) << "the blocks are the only set a draw needs";
    EXPECT_EQ(result.pipeline->layout->setLayouts[0], block_set);
    ASSERT_EQ(result.pipeline->layout->pushConstantRanges.size(), 1U);
    const VkPushConstantRange& push = result.pipeline->layout->pushConstantRanges.front();
    EXPECT_EQ(push.stageFlags, VK_SHADER_STAGE_VERTEX_BIT);
    EXPECT_EQ(push.offset, 0U);
    EXPECT_EQ(push.size, 128U) << "the ABI's push budget (see ShaderAbi.hpp)";
    ASSERT_EQ(layer->stages().size(), 2U);
    EXPECT_NE(layer->stages()[0]->module, nullptr) << "both stages are SPIR-V modules";
    EXPECT_NE(layer->stages()[1]->module, nullptr);
}

TEST(ContentPipelineTest, DynamicStateChurnNeverReachesThePoolOrThisLayer)
{
    const auto block_set = ::vsg::DescriptorSetLayout::create();
    auto       layer     = makeLayer(block_set);
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
    const auto block_set = ::vsg::DescriptorSetLayout::create();
    auto       layer     = makeLayer(block_set);
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

    const auto block_set = ::vsg::DescriptorSetLayout::create();
    auto       layer     = makeLayerWith(block_set, broken);
    EXPECT_EQ(layer, nullptr) << "a layer that can never build a pipeline must say so before anything is drawn";
}
