/**
 * @brief The pass' SAMPLED INPUTS: the plan carries them, and the content layer binds their colour textures
 * (see `.ai/design/vsg-reimplementation.md` §11.16n, milestone M3d-3b-8).
 *
 * Real device, no window: SKIPped when no device can be created.
 *
 * What this case is for. "A pass declares what it reads" is the whole input story of the SDK, and everything
 * between the declaration and the shader is invisible in every counter: the plan could drop the inputs, the
 * binding layer could bind nothing, and a pass that samples nothing still draws its geometry in its own
 * colours. So the frame here is built so that ONLY the sampled picture can explain the pixels:
 *
 *   * pass 1 clears the SOURCE target to red and draws nothing;
 *   * pass 2 draws into the DESTINATION - cleared to blue - a triangle whose fragment stage outputs
 *     `texture(input 0's colour attachment, 0.5)`.
 *
 * The centre of the destination is therefore red if the input really reached the shader, and blue (the plan's
 * clear) if it did not; the corner stays blue either way, which is what makes the pair of assertions a
 * picture rather than "something was drawn".
 *
 * The second half is the plan/world agreement, which is the one thing that can go wrong loudly: a caller that
 * offers a different number of images than the plan declares gets a REFUSAL (the whole pass, not a silent
 * approximation), with the message naming which entry moved.
 */

#include <gtest/gtest.h>

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include <vsg/app/CommandGraph.h>
#include <vsg/app/Viewer.h>
#include <vsg/state/ImageView.h>

#include <vine/Buffer.hpp>
#include <vine/graphics/Geometry.hpp>
#include <vine/graphics/Material.hpp>
#include <vine/graphics/BuiltinShaders.hpp>
#include <vine/graphics/RenderCommand.hpp>
#include <vine/graphics/RenderTarget.hpp>
#include <vine/graphics/ShaderProgram.hpp>

#include <vine/vsg/api/BlockDescriptors.hpp>
#include <vine/vsg/api/BlockStorage.hpp>
#include <vine/vsg/api/ContentDraw.hpp>
#include <vine/vsg/api/ContentFacts.hpp>
#include <vine/vsg/api/ContentPass.hpp>
#include <vine/vsg/api/ContentPipeline.hpp>
#include <vine/vsg/api/ContentSources.hpp>
#include <vine/vsg/api/Device.hpp>
#include <vine/vsg/api/GeometryFacts.hpp>
#include <vine/vsg/api/OffscreenTarget.hpp>
#include <vine/vsg/api/StreamUploads.hpp>
#include <vine/vsg/api/VsgExecutor.hpp>
#include <vine/vsg/core/FrameCompiler.hpp>
#include <vine/vsg/core/FrameRecorder.hpp>
#include <vine/vsg/core/StateRegistry.hpp>
#include <vine/vsg/core/VariantPool.hpp>
#include <vine/vsg/VsgDynamicState.hpp>

using vine::graphics::Geometry;
using vine::graphics::Material;
using vine::graphics::RenderCommand;
using vine::graphics::RenderTarget;
using vine::graphics::ShaderProgram;
using vine::graphics::ShaderStage;
using vine::graphics::ShaderStageType;
using vine::vsg::BlockDescriptors;
using vine::vsg::BlockStorage;
using vine::vsg::buildGeometryFacts;
using vine::vsg::buildMaterialFacts;
using vine::vsg::buildProgramFacts;
using vine::vsg::buildScreenProgramFacts;
using vine::vsg::ContentDraw;
using vine::vsg::ContentFacts;
using vine::vsg::ContentPass;
using vine::vsg::ContentPipeline;
using vine::vsg::FactMiss;
using vine::vsg::GeometryFacts;
using vine::vsg::InputImages;
using vine::vsg::MaterialFacts;
using vine::vsg::OffscreenTarget;
using vine::vsg::PassContent;
using vine::vsg::ProgramFacts;
using vine::vsg::StreamUploads;
using vine::vsg::VsgExecutor;
using vine::vsg::core::ClearPolicy;
using vine::vsg::core::CompiledFrame;
using vine::vsg::core::Diagnostics;
using vine::vsg::core::DrawKind;
using vine::vsg::core::FrameArena;
using vine::vsg::core::FrameCompiler;
using vine::vsg::core::FrameFacts;
using vine::vsg::core::FrameRecorder;
using vine::vsg::core::FrameToken;
using vine::vsg::core::Observe;
using vine::vsg::core::Rgba8;
using vine::vsg::core::StateRegistry;
using vine::vsg::core::TargetFacts;
using vine::vsg::core::TargetShape;
using vine::vsg::core::VariantPool;

namespace
{

constexpr std::uint32_t kSize = 64;

/// @brief The source's clear colour: what the sampled picture has to be.
constexpr float kRed[4]{ 0.75F, 0.0F, 0.0F, 1.0F };

/// @brief The destination's clear colour: what "the input did not reach the shader" looks like.
constexpr float kBlue[4]{ 0.0F, 0.0F, 0.75F, 1.0F };

/// @brief The program: a triangle whose fragments are the SOURCE's colour at its centre.
///
/// The fragment stage is the point of the case: it reads the pass' first declared input, through the sampled
/// set's binding 0 (see ContentPipeline's ABI), so a pass whose input never got bound cannot produce red.
void addSamplingStages(ShaderProgram& program)
{
    ShaderStage vertex;
    vertex.type   = ShaderStageType::Vertex;
    vertex.source = vine::String(reinterpret_cast<const char8_t*>(
        "layout(location = 0) in vec3 position;\n"
        "void main() { gl_Position = vec4(position.xy, 0.5, 1.0); }\n"));
    ShaderStage fragment;
    fragment.type   = ShaderStageType::Fragment;
    fragment.source = vine::String(reinterpret_cast<const char8_t*>(
        "layout(set = 1, binding = 0) uniform sampler2D sourceColor;\n"
        "layout(location = 0) out vec4 outColor;\n"
        "void main() { outColor = texture(sourceColor, vec2(0.5, 0.5)); }\n"));
    program.addStage(vertex);
    program.addStage(fragment);
}

/// @brief The scene's geometry: a triangle with positions and indices, at revision 1.
struct Triangle
{
    vine::intrusive_ptr<const vine::Buffer<float>> positions = vine::intrusive_ptr<const vine::Buffer<float>>(
        new vine::Buffer<float>(std::vector<float>{ -0.6F, -0.6F, 0.0F, 0.6F, -0.6F, 0.0F, 0.0F, 0.6F, 0.0F }));
    vine::intrusive_ptr<const vine::Buffer<std::uint32_t>> indices =
        vine::intrusive_ptr<const vine::Buffer<std::uint32_t>>(
            new vine::Buffer<std::uint32_t>(std::vector<std::uint32_t>{ 0U, 1U, 2U }));
};

bool isRed(const Rgba8& pixel)
{
    return pixel.r > 150 && pixel.g < 40 && pixel.b < 40;
}

bool isBlue(const Rgba8& pixel)
{
    return pixel.b > 150 && pixel.r < 40 && pixel.g < 40;
}

/// @brief The rectangle the full-screen draw covers: a picture-in-picture quarter of the destination.
constexpr vine::graphics::Viewport kPictureInPicture{ 8, 8, 16, 16 };

}  // namespace

