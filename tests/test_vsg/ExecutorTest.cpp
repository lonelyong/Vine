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
#include <vine/vsg/core/StateRegistry.hpp>
#include <vine/vsg/core/Streams.hpp>
#include <vine/vsg/core/VariantPool.hpp>
#include <vine/vsg/VsgDynamicState.hpp>

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
using vine::vsg::core::FrameToken;
using vine::vsg::core::Observe;
using vine::vsg::core::PassId;
using vine::vsg::core::Rgba8;
using vine::vsg::core::StateRegistry;
using vine::vsg::core::StreamKey;
using vine::vsg::core::StreamKind;
using vine::vsg::core::TargetFacts;
using vine::vsg::core::TargetShape;
using vine::vsg::core::VariantPool;

namespace
{

constexpr std::uint32_t kSize = 64;

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
        descriptors = BlockDescriptors::create(created.device, *storage);
        if (descriptors == nullptr)
        {
            return false;
        }

        const ContentPipeline::VertexBinding   binding{ 0U, sizeof(float) * 3U, false };
        const ContentPipeline::VertexAttribute attribute{ 0U, 0U, VK_FORMAT_R32G32B32_SFLOAT, 0U };
        ContentPipeline::Settings              settings;
        settings.color_attachments = 1U;
        pipelines = ContentPipeline::create(descriptors->layout(),
                                           std::span<const ContentPipeline::VertexBinding>(&binding, 1U),
                                           std::span<const ContentPipeline::VertexAttribute>(&attribute, 1U),
                                           triangleShaders(), settings);
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
        draw.blocks = descriptors->bind(pipelines->layout(), BlockDescriptors::Offsets{ view.offset, block.offset,
                                                                                       material.offset });
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
    bool render(const CompiledFrame& frame, std::span<const PassContent> content = {})
    {
        auto command_graph = ::vsg::CommandGraph::create(created.device, created.queue_family);
        executor.record(frame, command_graph, content);

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
