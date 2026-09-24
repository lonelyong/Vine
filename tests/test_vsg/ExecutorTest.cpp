/**
 * @brief The frame's EXECUTION stage (see `.ai/design/vsg-reimplementation.md` §2.4, milestone M3d).
 *
 * What this case is for, and why it is a PICTURE rather than a formality. The whole reason the backend is
 * split into collect → compile → execute is that the record order is the SCHEDULE's order, not the order the
 * host announced its passes in. Every other stage can be perfect and this one still gets it backwards, and
 * the mistake is invisible in every counter: the frame submits, the validation layer is happy, and the
 * picture is the other one.
 *
 * So the frame here is built so that the two possible answers are two different colours:
 *
 *   * target A: RED announced first (order 10), GREEN second (order 0) ⇒ the schedule runs GREEN then RED,
 *     so a correct executor ends with RED - and an executor that walked the call order ends with GREEN;
 *   * target B: the same two colours announced in the opposite stack order ⇒ the schedule runs RED then
 *     GREEN, so the correct answer is GREEN and the call-order answer is still GREEN... which is why A and B
 *     are judged together: the pair (A=red, B=green) is only reachable through the schedule, while the pair
 *     (A=green, B=green) is what "record in call order" produces.
 *
 * The recorded pass ORDER is asserted as well (the executor reports it), so a failure says which stage moved
 * rather than only that a colour is wrong.
 *
 * Real device, no window (see the design's M2c-2b-2b-2a): SKIPped when no device can be created.
 */

#include <gtest/gtest.h>

#include <cmath>
#include <cstdint>
#include <memory>
#include <vector>

#include <vsg/app/CommandGraph.h>
#include <vsg/app/Viewer.h>
#include <vsg/commands/BindIndexBuffer.h>
#include <vsg/commands/BindVertexBuffers.h>

#include <vine/graphics/RenderCommand.hpp>
#include <vine/graphics/RenderTarget.hpp>

#include <vine/vsg/api/BlockDescriptors.hpp>
#include <vine/vsg/api/BlockStorage.hpp>
#include <vine/vsg/api/ContentDraw.hpp>
#include <vine/vsg/api/ContentPipeline.hpp>
#include <vine/vsg/api/Device.hpp>
#include <vine/vsg/api/OffscreenTarget.hpp>
#include <vine/vsg/api/StreamUploads.hpp>
#include <vine/vsg/api/VsgExecutor.hpp>
#include <vine/vsg/core/FrameCompiler.hpp>
#include <vine/vsg/core/FrameRecorder.hpp>
#include <vine/vsg/core/FrameTimeline.hpp>
#include <vine/vsg/core/RetirementQueue.hpp>
#include <vine/vsg/core/StateRegistry.hpp>
#include <vine/vsg/core/Streams.hpp>
#include <vine/vsg/core/VariantPool.hpp>
#include <vine/vsg/VsgDynamicState.hpp>

#include "DevicePhases.hpp"

using vine::graphics::RenderCommand;
using vine::graphics::RenderTarget;
using vine::vsg::BlockDescriptors;
using vine::vsg::BlockStorage;
using vine::vsg::ContentDraw;
using vine::vsg::ContentPipeline;
using vine::vsg::OffscreenTarget;
using vine::vsg::PassContent;
using vine::vsg::StreamUploads;
using vine::vsg::ViewportRect;
using vine::vsg::VsgExecutor;
using vine::vsg::core::ClearPolicy;
using vine::vsg::core::CompiledFrame;
using vine::vsg::core::CompiledPass;
using vine::vsg::core::Diagnostics;
using vine::vsg::core::FrameArena;
using vine::vsg::core::FrameCompiler;
using vine::vsg::core::FrameFacts;
using vine::vsg::core::FrameRecorder;
using vine::vsg::core::FrameTimeline;
using vine::vsg::core::FrameToken;
using vine::vsg::core::Observe;
using vine::vsg::core::PassId;
using vine::vsg::core::RepairReason;
using vine::vsg::core::RetirementQueue;
using vine::vsg::core::Rgba8;
using vine::vsg::core::StateRegistry;
using vine::vsg::core::StreamKey;
using vine::vsg::core::StreamKind;
using vine::vsg::core::TargetAction;
using vine::vsg::core::TargetDesc;
using vine::vsg::core::TargetFacts;
using vine::vsg::core::TargetShape;
using vine::vsg::core::VariantPool;

namespace
{

constexpr std::uint32_t kSize = 64;

/// @brief The 8-bit value a clear colour quantises to (UNORM conversion, round to nearest).
std::uint8_t quantise(float value)
{
    return static_cast<std::uint8_t>(std::lround(value * 255.0F));
}

/// @brief The pass' clear colour: nothing is black, so "the plan's clear" and "nothing cleared" differ.
constexpr float kClear[4]{ 0.25F, 0.5F, 0.75F, 1.0F };

/// @brief A shader pair that draws the triangle solid green and ignores the camera (no push constants).
ContentPipeline::Shaders triangleShaders()
{
    ContentPipeline::Shaders shaders;
    shaders.vertex = "#version 450\n"
                     "layout(location = 0) in vec3 position;\n"
                     "void main() { gl_Position = vec4(position.x, position.y, 0.5, 1.0); }\n";
    shaders.fragment = "#version 450\n"
                       "layout(location = 0) out vec4 outColor;\n"
                       "void main() { outColor = vec4(0.0, 1.0, 0.0, 1.0); }\n";
    return shaders;
}

/// @brief The compatibility half of a pipeline's identity, from the shape the target really has.

std::vector<std::byte> bytesOf(std::size_t count)
{
    return std::vector<std::byte>(count, std::byte{ 0 });
}

/// @brief Two off-screen targets, one device, and the whole pipeline that produces a compiled frame.
struct Fixture
{
    FrameArena                  arena{ 64 * 1024 };
    Diagnostics                 diagnostics;
    Observe                     observe;
    FrameRecorder               recorder{ arena, diagnostics, observe };
    FrameCompiler               compiler{ arena, diagnostics, observe };
    vine::vsg::DeviceResult     created;
    std::unique_ptr<OffscreenTarget> first;
    std::unique_ptr<OffscreenTarget> second;
    std::vector<TargetFacts>         facts;
    VsgExecutor                      executor{ diagnostics };
    ::vsg::ref_ptr<::vsg::Viewer>    viewer;