TEST(SampledInputTest, APassInputReachesTheShaderAndItsPixelsProveIt)
{
    // 1. A device, the two targets, and the content stack the layer drives.
    const vine::vsg::DeviceResult created = vine::vsg::createDevice();
    if (!created.ok)
    {
        GTEST_SKIP() << "no Vulkan device available (lavapipe + X11 are needed): " << created.error.as_std_str();
    }

    OffscreenTarget::Layout source_layout;
    source_layout.width  = kSize;
    source_layout.height = kSize;
    for (std::size_t index = 0; index < 4U; ++index)
    {
        source_layout.clear_color[index] = kRed[index];
    }
    std::unique_ptr<OffscreenTarget> source = OffscreenTarget::create(created.device, source_layout);
    ASSERT_NE(source, nullptr);

    OffscreenTarget::Layout destination_layout;
    destination_layout.width  = kSize;
    destination_layout.height = kSize;
    for (std::size_t index = 0; index < 4U; ++index)
    {
        destination_layout.clear_color[index] = kBlue[index];
    }
    std::unique_ptr<OffscreenTarget> destination = OffscreenTarget::create(created.device, destination_layout);
    ASSERT_NE(destination, nullptr);

    std::unique_ptr<BlockStorage>     storage     = BlockStorage::create(created.device, BlockStorage::Layout{});
    ASSERT_NE(storage, nullptr);

    const ContentPipeline::VertexBinding   binding{ 0U, sizeof(float) * 3U, false };
    const ContentPipeline::VertexAttribute attribute{ 0U, 0U, VK_FORMAT_R32G32B32_SFLOAT, 0U };
    ContentPipeline::Settings              settings;
    settings.color_attachments = 1U;

    // The SDK objects are heap-owned: the plan names them by an intrusive_ptr, so they must outlive the frame.
    const vine::intrusive_ptr<ShaderProgram> program(new ShaderProgram());
    addSamplingStages(*program);

    ProgramFacts program_facts;
    ASSERT_EQ(buildProgramFacts(*program, program_facts), FactMiss::None);

    std::unique_ptr<ContentPipeline> pipelines = ContentPipeline::create(
        program_facts.abi, std::span<const ContentPipeline::VertexBinding>(&binding, 1U),
        std::span<const ContentPipeline::VertexAttribute>(&attribute, 1U), program_facts.shaders, settings);
    ASSERT_NE(pipelines, nullptr);

    VariantPool   pool;
    StateRegistry registry(pool);
    StreamUploads uploads;
    ContentDraw   draws(*pipelines, pool, vine::vsg::detail::fetchDynamicStateEntryPoints(created.device->vk(),
                                                                                         created.instance->vk()));

    // 2. The three tables, built from the SDK objects the host authored.
    Triangle                            triangle;
    const vine::intrusive_ptr<Geometry> geometry(new Geometry());
    geometry->setPositions(triangle.positions);
    geometry->setIndices(triangle.indices);
    geometry->setRevision(1U);

    GeometryFacts               geometry_facts;
    std::vector<vine::vsg::ChannelFacts> channel_storage;
    ASSERT_EQ(buildGeometryFacts(*geometry, geometry_facts, channel_storage), FactMiss::None);

    const vine::intrusive_ptr<Material> material(new Material());
    material->setDiffuse(vine::Colorf(0.2F, 0.3F, 0.4F, 1.0F));
    MaterialFacts          material_facts;
    std::vector<std::byte> material_storage;
    ASSERT_EQ(buildMaterialFacts(material.get(), 1U, material_facts, material_storage), FactMiss::None);

    const ProgramFacts  programs[]   = { program_facts };
    const GeometryFacts geometries[] = { geometry_facts };
    const MaterialFacts materials[]  = { material_facts };
    ContentFacts        facts;
    facts.programs   = programs;
    facts.geometries = geometries;
    facts.materials  = materials;

    // 3. The frame: pass 1 clears the SOURCE, pass 2 draws into the DESTINATION and declares the source as its
    //    input. The producer is announced SECOND, so the plan's order is the graph's answer, not the call order.
    FrameArena    arena{ 64 * 1024 };
    Diagnostics   diagnostics;
    std::vector<vine::String> messages;
    diagnostics.setSink([&messages](const vine::graphics::RenderDiagnostic& diagnostic) {
        messages.push_back(diagnostic.message);
    });
    Observe       observe;
    FrameRecorder recorder{ arena, diagnostics, observe };
    FrameCompiler compiler{ arena, diagnostics, observe };

    // The identity the frame names a target by is the SDK handle the engine announces it with (the executor
    // resolves that identity to the off-screen object), so the declarations, the facts and the executor all
    // speak about the same target.
    const vine::intrusive_ptr<RenderTarget> source_handle(new RenderTarget());
    const vine::intrusive_ptr<RenderTarget> destination_handle(new RenderTarget());

    TargetFacts source_facts;
    source_facts.target        = source_handle.get();
    source_facts.wanted.width  = static_cast<int>(kSize);
    source_facts.wanted.height = static_cast<int>(kSize);
    source_facts.wanted.shape.color_formats.push_back(RenderTarget::ColorFormat::RGBA8);
    // The facts come from the target itself (`instance()`): nothing has been written into it yet, so its
    // first pass is its bootstrap one and must clear (see OffscreenTarget::instance).
    source_facts.current       = source->instance();

    TargetFacts destination_facts;
    destination_facts.target        = destination_handle.get();
    destination_facts.wanted.width  = static_cast<int>(kSize);
    destination_facts.wanted.height = static_cast<int>(kSize);
    destination_facts.wanted.shape.color_formats.push_back(RenderTarget::ColorFormat::RGBA8);
    destination_facts.current       = destination->instance();
    const std::vector<TargetFacts> target_table{ source_facts, destination_facts };

    ClearPolicy red_clear;
    red_clear.color          = true;
    red_clear.color_value[0] = kRed[0];
    red_clear.color_value[1] = kRed[1];
    red_clear.color_value[2] = kRed[2];
    red_clear.color_value[3] = 1.0F;

    ClearPolicy blue_clear;
    blue_clear.color          = true;
    blue_clear.color_value[0] = kBlue[0];
    blue_clear.color_value[1] = kBlue[1];
    blue_clear.color_value[2] = kBlue[2];
    blue_clear.color_value[3] = 1.0F;

    RenderCommand command;
    command.geometry = geometry;
    command.material = material;
    command.program  = program;
    // Two drawing calls: the sampled-input set is the PASS', so the second one must not re-issue its bind.
    const std::vector<RenderCommand> commands{ command, command };

    std::vector<RenderTarget*> inputs{ source_handle.get() };

    recorder.beginFrame(FrameToken{ 1 });
    recorder.beginPass(2U);
    recorder.setRenderTarget(destination_handle.get());
    recorder.setClearPolicy(blue_clear);
    recorder.setPassInputs(inputs);
    recorder.render(commands, nullptr);
    recorder.endPass();

    recorder.beginPass(1U);
    recorder.setRenderTarget(source_handle.get());
    recorder.setClearPolicy(red_clear);  // the clear IS the source picture: no content required
    recorder.endPass();
    recorder.endFrame();

    const CompiledFrame& frame = compiler.compile(recorder.description(), FrameFacts{ target_table });
    ASSERT_EQ(frame.passes.size(), 2U);
    ASSERT_EQ(frame.passes[0].pass, 1U) << "the producer runs first - the sampled edge says so";
    const vine::vsg::core::CompiledPass& consumer = frame.passes[1];
    ASSERT_EQ(consumer.inputs.size(), 1U) << "the plan carries what the pass declared";
    EXPECT_EQ(consumer.inputs[0].target, static_cast<const void*>(source_handle.get()));
    EXPECT_EQ(consumer.inputs[0].color_attachments, 1U);

    // 4. The content layer records pass 2, offered the images the SOURCE target has.
    storage->beginFrame();
    const ContentPass::Scope::Entry halves[]{
        ContentPass::Scope::Entry{ vine::vsg::core::DrawKind::Content, program.get(), program_facts.revision,
                                   geometry_facts.layout, pipelines.get(), &draws } };
    ContentPass::Scope scope;
    scope.entries  = halves;
    scope.registry = &registry;
    scope.storage  = storage.get();
    scope.uploads  = &uploads;
    ContentPass content(scope, diagnostics);

    const ::vsg::ref_ptr<::vsg::ImageView> source_colors[] = { source->colorView(0) };
    ASSERT_NE(source_colors[0], nullptr) << "the target answers with its own colour attachment view";
    const InputImages images[] = { InputImages{ std::span<const ::vsg::ref_ptr<::vsg::ImageView>>(source_colors, 1U) } };

    const std::vector<std::byte> view_block(288U, std::byte{ 0 });
    ::vsg::ref_ptr<::vsg::Node>  content_node;
    ASSERT_TRUE(content.record(consumer, facts, destination->shape().compatibility(), images, view_block,
                               content_node));
    ASSERT_TRUE(messages.empty()) << "nothing may be refused: the images match the plan";
    EXPECT_EQ(draws.draws(), 2U);
    EXPECT_EQ(draws.input_binds(), 1U)
        << "the pass' inputs are a property of the PASS: one set, bound once for both drawing calls";

    // 5. The executor places it, and the pixels say whether the shader read the input.
    VsgExecutor executor(diagnostics);
    executor.addTarget(source_handle.get(), source.get());
    executor.addTarget(destination_handle.get(), destination.get());

    auto              command_graph = ::vsg::CommandGraph::create(created.device, created.queue_family);
    const PassContent packet{ consumer.pass, content_node };
    ASSERT_TRUE(executor.record(frame, command_graph, std::span<const PassContent>(&packet, 1U)));

    ::vsg::ref_ptr<::vsg::Viewer> viewer = ::vsg::Viewer::create();
    ASSERT_NE(viewer, nullptr);
    viewer->assignRecordAndSubmitTaskAndPresentation(::vsg::CommandGraphs{ command_graph });
    ASSERT_TRUE(viewer->compile());
    viewer->advanceToNextFrame();
    viewer->handleEvents();
    viewer->recordAndSubmit();
    viewer->deviceWaitIdle();

    const Rgba8 centre = destination->probe().pixel(static_cast<int>(kSize) / 2, static_cast<int>(kSize) / 2);
    EXPECT_TRUE(isRed(centre)) << "the fragment stage sampled the pass' input, so the triangle is the SOURCE's "
                                  "colour - got ("
                               << static_cast<int>(centre.r) << ", " << static_cast<int>(centre.g) << ", "
                               << static_cast<int>(centre.b) << ")";
    const Rgba8 corner = destination->probe().pixel(1, 1);
    EXPECT_TRUE(isBlue(corner)) << "and outside the triangle the plan's clear still is - got ("
                                << static_cast<int>(corner.r) << ", " << static_cast<int>(corner.g) << ", "
                                << static_cast<int>(corner.b) << ")";

    // 6. The plan/world agreement: a caller that offers images the plan did not describe is REFUSED, and the
    //    message names the entry that moved - the pass' inputs are one fact, so the whole pass is not drawn.
    storage->beginFrame();
    messages.clear();
    ::vsg::ref_ptr<::vsg::Node> refused_node;
    EXPECT_FALSE(content.record(consumer, facts, destination->shape().compatibility(),
                                std::span<const InputImages>{}, view_block, refused_node));
    ASSERT_EQ(messages.size(), 1U);
    EXPECT_NE(messages[0].as_std_str().find("declares 1 input(s) and the caller offered 0"), std::string::npos)
        << messages[0].as_std_str();

    storage->beginFrame();
    messages.clear();
    const InputImages empty_entry[] = { InputImages{} };
    EXPECT_FALSE(content.record(consumer, facts, destination->shape().compatibility(),
                                std::span<const InputImages>(empty_entry, 1U), view_block, refused_node));
    ASSERT_EQ(messages.size(), 1U);
    EXPECT_NE(messages[0].as_std_str().find("input 0 offers 0 colour texture(s) where the plan says 1"),
              std::string::npos)
        << messages[0].as_std_str();
}

