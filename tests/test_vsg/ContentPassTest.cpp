/**
 * @brief Recording a pass' content from the three tables (see `.ai/design/vsg-reimplementation.md` §11.17).
 *
 * Real device, no window: SKIPped when no device can be created.
 *
 * What this case is for. Everything above it is a plan, everything below it is GPU work, and this layer is
 * where an identity becomes bytes: the geometry's channels become streams, the program's stages become a
 * pipeline, the material's block bytes become the descriptor's payload. So the evidence is the two things only
 * a real frame can show:
 *
 *   * the PICTURE (a triangle drawn from the tables, inside the pass the plan named, over the plan's clear);
 *   * the COST of the second frame with the same content - one upload and one alias for a geometry that did not
 *     change, and a material HIT rather than a write. "It rendered" would be satisfied by a layer that
 *     re-uploaded everything every frame, which is the failure this backend family kept producing.///
/// The second case is the scope with MORE THAN ONE compiled half: which half a command goes to, that the halves
/// share the pass' registry, and which of the two misses a refusal names. */

#include <gtest/gtest.h>

#include <cstddef>
#include <cstdint>
#include <memory>
#include <vector>

#include <vsg/app/CommandGraph.h>
#include <vsg/app/Viewer.h>

#include <vine/Buffer.hpp>
#include <vine/graphics/Camera.hpp>
#include <vine/graphics/Geometry.hpp>
#include <vine/graphics/Material.hpp>
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
#include <vine/vsg/api/WhiteImage.hpp>
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
using vine::vsg::ChannelFacts;
using vine::vsg::ContentDraw;
using vine::vsg::ContentFacts;
using vine::vsg::ContentPass;
using vine::vsg::ContentPipeline;
using vine::vsg::FactMiss;
using vine::vsg::GeometryFacts;
using vine::vsg::MaterialFacts;
using vine::vsg::OffscreenTarget;
using vine::vsg::PassContent;
using vine::vsg::ProgramFacts;
using vine::vsg::StreamUploads;
using vine::vsg::VsgExecutor;
using vine::vsg::core::ClearPolicy;
using vine::vsg::core::CompiledFrame;
using vine::vsg::core::Diagnostics;
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
using vine::vsg::core::VertexLayoutKey;