    // The content world: what records a draw is the layer that owns these, not the executor. The fixture
    // builds the same stack the MRT case does, over a shader pair that draws a solid triangle.
    std::unique_ptr<BlockStorage>       storage;
    std::unique_ptr<BlockDescriptors>   descriptors;
    std::unique_ptr<ContentPipeline>    pipelines;
    std::unique_ptr<StreamUploads>      uploads;
    std::unique_ptr<ContentDraw>        draws;
    VariantPool                         pool;
    std::unique_ptr<StateRegistry>      registry;

    bool build()
    {
        created = vine::vsg::createDevice();
        if (!created.ok)
        {
            return false;
        }

        first  = makeTarget();
        second = makeTarget();
        if (first == nullptr || second == nullptr)
        {
            return false;
        }
        executor.addTarget(first.get(), first.get());
        executor.addTarget(second.get(), second.get());

        viewer = ::vsg::Viewer::create();
        return viewer != nullptr;
    }

    /// @brief Builds the content stack (a one-colour-attachment pipeline over a position-only vertex layout).
    bool buildContent()
    {
        storage = BlockStorage::create(created.device, BlockStorage::Layout{});
        if (storage == nullptr)
        {
            return false;
        }
        const ContentPipeline::Shaders shader_pair = triangleShaders();
        vine::vsg::ProgramAbi            abi;
        if (vine::vsg::scanProgramAbi(shader_pair.vertex, shader_pair.fragment, {}, abi) != vine::vsg::FactMiss::None) {
            return false;
        }
        descriptors = BlockDescriptors::forAbi(abi, 0U, created.device, *storage);
        if (descriptors == nullptr)
        {
            return false;
        }

        const ContentPipeline::VertexBinding   binding{ 0U, sizeof(float) * 3U, false };
        const ContentPipeline::VertexAttribute attribute{ 0U, 0U, VK_FORMAT_R32G32B32_SFLOAT, 0U };
        ContentPipeline::Settings              settings;
        settings.color_attachments = 1U;
        pipelines = ContentPipeline::create(abi,
                                           std::span<const ContentPipeline::VertexBinding>(&binding, 1U),
                                           std::span<const ContentPipeline::VertexAttribute>(&attribute, 1U),
                                           shader_pair, settings);
        if (pipelines == nullptr)
        {
            return false;
        }

        uploads  = std::make_unique<StreamUploads>();
        draws    = std::make_unique<ContentDraw>(*pipelines, pool,
                                                 vine::vsg::detail::fetchDynamicStateEntryPoints(
                                                     created.device->vk(), created.instance->vk()));
        registry = std::make_unique<StateRegistry>(pool);
        return true;
    }

    /// @brief Records one triangle for @p pass, taking the state it delivers from the PLAN.
    ::vsg::ref_ptr<::vsg::Node> buildTriangle(const CompiledPass& pass)
    {
        storage->beginFrame();
        const auto view     = storage->writeView(bytesOf(288U));
        const auto block    = storage->writeDraw(bytesOf(80U));
        const auto material = storage->writeMaterial(&material_identity, 1U, bytesOf(64U));
        if (!view.valid || !block.valid)
        {
            return {};
        }

        auto positions  = ::vsg::vec3Array::create(3U);
        (*positions)[0] = ::vsg::vec3(-0.6F, -0.6F, 0.0F);
        (*positions)[1] = ::vsg::vec3(0.6F, -0.6F, 0.0F);
        (*positions)[2] = ::vsg::vec3(0.0F, 0.6F, 0.0F);
        auto indices    = ::vsg::uintArray::create(3U);
        (*indices)[0]   = 0U;
        (*indices)[1]   = 1U;
        (*indices)[2]   = 2U;

        static int triangle_vertices = 0;
        static int triangle_indices  = 0;

        StreamKey vertex_key;
        vertex_key.kind       = StreamKind::Vertex;
        vertex_key.location   = 0U;
        vertex_key.components = 3U;
        vertex_key.buffer     = &triangle_vertices;
        vertex_key.revision   = 1U;
        vertex_key.count      = 9U;

        StreamKey index_key;
        index_key.kind       = StreamKind::Index;
        index_key.location   = 0U;
        index_key.components = 1U;
        index_key.buffer     = &triangle_indices;
        index_key.revision   = 1U;
        index_key.count      = 3U;

        const auto vertex_bind = uploads->acquireVertex(vertex_key, positions);
        const auto index_bind  = uploads->acquireIndex(index_key, indices);
        if (vertex_bind.bind == nullptr || index_bind.bind == nullptr)
        {
            return {};
        }

        ContentDraw::Draw draw;
        static int       program = 0;
        draw.key.program                      = &program;
        draw.key.revision                     = 1U;
        draw.key.vertex_layout.canonical_mask = 0x1U;
        draw.key.compatibility                = first->shape().compatibility();
        draw.key.sampled_color_count          = 0U;
        draw.dynamic                          = pass.draws[0].commands[0].dynamic;
        const ::vsg::ref_ptr<::vsg::BindDescriptorSet> block_binds[] = {
            descriptors->bind(pipelines->layout(), BlockDescriptors::Offsets{ view.offset, block.offset,
                                                                               material.offset })
        };
        // The program's own text declares no blocks, so its pipeline layout declares no sets either and there
        // is nothing to bind: a set the layout does not have is an invalid handle, not an empty bind (the
        // bytes above are written for a program that reads nothing).
        if (!descriptors->shape().empty()) {
            draw.blocks = block_binds;
        }
        draw.vertex_binds = std::span<const ::vsg::ref_ptr<::vsg::BindVertexBuffers>>(&vertex_bind.bind, 1U);
        draw.index        = index_bind.bind;
        draw.viewport     = ViewportRect{ static_cast<float>(pass.viewport.x), static_cast<float>(pass.viewport.y),
                                          static_cast<float>(pass.viewport.width),
                                          static_cast<float>(pass.viewport.height) };
        draw.index_count      = 3U;
        draw.color_attachments = pass.color_attachments;
        return draws->record(*registry, draw);
    }

    /// @brief The material identity this fixture's one material is known by.
    static int material_identity;

    std::unique_ptr<OffscreenTarget> makeTarget()
    {
        OffscreenTarget::Layout layout;
        layout.width  = kSize;
        layout.height = kSize;
        auto target   = OffscreenTarget::create(created.device, layout);
        if (target == nullptr)
        {
            return nullptr;
        }

        TargetFacts entry;
        entry.target        = target.get();
        entry.wanted.width  = static_cast<int>(kSize);
        entry.wanted.height = static_cast<int>(kSize);
        entry.wanted.shape.color_formats.push_back(RenderTarget::ColorFormat::RGBA8);
        entry.current.desc  = entry.wanted;
        entry.current.built = true;
        facts.push_back(entry);
        return target;
    }

