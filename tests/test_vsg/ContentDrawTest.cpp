/**
 * @brief Draw recording: a resolved draw becomes the command graph that executes it.
 *
 * What these cases pin, and why each of them is a failure the old picture would not have shown:
 *
 *   * the ORDER of the recorded commands (pipeline bind, dynamic block, block descriptors, then the
 *     rectangle, the geometry and the draw) - a driver fault or a silent "drew with whatever was bound last"
 *     is what a wrong order produces;
 *   * the two "only when" clauses: a second draw of the same variant and state records NO pipeline bind and
 *     NO dynamic block, so a pass' command buffer stays proportional to its content rather than its draw
 *     count;
 *   * the DYNAMIC block is a `vsg::StateCommand` in its own slot - the slot is not a priority but a state
 *     stack's identity, and taking the default slot 0 replaces the group's pipeline bind (the old backend
 *     learned that from a driver crash);
 *   * the mapped values follow the ENGINE's conventions: reverse-Z (`GREATER`), clockwise front faces
 *     (vsg's projection inverts Y), always-on blending, and the depth policy's test/write pair.
 *
 * No device is created: a `vsg::GraphicsPipeline`, a descriptor bind and the command graph are all
 * create-info until a `vsg::Context` compiles them, which is where the pixel phase will pick them up.
 */

#include <gtest/gtest.h>

#include <cstdint>

#include <vsg/commands/BindIndexBuffer.h>
#include <vsg/commands/BindVertexBuffers.h>
#include <vsg/commands/Draw.h>
#include <vsg/commands/DrawIndexed.h>
#include <vsg/commands/SetScissor.h>
#include <vsg/commands/SetViewport.h>
#include <vsg/core/Array.h>
#include <vsg/commands/Commands.h>
#include <vsg/nodes/StateGroup.h>
#include <vsg/state/BindDescriptorSet.h>
#include <vsg/state/GraphicsPipeline.h>
#include <vsg/state/Buffer.h>
#include <vsg/state/BufferInfo.h>
#include <vsg/state/DescriptorBuffer.h>
#include <vsg/state/DescriptorImage.h>
#include <vsg/state/DescriptorSet.h>
#include <vsg/state/PushConstants.h>
#include <vsg/state/DescriptorSetLayout.h>
#include <vsg/state/PipelineLayout.h>

#include <vine/vsg/VsgDynamicState.hpp>
#include <vine/vsg/api/ContentDraw.hpp>
#include <vine/vsg/api/ContentPipeline.hpp>
#include <vine/vsg/api/StateCommands.hpp>
#include <vine/vsg/core/StateRegistry.hpp>
#include <vine/vsg/core/VariantPool.hpp>

using vine::vsg::ContentDraw;
using vine::vsg::ContentPipeline;
using vine::vsg::ViewportRect;
using vine::vsg::core::DynamicState;
using vine::vsg::core::PipelineKey;
using vine::vsg::core::StateRegistry;
using vine::vsg::core::VariantPool;

namespace
{

/// @brief Everything one recorded draw needs, built without a device.
class Fixture
{
  public:
    Fixture()
    {
        set_layout = ::vsg::DescriptorSetLayout::create();
        set_layout->addBinding(0U, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC, 1U,
                               VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT);
        pipeline_layout = ::vsg::PipelineLayout::create(::vsg::DescriptorSetLayouts{ set_layout },
                                                        ::vsg::PushConstantRanges{});

        ContentPipeline::Shaders shaders;
        shaders.vertex = "#version 450\n"
                         "layout(location = 0) in vec3 position;\n"
                         "void main() { gl_Position = vec4(position, 1.0); }\n";
        shaders.fragment = "#version 450\n"
                           "layout(location = 0) out vec4 outColor;\n"
                           "void main() { outColor = vec4(1.0, 0.0, 0.0, 1.0); }\n";
        // The text declares no bindings, so the layer builds a layout with no sets - the recorder's own
        // command shapes are what these cases pin (nothing here is executed on a device).
        vine::vsg::ProgramAbi abi;
        EXPECT_EQ(vine::vsg::scanProgramAbi(shaders.vertex, shaders.fragment, {}, abi), vine::vsg::FactMiss::None);
        const ContentPipeline::VertexBinding   binding{ 0U, sizeof(float) * 3U, false };
        const ContentPipeline::VertexAttribute attribute{ 0U, 0U, VK_FORMAT_R32G32B32_SFLOAT, 0U };
        pipelines = ContentPipeline::create(abi, std::span<const ContentPipeline::VertexBinding>(&binding, 1),
                                            std::span<const ContentPipeline::VertexAttribute>(&attribute, 1), shaders);

        recorder = std::make_unique<ContentDraw>(*pipelines, pool);
    }