TEST(SampledInputTest, APassInputReachesAFullScreenProgramThroughThePlan)
{
    // The sampled-input line's OTHER consumer: a full-screen drawing call. Same pass, same declared input, same
    // images - and the same "one set per pass" rule - but the sampler lives at set 0 (the full-screen ABI) and
    // the draw is three generated vertices inside the announced picture-in-picture rectangle. The program is the
    // SDK'S screen copy, so a set bound at the wrong index, an input that never arrived or a rectangle that was
    // ignored each produce a different picture.
    const vine::vsg::DeviceResult created = vine::vsg::createDevice();
    if (!created.ok)
    {
        GTEST_SKIP() << "no Vulkan device available (lavapipe + X11 are needed): " << created.error.as_std_str();
    }

    OffscreenTarget::Layout source_layout;
    source_layout.width  = kSize;
    source_layout.height = kSize;
    for (std::size_t index = 0; index < 4U; ++index)
    {
        source_layout.clear_color[index] = kRed[index];
    }
    std::unique_ptr<OffscreenTarget> source = OffscreenTarget::create(created.device, source_layout);
    ASSERT_NE(source, nullptr);

    OffscreenTarget::TargetLayout destination_layout;
    destination_layout.width         = kSize;
    destination_layout.height        = kSize;
    destination_layout.color_formats = { RenderTarget::ColorFormat::RGBA8 };
    // A depth attachment, because a window has one: it is what makes the full-screen call's depth policy
    // visible. The engine's canonical triangle sits at clip z = 0.0, exactly where this pass' depth is cleared
    // to (the reverse-Z far plane), so a draw that inherited the pass' TestAndWrite would be rejected whole and
    // the rectangle would keep the destination's clear colour.
    destination_layout.depth_format         = RenderTarget::DepthFormat::D32F;
    destination_layout.clear.color          = true;
    destination_layout.clear.color_value[0] = kBlue[0];
    destination_layout.clear.color_value[1] = kBlue[1];
    destination_layout.clear.color_value[2] = kBlue[2];
    destination_layout.clear.color_value[3] = 1.0F;
    std::unique_ptr<OffscreenTarget> destination = OffscreenTarget::create(created.device, destination_layout);
    ASSERT_NE(destination, nullptr);
    ASSERT_TRUE(destination->hasDepth()) << "the case needs a depth attachment for its claim to be testable";

    std::unique_ptr<BlockStorage>     storage     = BlockStorage::create(created.device, BlockStorage::Layout{});
    ASSERT_NE(storage, nullptr);

    // The screen program (the SDK's own copy) and the layer that compiles it: no blocks, no vertex streams.
    const vine::intrusive_ptr<ShaderProgram> screen_program(vine::graphics::screenCopyProgram(0));
    ASSERT_NE(screen_program, nullptr);
    ProgramFacts screen_facts;
    ASSERT_EQ(buildScreenProgramFacts(*screen_program, screen_facts), FactMiss::None);

    std::unique_ptr<ContentPipeline> screen_pipelines = ContentPipeline::createScreen(screen_facts.abi, screen_facts.shaders);
    ASSERT_NE(screen_pipelines, nullptr);

    VariantPool   pool;
    StateRegistry registry(pool);
    StreamUploads uploads;  // unused by the screen path, but the scope's shape is the pass'
    ContentDraw   screen_draws(*screen_pipelines, pool,
                               vine::vsg::detail::fetchDynamicStateEntryPoints(created.device->vk(),
                                                                               created.instance->vk()));

    const ProgramFacts  programs[] = { screen_facts };
    ContentFacts        facts;
    facts.programs = programs;

    // The frame: pass 1 clears the SOURCE (its clear IS the picture), pass 2 copies it into the DESTINATION
    // through a full-screen call inside a picture-in-picture rectangle.
    FrameArena    arena{ 64 * 1024 };
    Diagnostics   diagnostics;
    std::vector<vine::String> messages;
    diagnostics.setSink([&messages](const vine::graphics::RenderDiagnostic& diagnostic) {
        messages.push_back(diagnostic.message);
    });
    Observe       observe;
    FrameRecorder recorder{ arena, diagnostics, observe };
    FrameCompiler compiler{ arena, diagnostics, observe };

    const vine::intrusive_ptr<RenderTarget> source_handle(new RenderTarget());
    const vine::intrusive_ptr<RenderTarget> destination_handle(new RenderTarget());

    TargetFacts source_facts;
    source_facts.target        = source_handle.get();
    source_facts.wanted.width  = static_cast<int>(kSize);
    source_facts.wanted.height = static_cast<int>(kSize);
    source_facts.wanted.shape.color_formats.push_back(RenderTarget::ColorFormat::RGBA8);
    source_facts.current       = source->instance();

    TargetFacts destination_facts;
    destination_facts.target        = destination_handle.get();
    destination_facts.wanted.width  = static_cast<int>(kSize);
    destination_facts.wanted.height = static_cast<int>(kSize);
    destination_facts.wanted.shape.color_formats.push_back(RenderTarget::ColorFormat::RGBA8);
    destination_facts.current       = destination->instance();
    const std::vector<TargetFacts> target_table{ source_facts, destination_facts };

    ClearPolicy red_clear;
    red_clear.color          = true;
    red_clear.color_value[0] = kRed[0];
    red_clear.color_value[1] = kRed[1];
    red_clear.color_value[2] = kRed[2];
    red_clear.color_value[3] = 1.0F;

    ClearPolicy blue_clear;
    blue_clear.color          = true;
    blue_clear.color_value[0] = kBlue[0];
    blue_clear.color_value[1] = kBlue[1];
    blue_clear.color_value[2] = kBlue[2];
    blue_clear.color_value[3] = 1.0F;

    // The SDK refuses a full-screen pass without a camera at wiring time, so the host's call always has one.
    const vine::intrusive_ptr<vine::graphics::Camera> camera(new vine::graphics::Camera());

    std::vector<RenderTarget*> inputs{ source_handle.get() };

    recorder.beginFrame(FrameToken{ 1 });
    recorder.beginPass(2U);
    recorder.setRenderTarget(destination_handle.get());
    recorder.setClearPolicy(blue_clear);
    recorder.setPassInputs(inputs);
    recorder.setViewport(kPictureInPicture.x, kPictureInPicture.y, kPictureInPicture.width,
                         kPictureInPicture.height);
    recorder.drawScreenProgram(source_handle.get(), screen_program.get(), camera.get());
    recorder.endPass();

    recorder.beginPass(1U);
    recorder.setRenderTarget(source_handle.get());
    recorder.setClearPolicy(red_clear);
    recorder.endPass();
    recorder.endFrame();

    const CompiledFrame& frame = compiler.compile(recorder.description(), FrameFacts{ target_table });
    ASSERT_EQ(frame.passes.size(), 2U);
    ASSERT_EQ(frame.passes[0].pass, 1U) << "the producer runs first - the sampled edge says so";
    const vine::vsg::core::CompiledPass& consumer = frame.passes[1];
    ASSERT_EQ(consumer.draws.size(), 1U);
    ASSERT_EQ(consumer.draws[0].kind, DrawKind::Screen);
    EXPECT_EQ(consumer.draws[0].viewport.width, kPictureInPicture.width) << "the rectangle is the call's own";

    // The content layer records the pass: the entry is the SCREEN half (kind), so the set it binds is the
    // full-screen ABI's (set 0) over the same images the content path would have bound at set 1.
    storage->beginFrame();
    const ContentPass::Scope::Entry halves[]{
        ContentPass::Scope::Entry{ DrawKind::Screen, screen_program.get(), screen_facts.revision, {},
                                   screen_pipelines.get(), &screen_draws } };
    ContentPass::Scope scope;
    scope.entries  = halves;
    scope.registry = &registry;
    scope.storage  = storage.get();
    scope.uploads  = &uploads;
    ContentPass content(scope, diagnostics);

    const ::vsg::ref_ptr<::vsg::ImageView> source_colors[] = { source->colorView(0) };
    ASSERT_NE(source_colors[0], nullptr);
    const InputImages images[] = { InputImages{ std::span<const ::vsg::ref_ptr<::vsg::ImageView>>(source_colors, 1U) } };

    const std::vector<std::byte> view_block(288U, std::byte{ 0 });
    ::vsg::ref_ptr<::vsg::Node>  content_node;
    ASSERT_TRUE(content.record(consumer, facts, destination->shape().compatibility(), images, view_block,
                               content_node));
    ASSERT_TRUE(messages.empty()) << "nothing may be refused: the screen half is there and the images match";
    EXPECT_EQ(screen_draws.screen_draws(), 1U);
    EXPECT_EQ(screen_draws.input_binds(), 1U) << "one sampled set per pass, bound once";

    VsgExecutor executor(diagnostics);
    executor.addTarget(source_handle.get(), source.get());
    executor.addTarget(destination_handle.get(), destination.get());

    auto              command_graph = ::vsg::CommandGraph::create(created.device, created.queue_family);
    const PassContent packet{ consumer.pass, content_node };
    ASSERT_TRUE(executor.record(frame, command_graph, std::span<const PassContent>(&packet, 1U)));

    ::vsg::ref_ptr<::vsg::Viewer> viewer = ::vsg::Viewer::create();
    ASSERT_NE(viewer, nullptr);
    viewer->assignRecordAndSubmitTaskAndPresentation(::vsg::CommandGraphs{ command_graph });
    ASSERT_TRUE(viewer->compile());
    viewer->advanceToNextFrame();
    viewer->handleEvents();
    viewer->recordAndSubmit();
    viewer->deviceWaitIdle();

    const Rgba8 inside = destination->probe().pixel(kPictureInPicture.x + kPictureInPicture.width / 2,
                                                    kPictureInPicture.y + kPictureInPicture.height / 2);
    EXPECT_TRUE(isRed(inside))
        << "the full-screen program sampled the pass' input and copied it inside the rectangle - got ("
        << static_cast<int>(inside.r) << ", " << static_cast<int>(inside.g) << ", " << static_cast<int>(inside.b)
        << ")";
    const Rgba8 outside = destination->probe().pixel(2, 2);
    EXPECT_TRUE(isBlue(outside)) << "and the rest of the target kept its own clear - got ("
                                 << static_cast<int>(outside.r) << ", " << static_cast<int>(outside.g) << ", "
                                 << static_cast<int>(outside.b) << ")";
}