    /// @brief Collects one clear-only pass into @p target (the clear IS the picture; no content involved).
    void clearPass(const void* target, PassId id, int order, float red, float green, float blue)
    {
        ClearPolicy policy;
        policy.color          = true;
        policy.color_value[0] = red;
        policy.color_value[1] = green;
        policy.color_value[2] = blue;
        policy.color_value[3] = 1.0F;

        EXPECT_TRUE(recorder.beginPass(id));
        EXPECT_TRUE(recorder.setPassOrder(order));
        EXPECT_TRUE(recorder.setRenderTarget(target));
        EXPECT_TRUE(recorder.setClearPolicy(policy));
        EXPECT_TRUE(recorder.endPass());
    }

    /// @brief Records the compiled frame and renders it once.
    ::vsg::ref_ptr<::vsg::CommandGraph> last_graph;  ///< The graph the last render() recorded into.

    bool render(const CompiledFrame& frame, std::span<const PassContent> content = {})
    {
        auto command_graph = ::vsg::CommandGraph::create(created.device, created.queue_family);
        executor.record(frame, command_graph, content);
        last_graph = command_graph;

        viewer->assignRecordAndSubmitTaskAndPresentation(::vsg::CommandGraphs{ command_graph });
        if (!viewer->compile())
        {
            return false;
        }
        viewer->advanceToNextFrame();
        viewer->handleEvents();
        viewer->recordAndSubmit();
        viewer->deviceWaitIdle();
        return true;
    }

    /// @brief Reads the centre pixel of @p target.
    Rgba8 centre(const OffscreenTarget& target) const
    {
        return target.probe().pixel(static_cast<int>(kSize) / 2, static_cast<int>(kSize) / 2);
    }
};

bool isRed(const Rgba8& pixel)
{
    return pixel.r > 200 && pixel.g < 40 && pixel.b < 40;
}

bool isGreen(const Rgba8& pixel)
{
    return pixel.g > 200 && pixel.r < 40 && pixel.b < 40;
}

}  // namespace