    /** @brief A block descriptor bind over the same set layout (what BlockDescriptors hands a real draw). */
    ::vsg::ref_ptr<::vsg::BindDescriptorSet> blocks() const
    {
        auto buffer = ::vsg::Buffer::create(256U, VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT, VK_SHARING_MODE_EXCLUSIVE);
        auto info   = ::vsg::BufferInfo::create(buffer, 0U, 64U);
        auto descriptor = ::vsg::DescriptorBuffer::create(::vsg::BufferInfoList{ info }, 0U, 0U,
                                                          VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC);
        auto set = ::vsg::DescriptorSet::create(set_layout, ::vsg::Descriptors{ descriptor });
        auto bind = ::vsg::BindDescriptorSet::create(VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline_layout, 0U, set);
        bind->dynamicOffsets = { 0U, 0U, 0U };
        return bind;
    }

    /** @brief A draw of one triangle, at a known identity and state. */
    ContentDraw::Draw draw(std::uint64_t revision = 1U) const
    {
        static int program = 0;
        ContentDraw::Draw result;
        result.key.program                      = &program;
        result.key.revision                     = revision;
        result.key.vertex_layout.canonical_mask = 0x1U;
        result.key.compatibility.samples        = 1U;
        block_binds[0]                          = blocks();
        result.blocks                           = block_binds;
        result.vertex_binds                     = std::span<const ::vsg::ref_ptr<::vsg::BindVertexBuffers>>(&vertex_bind, 1);
        result.index                            = ::vsg::BindIndexBuffer::create(::vsg::uintArray::create(3U));
        result.viewport                         = ViewportRect{ 0.0F, 0.0F, 320.0F, 240.0F };
        result.index_count                      = 3U;
        return result;
    }

    ::vsg::ref_ptr<::vsg::DescriptorSetLayout> set_layout;
    ::vsg::ref_ptr<::vsg::PipelineLayout>      pipeline_layout;
    /// The one block bind the fixture's draws carry (a draw binds a span, one entry per declared set).
    mutable ::vsg::ref_ptr<::vsg::BindDescriptorSet> block_binds[1];
    std::unique_ptr<ContentPipeline>           pipelines;
    VariantPool                                pool;
    std::unique_ptr<ContentDraw>               recorder;
    ::vsg::ref_ptr<::vsg::BindVertexBuffers>   vertex_bind{
        ::vsg::BindVertexBuffers::create(0U, ::vsg::DataList{ ::vsg::vec3Array::create(3U) })
    };
};

}  // namespace