TEST(SampledInputTest, APassSamplesTheDepthAShadowPassWrote)
{
    // THE SHADOW SPINE. A depth-only target is a shadow map, and a later pass samples it: pass 1 draws a
    // triangle into a DEPTH-ONLY target (no colour attachment at all), pass 2 samples that target's depth
    // through the sampled set's binding 0 and writes the sampled value out as a grey level. Three claims, and
    // the pixels are the judge of all three:
    //
    //   * the plan says the input's depth is sampleable, and the content layer binds it (a pass whose input's
    //     depth never got bound cannot produce a grey different from the far plane);
    //   * a sampleable DEPTH-ONLY target leaves its depth in the layout a sampler reads (see
    //     core::depthFinalLayout), so the sampling pass needs no barrier and no transition of its own;
    //   * the depth values are the producer's: inside its triangle the depth it wrote, everywhere else the
    //     cleared reverse-Z far plane (0.0) - which is what makes this a depth READ and not a texture bind.
    const vine::vsg::DeviceResult created = vine::vsg::createDevice();
    if (!created.ok)
    {
        GTEST_SKIP() << "no Vulkan device available (lavapipe + X11 are needed): " << created.error.as_std_str();
    }

    // The shadow map: a DEPTH-ONLY target (the shape a shadow pass renders into), sampleable, so a later pass
    // can read it. No colour formats at all.
    OffscreenTarget::TargetLayout shadow_layout;
    shadow_layout.width            = kSize;
    shadow_layout.height           = kSize;
    shadow_layout.color_formats.clear();  // DEPTH ONLY: the shape is the shadow map's, not a colour target's
    shadow_layout.depth_format     = RenderTarget::DepthFormat::D32;
    shadow_layout.depth_sampleable = true;
    shadow_layout.clear.depth      = true;  // the reverse-Z far plane (0.0), the value "nothing was written"
    std::unique_ptr<OffscreenTarget> shadow_map = OffscreenTarget::create(created.device, shadow_layout);
    ASSERT_NE(shadow_map, nullptr);
    ASSERT_TRUE(shadow_map->hasDepth());
    EXPECT_NE(shadow_map->depthView(), nullptr) << "a shader samples the DEPTH view";

    OffscreenTarget::Layout receiver_layout;
    receiver_layout.width  = kSize;
    receiver_layout.height = kSize;
    for (std::size_t index = 0; index < 4U; ++index) {
        receiver_layout.clear_color[index] = kBlue[index];
    }
    // The recorder is told about it explicitly: a pass' clear is a HOST decision (the plan only decides when
    // the bootstrap rule overrides it), so the fixture hands the same policy to both the target and the frame.
    ClearPolicy receiver_clear;
    receiver_clear.color = true;
    for (std::size_t index = 0; index < 4U; ++index) {
        receiver_clear.color_value[index] = kBlue[index];
    }
    std::unique_ptr<OffscreenTarget> receiver = OffscreenTarget::create(created.device, receiver_layout);
    ASSERT_NE(receiver, nullptr);

    // The identity the plan names the shadow map by: the SDK handle the host announced it with (the same
    // pairing every fixture makes between a target object and the handle the engine knows it as).
    const vine::intrusive_ptr<RenderTarget> shadow_handle(new RenderTarget());
    const vine::intrusive_ptr<RenderTarget> receiver_handle(new RenderTarget());

    std::unique_ptr<BlockStorage>     storage     = BlockStorage::create(created.device, BlockStorage::Layout{});
    ASSERT_NE(storage, nullptr);

    // The producer: a triangle written into the depth map, with NO fragment output (the pass has no colour
    // attachment to write to - a depth-only pipeline, which is what settings.color_attachments = 0 declares).
    const vine::intrusive_ptr<ShaderProgram> shadow_program(new ShaderProgram());
    {
        ShaderStage vertex;
        vertex.type   = ShaderStageType::Vertex;
        vertex.source = vine::String(reinterpret_cast<const char8_t*>(
            "layout(location = 0) in vec3 position;\n"
            "void main() { gl_Position = vec4(position.xy, 0.5, 1.0); }\n"));
        ShaderStage fragment;
        fragment.type   = ShaderStageType::Fragment;
        fragment.source = vine::String(reinterpret_cast<const char8_t*>(
            "void main() { }\n"));
        shadow_program->addStage(vertex);
        shadow_program->addStage(fragment);
    }
    ProgramFacts shadow_facts;
    ASSERT_EQ(buildProgramFacts(*shadow_program, shadow_facts), FactMiss::None);

    // The consumer: a triangle that samples the depth map and paints the sampled value. Its uv comes from the
    // vertex position (the same NDC the geometry is authored in), so "which texel it reads" is arithmetic.
    const vine::intrusive_ptr<ShaderProgram> sampling_program(new ShaderProgram());
    {
        ShaderStage vertex;
        vertex.type   = ShaderStageType::Vertex;
        vertex.source = vine::String(reinterpret_cast<const char8_t*>(
            "layout(location = 0) in vec3 position;\n"
            "layout(location = 0) out vec2 uv;\n"
            "void main() { gl_Position = vec4(position.xy, 0.5, 1.0); uv = position.xy * 0.5 + 0.5; }\n"));
        ShaderStage fragment;
        fragment.type   = ShaderStageType::Fragment;
        fragment.source = vine::String(reinterpret_cast<const char8_t*>(
            "layout(set = 1, binding = 0) uniform sampler2D shadowDepth;\n"
            "layout(location = 0) in vec2 uv;\n"
            "layout(location = 0) out vec4 outColor;\n"
            "void main() { outColor = vec4(vec3(texture(shadowDepth, uv).r), 1.0); }\n"));
        sampling_program->addStage(vertex);
        sampling_program->addStage(fragment);
    }
    ProgramFacts sampling_facts;
    ASSERT_EQ(buildProgramFacts(*sampling_program, sampling_facts), FactMiss::None);

    const ContentPipeline::VertexBinding   binding{ 0U, sizeof(float) * 3U, false };
    const ContentPipeline::VertexAttribute attribute{ 0U, 0U, VK_FORMAT_R32G32B32_SFLOAT, 0U };

    ContentPipeline::Settings shadow_settings;
    shadow_settings.color_attachments = 0U;  // a depth-only pass: no colour attachment to blend into
    std::unique_ptr<ContentPipeline> shadow_pipelines =
        ContentPipeline::create(shadow_facts.abi, std::span<const ContentPipeline::VertexBinding>(&binding, 1U),
                                std::span<const ContentPipeline::VertexAttribute>(&attribute, 1U),
                                shadow_facts.shaders, shadow_settings);
    ASSERT_NE(shadow_pipelines, nullptr);

    ContentPipeline::Settings sampling_settings;
    sampling_settings.color_attachments = 1U;
    std::unique_ptr<ContentPipeline> sampling_pipelines =
        ContentPipeline::create(sampling_facts.abi, std::span<const ContentPipeline::VertexBinding>(&binding, 1U),
                                std::span<const ContentPipeline::VertexAttribute>(&attribute, 1U),
                                sampling_facts.shaders, sampling_settings);
    ASSERT_NE(sampling_pipelines, nullptr);

    // The producer's geometry covers the LEFT HALF of the target; the consumer's covers the middle, so one of
    // its fragments samples a texel the producer wrote and another samples one it did not.
    const auto make_geometry = [](std::vector<float> positions) {
        auto indices = vine::intrusive_ptr<const vine::Buffer<std::uint32_t>>(
            new vine::Buffer<std::uint32_t>(std::vector<std::uint32_t>{ 0U, 1U, 2U }));
        auto geometry = vine::intrusive_ptr<Geometry>(new Geometry());
        geometry->setPositions(vine::intrusive_ptr<const vine::Buffer<float>>(
            new vine::Buffer<float>(std::move(positions))));
        geometry->setIndices(indices);
        geometry->setRevision(1U);
        return geometry;
    };
    const vine::intrusive_ptr<Geometry> shadow_geometry =
        make_geometry({ -1.0F, -1.0F, 0.0F, 0.0F, -1.0F, 0.0F, -0.5F, 1.0F, 0.0F });
    const vine::intrusive_ptr<Geometry> sampling_geometry =
        make_geometry({ -1.0F, -1.0F, 0.0F, 1.0F, -1.0F, 0.0F, 0.0F, 1.0F, 0.0F });

    GeometryFacts                        shadow_geometry_facts;
    GeometryFacts                        sampling_geometry_facts;
    std::vector<vine::vsg::ChannelFacts> shadow_channels;
    std::vector<vine::vsg::ChannelFacts> sampling_channels;
    ASSERT_EQ(buildGeometryFacts(*shadow_geometry, shadow_geometry_facts, shadow_channels), FactMiss::None);
    ASSERT_EQ(buildGeometryFacts(*sampling_geometry, sampling_geometry_facts, sampling_channels), FactMiss::None);

    const vine::intrusive_ptr<Material> material(new Material());
    material->setDiffuse(vine::Colorf(0.2F, 0.3F, 0.4F, 1.0F));
    MaterialFacts          material_facts;
    std::vector<std::byte> material_storage;
    ASSERT_EQ(buildMaterialFacts(material.get(), 1U, material_facts, material_storage), FactMiss::None);

    const ProgramFacts   programs[]   = { shadow_facts, sampling_facts };
    const GeometryFacts  geometries[] = { shadow_geometry_facts, sampling_geometry_facts };
    const MaterialFacts  materials[]  = { material_facts };
    ContentFacts         facts;
    facts.programs   = programs;
    facts.geometries = geometries;
    facts.materials  = materials;

    VariantPool   pool;
    StreamUploads uploads;
    const auto    entry_points =
        vine::vsg::detail::fetchDynamicStateEntryPoints(created.device->vk(), created.instance->vk());
    ContentDraw   shadow_draws(*shadow_pipelines, pool, entry_points);
    ContentDraw   sampling_draws(*sampling_pipelines, pool, entry_points);

    // One frame, two passes: the shadow pass into the depth-only target, then the sampling pass, which
    // declares the depth map as its input.
    FrameArena    arena{ 64 * 1024 };
    Diagnostics   diagnostics;
    Observe       observe;
    FrameRecorder recorder{ arena, diagnostics, observe };
    FrameCompiler compiler{ arena, diagnostics, observe };

    TargetFacts shadow_facts_table;
    shadow_facts_table.target        = shadow_handle.get();
    shadow_facts_table.wanted.width  = static_cast<int>(kSize);
    shadow_facts_table.wanted.height = static_cast<int>(kSize);
    shadow_facts_table.wanted.shape  = shadow_map->shape();
    shadow_facts_table.current       = shadow_map->instance();
    // The host asked for a sampleable depth, and this pass is the one that writes it: that is the fact the
    // plan's input table turns into "a shader may sample it".
    shadow_facts_table.depth.has_depth   = true;
    shadow_facts_table.depth.promotion   = true;
    shadow_facts_table.depth.borrowed    = false;
    shadow_facts_table.depth.source      = nullptr;
    shadow_facts_table.depth.any_pass_preserves_depth = false;

    TargetFacts receiver_facts;
    receiver_facts.target        = receiver_handle.get();
    receiver_facts.wanted.width  = static_cast<int>(kSize);
    receiver_facts.wanted.height = static_cast<int>(kSize);
    receiver_facts.wanted.shape  = receiver->shape();
    receiver_facts.current       = receiver->instance();
    const std::vector<TargetFacts> target_table{ shadow_facts_table, receiver_facts };

    RenderCommand shadow_command;
    shadow_command.geometry = shadow_geometry;
    shadow_command.material = material;
    shadow_command.program  = shadow_program;
    RenderCommand sampling_command;
    sampling_command.geometry = sampling_geometry;
    sampling_command.material = material;
    sampling_command.program  = sampling_program;

    const RenderTarget* shadow_inputs[]{ shadow_handle.get() };

    recorder.beginFrame(FrameToken{ 1 });
    recorder.beginPass(1U);
    recorder.setRenderTarget(shadow_handle.get());
    recorder.setClearPolicy(shadow_layout.clear);
    recorder.render(std::vector<RenderCommand>{ shadow_command }, nullptr);
    recorder.endPass();
    recorder.beginPass(2U);
    recorder.setRenderTarget(receiver_handle.get());
    recorder.setClearPolicy(receiver_clear);
    recorder.setPassInputs(std::span<const RenderTarget* const>(shadow_inputs, 1U));
    recorder.render(std::vector<RenderCommand>{ sampling_command }, nullptr);
    recorder.endPass();
    recorder.endFrame();

    const CompiledFrame& frame = compiler.compile(recorder.description(), FrameFacts{ target_table });
    ASSERT_EQ(frame.passes.size(), 2U);
    ASSERT_EQ(frame.passes[1].inputs.size(), 1U);
    EXPECT_EQ(frame.passes[1].inputs[0].target, static_cast<const void*>(shadow_handle.get()));
    EXPECT_EQ(frame.passes[1].inputs[0].color_attachments, 0U) << "a depth-only target offers no colour texture";
    EXPECT_TRUE(frame.passes[1].inputs[0].depth_sampleable) << "and its depth IS sampleable: that is the input";
    EXPECT_TRUE(frame.passes[0].bootstrap) << "nothing has been written into the shadow map yet";

    // The content layer: one scope per pass (each with its own registry), sharing the pool, the storage and the
    // descriptors - and the consumer's set gets the DEPTH binding (its pass offers no colour texture at all).
    storage->beginFrame();
    StateRegistry shadow_registry(pool);
    StateRegistry sampling_registry(pool);

    const ContentPass::Scope::Entry shadow_halves[]{ ContentPass::Scope::Entry{
        DrawKind::Content, shadow_program.get(), shadow_facts.revision, shadow_geometry_facts.layout,
        shadow_pipelines.get(), &shadow_draws } };
    ContentPass::Scope shadow_scope;
    shadow_scope.entries  = shadow_halves;
    shadow_scope.registry = &shadow_registry;
    shadow_scope.storage  = storage.get();
    shadow_scope.uploads  = &uploads;
    ContentPass shadow_content(shadow_scope, diagnostics);

    const ContentPass::Scope::Entry sampling_halves[]{ ContentPass::Scope::Entry{
        DrawKind::Content, sampling_program.get(), sampling_facts.revision, sampling_geometry_facts.layout,
        sampling_pipelines.get(), &sampling_draws } };
    ContentPass::Scope sampling_scope;
    sampling_scope.entries  = sampling_halves;
    sampling_scope.registry = &sampling_registry;
    sampling_scope.storage  = storage.get();
    sampling_scope.uploads  = &uploads;
    ContentPass sampling_content(sampling_scope, diagnostics);

    const std::vector<std::byte> view_block(288U, std::byte{ 0 });
    ::vsg::ref_ptr<::vsg::Node>  shadow_node;
    ASSERT_TRUE(shadow_content.record(frame.passes[0], facts, shadow_map->shape().compatibility(), {}, view_block,
                                      shadow_node));

    // The caller offers what the plan says the input has: no colour texture, and the depth view.
    const InputImages offered_inputs[] = { InputImages{ {}, shadow_map->depthView() } };
    ::vsg::ref_ptr<::vsg::Node> sampling_node;
    ASSERT_TRUE(sampling_content.record(frame.passes[1], facts, receiver->shape().compatibility(),
                                        offered_inputs, view_block, sampling_node));
    ASSERT_TRUE(diagnostics.clean()) << "nothing may be refused: the offer matches the plan";

    VsgExecutor executor(diagnostics);
    executor.addTarget(shadow_handle.get(), shadow_map.get());
    executor.addTarget(receiver_handle.get(), receiver.get());

    auto command_graph = ::vsg::CommandGraph::create(created.device, created.queue_family);
    const PassContent packets[] = { PassContent{ 1U, shadow_node }, PassContent{ 2U, sampling_node } };
    ASSERT_TRUE(executor.record(frame, command_graph, packets));
    EXPECT_EQ(executor.skipped(), 0U);

    ::vsg::ref_ptr<::vsg::Viewer> viewer = ::vsg::Viewer::create();
    ASSERT_NE(viewer, nullptr);
    viewer->assignRecordAndSubmitTaskAndPresentation(::vsg::CommandGraphs{ command_graph });
    ASSERT_TRUE(viewer->compile());
    viewer->advanceToNextFrame();
    viewer->handleEvents();
    viewer->recordAndSubmit();
    viewer->deviceWaitIdle();

    // What the producer wrote, read straight out of the shadow map's depth: 0.5 inside its triangle (the
    // vertex stage puts every fragment there) and the cleared far plane (0.0) outside it.
    const vine::vsg::core::DepthProbe depth = shadow_map->depthProbe();
    ASSERT_TRUE(depth.valid()) << "a D32 depth target is readable";
    const float inside_depth  = depth.depthAt(16, 32);
    const float outside_depth = depth.depthAt(48, 32);
    EXPECT_NEAR(inside_depth, 0.5F, 0.02F) << "the shadow pass' own depth";
    EXPECT_NEAR(outside_depth, 0.0F, 0.02F) << "the cleared reverse-Z far plane";

    // What the CONSUMER drew from it: its fragment left of centre samples a texel the producer wrote (0.5), so
    // it paints mid grey; right of centre it samples the cleared value (0.0), so it paints black.
    const auto near = [](std::uint8_t byte, double value) {
        return std::abs(static_cast<double>(byte) - 255.0 * value) <= 8.0;
    };
    // Which pixel shows what: the framebuffer's origin is the UPPER LEFT and the viewport transform sends NDC
    // y = -1 to framebuffer row 0 (Vulkan's convention), so the consumer's triangle has its BASE along the top
    // rows and its apex (NDC (0, +1)) at the bottom - and its left half samples uv.x < 0.5, which is where the
    // producer wrote. The probes below are one inside each half and one outside the triangle entirely.
    const Rgba8 lit = receiver->probe().pixel(24, 2);
    EXPECT_TRUE(near(lit.r, 0.5) && near(lit.g, 0.5) && near(lit.b, 0.5))
        << "the sampled depth inside the shadow pass' triangle, got (" << static_cast<int>(lit.r) << ", "
        << static_cast<int>(lit.g) << ", " << static_cast<int>(lit.b) << ")";
    const Rgba8 unlit = receiver->probe().pixel(40, 2);
    EXPECT_TRUE(near(unlit.r, 0.0) && near(unlit.g, 0.0) && near(unlit.b, 0.0))
        << "the cleared depth outside it, got (" << static_cast<int>(unlit.r) << ", " << static_cast<int>(unlit.g)
        << ", " << static_cast<int>(unlit.b) << ")";
    const Rgba8 untouched = receiver->probe().pixel(2, 62);
    EXPECT_TRUE(isBlue(untouched)) << "outside the sampling pass' own geometry the receiver keeps its clear";
}