void runPlanDrivenTargetPhase(const vine::vsg::DeviceResult& device, DevicePhaseCounters& counters)
{
    const float kFirst[4]{ 0.25F, 0.5F, 0.75F, 1.0F };

    auto target = OffscreenTarget::create(device.device,
                                          OffscreenTarget::Layout{ 8U, 4U,
                                                                   { kFirst[0], kFirst[1], kFirst[2], 1.0F } });
    ASSERT_NE(target, nullptr);
    ++counters.targets_built;

    // A second target this executor holds and no frame of this phase names. The drive walks the PLAN's
    // targets, so this one must not move - the observable that says "the plan was applied" rather than
    // "everything the executor holds was reshaped".
    auto bystander = OffscreenTarget::create(device.device,
                                             OffscreenTarget::Layout{ 8U, 4U, { 0.0F, 0.0F, 0.0F, 1.0F } });
    ASSERT_NE(bystander, nullptr);
    ++counters.targets_built;

    FrameArena    arena(64 * 1024);
    Diagnostics   diagnostics;
    Observe       observe;
    FrameRecorder recorder{ arena, diagnostics, observe };
    FrameCompiler compiler{ arena, diagnostics, observe };

    // A plan names IDENTITIES; the executor is the layer that holds the targets they resolve to.
    vine::intrusive_ptr<RenderTarget> handle(new RenderTarget());

    VsgExecutor executor(diagnostics);
    executor.addTarget(handle.get(), target.get());
    executor.addTarget(bystander.get(), bystander.get());

    FrameTimeline   timeline;
    RetirementQueue queue(3U);

    const auto facts_of = [&](const TargetDesc& wanted) {
        std::vector<TargetFacts> table(1U);
        table[0].target  = handle.get();
        table[0].wanted  = wanted;
        table[0].current = target->instance();
        return table;
    };
    const auto record_frame = [&](std::uint64_t token, const ClearPolicy& policy,
                                  const std::vector<TargetFacts>& table) {
        EXPECT_TRUE(recorder.beginFrame(FrameToken{ token }));
        EXPECT_TRUE(recorder.beginPass(1U));
        EXPECT_TRUE(recorder.setRenderTarget(handle.get()));
        EXPECT_TRUE(recorder.setClearPolicy(policy));
        EXPECT_TRUE(recorder.endPass());
        EXPECT_TRUE(recorder.endFrame());
        EXPECT_TRUE(recorder.swapBuffers());
        return &compiler.compile(recorder.description(), FrameFacts{ table });
    };
    // The whole frame drive: the plan's answers become real, the plan is recorded, and the frame is submitted
    // through the executor's own step (which also says whether the submission happened).
    const auto drive = [&](const CompiledFrame& frame, const std::vector<TargetFacts>& table) {
        const VsgExecutor::TargetApplications applied = executor.applyTargetPlans(frame, table, timeline, queue);
        auto command_graph = ::vsg::CommandGraph::create(device.device, device.queue_family);
        EXPECT_TRUE(executor.record(frame, command_graph));
        EXPECT_EQ(executor.skipped(), 0U) << "every pass of the plan has a target this executor holds";
        // A second colour attachment is only readable if its copy is recorded too (the executor hands out
        // attachment 0 for a probe); a one-colour shape has no such node and nothing is added.
        if (const ::vsg::ref_ptr<::vsg::Node> extra = target->capture(1U)) {
            command_graph->addChild(extra);
        }
        auto viewer = ::vsg::Viewer::create();
        viewer->assignRecordAndSubmitTaskAndPresentation(::vsg::CommandGraphs{ command_graph });
        EXPECT_TRUE(viewer->compile());
        viewer->advanceToNextFrame();
        viewer->handleEvents();
        EXPECT_TRUE(executor.submit(frame, *viewer)) << "the submission step says the frame happened";
        viewer->deviceWaitIdle();
        ++counters.frames;
        return applied;
    };

    ClearPolicy fill;
    fill.color          = true;
    fill.color_value[0] = kFirst[0];
    fill.color_value[1] = kFirst[1];
    fill.color_value[2] = kFirst[2];
    fill.color_value[3] = 1.0F;

    // Frame 1: the plan asks for what the target already IS, but nothing has been written into it yet -
    // so the plan answers the load-op repair (the first pass in clears), which the drive does not apply: a
    // repair is what the RECORDING answers. The frame renders, the baseline the next two are a change
    // against.
    const auto           table_1 = facts_of(TargetDesc{ 8, 4, target->shape() });
    const CompiledFrame* first   = record_frame(1U, fill, table_1);
    ASSERT_EQ(first->passes.size(), 1U);
    ASSERT_EQ(first->targets.size(), 1U);
    EXPECT_EQ(static_cast<int>(first->targets[0].decision.action), static_cast<int>(TargetAction::Repair));
    EXPECT_EQ(static_cast<int>(first->targets[0].decision.reason), static_cast<int>(RepairReason::Bootstrap));
    {
        const VsgExecutor::TargetApplications applied = drive(*first, table_1);
        EXPECT_EQ(applied.resized + applied.rebuilt + applied.refused + applied.failed, 0U)
            << "the repair arms are the recording's, not a replacement: the drive applies nothing";
    }
    EXPECT_TRUE(target->written()) << "the bootstrapping pass was recorded: the target holds something";
    {
        const vine::vsg::core::PixelProbe probe = target->probe();
        ASSERT_TRUE(probe.valid());
        EXPECT_EQ(probe.width(), 8);
        EXPECT_EQ(probe.height(), 4);
    }

    // Frame 2: the plan asks for a new EXTENT of the same shape - ResizeInPlace - and the drive applies it.
    // The recording that follows lands in the new images, whose clear is what the probe reads.
    const auto           table_2 = facts_of(TargetDesc{ 16, 12, target->shape() });
    const CompiledFrame* second  = record_frame(2U, fill, table_2);
    ASSERT_EQ(static_cast<int>(second->targets[0].decision.action),
              static_cast<int>(TargetAction::ResizeInPlace));
    {
        const VsgExecutor::TargetApplications applied = drive(*second, table_2);
        EXPECT_EQ(applied.resized, 1U) << "the plan's answer became a replacement";
        EXPECT_EQ(applied.rebuilt + applied.refused + applied.failed, 0U);
    }
    ++counters.resizes_replaced;
    ++counters.plan_applied;
    ++counters.parked;
    EXPECT_EQ(target->width(), 16U);
    EXPECT_EQ(target->height(), 12U);
    EXPECT_EQ(target->generation(), 1U);
    {
        const vine::vsg::core::PixelProbe probe = target->probe();
        ASSERT_TRUE(probe.valid());
        EXPECT_EQ(probe.width(), 16) << "the probe follows the extent the DRIVE's plan asked for";
        EXPECT_EQ(probe.height(), 12);
        const Rgba8 expected{ quantise(kFirst[0]), quantise(kFirst[1]), quantise(kFirst[2]), 255U };
        const Rgba8 sampled = probe.pixel(8, 6);
        EXPECT_NEAR(sampled.r, expected.r, 1);
        EXPECT_NEAR(sampled.g, expected.g, 1);
        EXPECT_NEAR(sampled.b, expected.b, 1);
        EXPECT_TRUE(probe.wholeImageMatches(sampled));
    }

    // Frame 3: the plan asks for a new SHAPE (a second colour attachment and a depth) - Rebuild - and the
    // drive applies that too. The frame records through the REBUILT pass, which is the half a shape change
    // that only replaced images would fail.
    //
    // The wanted shape is DECLARED in the engine's terms: the host asks for a second colour attachment and a
    // depth, and the DEVICE spellings are the target layer's answer to that request (the rebuild derives
    // them through the same mapping create uses). Not stating them is honest - "not known" is not "absent"
    // (see CompiledShape) - while a HALF-stated device list (the old shape's, with one more engine format
    // pushed onto it) would describe a target nobody asked for, which the record step reports.
    TargetShape wanted_shape;
    wanted_shape.color_formats = { RenderTarget::ColorFormat::RGBA8, RenderTarget::ColorFormat::RGBA8 };
    wanted_shape.depth_format  = RenderTarget::DepthFormat::D32F;
    const auto           table_3 = facts_of(TargetDesc{ 16, 12, wanted_shape });
    const CompiledFrame* third   = record_frame(3U, fill, table_3);
    ASSERT_EQ(static_cast<int>(third->targets[0].decision.action), static_cast<int>(TargetAction::Rebuild));
    {
        const VsgExecutor::TargetApplications applied = drive(*third, table_3);
        EXPECT_EQ(applied.rebuilt, 1U) << "a shape change is the rebuild arm, not a resize";
        EXPECT_EQ(applied.resized + applied.refused + applied.failed, 0U);
    }
    ++counters.rebuilds_replaced;
    ++counters.plan_applied;
    ++counters.parked;
    EXPECT_EQ(target->colorAttachmentCount(), 2U);
    EXPECT_TRUE(target->hasDepth());
    EXPECT_EQ(target->generation(), 2U);
    {
        const vine::vsg::core::PixelProbe probe = target->probe();
        ASSERT_TRUE(probe.valid());
        const Rgba8 expected{ quantise(kFirst[0]), quantise(kFirst[1]), quantise(kFirst[2]), 255U };
        const Rgba8 sampled = probe.pixel(8, 6);
        EXPECT_NEAR(sampled.r, expected.r, 1);
        EXPECT_TRUE(probe.wholeImageMatches(sampled));
    }
    {
        const vine::vsg::core::PixelProbe extra = target->probe(1U);
        ASSERT_TRUE(extra.valid()) << "the rebuilt shape has a second colour attachment, and it is readable";
        EXPECT_EQ(extra.width(), 16);
        EXPECT_EQ(extra.pixel(8, 6).a, 0U) << "an extra colour attachment clears to transparent black";
    }

    // The bystander: held by this executor, named by no plan - untouched by every application above.
    EXPECT_EQ(bystander->generation(), 0U) << "the drive walks the PLAN's targets, not everything it holds";
    EXPECT_EQ(bystander->width(), 8U);
    EXPECT_EQ(bystander->height(), 4U);
    EXPECT_FALSE(bystander->written()) << "no pass of any driven frame named it";
}

int Fixture::material_identity = 0;