TEST(ContentDrawTest, TheFirstDrawRecordsThePipelineTheStateAndTheGeometry)
{
    Fixture             fixture;
    StateRegistry       registry(fixture.pool);
    const ContentDraw::Draw draw  = fixture.draw();
    const auto          group = fixture.recorder->record(registry, draw);
    ASSERT_NE(group, nullptr);

    ASSERT_EQ(group->stateCommands.size(), 3U) << "pipeline, dynamic block, block descriptors";
    const auto* pipeline_bind = dynamic_cast<const ::vsg::BindGraphicsPipeline*>(group->stateCommands[0].get());
    ASSERT_NE(pipeline_bind, nullptr);
    EXPECT_NE(pipeline_bind->pipeline, nullptr);
    const auto* dynamic = dynamic_cast<const vine::vsg::detail::SetDynamicState*>(group->stateCommands[1].get());
    ASSERT_NE(dynamic, nullptr);
    EXPECT_EQ(dynamic->slot, vine::vsg::detail::kDynamicStateSlot)
        << "the dynamic command must not take the pipeline bind's slot";
    EXPECT_EQ(group->stateCommands[2], draw.blocks[0]) << "the third command is this draw's block bind";

    ASSERT_EQ(group->children.size(), 1U);
    const auto* commands = dynamic_cast<const ::vsg::Commands*>(group->children[0].get());
    ASSERT_NE(commands, nullptr);
    ASSERT_EQ(commands->children.size(), 5U) << "viewport, scissor, vertex bind, index bind, draw";
    EXPECT_NE(dynamic_cast<const ::vsg::SetViewport*>(commands->children[0].get()), nullptr);
    EXPECT_NE(dynamic_cast<const ::vsg::SetScissor*>(commands->children[1].get()), nullptr);
    EXPECT_EQ(commands->children[2].get(), fixture.vertex_bind.get());
    EXPECT_NE(dynamic_cast<const ::vsg::BindIndexBuffer*>(commands->children[3].get()), nullptr);
    const auto* indexed = dynamic_cast<const ::vsg::DrawIndexed*>(commands->children[4].get());
    ASSERT_NE(indexed, nullptr);
    EXPECT_EQ(indexed->indexCount, 3U);
    EXPECT_EQ(indexed->instanceCount, 1U);
    EXPECT_EQ(indexed->firstIndex, 0U);
    EXPECT_EQ(indexed->vertexOffset, 0);

    EXPECT_EQ(fixture.recorder->draws(), 1U);
    EXPECT_EQ(fixture.recorder->pipeline_binds(), 1U);
    EXPECT_EQ(fixture.recorder->dynamic_commands(), 1U);
}

TEST(ContentDrawTest, ANonIndexedDrawRecordsTheVertexStreamsItIsAssembledFrom)
{
    Fixture      fixture;
    StateRegistry registry(fixture.pool);

    // A point cloud: no index stream exists in the model at all, so the draw names the vertices instead - and
    // the two modes are different API calls, not one call with a zero.
    ContentDraw::Draw draw = fixture.draw();
    draw.index             = nullptr;
    draw.index_count       = 0U;
    draw.vertex_count      = 12U;
    draw.key.topology      = vine::graphics::Topology::Points;

    const auto group = fixture.recorder->record(registry, draw);
    ASSERT_NE(group, nullptr);

    ASSERT_EQ(group->children.size(), 1U);
    const auto* commands = dynamic_cast<const ::vsg::Commands*>(group->children[0].get());
    ASSERT_NE(commands, nullptr);
    ASSERT_EQ(commands->children.size(), 4U) << "viewport, scissor, vertex bind, draw - and NO index bind";
    EXPECT_NE(dynamic_cast<const ::vsg::SetViewport*>(commands->children[0].get()), nullptr);
    EXPECT_NE(dynamic_cast<const ::vsg::SetScissor*>(commands->children[1].get()), nullptr);
    EXPECT_EQ(commands->children[2].get(), fixture.vertex_bind.get());
    const auto* plain = dynamic_cast<const ::vsg::Draw*>(commands->children[3].get());
    ASSERT_NE(plain, nullptr) << "an unindexed draw is vkCmdDraw, never a DrawIndexed with zero indices";
    EXPECT_EQ(plain->vertexCount, 12U);
    EXPECT_EQ(plain->instanceCount, 1U);
    EXPECT_EQ(plain->firstVertex, 0U);
    EXPECT_EQ(plain->firstInstance, 0U);

    EXPECT_EQ(fixture.recorder->draws(), 1U);
    EXPECT_EQ(fixture.recorder->refusals(), 0U);
}