namespace
{

constexpr std::uint32_t kSize = 64;

/// @brief The pass' clear colour (the plan's, not the target's).
constexpr float kClear[4]{ 0.25F, 0.5F, 0.75F, 1.0F };

/// @brief A shader pair that draws the triangle solid green (no push constants, so the view block is unused).
void addStages(ShaderProgram& program)
{
    ShaderStage vertex;
    vertex.type   = ShaderStageType::Vertex;
    vertex.source = vine::String(
        reinterpret_cast<const char8_t*>("layout(location = 0) in vec3 position;\n"
                                         "void main() { gl_Position = vec4(position.xy, 0.5, 1.0); }\n"));
    ShaderStage fragment;
    fragment.type   = ShaderStageType::Fragment;
    fragment.source = vine::String(reinterpret_cast<const char8_t*>(
        "layout(location = 0) out vec4 outColor;\nvoid main() { outColor = vec4(0.0, 1.0, 0.0, 1.0); }\n"));
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

/// @brief A shader pair that draws the triangle solid blue (the wider layout's colour, see the multi-layout case).
void addBlueStages(ShaderProgram& program)
{
    ShaderStage vertex;
    vertex.type   = ShaderStageType::Vertex;
    vertex.source = vine::String(
        reinterpret_cast<const char8_t*>("layout(location = 0) in vec3 position;\n"
                                         "void main() { gl_Position = vec4(position.xy, 0.5, 1.0); }\n"));
    ShaderStage fragment;
    fragment.type   = ShaderStageType::Fragment;
    fragment.source = vine::String(reinterpret_cast<const char8_t*>(
        "layout(location = 0) out vec4 outColor;\nvoid main() { outColor = vec4(0.0, 0.0, 1.0, 1.0); }\n"));
    program.addStage(vertex);
    program.addStage(fragment);
}

/// @brief One mesh: its buffers, and the geometry reading them (an empty channel is simply not fed).
struct Mesh
{
    vine::intrusive_ptr<const vine::Buffer<float>>         positions;
    vine::intrusive_ptr<const vine::Buffer<float>>         texcoords;
    vine::intrusive_ptr<const vine::Buffer<float>>         normals;
    vine::intrusive_ptr<const vine::Buffer<std::uint32_t>> indices;
    vine::intrusive_ptr<Geometry>                          geometry;

    Mesh(std::vector<float> position_values, std::vector<float> texcoord_values, std::vector<float> normal_values)
        : positions(new vine::Buffer<float>(position_values))
        , texcoords(new vine::Buffer<float>(texcoord_values))
        , normals(new vine::Buffer<float>(normal_values))
        , indices(new vine::Buffer<std::uint32_t>(std::vector<std::uint32_t>{ 0U, 1U, 2U }))
        , geometry(new Geometry())
    {
        geometry->setPositions(positions);
        geometry->setIndices(indices);
        if (!texcoord_values.empty())
        {
            geometry->setTexcoords2(texcoords);
        }
        if (!normal_values.empty())
        {
            geometry->setNormals(normals);
        }
        geometry->setRevision(1U);
    }
};

/// @brief The `VkFormat` of one channel (a channel's components are 32-bit floats, see `StreamKey`).
std::uint32_t channelFormat(const ChannelFacts& channel)
{
    switch (channel.key.components)
    {
    case 1:
        return VK_FORMAT_R32_SFLOAT;
    case 2:
        return VK_FORMAT_R32G32_SFLOAT;
    case 3:
        return VK_FORMAT_R32G32B32_SFLOAT;
    default:
        return VK_FORMAT_R32G32B32A32_SFLOAT;
    }
}

/// @brief Builds the pipeline layer for one layout, declaring exactly the channels the entry lists.
std::unique_ptr<ContentPipeline> pipelineFor(const GeometryFacts& facts, const ContentPipeline::Shaders& shaders,
                                             const vine::vsg::ProgramAbi& abi)
{
    std::vector<ContentPipeline::VertexBinding>   bindings;
    std::vector<ContentPipeline::VertexAttribute> attributes;
    for (const ChannelFacts& channel : facts.channels)
    {
        const std::uint32_t binding = static_cast<std::uint32_t>(bindings.size());
        bindings.push_back(ContentPipeline::VertexBinding{ binding,
                                                           static_cast<std::uint32_t>(channel.key.components *
                                                                                      sizeof(float)),
                                                           false });
        attributes.push_back(
            ContentPipeline::VertexAttribute{ channel.key.location, binding, channelFormat(channel), 0U });
    }

    ContentPipeline::Settings settings;
    settings.color_attachments = 1U;
    return ContentPipeline::create(abi, bindings, attributes, shaders, settings);
}

bool isGreen(const Rgba8& pixel)
{
    return pixel.g > 200 && pixel.r < 40 && pixel.b < 40;
}

bool isBlue(const Rgba8& pixel)
{
    return pixel.b > 200 && pixel.r < 40 && pixel.g < 40;
}

}  // namespace

TEST(ContentPassTest, TheTablesRecordTheFrameAndASecondFrameReusesWhatDidNotChange)
{
    // 1. A device, a target, and the content stack the layer drives.
    const vine::vsg::DeviceResult created = vine::vsg::createDevice();
    if (!created.ok)
    {
        GTEST_SKIP() << "no Vulkan device available (lavapipe + X11 are needed): " << created.error.as_std_str();
    }

    OffscreenTarget::Layout layout;
    layout.width  = kSize;
    layout.height = kSize;
    std::unique_ptr<OffscreenTarget> target = OffscreenTarget::create(created.device, layout);
    ASSERT_NE(target, nullptr);

    std::unique_ptr<BlockStorage>     storage     = BlockStorage::create(created.device, BlockStorage::Layout{});
    ASSERT_NE(storage, nullptr);
    std::unique_ptr<BlockDescriptors> descriptors = BlockDescriptors::create(created.device, *storage);
    ASSERT_NE(descriptors, nullptr);

    const ContentPipeline::VertexBinding   binding{ 0U, sizeof(float) * 3U, false };
    const ContentPipeline::VertexAttribute attribute{ 0U, 0U, VK_FORMAT_R32G32B32_SFLOAT, 0U };
    ContentPipeline::Settings              settings;
    settings.color_attachments = 1U;

    // The SDK objects are heap-owned: the plan names them by an intrusive_ptr, so they must outlive the frame
    // (and must never be a stack object, which the release would try to delete).
    const vine::intrusive_ptr<ShaderProgram> program(new ShaderProgram());
    addStages(*program);

    ProgramFacts program_facts;
    ASSERT_EQ(buildProgramFacts(*program, program_facts), FactMiss::None);

    std::unique_ptr<ContentPipeline> pipelines = ContentPipeline::create(
        program_facts.abi, std::span<const ContentPipeline::VertexBinding>(&binding, 1U),
        std::span<const ContentPipeline::VertexAttribute>(&attribute, 1U), program_facts.shaders, settings);
    ASSERT_NE(pipelines, nullptr);

    VariantPool       pool;
    StateRegistry     registry(pool);
    StreamUploads     uploads;
    ContentDraw       draws(*pipelines, pool, vine::vsg::detail::fetchDynamicStateEntryPoints(created.device->vk(),
                                                                                             created.instance->vk()));

    // 2. The three tables, built from the SDK objects the host authored.
    Triangle                                   triangle;
    const vine::intrusive_ptr<Geometry>        geometry(new Geometry());
    geometry->setPositions(triangle.positions);
    geometry->setIndices(triangle.indices);
    geometry->setRevision(1U);

    GeometryFacts                        geometry_facts;
    std::vector<vine::vsg::ChannelFacts> channel_storage;
    ASSERT_EQ(buildGeometryFacts(*geometry, geometry_facts, channel_storage), FactMiss::None);

    const vine::intrusive_ptr<Material> material(new Material());
    material->setDiffuse(vine::Colorf(0.2F, 0.3F, 0.4F, 1.0F));
    MaterialFacts          material_facts;
    std::vector<std::byte> material_storage;
    ASSERT_EQ(buildMaterialFacts(material.get(), 1U, material_facts, material_storage), FactMiss::None);

    const ProgramFacts   programs[]   = { program_facts };
    const GeometryFacts  geometries[] = { geometry_facts };
    const MaterialFacts  materials[]  = { material_facts };
    ContentFacts         facts;
    facts.programs   = programs;
    facts.geometries = geometries;
    facts.materials  = materials;

    // 3. One frame: one pass into the target, one render command naming that geometry, program and material.
    FrameArena    arena{ 64 * 1024 };
    Diagnostics   diagnostics;
    Observe       observe;
    FrameRecorder recorder{ arena, diagnostics, observe };
    FrameCompiler compiler{ arena, diagnostics, observe };

    TargetFacts target_facts;
    target_facts.target        = target.get();
    target_facts.wanted.width  = static_cast<int>(kSize);
    target_facts.wanted.height = static_cast<int>(kSize);
    target_facts.wanted.shape.color_formats.push_back(RenderTarget::ColorFormat::RGBA8);
    target_facts.current       = target->instance();
    const std::vector<TargetFacts> target_table{ target_facts };

    ClearPolicy clear;
    clear.color          = true;
    clear.color_value[0] = kClear[0];
    clear.color_value[1] = kClear[1];
    clear.color_value[2] = kClear[2];
    clear.color_value[3] = 1.0F;

    RenderCommand command;
    command.geometry = geometry;
    command.material = material;
    command.program  = program;
    const std::vector<RenderCommand> commands{ command };

    recorder.beginFrame(FrameToken{ 1 });
    recorder.beginPass(1U);
    recorder.setRenderTarget(target.get());
    recorder.setClearPolicy(clear);
    recorder.render(commands, nullptr);
    recorder.endPass();
    recorder.endFrame();

    const CompiledFrame& frame = compiler.compile(recorder.description(), FrameFacts{ target_table });
    ASSERT_EQ(frame.passes.size(), 1U);
    ASSERT_EQ(frame.passes[0].draws.size(), 1U);
    ASSERT_EQ(frame.passes[0].draws[0].commands.size(), 1U);

    // 4. The content layer records that pass from the tables.
    storage->beginFrame();
    const ContentPass::Scope::Entry halves[]{ ContentPass::Scope::Entry{
        vine::vsg::core::DrawKind::Content, program.get(), program_facts.revision, geometry_facts.layout,
        pipelines.get(), &draws } };
    ContentPass::Scope scope;
    scope.entries  = halves;
    scope.registry = &registry;
    scope.storage  = storage.get();
    scope.uploads  = &uploads;
    ContentPass content(scope, diagnostics);

    const std::vector<std::byte> view_block(288U, std::byte{ 0 });
    ::vsg::ref_ptr<::vsg::Node>  content_node;
    ASSERT_TRUE(content.record(frame.passes[0], facts, target->shape().compatibility(), {}, view_block, content_node));
    ASSERT_TRUE(diagnostics.clean()) << "nothing may be refused: every identity is in the tables";

    const std::uint64_t uploads_after_first = uploads.uploads();
    const std::uint64_t material_writes     = storage->materialWrites();
    EXPECT_EQ(uploads_after_first, 2U) << "one vertex channel and one index stream: two uploads";
    EXPECT_EQ(material_writes, 1U);

    // 5. The executor places it, and the picture is the triangle over the plan's clear.
    VsgExecutor executor(diagnostics);
    executor.addTarget(target.get(), target.get());

    auto command_graph = ::vsg::CommandGraph::create(created.device, created.queue_family);
    const PassContent packet{ frame.passes[0].pass, content_node };
    ASSERT_TRUE(executor.record(frame, command_graph, std::span<const PassContent>(&packet, 1U)));

    ::vsg::ref_ptr<::vsg::Viewer> viewer = ::vsg::Viewer::create();
    ASSERT_NE(viewer, nullptr);
    viewer->assignRecordAndSubmitTaskAndPresentation(::vsg::CommandGraphs{ command_graph });
    ASSERT_TRUE(viewer->compile());
    viewer->advanceToNextFrame();
    viewer->handleEvents();
    viewer->recordAndSubmit();
    viewer->deviceWaitIdle();

    const Rgba8 centre = target->probe().pixel(static_cast<int>(kSize) / 2, static_cast<int>(kSize) / 2);
    EXPECT_TRUE(isGreen(centre)) << "the content recorded from the tables must be on screen, got ("
                                 << static_cast<int>(centre.r) << ", " << static_cast<int>(centre.g) << ", "
                                 << static_cast<int>(centre.b) << ")";

    // 6. A second frame with the SAME content costs no upload and no material write: the streams are shared by
    //    identity and the material block is a hit. This is the claim "a steady frame does not re-upload", and
    //    it is the reason the tables carry revisions rather than bytes.
    storage->beginFrame();
    ::vsg::ref_ptr<::vsg::Node> again;
    ASSERT_TRUE(content.record(frame.passes[0], facts, target->shape().compatibility(), {}, view_block, again));
    EXPECT_EQ(uploads.uploads(), uploads_after_first) << "the same geometry must not be uploaded twice";
    EXPECT_EQ(uploads.aliases(), 2U) << "the second frame aliases both streams the first one uploaded";
    EXPECT_EQ(storage->materialWrites(), material_writes) << "the same material must not be written twice";
    EXPECT_EQ(storage->materialHits(), 1U);

    // 7. A table that does not know the geometry refuses THAT command and says why - the pass still records, the
    //    picture simply misses the drawable, and nothing is uploaded for it.
    const std::uint64_t refusals_before = diagnostics.count(vine::graphics::DiagnosticCategory::ContentSkipped);
    ContentFacts        without_geometry;
    without_geometry.programs  = programs;
    without_geometry.materials = materials;

    storage->beginFrame();
    ::vsg::ref_ptr<::vsg::Node> missing;
    EXPECT_FALSE(content.record(frame.passes[0], without_geometry, target->shape().compatibility(), {}, view_block,
                                missing))
        << "a command whose geometry the table cannot answer is not drawn";
    EXPECT_EQ(diagnostics.count(vine::graphics::DiagnosticCategory::ContentSkipped), refusals_before + 1U);
    EXPECT_EQ(uploads.uploads(), uploads_after_first) << "a refused command uploads nothing";
    EXPECT_NE(missing, nullptr);  // the node exists and is empty: the executor still has something to place
}

TEST(ContentPassTest, AMultiLayoutScopeServesEveryHalfItWasBuiltFor)
{
    // A pipeline layer is one program's stages against ONE vertex layout, so a pass that draws meshes of two
    // layouts needs two of them. This case pins what the scope has to get right once there is more than one:
    //
    //   * each command goes to the half built for ITS (program, revision, layout) - the counters say which
    //     drawer recorded what, and the picture says which half drew which mesh (two programs, two colours);
    //   * the halves SHARE the pass' registry, so returning to a half after the other one drew re-issues the
    //     pipeline bind the other half replaced (three binds for three drawing calls, not two);
    //   * a command whose layout no half was built for is REFUSED, and the message names the layout - while a
    //     command whose program no half was built for is refused with a different message, because the fix is
    //     a different one (compile that program, not that layout).
    const vine::vsg::DeviceResult created = vine::vsg::createDevice();
    if (!created.ok)
    {
        GTEST_SKIP() << "no Vulkan device available (lavapipe + X11 are needed): " << created.error.as_std_str();
    }

    OffscreenTarget::Layout target_layout;
    target_layout.width  = kSize;
    target_layout.height = kSize;
    std::unique_ptr<OffscreenTarget> target = OffscreenTarget::create(created.device, target_layout);
    ASSERT_NE(target, nullptr);

    std::unique_ptr<BlockStorage>     storage     = BlockStorage::create(created.device, BlockStorage::Layout{});
    ASSERT_NE(storage, nullptr);
    std::unique_ptr<BlockDescriptors> descriptors = BlockDescriptors::create(created.device, *storage);
    ASSERT_NE(descriptors, nullptr);

    // Three programs: two the halves are built from (green for the narrow layout, blue for the wider one) and
    // one the tables know but no half was compiled from.
    const vine::intrusive_ptr<ShaderProgram> green_program(new ShaderProgram());
    addStages(*green_program);
    const vine::intrusive_ptr<ShaderProgram> blue_program(new ShaderProgram());
    addBlueStages(*blue_program);
    const vine::intrusive_ptr<ShaderProgram> other_program(new ShaderProgram());
    addStages(*other_program);

    ProgramFacts green_facts;
    ProgramFacts blue_facts;
    ProgramFacts other_facts;
    ASSERT_EQ(buildProgramFacts(*green_program, green_facts), FactMiss::None);
    ASSERT_EQ(buildProgramFacts(*blue_program, blue_facts), FactMiss::None);
    ASSERT_EQ(buildProgramFacts(*other_program, other_facts), FactMiss::None);

    // Three meshes: the left triangle (positions only), the right one (positions + texcoords) and one whose
    // layout no half was built for (positions + normals).
    Mesh left(std::vector<float>{ -0.8F, -0.8F, 0.0F, 0.0F, -0.8F, 0.0F, -0.4F, 0.8F, 0.0F }, {}, {});
    Mesh right(std::vector<float>{ 0.0F, -0.8F, 0.0F, 0.8F, -0.8F, 0.0F, 0.4F, 0.8F, 0.0F },
               std::vector<float>{ 0.0F, 0.0F, 1.0F, 0.0F, 0.5F, 1.0F }, {});
    Mesh unserved(std::vector<float>{ 0.0F, 0.0F, 0.0F, 0.2F, 0.0F, 0.0F, 0.1F, 0.2F, 0.0F }, {},
                  std::vector<float>{ 0.0F, 0.0F, 1.0F, 0.0F, 0.0F, 1.0F, 0.0F, 0.0F, 1.0F });

    GeometryFacts             left_facts;
    GeometryFacts             right_facts;
    GeometryFacts             unserved_facts;
    std::vector<ChannelFacts> left_channels;
    std::vector<ChannelFacts> right_channels;
    std::vector<ChannelFacts> unserved_channels;
    ASSERT_EQ(buildGeometryFacts(*left.geometry, left_facts, left_channels), FactMiss::None);
    ASSERT_EQ(buildGeometryFacts(*right.geometry, right_facts, right_channels), FactMiss::None);
    ASSERT_EQ(buildGeometryFacts(*unserved.geometry, unserved_facts, unserved_channels), FactMiss::None);
    ASSERT_FALSE(left_facts.layout == right_facts.layout) << "the two meshes have to feed different layouts";
    ASSERT_EQ(right_facts.channels.size(), 2U);

    // The two halves, and the one registry and pool they share.
    std::unique_ptr<ContentPipeline> left_pipeline =
        pipelineFor(left_facts, green_facts.shaders, green_facts.abi);
    std::unique_ptr<ContentPipeline> right_pipeline =
        pipelineFor(right_facts, blue_facts.shaders, blue_facts.abi);
    ASSERT_NE(left_pipeline, nullptr);
    ASSERT_NE(right_pipeline, nullptr);

    VariantPool   pool;
    StateRegistry registry(pool);
    StreamUploads uploads;
    const auto    entry_points =
        vine::vsg::detail::fetchDynamicStateEntryPoints(created.device->vk(), created.instance->vk());
    ContentDraw left_draws(*left_pipeline, pool, entry_points);
    ContentDraw right_draws(*right_pipeline, pool, entry_points);

    const ContentPass::Scope::Entry halves[]{
        { vine::vsg::core::DrawKind::Content, green_program.get(), green_facts.revision, left_facts.layout,
          left_pipeline.get(), &left_draws },
        { vine::vsg::core::DrawKind::Content, blue_program.get(), blue_facts.revision, right_facts.layout,
          right_pipeline.get(), &right_draws },
    };
    const vine::intrusive_ptr<Material> material(new Material());
    material->setDiffuse(vine::Colorf(0.2F, 0.3F, 0.4F, 1.0F));
    MaterialFacts          material_facts;
    std::vector<std::byte> material_storage;
    ASSERT_EQ(buildMaterialFacts(material.get(), 1U, material_facts, material_storage), FactMiss::None);

    const ProgramFacts  programs[]{ green_facts, blue_facts, other_facts };
    const GeometryFacts geometries[]{ left_facts, right_facts, unserved_facts };
    const MaterialFacts materials[]{ material_facts };
    ContentFacts        facts;
    facts.programs   = programs;
    facts.geometries = geometries;
    facts.materials  = materials;

    // One frame: one pass, one drawing call, and the left mesh drawn TWICE around the right one - the
    // alternation is what shows whether the halves share the pass' state memory.
    FrameArena                arena{ 64 * 1024 };
    Diagnostics               diagnostics;
    std::vector<vine::String> messages;
    diagnostics.setSink([&messages](const vine::graphics::RenderDiagnostic& diagnostic) {
        messages.push_back(diagnostic.message);
    });
    Observe       observe;
    FrameRecorder recorder{ arena, diagnostics, observe };
    FrameCompiler compiler{ arena, diagnostics, observe };

    TargetFacts target_facts;
    target_facts.target        = target.get();
    target_facts.wanted.width  = static_cast<int>(kSize);
    target_facts.wanted.height = static_cast<int>(kSize);
    target_facts.wanted.shape.color_formats.push_back(RenderTarget::ColorFormat::RGBA8);
    target_facts.current       = target->instance();
    const std::vector<TargetFacts> target_table{ target_facts };

    ClearPolicy clear;
    clear.color          = true;
    clear.color_value[0] = kClear[0];
    clear.color_value[1] = kClear[1];
    clear.color_value[2] = kClear[2];
    clear.color_value[3] = 1.0F;

    RenderCommand left_command;
    left_command.geometry = left.geometry;
    left_command.material = material;
    left_command.program  = green_program;
    RenderCommand right_command;
    right_command.geometry = right.geometry;
    right_command.material = material;
    right_command.program  = blue_program;
    const std::vector<RenderCommand> commands{ left_command, right_command, left_command };

    recorder.beginFrame(FrameToken{ 1 });
    recorder.beginPass(1U);
    recorder.setRenderTarget(target.get());
    recorder.setClearPolicy(clear);
    recorder.render(commands, nullptr);
    recorder.endPass();
    recorder.endFrame();
    recorder.swapBuffers();  // the call that closes the frame: a new one may not open before it

    const CompiledFrame& frame = compiler.compile(recorder.description(), FrameFacts{ target_table });
    ASSERT_EQ(frame.passes.size(), 1U);
    std::size_t recorded_commands = 0;
    for (const vine::vsg::core::CompiledDraw& draw : frame.passes[0].draws)
    {
        recorded_commands += draw.commands.size();
    }
    ASSERT_EQ(recorded_commands, 3U);

    storage->beginFrame();
    ContentPass::Scope scope;
    scope.entries  = halves;
    scope.registry = &registry;
    scope.storage  = storage.get();
    scope.uploads  = &uploads;
    ContentPass content(scope, diagnostics);

    const std::vector<std::byte> view_block(288U, std::byte{ 0 });
    ::vsg::ref_ptr<::vsg::Node>  content_node;
    ASSERT_TRUE(content.record(frame.passes[0], facts, target->shape().compatibility(), {}, view_block, content_node));
    ASSERT_TRUE(messages.empty()) << "every identity is in the tables: nothing may be refused";

    EXPECT_EQ(pool.created(), 2U) << "one program against two layouts is two identities, not one";
    EXPECT_EQ(left_draws.draws(), 2U);
    EXPECT_EQ(right_draws.draws(), 1U);
    EXPECT_EQ(left_draws.pipeline_binds(), 2U)
        << "the third drawing call returns to the left half: the bind the right half replaced has to be re-issued";
    EXPECT_EQ(right_draws.pipeline_binds(), 1U);
    EXPECT_EQ(uploads.uploads(), 5U) << "left: a position channel and an index; right: two channels and an index";
    EXPECT_EQ(uploads.aliases(), 2U) << "the second left mesh aliases both of the streams it already had";

    // The picture: the left mesh green, the right one blue - the halves really drew their own meshes.
    VsgExecutor executor(diagnostics);
    executor.addTarget(target.get(), target.get());

    auto command_graph = ::vsg::CommandGraph::create(created.device, created.queue_family);
    const PassContent packet{ frame.passes[0].pass, content_node };
    ASSERT_TRUE(executor.record(frame, command_graph, std::span<const PassContent>(&packet, 1U)));

    ::vsg::ref_ptr<::vsg::Viewer> viewer = ::vsg::Viewer::create();
    ASSERT_NE(viewer, nullptr);
    viewer->assignRecordAndSubmitTaskAndPresentation(::vsg::CommandGraphs{ command_graph });
    ASSERT_TRUE(viewer->compile());
    viewer->advanceToNextFrame();
    viewer->handleEvents();
    viewer->recordAndSubmit();
    viewer->deviceWaitIdle();

    const Rgba8 left_pixel  = target->probe().pixel(19, 40);
    const Rgba8 right_pixel = target->probe().pixel(45, 40);
    EXPECT_TRUE(isGreen(left_pixel)) << "the narrow layout's mesh must be drawn by its own half, got ("
                                     << static_cast<int>(left_pixel.r) << ", " << static_cast<int>(left_pixel.g)
                                     << ", " << static_cast<int>(left_pixel.b) << ")";
    EXPECT_TRUE(isBlue(right_pixel)) << "the wider layout's mesh must be drawn by ITS half, got ("
                                     << static_cast<int>(right_pixel.r) << ", " << static_cast<int>(right_pixel.g)
                                     << ", " << static_cast<int>(right_pixel.b) << ")";

    // A layout no half was built for: refused, and the message names the layout - the program IS one of the
    // pass' (the green half), so "compile that program" would be the wrong advice.
    const std::uint64_t skipped_before = diagnostics.count(vine::graphics::DiagnosticCategory::ContentSkipped);
    const std::uint64_t uploads_before = uploads.uploads();

    RenderCommand unserved_command;
    unserved_command.geometry = unserved.geometry;
    unserved_command.material = material;
    unserved_command.program  = green_program;

    recorder.beginFrame(FrameToken{ 2 });
    recorder.beginPass(1U);
    recorder.setRenderTarget(target.get());
    recorder.setClearPolicy(clear);
    recorder.render(std::vector<RenderCommand>{ unserved_command }, nullptr);
    recorder.endPass();
    recorder.endFrame();
    recorder.swapBuffers();
    const CompiledFrame& unserved_frame = compiler.compile(recorder.description(), FrameFacts{ target_table });

    storage->beginFrame();
    messages.clear();
    ::vsg::ref_ptr<::vsg::Node> refused_node;
    EXPECT_FALSE(content.record(unserved_frame.passes[0], facts, target->shape().compatibility(), {}, view_block,
                                refused_node));
    ASSERT_EQ(messages.size(), 1U);
    EXPECT_NE(messages[0].as_std_str().find("vertex layout"), std::string::npos) << messages[0].as_std_str();
    EXPECT_EQ(diagnostics.count(vine::graphics::DiagnosticCategory::ContentSkipped), skipped_before + 1U);
    EXPECT_EQ(uploads.uploads(), uploads_before) << "a refused command uploads nothing";

    // A program no half was compiled from: refused too, with the message that sends the reader to the program
    // half of the key instead.
    RenderCommand other_command;
    other_command.geometry = left.geometry;  // a geometry whose layout IS served
    other_command.material = material;
    other_command.program  = other_program;

    recorder.beginFrame(FrameToken{ 3 });
    recorder.beginPass(1U);
    recorder.setRenderTarget(target.get());
    recorder.setClearPolicy(clear);
    recorder.render(std::vector<RenderCommand>{ other_command }, nullptr);
    recorder.endPass();
    recorder.endFrame();
    recorder.swapBuffers();
    const CompiledFrame& other_frame = compiler.compile(recorder.description(), FrameFacts{ target_table });

    storage->beginFrame();
    messages.clear();
    ::vsg::ref_ptr<::vsg::Node> unmatched_node;
    EXPECT_FALSE(content.record(other_frame.passes[0], facts, target->shape().compatibility(), {}, view_block,
                                unmatched_node));
    ASSERT_EQ(messages.size(), 1U);
    EXPECT_NE(messages[0].as_std_str().find("program"), std::string::npos) << messages[0].as_std_str();
    EXPECT_EQ(uploads.uploads(), uploads_before) << "a refused command uploads nothing";
}

TEST(ContentPassTest, ASecondPassLoadsWhatTheFirstWroteAndBothDrawThroughOneVariant)
{
    // TWO PASSES OVER ONE TARGET, ONE FRAME. The first pass clears its own colour and draws the left mesh; the
    // second one asks for NO clear, so what it does to the attachment is LOAD - and it draws the right mesh on
    // top of the picture that is already there.
    //
    // Three things this case is the evidence for, and none of them is visible to a counter alone:
    //
    //   * the target serves the two passes with TWO RENDER PASS VARIANTS (a clearing one and a loading one,
    //     see core::LoadOpVariantKey) built over the SAME attachments - `passVariantCount()` says so;
    //   * the loading variant is COMPATIBLE with the clearing one, so both passes compile and bind through ONE
    //     variant object (`pool.created() == 1`): load/store operations and layouts are not part of render pass
    //     compatibility, which is what lets the pipeline be shared. If they were, the validation layer would
    //     report VUID-vkCmdDrawIndexed-renderPass-02684 here - the same trap M4c found between a window pass
    //     and an off-screen one, reached from the other side;
    //   * the loading pass really LOADs: the first pass' clear colour is still there afterwards, and the left
    //     mesh it drew is still on the screen under the second pass' work.
    const vine::vsg::DeviceResult created = vine::vsg::createDevice();
    if (!created.ok)
    {
        GTEST_SKIP() << "no Vulkan device available (lavapipe + X11 are needed): " << created.error.as_std_str();
    }

    OffscreenTarget::Layout layout;
    layout.width  = kSize;
    layout.height = kSize;
    std::unique_ptr<OffscreenTarget> target = OffscreenTarget::create(created.device, layout);
    ASSERT_NE(target, nullptr);
    EXPECT_EQ(target->passVariantCount(), 1U) << "create() builds the bootstrap variant; no pass has run yet";

    std::unique_ptr<BlockStorage>     storage     = BlockStorage::create(created.device, BlockStorage::Layout{});
    ASSERT_NE(storage, nullptr);
    std::unique_ptr<BlockDescriptors> descriptors = BlockDescriptors::create(created.device, *storage);
    ASSERT_NE(descriptors, nullptr);

    // ONE program, ONE layout: the two meshes differ in their vertex data, not in what a pipeline is compiled
    // against, so both passes ask for the same variant.
    const vine::intrusive_ptr<ShaderProgram> program(new ShaderProgram());
    addStages(*program);
    ProgramFacts program_facts;
    ASSERT_EQ(buildProgramFacts(*program, program_facts), FactMiss::None);

    // The mesh to the LEFT (drawn by the clearing pass) and the one to the RIGHT (drawn by the loading one).
    // Both feed positions only, and the probes below are the centroids of the two triangles in a 64x64 NDC.
    Mesh left(std::vector<float>{ -0.8F, -0.8F, 0.0F, 0.0F, -0.8F, 0.0F, -0.4F, 0.8F, 0.0F }, {}, {});
    Mesh right(std::vector<float>{ 0.0F, -0.8F, 0.0F, 0.8F, -0.8F, 0.0F, 0.4F, 0.8F, 0.0F }, {}, {});

    GeometryFacts             left_facts;
    GeometryFacts             right_facts;
    std::vector<ChannelFacts> left_channels;
    std::vector<ChannelFacts> right_channels;
    ASSERT_EQ(buildGeometryFacts(*left.geometry, left_facts, left_channels), FactMiss::None);
    ASSERT_EQ(buildGeometryFacts(*right.geometry, right_facts, right_channels), FactMiss::None);
    ASSERT_TRUE(left_facts.layout == right_facts.layout) << "the two meshes must ask for the same layout";

    std::unique_ptr<ContentPipeline> pipelines =
        pipelineFor(left_facts, program_facts.shaders, program_facts.abi);
    ASSERT_NE(pipelines, nullptr);

    VariantPool   pool;
    StreamUploads uploads;
    const auto    entry_points =
        vine::vsg::detail::fetchDynamicStateEntryPoints(created.device->vk(), created.instance->vk());
    ContentDraw   draws(*pipelines, pool, entry_points);

    // One registry per PASS (what a pass has bound is not what another pass has bound), one pool between them.
    StateRegistry first_registry(pool);
    StateRegistry second_registry(pool);

    const vine::intrusive_ptr<Material> material(new Material());
    material->setDiffuse(vine::Colorf(0.2F, 0.3F, 0.4F, 1.0F));
    MaterialFacts          material_facts;
    std::vector<std::byte> material_storage;
    ASSERT_EQ(buildMaterialFacts(material.get(), 1U, material_facts, material_storage), FactMiss::None);

    const ProgramFacts   programs[]   = { program_facts };
    const GeometryFacts  geometries[] = { left_facts, right_facts };
    const MaterialFacts  materials[]  = { material_facts };
    ContentFacts         facts;
    facts.programs   = programs;
    facts.geometries = geometries;
    facts.materials  = materials;

    FrameArena    arena{ 64 * 1024 };
    Diagnostics   diagnostics;
    Observe       observe;
    FrameRecorder recorder{ arena, diagnostics, observe };
    FrameCompiler compiler{ arena, diagnostics, observe };

    // The target has NEVER been written into, and the facts say so: its first writer is a bootstrap pass and
    // must clear (an image in the UNDEFINED layout cannot be loaded), while the second one loads what that
    // first pass left.
    TargetFacts target_facts;
    target_facts.target        = target.get();
    target_facts.wanted.width  = static_cast<int>(kSize);
    target_facts.wanted.height = static_cast<int>(kSize);
    target_facts.wanted.shape.color_formats.push_back(RenderTarget::ColorFormat::RGBA8);
    target_facts.current       = target->instance();
    ASSERT_FALSE(target_facts.current.built) << "nothing has been recorded into it yet";
    const std::vector<TargetFacts> target_table{ target_facts };

    ClearPolicy clear;
    clear.color          = true;
    clear.color_value[0] = kClear[0];
    clear.color_value[1] = kClear[1];
    clear.color_value[2] = kClear[2];
    clear.color_value[3] = 1.0F;

    RenderCommand left_command;
    left_command.geometry = left.geometry;
    left_command.material = material;
    left_command.program  = program;
    RenderCommand right_command;
    right_command.geometry = right.geometry;
    right_command.material = material;
    right_command.program  = program;

    recorder.beginFrame(FrameToken{ 1 });
    recorder.beginPass(1U);
    recorder.setRenderTarget(target.get());
    recorder.setClearPolicy(clear);
    recorder.render(std::vector<RenderCommand>{ left_command }, nullptr);
    recorder.endPass();
    recorder.beginPass(2U);
    recorder.setRenderTarget(target.get());
    // NO clear policy for the second pass: it keeps what the first one left.
    recorder.render(std::vector<RenderCommand>{ right_command }, nullptr);
    recorder.endPass();
    recorder.endFrame();

    const CompiledFrame& frame = compiler.compile(recorder.description(), FrameFacts{ target_table });
    ASSERT_EQ(frame.passes.size(), 2U) << "two passes, two render pass instances";
    EXPECT_EQ(frame.passes[0].target_index, frame.passes[1].target_index) << "and one target between them";
    EXPECT_TRUE(frame.passes[0].bootstrap)
        << "the first writer of a target nobody has written into must clear: its images are UNDEFINED";
    EXPECT_FALSE(frame.passes[1].bootstrap) << "the bootstrap is the FIRST writer's, not every pass'";
    EXPECT_TRUE(frame.passes[0].clear.color);
    EXPECT_FALSE(frame.passes[1].clear.color) << "the second pass asked for no clear: it LOADs";

    // The content layer records each pass with its OWN registry (the second pass binds for itself).
    storage->beginFrame();
    const ContentPass::Scope::Entry halves[]{ ContentPass::Scope::Entry{
        vine::vsg::core::DrawKind::Content, program.get(), program_facts.revision, left_facts.layout,
        pipelines.get(), &draws } };

    ContentPass::Scope first_scope;
    first_scope.entries  = halves;
    first_scope.registry = &first_registry;
    first_scope.storage  = storage.get();
    first_scope.uploads  = &uploads;
    ContentPass first_content(first_scope, diagnostics);

    ContentPass::Scope second_scope;
    second_scope.entries  = halves;
    second_scope.registry = &second_registry;
    second_scope.storage  = storage.get();
    second_scope.uploads  = &uploads;
    ContentPass second_content(second_scope, diagnostics);

    const std::vector<std::byte> view_block(288U, std::byte{ 0 });
    ::vsg::ref_ptr<::vsg::Node>  first_node;
    ::vsg::ref_ptr<::vsg::Node>  second_node;
    ASSERT_TRUE(first_content.record(frame.passes[0], facts, target->shape().compatibility(), {}, view_block,
                                     first_node));
    ASSERT_TRUE(second_content.record(frame.passes[1], facts, target->shape().compatibility(), {}, view_block,
                                      second_node));
    ASSERT_TRUE(diagnostics.clean()) << "nothing may be refused: every identity is in the tables";
    EXPECT_EQ(pool.created(), 1U)
        << "two passes, one program, one layout: ONE variant object, because load ops are not identity";
    EXPECT_EQ(draws.pipeline_binds(), 2U) << "each pass binds it for itself: the registries do not share state";

    // The executor places both passes into the target, in the plan's order.
    VsgExecutor executor(diagnostics);
    executor.addTarget(target.get(), target.get());

    auto command_graph = ::vsg::CommandGraph::create(created.device, created.queue_family);
    const PassContent packets[] = { PassContent{ 1U, first_node }, PassContent{ 2U, second_node } };
    ASSERT_TRUE(executor.record(frame, command_graph, packets));
    EXPECT_EQ(executor.skipped(), 0U);
    EXPECT_EQ(target->passVariantCount(), 2U) << "a clearing variant and a loading one, over one framebuffer";

    ::vsg::ref_ptr<::vsg::Viewer> viewer = ::vsg::Viewer::create();
    ASSERT_NE(viewer, nullptr);
    viewer->assignRecordAndSubmitTaskAndPresentation(::vsg::CommandGraphs{ command_graph });
    ASSERT_TRUE(viewer->compile());
    viewer->advanceToNextFrame();
    viewer->handleEvents();
    viewer->recordAndSubmit();
    viewer->deviceWaitIdle();

    // The picture: the first pass' clear colour everywhere the meshes are not, the LEFT mesh it drew, and the
    // RIGHT mesh the second pass added - which is what "the loading pass kept the picture" means. Had the
    // second pass cleared, both of the first two would be gone (black), and the case would be red.
    const auto near = [](std::uint8_t byte, double linear) {
        return std::abs(static_cast<double>(byte) - 255.0 * linear) <= 8.0;
    };
    const Rgba8 background = target->probe().pixel(2, 2);
    EXPECT_TRUE(near(background.r, kClear[0]) && near(background.g, kClear[1]) && near(background.b, kClear[2]))
        << "the first pass' clear must survive the second pass, got (" << static_cast<int>(background.r) << ", "
        << static_cast<int>(background.g) << ", " << static_cast<int>(background.b) << ")";

    const Rgba8 left_pixel = target->probe().pixel(19, 40);
    EXPECT_TRUE(isGreen(left_pixel)) << "the clearing pass' mesh is still there, got ("
                                     << static_cast<int>(left_pixel.r) << ", " << static_cast<int>(left_pixel.g)
                                     << ", " << static_cast<int>(left_pixel.b) << ")";
    const Rgba8 right_pixel = target->probe().pixel(45, 40);
    EXPECT_TRUE(isGreen(right_pixel)) << "the loading pass drew through the shared variant, got ("
                                      << static_cast<int>(right_pixel.r) << ", " << static_cast<int>(right_pixel.g)
                                      << ", " << static_cast<int>(right_pixel.b) << ")";
}

TEST(ContentPassTest, TheBlocksAreReadWhereverTheProgramDeclaresThem)
{
    // The ENGINE's own programs put the material at set 0 / binding 0 and the per-drawable block in set 1 -
    // not the arrangement this backend's own programs use (view 0 / draw 1 / material 2). This case draws with
    // such a program, and the two halves make the claim testable with pixels:
    //
    //   * the fragment stage's colour IS the material block's diffuse, read from set 0 / binding 0 - where the
    //     canonical arrangement has the VIEW block, whose bytes are all zero here, so a role that had been
    //     guessed by POSITION would paint black;
    //   * the vertex stage places the triangle by the draw block in SET 1, so the geometry, the dynamic
    //     offsets and the two sets all have to be the ones the text declares.
    const vine::vsg::DeviceResult created = vine::vsg::createDevice();
    if (!created.ok)
    {
        GTEST_SKIP() << "no Vulkan device available (lavapipe + X11 are needed): " << created.error.as_std_str();
    }

    OffscreenTarget::Layout target_layout;
    target_layout.width  = kSize;
    target_layout.height = kSize;
    std::unique_ptr<OffscreenTarget> target = OffscreenTarget::create(created.device, target_layout);
    ASSERT_NE(target, nullptr);

    std::unique_ptr<BlockStorage> storage = BlockStorage::create(created.device, BlockStorage::Layout{});
    ASSERT_NE(storage, nullptr);

    const vine::intrusive_ptr<ShaderProgram> program(new ShaderProgram());
    {
        ShaderStage vertex;
        vertex.type   = ShaderStageType::Vertex;
        vertex.source = vine::String(reinterpret_cast<const char8_t*>(
            "layout(location = 0) in vec3 position;\n"
            "layout(set = 1, binding = 0, std140) uniform VineDrawBlock { mat4 model; vec4 params; } draw;\n"
            "void main() { gl_Position = draw.model * vec4(position, 1.0); }\n"));
        ShaderStage fragment;
        fragment.type   = ShaderStageType::Fragment;
        fragment.source = vine::String(reinterpret_cast<const char8_t*>(
            "layout(location = 0) out vec4 outColor;\n"
            "layout(set = 0, binding = 0, std140) uniform VineMaterialBlock\n"
            "{\n"
            "    vec4 ambient; vec4 diffuse; vec4 specular; float shininess;\n"
            "} material;\n"
            "void main() { outColor = vec4(material.diffuse.rgb, 1.0); }\n"));
        program->addStage(vertex);
        program->addStage(fragment);
    }
    ProgramFacts program_facts;
    ASSERT_EQ(buildProgramFacts(*program, program_facts), FactMiss::None);

    // The block sets the program's OWN declarations need: one per set it declares a block in, each built from
    // the shape the layer compiles its pipelines against (see api/BlockDescriptors).
    std::unique_ptr<BlockDescriptors> material_set =
        BlockDescriptors::forAbi(program_facts.abi, 0U, created.device, *storage);
    std::unique_ptr<BlockDescriptors> draw_set = BlockDescriptors::forAbi(program_facts.abi, 1U, created.device, *storage);
    ASSERT_NE(material_set, nullptr);
    ASSERT_NE(draw_set, nullptr);
    ASSERT_EQ(material_set->shape().size(), 1U);
    ASSERT_EQ(material_set->shape()[0].role, vine::vsg::AbiBlockRole::Material);
    ASSERT_EQ(draw_set->shape().size(), 1U);
    ASSERT_EQ(draw_set->shape()[0].role, vine::vsg::AbiBlockRole::Draw);
    BlockDescriptors* block_sets[] = { material_set.get(), draw_set.get() };

    const ContentPipeline::VertexBinding   binding{ 0U, sizeof(float) * 3U, false };
    const ContentPipeline::VertexAttribute attribute{ 0U, 0U, VK_FORMAT_R32G32B32_SFLOAT, 0U };
    ContentPipeline::Settings              settings;
    settings.color_attachments = 1U;
    std::unique_ptr<ContentPipeline> pipelines = ContentPipeline::create(
        program_facts.abi, std::span<const ContentPipeline::VertexBinding>(&binding, 1U),
        std::span<const ContentPipeline::VertexAttribute>(&attribute, 1U), program_facts.shaders, settings);
    ASSERT_NE(pipelines, nullptr);

    VariantPool   pool;
    StateRegistry registry(pool);
    StreamUploads uploads;
    ContentDraw   draws(*pipelines, pool, vine::vsg::detail::fetchDynamicStateEntryPoints(created.device->vk(),
                                                                                         created.instance->vk()));

    Triangle                            triangle;
    const vine::intrusive_ptr<Geometry> geometry(new Geometry());
    geometry->setPositions(triangle.positions);
    geometry->setIndices(triangle.indices);
    geometry->setRevision(1U);
    GeometryFacts                        geometry_facts;
    std::vector<vine::vsg::ChannelFacts> channel_storage;
    ASSERT_EQ(buildGeometryFacts(*geometry, geometry_facts, channel_storage), FactMiss::None);

    // A colour the CLEAR's channels do not match, so a role read from the wrong binding cannot pass: the clear
    // is (0.25, 0.5, 0.75) and the material is its mirror in red and blue.
    const vine::intrusive_ptr<Material> material(new Material());
    material->setDiffuse(vine::Colorf(0.75F, 0.25F, 0.5F, 1.0F));
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

    FrameArena    arena{ 64 * 1024 };
    Diagnostics   diagnostics;
    Observe       observe;
    FrameRecorder recorder{ arena, diagnostics, observe };
    FrameCompiler compiler{ arena, diagnostics, observe };

    TargetFacts target_facts;
    target_facts.target        = target.get();
    target_facts.wanted.width  = static_cast<int>(kSize);
    target_facts.wanted.height = static_cast<int>(kSize);
    target_facts.wanted.shape.color_formats.push_back(RenderTarget::ColorFormat::RGBA8);
    target_facts.current       = target->instance();
    const std::vector<TargetFacts> target_table{ target_facts };

    ClearPolicy clear;
    clear.color          = true;
    clear.color_value[0] = kClear[0];
    clear.color_value[1] = kClear[1];
    clear.color_value[2] = kClear[2];
    clear.color_value[3] = 1.0F;

    RenderCommand command;
    command.geometry = geometry;
    command.material = material;
    command.program  = program;
    const std::vector<RenderCommand> commands{ command };

    recorder.beginFrame(FrameToken{ 1 });
    recorder.beginPass(1U);
    recorder.setRenderTarget(target.get());
    recorder.setClearPolicy(clear);
    recorder.render(commands, nullptr);
    recorder.endPass();
    recorder.endFrame();

    const CompiledFrame& frame = compiler.compile(recorder.description(), FrameFacts{ target_table });
    ASSERT_EQ(frame.passes.size(), 1U);
    ASSERT_EQ(frame.passes[0].draws.size(), 1U);

    storage->beginFrame();
    const ContentPass::Scope::Entry halves[]{ ContentPass::Scope::Entry{
        vine::vsg::core::DrawKind::Content, program.get(), program_facts.revision, geometry_facts.layout,
        pipelines.get(), &draws } };
    ContentPass::Scope scope;
    scope.entries    = halves;
    scope.registry   = &registry;
    scope.storage    = storage.get();
    scope.block_sets = block_sets;
    scope.uploads    = &uploads;
    ContentPass content(scope, diagnostics);

    const std::vector<std::byte> view_block(288U, std::byte{ 0 });
    ::vsg::ref_ptr<::vsg::Node>  content_node;
    ASSERT_TRUE(content.record(frame.passes[0], facts, target->shape().compatibility(), {}, view_block, content_node));
    ASSERT_TRUE(diagnostics.clean()) << "nothing may be refused: both declared sets are there";

    VsgExecutor executor(diagnostics);
    executor.addTarget(target.get(), target.get());
    auto command_graph = ::vsg::CommandGraph::create(created.device, created.queue_family);
    const PassContent packet{ frame.passes[0].pass, content_node };
    ASSERT_TRUE(executor.record(frame, command_graph, std::span<const PassContent>(&packet, 1U)));

    ::vsg::ref_ptr<::vsg::Viewer> viewer = ::vsg::Viewer::create();
    ASSERT_NE(viewer, nullptr);
    viewer->assignRecordAndSubmitTaskAndPresentation(::vsg::CommandGraphs{ command_graph });
    ASSERT_TRUE(viewer->compile());
    viewer->advanceToNextFrame();
    viewer->handleEvents();
    viewer->recordAndSubmit();
    viewer->deviceWaitIdle();

    const auto near = [](std::uint8_t byte, double linear) {
        return std::abs(static_cast<double>(byte) - 255.0 * linear) <= 4.0;
    };
    const Rgba8 centre = target->probe().pixel(static_cast<int>(kSize) / 2, static_cast<int>(kSize) / 2);
    EXPECT_TRUE(near(centre.r, 0.75) && near(centre.g, 0.25) && near(centre.b, 0.5))
        << "the fragment must read the MATERIAL block at the binding its text names, got ("
        << static_cast<int>(centre.r) << ", " << static_cast<int>(centre.g) << ", " << static_cast<int>(centre.b)
        << ") - black would mean the view block (all zeros here) was bound there";

    const Rgba8 corner = target->probe().pixel(2, 2);
    EXPECT_TRUE(near(corner.r, kClear[0]) && near(corner.g, kClear[1]) && near(corner.b, kClear[2]))
        << "the triangle must be placed by the draw block in set 1, leaving the clear at the corner, got ("
        << static_cast<int>(corner.r) << ", " << static_cast<int>(corner.g) << ", " << static_cast<int>(corner.b)
        << ")";
}

TEST(ContentPassTest, TheDeclaredPushCarriesTheCameraMatricesTheVertexStageReads)
{
    // An ENGINE-SHAPED program whose whole interface is the push block: `gl_Position = pc.projection *
    // pc.modelView * vec4(position, 1.0)`. Nothing else is declared - no blocks, no samplers - so the pass
    // has no sets to bind and the picture can only come from the two matrices the text names. The three ways
    // this can go wrong are all measured here, because all three leave a valid-looking frame:
    //
    //   * the push is not written (or written as zeros) => every vertex lands at the origin => no triangle;
    //   * `modelView` is composed with the model on the WRONG side (or dropped) => the drawable stays centred
    //     while the test's model matrix moved it to the right => the right pixel is clear;
    //   * the projection is written UNFOLDED (the SDK's clip convention, `.ai/design/vsg-reimplementation.md`
    //     §11.16o) => the geometry is clipped away (device z outside [0, 1]) => no triangle.
    const vine::vsg::DeviceResult created = vine::vsg::createDevice();
    if (!created.ok)
    {
        GTEST_SKIP() << "no Vulkan device available (lavapipe + X11 are needed): " << created.error.as_std_str();
    }

    OffscreenTarget::Layout target_layout;
    target_layout.width  = kSize;
    target_layout.height = kSize;
    std::unique_ptr<OffscreenTarget> target = OffscreenTarget::create(created.device, target_layout);
    ASSERT_NE(target, nullptr);

    std::unique_ptr<BlockStorage> storage = BlockStorage::create(created.device, BlockStorage::Layout{});
    ASSERT_NE(storage, nullptr);

    const vine::intrusive_ptr<ShaderProgram> program(new ShaderProgram());
    {
        ShaderStage vertex;
        vertex.type   = ShaderStageType::Vertex;
        vertex.source = vine::String(reinterpret_cast<const char8_t*>(
            "layout(location = 0) in vec3 position;\n"
            "layout(push_constant) uniform PushConstants { mat4 projection; mat4 modelView; } pc;\n"
            "void main() { gl_Position = pc.projection * pc.modelView * vec4(position, 1.0); }\n"));
        ShaderStage fragment;
        fragment.type   = ShaderStageType::Fragment;
        fragment.source = vine::String(reinterpret_cast<const char8_t*>(
            "layout(location = 0) out vec4 outColor;\n"
            "void main() { outColor = vec4(0.0, 1.0, 0.0, 1.0); }\n"));
        program->addStage(vertex);
        program->addStage(fragment);
    }
    ProgramFacts program_facts;
    ASSERT_EQ(buildProgramFacts(*program, program_facts), FactMiss::None);
    ASSERT_EQ(program_facts.abi.pushes.size(), 1U) << "the text declares exactly one push range";

    const ContentPipeline::VertexBinding   binding{ 0U, sizeof(float) * 3U, false };
    const ContentPipeline::VertexAttribute attribute{ 0U, 0U, VK_FORMAT_R32G32B32_SFLOAT, 0U };
    ContentPipeline::Settings              settings;
    settings.color_attachments = 1U;
    std::unique_ptr<ContentPipeline> pipelines = ContentPipeline::create(
        program_facts.abi, std::span<const ContentPipeline::VertexBinding>(&binding, 1U),
        std::span<const ContentPipeline::VertexAttribute>(&attribute, 1U), program_facts.shaders, settings);
    ASSERT_NE(pipelines, nullptr) << "a program whose interface is its own declared push is servable";

    VariantPool   pool;
    StateRegistry registry(pool);
    StreamUploads uploads;
    ContentDraw   draws(*pipelines, pool, vine::vsg::detail::fetchDynamicStateEntryPoints(created.device->vk(),
                                                                                         created.instance->vk()));

    Triangle                            triangle;
    const vine::intrusive_ptr<Geometry> geometry(new Geometry());
    geometry->setPositions(triangle.positions);
    geometry->setIndices(triangle.indices);
    geometry->setRevision(1U);
    GeometryFacts                        geometry_facts;
    std::vector<vine::vsg::ChannelFacts> channel_storage;
    ASSERT_EQ(buildGeometryFacts(*geometry, geometry_facts, channel_storage), FactMiss::None);

    const vine::intrusive_ptr<Material> material(new Material());
    MaterialFacts                       material_facts;
    std::vector<std::byte>              material_storage;
    ASSERT_EQ(buildMaterialFacts(material.get(), 1U, material_facts, material_storage), FactMiss::None);

    const ProgramFacts  programs[]   = { program_facts };
    const GeometryFacts geometries[] = { geometry_facts };
    const MaterialFacts materials[]  = { material_facts };
    ContentFacts        facts;
    facts.programs   = programs;
    facts.geometries = geometries;
    facts.materials  = materials;

    // The camera: looking at the origin from 1.5 units out with an orthographic window of [-1, 1] squared (the
    // same framing SessionContentTest uses, and one the folded projection maps to device z ~ 0.857 - INSIDE
    // (0, 1): an unfolded one would be clipped away entirely).
    const vine::intrusive_ptr<vine::graphics::Camera> camera(new vine::graphics::Camera());
    camera->setViewMatrixAsLookAt(vine::math::Vec3d(0.0, 0.0, 1.5), vine::math::Vec3d(0.0, 0.0, 0.0),
                                  vine::math::Vec3d(0.0, 1.0, 0.0));
    camera->setProjectionMatrixAsOrtho(-1.0, 1.0, -1.0, 1.0, 0.5, 4.0);

    FrameArena    arena{ 64 * 1024 };
    Diagnostics   diagnostics;
    Observe       observe;
    FrameRecorder recorder{ arena, diagnostics, observe };
    FrameCompiler compiler{ arena, diagnostics, observe };

    TargetFacts target_facts;
    target_facts.target        = target.get();
    target_facts.wanted.width  = static_cast<int>(kSize);
    target_facts.wanted.height = static_cast<int>(kSize);
    target_facts.wanted.shape.color_formats.push_back(RenderTarget::ColorFormat::RGBA8);
    target_facts.current       = target->instance();
    const std::vector<TargetFacts> target_table{ target_facts };

    ClearPolicy clear;
    clear.color          = true;
    clear.color_value[0] = kClear[0];
    clear.color_value[1] = kClear[1];
    clear.color_value[2] = kClear[2];
    clear.color_value[3] = 1.0F;
    clear.depth          = true;
    clear.depth_value    = 0.0F;   // reverse-Z far, the value a written depth has to beat

    // The drawable is moved HALF A UNIT RIGHT and halved: its triangle then spans NDC x in [0.2, 0.8] at the
    // height the probe reads, so the right probe is inside it and the left one is not. With the model matrix
    // lost (or composed on the wrong side) the triangle stays centred and both probes are clear - which is
    // the difference this case is built to see.
    vine::math::Mat4d model;
    model(0, 0) = 0.5;
    model(1, 1) = 0.5;
    model(2, 2) = 0.5;
    model(0, 3) = 0.5;

    RenderCommand command;
    command.geometry    = geometry;
    command.material    = material;
    command.program     = program;
    command.modelMatrix = model;
    const std::vector<RenderCommand> commands{ command };

    recorder.beginFrame(FrameToken{ 1 });
    recorder.beginPass(1U);
    recorder.setRenderTarget(target.get());
    recorder.setClearPolicy(clear);
    recorder.render(commands, camera.get());
    recorder.endPass();
    recorder.endFrame();

    const CompiledFrame& frame = compiler.compile(recorder.description(), FrameFacts{ target_table });
    ASSERT_EQ(frame.passes.size(), 1U);
    ASSERT_EQ(frame.passes[0].draws.size(), 1U);
    ASSERT_TRUE(frame.passes[0].draws[0].camera.present) << "the plan carries the camera the host announced";

    storage->beginFrame();
    const ContentPass::Scope::Entry halves[]{ ContentPass::Scope::Entry{
        vine::vsg::core::DrawKind::Content, program.get(), program_facts.revision, geometry_facts.layout,
        pipelines.get(), &draws } };
    ContentPass::Scope scope;
    scope.entries  = halves;
    scope.registry = &registry;
    scope.storage  = storage.get();
    scope.uploads  = &uploads;
    ContentPass content(scope, diagnostics);

    // The view block is the pass' business and this program does not read it: the bytes are zero, and the
    // picture still has to be right - the matrices it reads are the PUSH, filled from the plan's camera.
    const std::vector<std::byte> view_block(288U, std::byte{ 0 });
    ::vsg::ref_ptr<::vsg::Node>  content_node;
    ASSERT_TRUE(content.record(frame.passes[0], facts, target->shape().compatibility(), {}, view_block, content_node));
    ASSERT_TRUE(diagnostics.clean()) << "nothing may be refused: a declared push is filled by the pass";
    EXPECT_EQ(draws.push_commands(), 1U) << "one push command per declared range, per drawable";

    VsgExecutor executor(diagnostics);
    executor.addTarget(target.get(), target.get());
    auto command_graph = ::vsg::CommandGraph::create(created.device, created.queue_family);
    const PassContent packet{ frame.passes[0].pass, content_node };
    ASSERT_TRUE(executor.record(frame, command_graph, std::span<const PassContent>(&packet, 1U)));

    ::vsg::ref_ptr<::vsg::Viewer> viewer = ::vsg::Viewer::create();
    ASSERT_NE(viewer, nullptr);
    viewer->assignRecordAndSubmitTaskAndPresentation(::vsg::CommandGraphs{ command_graph });
    ASSERT_TRUE(viewer->compile());
    viewer->advanceToNextFrame();
    viewer->handleEvents();
    viewer->recordAndSubmit();
    viewer->deviceWaitIdle();

    const auto near = [](std::uint8_t byte, double linear) {
        return std::abs(static_cast<double>(byte) - 255.0 * linear) <= 4.0;
    };
    const Rgba8 right = target->probe().pixel(48, 32);
    EXPECT_TRUE(isGreen(right)) << "the pushed matrices must place the drawable where its model matrix says, got ("
                                << static_cast<int>(right.r) << ", " << static_cast<int>(right.g) << ", "
                                << static_cast<int>(right.b) << ") - clear here means the model matrix never arrived";

    const Rgba8 left = target->probe().pixel(16, 32);
    EXPECT_TRUE(near(left.r, kClear[0]) && near(left.g, kClear[1]) && near(left.b, kClear[2]))
        << "the drawable moved right, so the left half of the picture is the plan's clear, got ("
        << static_cast<int>(left.r) << ", " << static_cast<int>(left.g) << ", " << static_cast<int>(left.b) << ")";
}

TEST(ContentPassTest, ADeclaredSetCarriesTheMaterialBlockAndItsMap)
{
    // The ENGINE's own arrangement: ONE set (its set 0) declares the material block at binding 0 AND the
    // diffuse map at binding 1. The caller builds that set - the block's arena binding plus the image - and
    // the pass binds it as one command; the picture proves both halves arrived, because the fragment
    // MULTIPLIES them:
    //
    //   * the source attachment's colour (0.5, 0.25, 1.0) and the material's diffuse (1.0, 0.5, 0.5) give
    //     (0.5, 0.125, 0.5) - a colour neither of them has, so a map that was not bound (the shader would
    //     read whatever happened to be there) or a material that did not arrive cannot produce it;
    //   * the corners are the plan's clear, which is what says the set was bound for a pass that drew.
    const vine::vsg::DeviceResult created = vine::vsg::createDevice();
    if (!created.ok)
    {
        GTEST_SKIP() << "no Vulkan device available (lavapipe + X11 are needed): " << created.error.as_std_str();
    }

    OffscreenTarget::Layout target_layout;
    target_layout.width  = kSize;
    target_layout.height = kSize;
    std::unique_ptr<OffscreenTarget> source = OffscreenTarget::create(created.device, target_layout);
    std::unique_ptr<OffscreenTarget> target = OffscreenTarget::create(created.device, target_layout);
    ASSERT_NE(source, nullptr);
    ASSERT_NE(target, nullptr);

    std::unique_ptr<BlockStorage> storage = BlockStorage::create(created.device, BlockStorage::Layout{});
    ASSERT_NE(storage, nullptr);

    FrameArena    arena{ 64 * 1024 };
    Diagnostics   diagnostics;
    Observe       observe;
    FrameRecorder recorder{ arena, diagnostics, observe };
    FrameCompiler compiler{ arena, diagnostics, observe };
    VsgExecutor   executor(diagnostics);

    const auto targetFacts = [](OffscreenTarget& which) {
        TargetFacts entry;
        entry.target        = &which;
        entry.wanted.width  = static_cast<int>(kSize);
        entry.wanted.height = static_cast<int>(kSize);
        entry.wanted.shape.color_formats.push_back(RenderTarget::ColorFormat::RGBA8);
        entry.current       = which.instance();
        return entry;
    };

    // The source: a clear-only pass through the target's OWN render graph (the canonical way to put a colour
    // into an attachment - see OffscreenTargetTest). The colour attachment ends the pass sampleable, which is
    // exactly the layout the caller's descriptor declares.
    {
        ClearPolicy policy;
        policy.color          = true;
        policy.color_value[0] = 0.5F;
        policy.color_value[1] = 0.25F;
        policy.color_value[2] = 1.0F;
        policy.color_value[3] = 1.0F;

        const ::vsg::ref_ptr<::vsg::RenderGraph> pass = source->passGraph(policy, /*bootstrap*/ true,
                                                                          /*depth_preserved*/ false);
        ASSERT_NE(pass, nullptr);
        ::vsg::ref_ptr<::vsg::Viewer> viewer = ::vsg::Viewer::create();
        ASSERT_NE(viewer, nullptr);
        auto graph = ::vsg::CommandGraph::create(created.device, created.queue_family);
        graph->addChild(pass);
        if (const ::vsg::ref_ptr<::vsg::Node> capture = source->capture()) {
            graph->addChild(capture);
        }
        viewer->assignRecordAndSubmitTaskAndPresentation(::vsg::CommandGraphs{ graph });
        ASSERT_TRUE(viewer->compile());
        viewer->advanceToNextFrame();
        viewer->handleEvents();
        viewer->recordAndSubmit();
        viewer->deviceWaitIdle();

        const auto near_source = [](std::uint8_t byte, double linear) {
            return std::abs(static_cast<double>(byte) - 255.0 * linear) <= 4.0;
        };
        const Rgba8 pixel = source->probe().pixel(static_cast<int>(kSize) / 2, static_cast<int>(kSize) / 2);
        ASSERT_TRUE(near_source(pixel.r, 0.5) && near_source(pixel.g, 0.25) && near_source(pixel.b, 1.0))
            << "the source must hold the colour the map is sampled for, got (" << static_cast<int>(pixel.r) << ", "
            << static_cast<int>(pixel.g) << ", " << static_cast<int>(pixel.b) << ")";
    }

    const vine::intrusive_ptr<ShaderProgram> program(new ShaderProgram());
    {
        ShaderStage vertex;
        vertex.type   = ShaderStageType::Vertex;
        vertex.source = vine::String(reinterpret_cast<const char8_t*>(
            "layout(location = 0) in vec3 position;\n"
            "layout(push_constant) uniform PushConstants { mat4 projection; mat4 modelView; } pc;\n"
            "void main() { gl_Position = pc.projection * pc.modelView * vec4(position, 1.0); }\n"));
        ShaderStage fragment;
        fragment.type   = ShaderStageType::Fragment;
        fragment.source = vine::String(reinterpret_cast<const char8_t*>(
            "layout(location = 0) out vec4 outColor;\n"
            "layout(set = 0, binding = 0, std140) uniform VineMaterialBlock\n"
            "{\n"
            "    vec4 ambient; vec4 diffuse; vec4 specular; float shininess;\n"
            "} material;\n"
            "layout(set = 0, binding = 1) uniform sampler2D map_tex;\n"
            "void main() { outColor = vec4(texture(map_tex, vec2(0.5, 0.5)).rgb * material.diffuse.rgb, 1.0); }\n"));
        program->addStage(vertex);
        program->addStage(fragment);
    }
    ProgramFacts program_facts;
    ASSERT_EQ(buildProgramFacts(*program, program_facts), FactMiss::None);

    const ContentPipeline::VertexBinding   binding{ 0U, sizeof(float) * 3U, false };
    const ContentPipeline::VertexAttribute attribute{ 0U, 0U, VK_FORMAT_R32G32B32_SFLOAT, 0U };
    ContentPipeline::Settings              settings;
    settings.color_attachments = 1U;
    std::unique_ptr<ContentPipeline> pipelines = ContentPipeline::create(
        program_facts.abi, std::span<const ContentPipeline::VertexBinding>(&binding, 1U),
        std::span<const ContentPipeline::VertexAttribute>(&attribute, 1U), program_facts.shaders, settings);
    ASSERT_NE(pipelines, nullptr) << "the engine's set 0 (a block and a map) must be describable";

    // The set the program declares, built by the caller: the block half from the arena, the map half from
    // whatever image the caller has (here the source pass' colour attachment).
    const vine::vsg::BlockDescriptors::SampledBinding maps[] = {
        vine::vsg::BlockDescriptors::SampledBinding{ 1U, source->colorView(0), pipelines->inputSampler() }
    };
    ASSERT_NE(maps[0].view, nullptr);
    std::unique_ptr<BlockDescriptors> declared = BlockDescriptors::forAbi(program_facts.abi, 0U, created.device,
                                                                         *storage, maps);
    ASSERT_NE(declared, nullptr);
    ASSERT_EQ(declared->samplers().size(), 1U);
    BlockDescriptors* declared_sets[] = { declared.get() };

    VariantPool   pool;
    StateRegistry registry(pool);
    StreamUploads uploads;
    ContentDraw   draws(*pipelines, pool, vine::vsg::detail::fetchDynamicStateEntryPoints(created.device->vk(),
                                                                                         created.instance->vk()));

    Triangle                            triangle;
    const vine::intrusive_ptr<Geometry> geometry(new Geometry());
    geometry->setPositions(triangle.positions);
    geometry->setIndices(triangle.indices);
    geometry->setRevision(1U);
    GeometryFacts                        geometry_facts;
    std::vector<vine::vsg::ChannelFacts> channel_storage;
    ASSERT_EQ(buildGeometryFacts(*geometry, geometry_facts, channel_storage), FactMiss::None);

    const vine::intrusive_ptr<Material> material(new Material());
    material->setDiffuse(vine::Colorf(1.0F, 0.5F, 0.5F, 1.0F));
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

    const vine::intrusive_ptr<vine::graphics::Camera> camera(new vine::graphics::Camera());
    camera->setViewMatrixAsLookAt(vine::math::Vec3d(0.0, 0.0, 1.5), vine::math::Vec3d(0.0, 0.0, 0.0),
                                  vine::math::Vec3d(0.0, 1.0, 0.0));
    camera->setProjectionMatrixAsOrtho(-1.0, 1.0, -1.0, 1.0, 0.5, 4.0);

    ClearPolicy clear;
    clear.color          = true;
    clear.color_value[0] = kClear[0];
    clear.color_value[1] = kClear[1];
    clear.color_value[2] = kClear[2];
    clear.color_value[3] = 1.0F;
    clear.depth          = true;
    clear.depth_value    = 0.0F;

    RenderCommand command;
    command.geometry = geometry;
    command.material = material;
    command.program  = program;
    const std::vector<RenderCommand> commands{ command };

    recorder.beginFrame(FrameToken{ 2 });
    recorder.beginPass(2U);
    recorder.setRenderTarget(target.get());
    recorder.setClearPolicy(clear);
    recorder.render(commands, camera.get());
    recorder.endPass();
    recorder.endFrame();

    const std::vector<TargetFacts> table{ targetFacts(*target) };
    const CompiledFrame&           frame = compiler.compile(recorder.description(), FrameFacts{ table });
    ASSERT_EQ(frame.passes.size(), 1U);
    ASSERT_EQ(frame.passes[0].draws.size(), 1U);

    storage->beginFrame();
    const ContentPass::Scope::Entry halves[]{ ContentPass::Scope::Entry{
        vine::vsg::core::DrawKind::Content, program.get(), program_facts.revision, geometry_facts.layout,
        pipelines.get(), &draws } };
    ContentPass::Scope scope;
    scope.entries    = halves;
    scope.registry   = &registry;
    scope.storage    = storage.get();
    scope.block_sets = declared_sets;
    scope.uploads    = &uploads;
    ContentPass content(scope, diagnostics);

    const std::vector<std::byte> view_block(288U, std::byte{ 0 });
    ::vsg::ref_ptr<::vsg::Node>  content_node;
    ASSERT_TRUE(content.record(frame.passes[0], facts, target->shape().compatibility(), {}, view_block, content_node));
    ASSERT_TRUE(diagnostics.clean()) << "one declared set, and the caller built exactly it";

    executor.addTarget(target.get(), target.get());
    auto command_graph = ::vsg::CommandGraph::create(created.device, created.queue_family);
    const PassContent packet{ frame.passes[0].pass, content_node };
    ASSERT_TRUE(executor.record(frame, command_graph, std::span<const PassContent>(&packet, 1U)));

    ::vsg::ref_ptr<::vsg::Viewer> viewer = ::vsg::Viewer::create();
    ASSERT_NE(viewer, nullptr);
    viewer->assignRecordAndSubmitTaskAndPresentation(::vsg::CommandGraphs{ command_graph });
    ASSERT_TRUE(viewer->compile());
    viewer->advanceToNextFrame();
    viewer->handleEvents();
    viewer->recordAndSubmit();
    viewer->deviceWaitIdle();

    const auto near = [](std::uint8_t byte, double linear) {
        return std::abs(static_cast<double>(byte) - 255.0 * linear) <= 4.0;
    };
    const Rgba8 centre = target->probe().pixel(static_cast<int>(kSize) / 2, static_cast<int>(kSize) / 2);
    EXPECT_TRUE(near(centre.r, 0.5) && near(centre.g, 0.125) && near(centre.b, 0.5))
        << "the map must be sampled AND multiplied by the material, got (" << static_cast<int>(centre.r) << ", "
        << static_cast<int>(centre.g) << ", " << static_cast<int>(centre.b)
        << ") - (255, 128, 128) would mean the map read white, (128, 64, 255) that the material was lost";

    const Rgba8 corner = target->probe().pixel(2, 2);
    EXPECT_TRUE(near(corner.r, kClear[0]) && near(corner.g, kClear[1]) && near(corner.b, kClear[2]))
        << "the pass' clear must survive where the triangle is not, got (" << static_cast<int>(corner.r) << ", "
        << static_cast<int>(corner.g) << ", " << static_cast<int>(corner.b) << ")";

    // ... and a set WITHOUT the image the text declares is refused rather than bound: the layout the pipeline
    // was compiled against carries that binding, and a set that does not carry it is a different set.
    std::unique_ptr<BlockDescriptors> blocks_only = BlockDescriptors::forAbi(program_facts.abi, 0U, created.device,
                                                                            *storage);
    ASSERT_NE(blocks_only, nullptr);
    EXPECT_TRUE(blocks_only->samplers().empty());
    BlockDescriptors* wrong_sets[] = { blocks_only.get() };

    const std::uint64_t refusals_before =
        diagnostics.count(vine::graphics::DiagnosticCategory::ContentSkipped);
    ContentPass::Scope want_more;
    want_more.entries    = halves;
    want_more.registry   = &registry;
    want_more.storage    = storage.get();
    want_more.block_sets = wrong_sets;
    want_more.uploads    = &uploads;
    ContentPass refusing(want_more, diagnostics);
    ::vsg::ref_ptr<::vsg::Node> refused_node;
    EXPECT_FALSE(refusing.record(frame.passes[0], facts, target->shape().compatibility(), {}, view_block,
                                 refused_node))
        << "a set without the declared image is not the set the program declares";
    EXPECT_EQ(diagnostics.count(vine::graphics::DiagnosticCategory::ContentSkipped), refusals_before + 1U)
        << "the refusal is reported, not silent";
}

TEST(ContentPassTest, AMaterialWithoutAMapSamplesWhite)
{
    // The engine's contract for "this material has no texture": the sample is WHITE, so the shading is the
    // material's own colour. A program that samples `diffuseMap` therefore cannot have that binding empty -
    // an unwritten descriptor is undefined data, not "no map" - and this case draws exactly that program with
    // the fallback bound: the map half of the declared set is a 1x1 white image (api/WhiteImage), whose fill
    // is recorded before the content, and the picture is the material's diffuse multiplied by white.
    //
    // The negative is built in: a black fallback (or one that never got filled, which would leave the image
    // in the layout the descriptor does not declare) paints something else, and the material colour is a
    // value no other node in this frame writes.
    const vine::vsg::DeviceResult created = vine::vsg::createDevice();
    if (!created.ok)
    {
        GTEST_SKIP() << "no Vulkan device available (lavapipe + X11 are needed): " << created.error.as_std_str();
    }

    OffscreenTarget::Layout target_layout;
    target_layout.width  = kSize;
    target_layout.height = kSize;
    std::unique_ptr<OffscreenTarget> target = OffscreenTarget::create(created.device, target_layout);
    ASSERT_NE(target, nullptr);

    std::unique_ptr<BlockStorage> storage = BlockStorage::create(created.device, BlockStorage::Layout{});
    ASSERT_NE(storage, nullptr);

    const std::shared_ptr<vine::vsg::WhiteImage> white = vine::vsg::WhiteImage::create();
    ASSERT_NE(white, nullptr);

    const vine::intrusive_ptr<ShaderProgram> program(new ShaderProgram());
    {
        ShaderStage vertex;
        vertex.type   = ShaderStageType::Vertex;
        vertex.source = vine::String(reinterpret_cast<const char8_t*>(
            "layout(location = 0) in vec3 position;\n"
            "layout(push_constant) uniform PushConstants { mat4 projection; mat4 modelView; } pc;\n"
            "void main() { gl_Position = pc.projection * pc.modelView * vec4(position, 1.0); }\n"));
        ShaderStage fragment;
        fragment.type   = ShaderStageType::Fragment;
        fragment.source = vine::String(reinterpret_cast<const char8_t*>(
            "layout(location = 0) out vec4 outColor;\n"
            "layout(set = 0, binding = 0, std140) uniform VineMaterialBlock\n"
            "{\n"
            "    vec4 ambient; vec4 diffuse; vec4 specular; float shininess;\n"
            "} material;\n"
            "layout(set = 0, binding = 1) uniform sampler2D diffuseMap;\n"
            "void main() { outColor = vec4(texture(diffuseMap, vec2(0.5, 0.5)).rgb * material.diffuse.rgb, 1.0); }\n"));
        program->addStage(vertex);
        program->addStage(fragment);
    }
    ProgramFacts program_facts;
    ASSERT_EQ(buildProgramFacts(*program, program_facts), FactMiss::None);

    const ContentPipeline::VertexBinding   binding{ 0U, sizeof(float) * 3U, false };
    const ContentPipeline::VertexAttribute attribute{ 0U, 0U, VK_FORMAT_R32G32B32_SFLOAT, 0U };
    ContentPipeline::Settings              settings;
    settings.color_attachments = 1U;
    std::unique_ptr<ContentPipeline> pipelines = ContentPipeline::create(
        program_facts.abi, std::span<const ContentPipeline::VertexBinding>(&binding, 1U),
        std::span<const ContentPipeline::VertexAttribute>(&attribute, 1U), program_facts.shaders, settings);
    ASSERT_NE(pipelines, nullptr);

    // The declared set: the material's block from the arena, and the fallback at the binding the text names.
    const vine::vsg::BlockDescriptors::SampledBinding maps[] = {
        vine::vsg::BlockDescriptors::SampledBinding{ 1U, white->view(), white->sampler() }
    };
    std::unique_ptr<BlockDescriptors> declared = BlockDescriptors::forAbi(program_facts.abi, 0U, created.device,
                                                                         *storage, maps);
    ASSERT_NE(declared, nullptr);
    BlockDescriptors* declared_sets[] = { declared.get() };

    VariantPool   pool;
    StateRegistry registry(pool);
    StreamUploads uploads;
    ContentDraw   draws(*pipelines, pool, vine::vsg::detail::fetchDynamicStateEntryPoints(created.device->vk(),
                                                                                         created.instance->vk()));

    Triangle                            triangle;
    const vine::intrusive_ptr<Geometry> geometry(new Geometry());
    geometry->setPositions(triangle.positions);
    geometry->setIndices(triangle.indices);
    geometry->setRevision(1U);
    GeometryFacts                        geometry_facts;
    std::vector<vine::vsg::ChannelFacts> channel_storage;
    ASSERT_EQ(buildGeometryFacts(*geometry, geometry_facts, channel_storage), FactMiss::None);

    const vine::intrusive_ptr<Material> material(new Material());
    material->setDiffuse(vine::Colorf(1.0F, 0.5F, 0.5F, 1.0F));
    ASSERT_EQ(material->texture(), nullptr) << "the material this case shades has no map (that is the point)";
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

    FrameArena    arena{ 64 * 1024 };
    Diagnostics   diagnostics;
    Observe       observe;
    FrameRecorder recorder{ arena, diagnostics, observe };
    FrameCompiler compiler{ arena, diagnostics, observe };

    const vine::intrusive_ptr<vine::graphics::Camera> camera(new vine::graphics::Camera());
    camera->setViewMatrixAsLookAt(vine::math::Vec3d(0.0, 0.0, 1.5), vine::math::Vec3d(0.0, 0.0, 0.0),
                                  vine::math::Vec3d(0.0, 1.0, 0.0));
    camera->setProjectionMatrixAsOrtho(-1.0, 1.0, -1.0, 1.0, 0.5, 4.0);

    TargetFacts target_facts;
    target_facts.target        = target.get();
    target_facts.wanted.width  = static_cast<int>(kSize);
    target_facts.wanted.height = static_cast<int>(kSize);
    target_facts.wanted.shape.color_formats.push_back(RenderTarget::ColorFormat::RGBA8);
    target_facts.current       = target->instance();
    const std::vector<TargetFacts> target_table{ target_facts };

    ClearPolicy clear;
    clear.color          = true;
    clear.color_value[0] = kClear[0];
    clear.color_value[1] = kClear[1];
    clear.color_value[2] = kClear[2];
    clear.color_value[3] = 1.0F;
    clear.depth          = true;
    clear.depth_value    = 0.0F;

    RenderCommand command;
    command.geometry = geometry;
    command.material = material;
    command.program  = program;
    const std::vector<RenderCommand> commands{ command };

    recorder.beginFrame(FrameToken{ 1 });
    recorder.beginPass(1U);
    recorder.setRenderTarget(target.get());
    recorder.setClearPolicy(clear);
    recorder.render(commands, camera.get());
    recorder.endPass();
    recorder.endFrame();

    const CompiledFrame& frame = compiler.compile(recorder.description(), FrameFacts{ target_table });
    ASSERT_EQ(frame.passes.size(), 1U);
    ASSERT_EQ(frame.passes[0].draws.size(), 1U);

    storage->beginFrame();
    const ContentPass::Scope::Entry halves[]{ ContentPass::Scope::Entry{
        vine::vsg::core::DrawKind::Content, program.get(), program_facts.revision, geometry_facts.layout,
        pipelines.get(), &draws } };
    ContentPass::Scope scope;
    scope.entries    = halves;
    scope.registry   = &registry;
    scope.storage    = storage.get();
    scope.block_sets = declared_sets;
    scope.uploads    = &uploads;
    ContentPass content(scope, diagnostics);

    const std::vector<std::byte> view_block(288U, std::byte{ 0 });
    ::vsg::ref_ptr<::vsg::Node>  content_node;
    ASSERT_TRUE(content.record(frame.passes[0], facts, target->shape().compatibility(), {}, view_block, content_node));
    ASSERT_TRUE(diagnostics.clean()) << "the material's map binding is served by the fallback";

    VsgExecutor executor(diagnostics);
    executor.addTarget(target.get(), target.get());
    auto command_graph = ::vsg::CommandGraph::create(created.device, created.queue_family);
    // The fallback's fill goes BEFORE the content: a clear needs no render pass, and the draws that sample
    // the image have to see it white (see api/WhiteImage).
    command_graph->addChild(white->fill());
    const PassContent packet{ frame.passes[0].pass, content_node };
    ASSERT_TRUE(executor.record(frame, command_graph, std::span<const PassContent>(&packet, 1U)));

    ::vsg::ref_ptr<::vsg::Viewer> viewer = ::vsg::Viewer::create();
    ASSERT_NE(viewer, nullptr);
    viewer->assignRecordAndSubmitTaskAndPresentation(::vsg::CommandGraphs{ command_graph });
    ASSERT_TRUE(viewer->compile());
    viewer->advanceToNextFrame();
    viewer->handleEvents();
    viewer->recordAndSubmit();
    viewer->deviceWaitIdle();

    const auto near = [](std::uint8_t byte, double linear) {
        return std::abs(static_cast<double>(byte) - 255.0 * linear) <= 4.0;
    };
    const Rgba8 centre = target->probe().pixel(static_cast<int>(kSize) / 2, static_cast<int>(kSize) / 2);
    EXPECT_TRUE(near(centre.r, 1.0) && near(centre.g, 0.5) && near(centre.b, 0.5))
        << "a material without a map must shade as its own diffuse (white map), got (" << static_cast<int>(centre.r)
        << ", " << static_cast<int>(centre.g) << ", " << static_cast<int>(centre.b)
        << ") - black means the fallback was not white (or never filled)";

    const Rgba8 corner = target->probe().pixel(2, 2);
    EXPECT_TRUE(near(corner.r, kClear[0]) && near(corner.g, kClear[1]) && near(corner.b, kClear[2]))
        << "the pass' clear must survive where the triangle is not, got (" << static_cast<int>(corner.r) << ", "
        << static_cast<int>(corner.g) << ", " << static_cast<int>(corner.b) << ")";
}