TEST(ExecutorTest, TheContentOfAPassIsRecordedInsideThatPassWithThePlansClear)
{
    Fixture fixture;
    if (!fixture.build() || !fixture.buildContent())
    {
        GTEST_SKIP() << "no Vulkan device available (lavapipe + X11 are needed): "
                     << fixture.created.error.as_std_str();
    }

    ClearPolicy clear;
    clear.color          = true;
    clear.color_value[0] = kClear[0];
    clear.color_value[1] = kClear[1];
    clear.color_value[2] = kClear[2];
    clear.color_value[3] = 1.0F;

    const std::vector<RenderCommand> commands{ RenderCommand{} };
    fixture.recorder.beginFrame(FrameToken{ 1 });
    fixture.recorder.beginPass(1U);
    fixture.recorder.setRenderTarget(fixture.first.get());
    fixture.recorder.setClearPolicy(clear);
    fixture.recorder.render(commands, nullptr);
    fixture.recorder.endPass();
    fixture.recorder.endFrame();

    const CompiledFrame& frame =
        fixture.compiler.compile(fixture.recorder.description(), FrameFacts{ fixture.facts });
    ASSERT_EQ(frame.passes.size(), 1U);
    ASSERT_EQ(frame.passes[0].draws.size(), 1U);
    ASSERT_EQ(frame.passes[0].draws[0].commands.size(), 1U);

    // The content layer records the draw, and everything it delivers comes from the PLAN: the dynamic state
    // and the viewport here were resolved by the compiler, not re-decided by the recorder of content.
    ::vsg::ref_ptr<::vsg::Node> content = fixture.buildTriangle(frame.passes[0]);
    ASSERT_NE(content, nullptr);
    const PassContent packet{ frame.passes[0].pass, content };

    ASSERT_TRUE(fixture.render(frame, std::span<const PassContent>(&packet, 1U)));
    ASSERT_EQ(fixture.executor.skipped(), 0U);

    // The triangle is inside the pass (green at the centre), and the background is the PLAN's clear colour -
    // not the target's own, which the pass graph never used.
    const Rgba8 centre = fixture.centre(*fixture.first);
    EXPECT_TRUE(isGreen(centre)) << "the recorded content must be drawn inside the pass, got ("
                                 << static_cast<int>(centre.r) << ", " << static_cast<int>(centre.g) << ", "
                                 << static_cast<int>(centre.b) << ")";

    const Rgba8 corner = fixture.first->probe().pixel(1, 1);
    EXPECT_NEAR(static_cast<int>(corner.r), 64, 2) << "the background is the plan's clear colour";
    EXPECT_NEAR(static_cast<int>(corner.g), 128, 2);
    EXPECT_NEAR(static_cast<int>(corner.b), 191, 2);
    EXPECT_TRUE(fixture.diagnostics.clean());
}

TEST(ExecutorTest, TheRecordOrderIsTheSchedulesOrderNotTheOrderThePassesWereAnnouncedIn)
{
    Fixture fixture;
    if (!fixture.build())
    {
        GTEST_SKIP() << "no Vulkan device available (lavapipe + X11 are needed): "
                     << fixture.created.error.as_std_str();
    }

    ASSERT_NE(fixture.first, nullptr);

    fixture.recorder.beginFrame(FrameToken{ 1 });
    // Target A: RED announced with order 10, GREEN with order 0 - so the schedule runs GREEN first and RED
    // last, and the frame's final colour there must be RED.
    fixture.clearPass(fixture.first.get(), 1U, 10, 1.0F, 0.0F, 0.0F);
    fixture.clearPass(fixture.first.get(), 2U, 0, 0.0F, 1.0F, 0.0F);

    // Target B: the same two colours with the opposite stack order - the schedule runs RED then GREEN.
    fixture.clearPass(fixture.second.get(), 3U, 10, 0.0F, 1.0F, 0.0F);
    fixture.clearPass(fixture.second.get(), 4U, 0, 1.0F, 0.0F, 0.0F);
    fixture.recorder.endFrame();

    const CompiledFrame& frame = fixture.compiler.compile(fixture.recorder.description(), FrameFacts{ fixture.facts });

    // The plan's order: the two order-0 passes first (call order within the tie), then the two order-10 ones.
    ASSERT_EQ(frame.passes.size(), 4U);
    EXPECT_EQ(frame.passes[0].pass, 2U);
    EXPECT_EQ(frame.passes[1].pass, 4U);
    EXPECT_EQ(frame.passes[2].pass, 1U);
    EXPECT_EQ(frame.passes[3].pass, 3U);

    ASSERT_TRUE(fixture.render(frame)) << "the graph must compile and submit";
    ASSERT_EQ(fixture.executor.skipped(), 0U) << "no pass may be skipped: every one has a target";

    // The executor recorded them in the plan's order - not in the order they were announced.
    const std::span<const PassId> recorded = fixture.executor.recorded();
    ASSERT_EQ(recorded.size(), 4U);
    EXPECT_EQ(recorded[0], 2U);
    EXPECT_EQ(recorded[1], 4U);
    EXPECT_EQ(recorded[2], 1U);
    EXPECT_EQ(recorded[3], 3U);

    // ...and the pixels say the same thing. A walk in call order would put GREEN last into target A and RED
    // last into target B - the pair (A, B) = (red, green) is only reachable through the schedule.
    const Rgba8 first_pixel  = fixture.centre(*fixture.first);
    const Rgba8 second_pixel = fixture.centre(*fixture.second);
    EXPECT_TRUE(isRed(first_pixel)) << "target A must end on the pass the SCHEDULE ran last (red), got ("
                                    << static_cast<int>(first_pixel.r) << ", " << static_cast<int>(first_pixel.g)
                                    << ", " << static_cast<int>(first_pixel.b) << ")";
    EXPECT_TRUE(isGreen(second_pixel)) << "target B must end on its schedule-last pass (green), got ("
                                       << static_cast<int>(second_pixel.r) << ", "
                                       << static_cast<int>(second_pixel.g) << ", "
                                       << static_cast<int>(second_pixel.b) << ")";

    EXPECT_TRUE(fixture.diagnostics.clean());
}

TEST(ExecutorTest, APlanThatDisagreesWithTheTargetAboutItsShapeIsNotRecorded)
{
    Fixture fixture;
    if (!fixture.build())
    {
        GTEST_SKIP() << "no Vulkan device available (lavapipe + X11 are needed): "
                     << fixture.created.error.as_std_str();
    }

    // The facts say the target has two colour attachments; the target that answers for that identity has one.
    // Something upstream drifted, and the plan now describes a different target than the one it resolves to -
    // a pipeline built against that plan would be compiled for a shape this target does not have.
    fixture.facts[0].wanted.shape.color_formats.push_back(RenderTarget::ColorFormat::RGBA16F);
    fixture.facts[0].current.desc = fixture.facts[0].wanted;

    fixture.recorder.beginFrame(FrameToken{ 1 });
    fixture.clearPass(fixture.first.get(), 1U, 0, 1.0F, 1.0F, 1.0F);
    fixture.recorder.endFrame();

    const CompiledFrame& frame =
        fixture.compiler.compile(fixture.recorder.description(), FrameFacts{ fixture.facts });
    ASSERT_EQ(frame.passes.size(), 1U);
    EXPECT_EQ(frame.passes[0].color_attachments, 2U);

    auto command_graph = ::vsg::CommandGraph::create(fixture.created.device, fixture.created.queue_family);
    EXPECT_FALSE(fixture.executor.record(frame, command_graph));
    EXPECT_EQ(fixture.executor.skipped(), 1U);
    EXPECT_TRUE(fixture.executor.recorded().empty());
    EXPECT_EQ(fixture.diagnostics.count(vine::graphics::DiagnosticCategory::ContentSkipped), 1U);
}