TEST(ContentDrawTest, ADrawThatNamesNoGeometryIsRefusedBeforeAnythingIsMemoised)
{
    Fixture      fixture;
    StateRegistry registry(fixture.pool);

    ContentDraw::Draw empty = fixture.draw();
    empty.index             = nullptr;
    empty.index_count       = 0U;
    empty.vertex_count      = 0U;

    EXPECT_EQ(fixture.recorder->record(registry, empty), nullptr);
    EXPECT_EQ(fixture.recorder->refusals(), 1U);
    EXPECT_EQ(fixture.recorder->draws(), 0U);
    EXPECT_EQ(fixture.pool.created(), 0U) << "a refused draw compiles nothing";

    // ...and the registry was not told the variant is bound: the next real draw still binds its pipeline, or it
    // would run with whatever the last pass left bound (the failure this recorder's memoisation can cause).
    const auto group = fixture.recorder->record(registry, fixture.draw());
    ASSERT_NE(group, nullptr);
    ASSERT_GE(group->stateCommands.size(), 1U);
    EXPECT_NE(dynamic_cast<const ::vsg::BindGraphicsPipeline*>(group->stateCommands[0].get()), nullptr);
    EXPECT_EQ(fixture.recorder->pipeline_binds(), 1U);
}

TEST(ContentDrawTest, ASecondDrawOfTheSameVariantAndStateRecordsOnlyTheGeometry)
{
    Fixture      fixture;
    StateRegistry registry(fixture.pool);

    const auto first  = fixture.recorder->record(registry, fixture.draw());
    const auto second = fixture.recorder->record(registry, fixture.draw());
    ASSERT_NE(first, nullptr);
    ASSERT_NE(second, nullptr);

    EXPECT_EQ(first->stateCommands.size(), 3U);
    EXPECT_EQ(second->stateCommands.size(), 1U)
        << "the pipeline is already bound and the dynamic block already issued: only the blocks bind again";
    EXPECT_EQ(fixture.recorder->pipeline_binds(), 1U) << "one bind per SWITCH, not per draw";
    EXPECT_EQ(fixture.recorder->dynamic_commands(), 1U);
    ASSERT_EQ(second->children.size(), 1U) << "the geometry is still recorded per draw";
    EXPECT_EQ(fixture.recorder->draws(), 2U);
}

TEST(ContentDrawTest, AStateChangeRecordsOnlyTheDynamicBlock)
{
    Fixture      fixture;
    StateRegistry registry(fixture.pool);

    (void)fixture.recorder->record(registry, fixture.draw());

    ContentDraw::Draw changed = fixture.draw();
    changed.dynamic.cull_mode = vine::graphics::CullMode::Front;
    changed.dynamic.depth     = vine::graphics::DepthMode::TestOnly;

    const auto group = fixture.recorder->record(registry, changed);
    ASSERT_NE(group, nullptr);
    ASSERT_EQ(group->stateCommands.size(), 2U) << "the dynamic block and the blocks bind; no pipeline rebind";

    const auto* dynamic = dynamic_cast<const vine::vsg::detail::SetDynamicState*>(group->stateCommands[0].get());
    ASSERT_NE(dynamic, nullptr);
    EXPECT_EQ(dynamic->cull_mode, VK_CULL_MODE_FRONT_BIT);
    EXPECT_EQ(dynamic->depth_test_enable, VK_TRUE);
    EXPECT_EQ(dynamic->depth_write_enable, VK_FALSE);
    EXPECT_EQ(fixture.recorder->pipeline_binds(), 1U);
    EXPECT_EQ(fixture.recorder->dynamic_commands(), 2U);
}

TEST(ContentDrawTest, AnIdentityChangeRebindsThePipeline)
{
    Fixture      fixture;
    StateRegistry registry(fixture.pool);

    (void)fixture.recorder->record(registry, fixture.draw(1U));
    const auto changed = fixture.recorder->record(registry, fixture.draw(2U));  // the program's revision moved
    ASSERT_NE(changed, nullptr);

    ASSERT_EQ(changed->stateCommands.size(), 2U)
        << "the pipeline is rebound, but the dynamic values did not change: they live in the recording";
    EXPECT_NE(dynamic_cast<const ::vsg::BindGraphicsPipeline*>(changed->stateCommands[0].get()), nullptr);
    EXPECT_EQ(fixture.recorder->pipeline_binds(), 2U) << "a new identity is a new bind";
    EXPECT_EQ(fixture.recorder->dynamic_commands(), 1U) << "and no dynamic block: the values are already issued";
    EXPECT_EQ(fixture.pool.created(), 2U);
}

