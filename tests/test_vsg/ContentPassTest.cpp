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
#include <string>
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
                                             const ::vsg::ref_ptr<::vsg::DescriptorSetLayout>& block_set)
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
    return ContentPipeline::create(block_set, bindings, attributes, shaders, settings);
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
        descriptors->layout(), std::span<const ContentPipeline::VertexBinding>(&binding, 1U),
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
    target_facts.current.desc  = target_facts.wanted;
    target_facts.current.built = true;
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
    scope.entries     = halves;
    scope.registry    = &registry;
    scope.storage     = storage.get();
    scope.descriptors = descriptors.get();
    scope.uploads     = &uploads;
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
        pipelineFor(left_facts, green_facts.shaders, descriptors->layout());
    std::unique_ptr<ContentPipeline> right_pipeline =
        pipelineFor(right_facts, blue_facts.shaders, descriptors->layout());
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
    target_facts.current.desc  = target_facts.wanted;
    target_facts.current.built = true;
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
    scope.entries     = halves;
    scope.registry    = &registry;
    scope.storage     = storage.get();
    scope.descriptors = descriptors.get();
    scope.uploads     = &uploads;
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