TEST(ExecutorTest, APlanThatGotOnlyTheFormatsWrongIsRefusedToo)
{
    Fixture fixture;
    if (!fixture.build())
    {
        GTEST_SKIP() << "no Vulkan device available (lavapipe + X11 are needed): "
                     << fixture.created.error.as_std_str();
    }

    // The truth about the target the plan resolves to: one colour attachment, and the DEVICE format its
    // image and render pass are really built with. The engine's vocabulary cannot tell two device formats
    // apart (see RenderPassCompatibility), so this is the half that only the plan's own copy can carry.
    const TargetShape actual = fixture.first->shape();
    ASSERT_EQ(actual.color_formats.size(), 1U);
    ASSERT_EQ(actual.device_color_formats.size(), 1U);

    fixture.recorder.beginFrame(FrameToken{ 1 });
    fixture.clearPass(fixture.first.get(), 1U, 0, 1.0F, 1.0F, 1.0F);
    fixture.recorder.endFrame();

    std::vector<TargetFacts> drifted = fixture.facts;

    // (1) The ENGINE's spelling drifted: same count, same samples, same depth - a different format. The
    // count-and-sampleability half of the check cannot see this.
    drifted[0].wanted.shape.color_formats[0] = RenderTarget::ColorFormat::RGBA16F;
    drifted[0].current.desc                  = drifted[0].wanted;

    const CompiledFrame& engine_drift =
        fixture.compiler.compile(fixture.recorder.description(), FrameFacts{ drifted });
    ASSERT_EQ(engine_drift.passes.size(), 1U);
    ASSERT_EQ(engine_drift.targets.size(), 1U);
    EXPECT_EQ(engine_drift.passes[0].color_attachments, 1U) << "the count agrees: only the format drifted";

    auto engine_graph = ::vsg::CommandGraph::create(fixture.created.device, fixture.created.queue_family);
    EXPECT_FALSE(fixture.executor.record(engine_drift, engine_graph)) << "the same count is not the same render pass";
    EXPECT_EQ(fixture.executor.skipped(), 1U);
    EXPECT_TRUE(fixture.executor.recorded().empty());
    EXPECT_EQ(fixture.diagnostics.count(vine::graphics::DiagnosticCategory::ContentSkipped), 1U);

    // (2) The engine's spelling is back; the DEVICE's drifted - and this one the engine cannot even express
    // (both spellings are RGBA8 to it), which is what the plan's copy of the device formats is for.
    const std::uint32_t other_format = actual.device_color_formats[0] == VK_FORMAT_R8G8B8A8_UNORM
                                           ? VK_FORMAT_B8G8R8A8_UNORM
                                           : VK_FORMAT_R8G8B8A8_UNORM;
    drifted[0].wanted.shape.color_formats[0]        = actual.color_formats[0];
    drifted[0].wanted.shape.device_color_formats    = { other_format };
    drifted[0].current.desc                         = drifted[0].wanted;

    const CompiledFrame& device_drift =
        fixture.compiler.compile(fixture.recorder.description(), FrameFacts{ drifted });
    ASSERT_EQ(device_drift.passes.size(), 1U);
    ASSERT_EQ(device_drift.targets[0].shape.device_color_formats.size(), 1U);
    EXPECT_EQ(device_drift.targets[0].shape.device_color_formats[0], other_format)
        << "the plan carries the device's spelling it was told";

    auto device_graph = ::vsg::CommandGraph::create(fixture.created.device, fixture.created.queue_family);
    EXPECT_FALSE(fixture.executor.record(device_drift, device_graph));
    EXPECT_EQ(fixture.executor.skipped(), 1U);
    EXPECT_EQ(fixture.diagnostics.count(vine::graphics::DiagnosticCategory::ContentSkipped), 2U);

    // (3) Told the truth, the SAME frame records: what was wrong was the plan's account of the target, not
    // the target.
    drifted[0].wanted.shape = actual;
    drifted[0].current.desc = drifted[0].wanted;
    const CompiledFrame& truthful =
        fixture.compiler.compile(fixture.recorder.description(), FrameFacts{ drifted });
    ASSERT_EQ(truthful.passes.size(), 1U);

    auto served_graph = ::vsg::CommandGraph::create(fixture.created.device, fixture.created.queue_family);
    EXPECT_TRUE(fixture.executor.record(truthful, served_graph));
    EXPECT_EQ(fixture.executor.skipped(), 0U);
    EXPECT_EQ(fixture.executor.recorded().size(), 1U);
}

TEST(ExecutorTest, ApplyingThePlansAnswerRepairsWhatTheRecordStepWouldOnlyReport)
{
    Fixture fixture;
    if (!fixture.build())
    {
        GTEST_SKIP() << "no Vulkan device available (lavapipe + X11 are needed): "
                     << fixture.created.error.as_std_str();
    }

    // The SAME drift the case above reports - the facts ask for a second colour attachment the target does not
    // have - except that here they tell the TRUTH about what the backend currently has, so the plan answers
    // Rebuild. What this case adds is the drive's half: the answer is APPLIED, and the frame that could not be
    // recorded a moment ago becomes recordable. "Reported" and "repaired" differ by exactly this call.
    //
    // The frame names the SECOND target on purpose: the drive has to match a plan's target to ITS facts entry
    // (the wanted description lives there), and a call that took "the first fact" or "the first registered
    // target" would answer with this one instead.
    fixture.facts[1].wanted.shape.color_formats.push_back(RenderTarget::ColorFormat::RGBA16F);
    fixture.facts[1].current = fixture.second->instance();

    fixture.recorder.beginFrame(FrameToken{ 1 });
    fixture.clearPass(fixture.second.get(), 1U, 0, 1.0F, 1.0F, 1.0F);
    fixture.recorder.endFrame();

    const CompiledFrame& frame =
        fixture.compiler.compile(fixture.recorder.description(), FrameFacts{ fixture.facts });
    ASSERT_EQ(frame.passes.size(), 1U);
    ASSERT_EQ(frame.targets.size(), 1U);
    EXPECT_EQ(frame.targets[0].target, fixture.second.get());
    EXPECT_EQ(frame.passes[0].color_attachments, 2U);
    EXPECT_EQ(static_cast<int>(frame.targets[0].decision.action), static_cast<int>(TargetAction::Rebuild))
        << "the facts describe a shape the target does not have yet";

    FrameTimeline                          timeline;
    RetirementQueue                        queue(3U);
    const VsgExecutor::TargetApplications  applied =
        fixture.executor.applyTargetPlans(frame, fixture.facts, timeline, queue);
    EXPECT_EQ(applied.rebuilt, 1U) << "the plan's answer became a real rebuild";
    EXPECT_EQ(applied.resized + applied.refused + applied.failed, 0U);
    EXPECT_EQ(fixture.second->colorAttachmentCount(), 2U);
    EXPECT_EQ(fixture.first->colorAttachmentCount(), 1U) << "the target the plan did NOT name is untouched";
    EXPECT_FALSE(fixture.second->written()) << "the fresh attachments make the pass the first writer again";

    ASSERT_TRUE(fixture.render(frame)) << "the recording that refused this frame before now serves it";
    EXPECT_EQ(fixture.executor.skipped(), 0U);

    const Rgba8 centre = fixture.centre(*fixture.second);
    EXPECT_GT(centre.r, 200);
    EXPECT_GT(centre.g, 200);
    EXPECT_GT(centre.b, 200) << "the rebuilt target renders the frame's own clear";
    EXPECT_TRUE(fixture.diagnostics.clean());
}