TEST(ContentDrawTest, TheDynamicMappingFollowsTheEngineConventions)
{
    const vine::vsg::detail::DynamicStateEntryPoints none;

    DynamicState disabled;
    disabled.depth = vine::graphics::DepthMode::Disabled;
    const auto no_depth = vine::vsg::makeDynamicStateCommand(disabled, 1U, none);
    EXPECT_EQ(no_depth->depth_test_enable, VK_FALSE);
    EXPECT_EQ(no_depth->depth_write_enable, VK_FALSE);

    DynamicState translucent;
    translucent.depth = vine::graphics::DepthMode::TestOnly;
    const auto test_only = vine::vsg::makeDynamicStateCommand(translucent, 1U, none);
    EXPECT_EQ(test_only->depth_test_enable, VK_TRUE);
    EXPECT_EQ(test_only->depth_write_enable, VK_FALSE);

    // The two conventions that are silent when wrong: reverse-Z and the inverted Y of vsg's projection.
    EXPECT_EQ(test_only->compare_op, VK_COMPARE_OP_GREATER) << "reverse-Z: closer is GREATER";
    EXPECT_EQ(test_only->front_face, VK_FRONT_FACE_CLOCKWISE) << "vsg's projection inverts Y";

    DynamicState churn;
    churn.cull_mode    = vine::graphics::CullMode::Back;
    churn.polygon_mode = vine::graphics::PolygonMode::Line;
    churn.topology     = vine::graphics::Topology::Points;
    const auto mapped  = vine::vsg::makeDynamicStateCommand(churn, 1U, none);
    EXPECT_EQ(mapped->cull_mode, VK_CULL_MODE_BACK_BIT);
    EXPECT_EQ(mapped->polygon_mode, VK_POLYGON_MODE_LINE);
    EXPECT_EQ(mapped->topology, VK_PRIMITIVE_TOPOLOGY_POINT_LIST);
    EXPECT_EQ(mapped->color_attachment_count, 1U);
    // Blending is always on for ONE attachment - the per-vertex opacity path, where an alpha may drop
    // below 1 without a rebuild - and always off for several, where the attachments are DATA (a
    // G-buffer's normal rides with the material's shininess in its alpha). Several attachments are
    // pinned device-free in DynamicStateTest.SeveralColourAttachmentsAreDeliveredUnblended; the rule
    // itself is the L2's own (applyOpaqueBlendForAttachments, VsgSceneRules.hpp).
    EXPECT_EQ(mapped->blend[0].blendEnable, VK_TRUE) << "one attachment: the engine's opacity path";
    EXPECT_EQ(mapped->blend[0].srcColorBlendFactor, VK_BLEND_FACTOR_SRC_ALPHA)
        << "a state that does not opt in gets the standard pair";
    EXPECT_EQ(mapped->blend[0].dstColorBlendFactor, VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA);
    EXPECT_EQ(mapped->blend[0].srcAlphaBlendFactor, VK_BLEND_FACTOR_SRC_ALPHA);
    EXPECT_EQ(mapped->blend[0].dstAlphaBlendFactor, VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA);

    DynamicState opted_in;
    opted_in.blend.enabled = true;
    opted_in.blend.src     = vine::graphics::BlendFactor::One;
    opted_in.blend.dst     = vine::graphics::BlendFactor::Zero;
    const auto factors     = vine::vsg::makeDynamicStateCommand(opted_in, 1U, none);
    EXPECT_EQ(factors->blend[0].srcColorBlendFactor, VK_BLEND_FACTOR_ONE);
    EXPECT_EQ(factors->blend[0].dstColorBlendFactor, VK_BLEND_FACTOR_ZERO);
    EXPECT_EQ(factors->blend[0].srcAlphaBlendFactor, VK_BLEND_FACTOR_ONE) << "alpha follows the colour pair";
    EXPECT_EQ(factors->blend[0].dstAlphaBlendFactor, VK_BLEND_FACTOR_ZERO);
}