TEST(ExecutorTest, APassIntoTheDefaultFramebufferIsReportedRatherThanDrawnSomewhereElse)
{
    Fixture fixture;
    if (!fixture.build())
    {
        GTEST_SKIP() << "no Vulkan device available (lavapipe + X11 are needed): "
                     << fixture.created.error.as_std_str();
    }

    fixture.recorder.beginFrame(FrameToken{ 1 });
    fixture.clearPass(nullptr, 9U, 0, 1.0F, 1.0F, 1.0F);  // the window pass: this executor does not serve it
    fixture.recorder.endFrame();

    // The window is a target the BACKEND knows (it compiled the pass), it is just not one this executor can
    // draw into yet - which is the difference the second case pins: the plan is complete, the execution layer
    // is not, and the caller is told rather than having its content drawn into some other target.
    TargetFacts window;
    window.target        = nullptr;
    window.wanted.width  = 128;
    window.wanted.height = 128;
    window.wanted.shape.color_formats.push_back(RenderTarget::ColorFormat::RGBA8);
    window.current.desc  = window.wanted;
    window.current.built = true;
    std::vector<TargetFacts> facts = fixture.facts;
    facts.push_back(window);

    const CompiledFrame& frame = fixture.compiler.compile(fixture.recorder.description(), FrameFacts{ facts });
    ASSERT_EQ(frame.passes.size(), 1U);

    auto command_graph = ::vsg::CommandGraph::create(fixture.created.device, fixture.created.queue_family);
    EXPECT_FALSE(fixture.executor.record(frame, command_graph)) << "an unserved pass makes the frame partial";
    EXPECT_EQ(fixture.executor.skipped(), 1U);
    EXPECT_TRUE(fixture.executor.recorded().empty());

    // Reported, not silently redirected: the caller hears about it through the one route.
    EXPECT_EQ(fixture.diagnostics.count(vine::graphics::DiagnosticCategory::ContentSkipped), 1U);
}

TEST(ExecutorTest, ProfilingOffAddsNothingToTheRecordedGraph)
{
    Fixture fixture;
    if (!fixture.build())
    {
        GTEST_SKIP() << "no Vulkan device available (lavapipe + X11 are needed): "
                     << fixture.created.error.as_std_str();
    }

    ASSERT_NE(fixture.first, nullptr);
    fixture.recorder.beginFrame(FrameToken{ 1 });
    fixture.clearPass(fixture.first.get(), 1U, 0, 1.0F, 0.0F, 0.0F);
    fixture.recorder.endFrame();

    const CompiledFrame& frame =
        fixture.compiler.compile(fixture.recorder.description(), FrameFacts{ fixture.facts });
    ASSERT_EQ(frame.passes.size(), 1U);

    // The default: no measurement, so the command graph is the pass graphs themselves - a frame that
    // silently carried wrappers would pay for a query pool nobody asked for, and a capture would show
    // names nobody set.
    ASSERT_TRUE(fixture.render(frame));
    ASSERT_NE(fixture.last_graph, nullptr);
    ASSERT_EQ(fixture.executor.profileEntries().size(), 0U);
    ASSERT_FALSE(fixture.executor.profiling());

    std::size_t wrappers = 0;
    for (const ::vsg::ref_ptr<::vsg::Node>& child : fixture.last_graph->children)
    {
        if (dynamic_cast<const ::vsg::InstrumentationNode*>(child.get()) != nullptr)
        {
            ++wrappers;
        }
    }
    EXPECT_EQ(wrappers, 0U) << "not measuring means not wrapping";
}

TEST(ExecutorTest, ProfilingWrapsEachPassAndNamesItForTheCapture)
{
    Fixture fixture;
    if (!fixture.build())
    {
        GTEST_SKIP() << "no Vulkan device available (lavapipe + X11 are needed): "
                     << fixture.created.error.as_std_str();
    }

    ASSERT_NE(fixture.first, nullptr);
    fixture.executor.setProfiling(true);
    EXPECT_TRUE(fixture.executor.profiling());

    const auto record_one = [&](std::uint64_t token, float red) {
        EXPECT_TRUE(fixture.recorder.beginFrame(FrameToken{ token }));
        fixture.clearPass(fixture.first.get(), 1U, 0, red, 0.0F, 0.0F);
        EXPECT_TRUE(fixture.recorder.endFrame());
        // The contract's last call of a frame: without it the NEXT beginFrame() is refused, and the
        // second frame of this case would silently compile the first one's collection (measured).
        EXPECT_TRUE(fixture.recorder.swapBuffers());
        return &fixture.compiler.compile(fixture.recorder.description(), FrameFacts{ fixture.facts });
    };

    const CompiledFrame* frame = record_one(1U, 1.0F);
    ASSERT_NE(frame, nullptr);
    ASSERT_EQ(frame->passes.size(), 1U);
    ASSERT_TRUE(fixture.render(*frame));
    ASSERT_NE(fixture.last_graph, nullptr);

    // Every pass is recorded through a wrapper whose child IS the pass' graph: the measurement interval
    // then carries that graph as its object, which is the only thing that makes it attributable.
    ASSERT_EQ(fixture.executor.profileEntries().size(), 1U);
    const VsgExecutor::ProfileEntry& entry = fixture.executor.profileEntries()[0];
    EXPECT_FALSE(entry.window);
    EXPECT_EQ(entry.pass, frame->passes[0].pass);
    EXPECT_EQ(entry.schedule, frame->passes[0].schedule_index);
    EXPECT_EQ(entry.name, "target0@0") << "the name is for a capture: target<i>@<schedule>";

    std::size_t wrappers = 0;
    for (const ::vsg::ref_ptr<::vsg::Node>& child : fixture.last_graph->children)
    {
        const auto* wrapper = dynamic_cast<const ::vsg::InstrumentationNode*>(child.get());
        if (wrapper != nullptr)
        {
            ++wrappers;
            EXPECT_EQ(wrapper->child.get(), entry.graph) << "the wrapper wraps THE pass graph, not a copy";
        }
    }
    EXPECT_EQ(wrappers, 1U);

    // The reader's half: a measurement interval carries a graph, and this answers which pass that graph was
    // - so attribution needs no second table (the name is only for a capture).
    ASSERT_NE(entry.graph, nullptr);
    const VsgExecutor::ProfileEntry* found = fixture.executor.profileOf(*entry.graph);
    ASSERT_NE(found, nullptr);
    EXPECT_EQ(found->pass, entry.pass);
    EXPECT_EQ(found->name, entry.name);

    // The frame still RENDERS: the wrapper changes the bookkeeping, not the picture.
    const Rgba8 corner = fixture.first->probe().pixel(1, 1);
    EXPECT_NEAR(static_cast<int>(corner.r), 255, 2) << "the clear colour is unchanged by the wrapper";

    // A second frame replaces the entries: an entry names a graph of ITS frame, and keeping the previous
    // frame's would attribute this frame's intervals to the last one's passes.
    const CompiledFrame* second = record_one(2U, 0.0F);
    ASSERT_NE(second, nullptr);
    ASSERT_TRUE(fixture.render(*second));
    ASSERT_EQ(second->passes.size(), 1U);
    ASSERT_EQ(fixture.executor.profileEntries().size(), 1U) << "one frame's entries, not two frames'";

    // ... and it names THIS frame's graph: the graph inside the wrapper the second frame recorded. (The
    // previous frame's ENTRY is dropped; its graph object may well have been freed and its address reused,
    // so "the old pointer is gone" is not a claim about anything.)
    ::vsg::RenderGraph* second_graph = nullptr;
    for (const ::vsg::ref_ptr<::vsg::Node>& child : fixture.last_graph->children) {
        if (const auto* wrapper = dynamic_cast<const ::vsg::InstrumentationNode*>(child.get())) {
            second_graph = dynamic_cast<::vsg::RenderGraph*>(wrapper->child.get());
        }
    }
    ASSERT_NE(second_graph, nullptr) << "the second frame records through a wrapper too";
    const VsgExecutor::ProfileEntry& replaced = fixture.executor.profileEntries()[0];
    EXPECT_EQ(replaced.pass, second->passes[0].pass);
    EXPECT_EQ(replaced.graph, second_graph) << "the entry names the frame that was just recorded";
    EXPECT_EQ(fixture.executor.profileOf(*second_graph), &replaced);
}

TEST(ExecutorTest, ATargetThatDidNotFollowItsDescriptionIsNamedInTheReport)
{
    // An unapplied answer is a WARNING a host has to act on, and a warning that says "a target" is one nobody
    // can act on: the report names the target the way the host does (the SDK target's own name - what
    // api/VsgBackend registers, and the same name the target's images get for the validation layer). This case
    // drives the report through the executor and reads the message off the sink.
    Fixture fixture;
    if (!fixture.build())
    {
        GTEST_SKIP() << "no Vulkan device available (lavapipe + X11 are needed): "
                     << fixture.created.error.as_std_str();
    }

    // A lender and a borrower: the borrower's depth is the lender's image, so the borrower may not grow past
    // it (VUID-VkFramebufferCreateInfo-pAttachments-00861, see core::coversBorrowers). A plan that asks it to
    // is REFUSED - and the refusal is exactly the answer that has to name the target.
    OffscreenTarget::TargetLayout layout;
    layout.width         = 8U;
    layout.height        = 6U;
    layout.color_formats = { RenderTarget::ColorFormat::RGBA8 };
    layout.depth_format  = RenderTarget::DepthFormat::D32;
    std::unique_ptr<OffscreenTarget> lender   = OffscreenTarget::create(fixture.created.device, layout);
    ASSERT_NE(lender, nullptr);
    std::unique_ptr<OffscreenTarget> borrower = OffscreenTarget::create(fixture.created.device, layout, lender.get());
    ASSERT_NE(borrower, nullptr);

    // Registered under the names the backend passes along, and watched through the host's sink.
    fixture.executor.addTarget(lender.get(), lender.get(), "probe-lender");
    fixture.executor.addTarget(borrower.get(), borrower.get(), "probe-borrower");

    std::vector<std::string> said;
    fixture.diagnostics.setSink([&said](const auto& diagnostic) { said.push_back(diagnostic.message.as_std_str()); });

    std::vector<TargetFacts> lying(2U);
    lying[0].target              = lender.get();
    lying[0].wanted.width        = 8;
    lying[0].wanted.height       = 6;
    lying[0].wanted.shape        = lender->shape();
    // A shape change on the LENDER: a target another target loads the depth of does not rebuild (see
    // OffscreenTarget::rebuild), so this is the answer that gets refused.
    lying[0].wanted.shape.color_formats.push_back(RenderTarget::ColorFormat::RGBA16F);
    lying[0].current             = lender->instance();
    lying[1]                     = lying[0];
    lying[1].target              = borrower.get();
    lying[1].wanted.width        = 8;
    lying[1].wanted.height       = 6;
    lying[1].wanted.shape        = borrower->shape();
    lying[1].current             = borrower->instance();

    fixture.recorder.beginFrame(FrameToken{ 1 });
    fixture.clearPass(lender.get(), 1U, 0, 1.0F, 0.0F, 0.0F);
    fixture.recorder.endFrame();
    const CompiledFrame& frame = fixture.compiler.compile(fixture.recorder.description(), FrameFacts{ lying });

    vine::vsg::core::FrameTimeline   timeline;
    vine::vsg::core::RetirementQueue queue(3U);
    const VsgExecutor::TargetApplications applied = fixture.executor.applyTargetPlans(frame, lying, timeline, queue);
    EXPECT_EQ(applied.refused, 1U) << "a lender with a borrower does not rebuild because a plan says so";
    EXPECT_EQ(applied.resized + applied.rebuilt + applied.failed, 0U);

    ASSERT_EQ(said.size(), 1U) << "one report, and it went to the host's sink";
    EXPECT_NE(said.front().find("probe-lender"), std::string::npos)
        << "the report has to say WHICH target, got: " << said.front();
}