TEST(ContentDrawTest, TheRectangleCommandsCarryTheRectAndClampWhatTheApiForbids)
{
    const auto viewport = vine::vsg::makeViewportCommand(ViewportRect{ 10.0F, 20.0F, 320.0F, 240.0F });
    ASSERT_EQ(viewport->viewports.size(), 1U);
    EXPECT_FLOAT_EQ(viewport->viewports[0].x, 10.0F);
    EXPECT_FLOAT_EQ(viewport->viewports[0].y, 20.0F);
    EXPECT_FLOAT_EQ(viewport->viewports[0].width, 320.0F);
    EXPECT_FLOAT_EQ(viewport->viewports[0].height, 240.0F);
    EXPECT_FLOAT_EQ(viewport->viewports[0].minDepth, 0.0F) << "reverse-Z lives in the projection";
    EXPECT_FLOAT_EQ(viewport->viewports[0].maxDepth, 1.0F);

    const auto scissor = vine::vsg::makeScissorCommand(ViewportRect{ -5.0F, -2.0F, 0.0F, 0.0F });
    ASSERT_EQ(scissor->scissors.size(), 1U);
    EXPECT_EQ(scissor->scissors[0].offset.x, 0) << "a negative origin is a validation error, not a rectangle";
    EXPECT_EQ(scissor->scissors[0].offset.y, 0);
    EXPECT_GE(scissor->scissors[0].extent.width, 1U) << "a zero-area scissor would silently draw nothing";
    EXPECT_GE(scissor->scissors[0].extent.height, 1U);
}

TEST(ContentDrawTest, AScreenDrawRecordsThreeGeneratedVerticesAndTheSamplerSetAtSetZero)
{
    // The other drawing call, and the shape it does NOT share with a content draw: no vertex stream, no index
    // stream, no block bind - one sampler set at set 0 and `Draw(3)`. The three vertices are generated by the
    // full-screen vertex stage, so a draw that bound an index buffer here would be drawing the wrong picture
    // with the right bookkeeping.
    const auto sampler_set_layout = ::vsg::DescriptorSetLayout::create();
    sampler_set_layout->addBinding(0U, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1U,
                                   VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT);
    sampler_set_layout->addBinding(1U, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1U,
                                   VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT);
    const auto pipeline_layout = ::vsg::PipelineLayout::create(::vsg::DescriptorSetLayouts{ sampler_set_layout },
                                                               ::vsg::PushConstantRanges{});

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
    // The layer's set is what the text declares: scan the pair (the engine's table entry does exactly that).
    vine::vsg::ProgramAbi abi;
    ASSERT_EQ(vine::vsg::scanProgramAbi(shaders.vertex, shaders.fragment, {}, abi), vine::vsg::FactMiss::None);
    auto pipelines = ContentPipeline::createScreen(abi, shaders);
    ASSERT_NE(pipelines, nullptr);

    VariantPool     pool;
    StateRegistry   registry(pool);
    ContentDraw     recorder(*pipelines, pool);

    // A descriptor set of two samplers over that layout (a real one comes from the content layer's
    // makeInputSet; here the object only has to be a well-formed bind of set 0).
    ::vsg::Descriptors descriptors;
    for (std::uint32_t binding = 0; binding < 2U; ++binding) {
        auto image = ::vsg::DescriptorImage::create(
            ::vsg::ImageInfoList{ ::vsg::ImageInfo::create(::vsg::ref_ptr<::vsg::Sampler>{},
                                                           ::vsg::ref_ptr<::vsg::ImageView>{},
                                                           VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL) },
            binding, 0U, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER);
        descriptors.push_back(image);
    }
    auto set = ::vsg::DescriptorSet::create(sampler_set_layout, descriptors);
    ASSERT_NE(set, nullptr);

    ContentDraw::ScreenDraw draw;
    static int              program = 0;
    draw.key.kind                   = vine::vsg::core::DrawKind::Screen;
    draw.key.program                = &program;
    draw.key.revision               = 1U;
    draw.key.compatibility.samples  = 1U;
    draw.key.sampled_color_count    = 2U;
    draw.samplers = ::vsg::BindDescriptorSet::create(VK_PIPELINE_BIND_POINT_GRAPHICS, pipelines->layoutFor(2U, 0U), 0U,
                                                     set);
    draw.push = ::vsg::PushConstants::create(VK_SHADER_STAGE_FRAGMENT_BIT, 0U, ::vsg::ubyteArray::create(128U));
    draw.viewport = ViewportRect{ 8.0F, 8.0F, 32.0F, 16.0F };
    ASSERT_NE(draw.samplers, nullptr) << "the set must have been created";
    ASSERT_NE(draw.push, nullptr) << "the push block must have been created";

    const auto group = recorder.recordScreen(registry, draw);
    ASSERT_NE(group, nullptr);
    ASSERT_EQ(group->stateCommands.size(), 3U) << "pipeline, dynamic block, the sampler set (no blocks bind)";
    EXPECT_NE(dynamic_cast<const ::vsg::BindGraphicsPipeline*>(group->stateCommands[0].get()), nullptr);
    EXPECT_NE(dynamic_cast<const vine::vsg::detail::SetDynamicState*>(group->stateCommands[1].get()), nullptr);
    EXPECT_EQ(group->stateCommands[2], draw.samplers) << "the sampler set is the third command";

    ASSERT_EQ(group->children.size(), 1U);
    const auto* commands = dynamic_cast<const ::vsg::Commands*>(group->children[0].get());
    ASSERT_NE(commands, nullptr);
    ASSERT_EQ(commands->children.size(), 4U)
        << "viewport, scissor, the push block, ONE draw - there is no geometry to bind";
    EXPECT_NE(dynamic_cast<const ::vsg::SetViewport*>(commands->children[0].get()), nullptr);
    EXPECT_NE(dynamic_cast<const ::vsg::SetScissor*>(commands->children[1].get()), nullptr);
    const auto* push = dynamic_cast<const ::vsg::PushConstants*>(commands->children[2].get());
    ASSERT_NE(push, nullptr) << "the full-screen ABI's block is what the push range is for";
    EXPECT_EQ(push->stageFlags, VK_SHADER_STAGE_FRAGMENT_BIT) << "the screen vertex stage declares no constants";
    ASSERT_NE(push->data, nullptr);
    EXPECT_EQ(push->data->dataSize(), 128U);
    const auto* generated = dynamic_cast<const ::vsg::Draw*>(commands->children[3].get());
    ASSERT_NE(generated, nullptr) << "a full-screen draw is Draw(3), not DrawIndexed";
    EXPECT_EQ(generated->vertexCount, 3U);
    EXPECT_EQ(generated->instanceCount, 1U);
    EXPECT_EQ(generated->firstVertex, 0U);

    EXPECT_EQ(recorder.draws(), 1U);
    EXPECT_EQ(recorder.screen_draws(), 1U);
    EXPECT_EQ(recorder.pipeline_binds(), 1U);
    EXPECT_EQ(recorder.input_binds(), 1U) << "the sampled set is bound once and reused by later draws of the pass";

    // A second screen draw of the same pass: the variant is current, the dynamic state was issued and the
    // sampler set is already bound (it is the pass' one set), so its state group is EMPTY - while the draw
    // itself is still recorded per call, because the rectangle and the draw are what the pass fills.
    auto second = draw;
    second.viewport = ViewportRect{ 48.0F, 8.0F, 32.0F, 16.0F };
    const auto again = recorder.recordScreen(registry, second);
    ASSERT_NE(again, nullptr);
    EXPECT_TRUE(again->stateCommands.empty()) << "nothing left to issue: the one set was bound by the first draw";
    ASSERT_EQ(again->children.size(), 1U);
    EXPECT_EQ(recorder.draws(), 2U);
    EXPECT_EQ(recorder.input_binds(), 1U) << "one bind per pass, not per draw";

    // An identity of the OTHER kind is refused, not recorded: the layer compiles one descriptor ABI.
    ContentDraw::ScreenDraw wrong_kind = draw;
    wrong_kind.key.kind                = vine::vsg::core::DrawKind::Content;
    EXPECT_EQ(recorder.recordScreen(registry, wrong_kind), nullptr);
    EXPECT_EQ(recorder.refusals(), 1U);
    EXPECT_EQ(recorder.draws(), 2U) << "a refused draw is not a recorded one";
}
