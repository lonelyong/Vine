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
#include <span>
#include <vector>

#include <vsg/app/CommandGraph.h>
#include <vsg/app/Viewer.h>

#include <vine/Buffer.hpp>
#include <vine/graphics/Camera.hpp>
#include <vine/graphics/BuiltinShaders.hpp>
#include <vine/graphics/Geometry.hpp>
#include <vine/graphics/Light.hpp>
#include <vine/graphics/Material.hpp>
#include <vine/graphics/RenderCommand.hpp>
#include <vine/graphics/RenderTarget.hpp>
#include <vine/graphics/ShaderProgram.hpp>
#include <vine/graphics/Texture.hpp>

#include <vine/vsg/api/BlockDescriptors.hpp>
#include <vine/vsg/api/BlockStorage.hpp>
#include <vine/vsg/api/ContentDraw.hpp>
#include <vine/vsg/api/ContentFacts.hpp>
#include <vine/vsg/api/ContentImages.hpp>
#include <vine/vsg/api/ContentPass.hpp>
#include <vine/vsg/api/ContentStore.hpp>
#include <vine/vsg/api/ContentPipeline.hpp>
#include <vine/vsg/api/ContentSources.hpp>
#include <vine/vsg/api/MaterialImages.hpp>
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
        // The binding a channel is fed at is the BACKEND's canonical order (see StreamUploads::
        // bindingOfCanonical): positions 0, normals 1, texcoords 2, colours 3 - NOT the channel's order in
        // this list, and not the shader's location either. The two agree for positions and normals and
        // disagree for everything else: the engine's reserved texcoord slot is location 8, and a pipeline
        // that bound it as "the second channel" would leave the shader's vec2 un-fed (the draw binds by the
        // canonical number, so the attribute must be declared against that same number).
        const std::uint32_t binding = vine::vsg::StreamUploads::bindingOfCanonical(channel.key.location);
        EXPECT_NE(binding, vine::vsg::StreamUploads::kNoBinding) << "a canonical channel has a binding";
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

TEST(ContentPassTest, ATexturedMaterialSamplesTheTextureTheCacheUploaded)
{
    // The other half of AMaterialWithoutAMapSamplesWhite: the material HAS a texture, and what the shader
    // reads has to be the pixels the caller filled in. api/MaterialImages describes the image - a
    // data-backed vsg::Image, which the viewer's transfer step uploads before the frame that samples it
    // records - the caller binds its view and sampler where its text declares `diffuseMap`, and this frame
    // draws a quad whose UVs run 0..1 over a 2x2 texture of four distinct colours: each quadrant of the
    // picture is one texel, so "the map reached the shader" is a picture rather than a claim.
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

    // The texture: 2x2, four colours, filled through the engine's own API (the description is the contract,
    // the image is the content).
    const vine::intrusive_ptr<vine::graphics::Texture2D> texture(
        new vine::graphics::Texture2D(2, 2, vine::imaging::PixelFormat::Rgba8Unorm));
    {
        auto image = vine::intrusive_ptr<vine::imaging::Image>(
            new vine::imaging::Image(2, 2, vine::imaging::PixelFormat::Rgba8Unorm));
        const std::span<std::byte> pixels = image->mipData(0);
        const std::uint8_t         texels[4][4]{ { 255U, 0U, 0U, 255U },      // the base row's first texel
                                                 { 0U, 255U, 0U, 255U },      // and its second
                                                 { 0U, 0U, 255U, 255U },      // the second row's first
                                                 { 255U, 255U, 255U, 255U } };// and its second
        for (std::size_t texel = 0; texel < 4U; ++texel) {
            for (std::size_t byte = 0; byte < 4U; ++byte) {
                pixels[texel * 4U + byte] = static_cast<std::byte>(texels[texel][byte]);
            }
        }
        texture->setImage(vine::intrusive_ptr<const vine::imaging::Image>(image));
    }

    const std::shared_ptr<vine::vsg::MaterialImages> images = vine::vsg::MaterialImages::create();
    ASSERT_NE(images, nullptr);
    vine::vsg::detail::TextureReject reason = vine::vsg::detail::TextureReject::Ok;
    const vine::vsg::SamplerImage    map    = images->acquire(texture.get(), reason);
    ASSERT_EQ(reason, vine::vsg::detail::TextureReject::Ok) << "a complete 2x2 texture must be uploadable";
    ASSERT_NE(map.view, nullptr);
    ASSERT_NE(map.sampler, nullptr);

    // The program: the quad's position, its UV pair (the L1's reserved texcoord slot), the material block
    // and the map at the binding the text names - the shape the engine's own forward program has when a
    // material carries a texture.
    const vine::intrusive_ptr<ShaderProgram> program(new ShaderProgram());
    {
        ShaderStage vertex;
        vertex.type   = ShaderStageType::Vertex;
        vertex.source = vine::String(reinterpret_cast<const char8_t*>(
            "layout(location = 0) in vec3 position;\n"
            "layout(location = 8) in vec2 texcoord;\n"
            "layout(location = 0) out vec2 uv;\n"
            "layout(push_constant) uniform PushConstants { mat4 projection; mat4 modelView; } pc;\n"
            "void main()\n"
            "{\n"
            "    uv = texcoord;\n"
            "    gl_Position = pc.projection * pc.modelView * vec4(position, 1.0);\n"
            "}\n"));
        ShaderStage fragment;
        fragment.type   = ShaderStageType::Fragment;
        fragment.source = vine::String(reinterpret_cast<const char8_t*>(
            "layout(location = 0) in vec2 uv;\n"
            "layout(location = 0) out vec4 outColor;\n"
            "layout(set = 0, binding = 0, std140) uniform VineMaterialBlock\n"
            "{\n"
            "    vec4 ambient; vec4 diffuse; vec4 specular; float shininess;\n"
            "} material;\n"
            "layout(set = 0, binding = 1) uniform sampler2D diffuseMap;\n"
            "void main() { outColor = vec4(texture(diffuseMap, uv).rgb, 1.0); }\n"));
        program->addStage(vertex);
        program->addStage(fragment);
    }
    // The entry is built for the variant the material asks for (it carries a texture): the tables key
    // their program entries by the variant, so an entry built for another text is not this entry.
    vine::vsg::ProgramVariant textured_variant;
    textured_variant.diffuse_map = true;
    ProgramFacts program_facts;
    ASSERT_EQ(buildProgramFacts(*program, textured_variant, program_facts), FactMiss::None);

    const vine::intrusive_ptr<Geometry> geometry(new Geometry());
    {
        const std::vector<float> positions{ -1.0F, -1.0F, 0.0F, 1.0F, -1.0F, 0.0F,
                                            1.0F,  1.0F,  0.0F, -1.0F, 1.0F, 0.0F };
        const std::vector<float> texcoords{ 0.0F, 0.0F, 1.0F, 0.0F, 1.0F, 1.0F, 0.0F, 1.0F };
        geometry->setPositions(
            vine::intrusive_ptr<const vine::Buffer<float>>(new vine::Buffer<float>(positions)));
        geometry->setTexcoords2(
            vine::intrusive_ptr<const vine::Buffer<float>>(new vine::Buffer<float>(texcoords)));
        geometry->setIndices(vine::intrusive_ptr<const vine::Buffer<std::uint32_t>>(
            new vine::Buffer<std::uint32_t>(std::vector<std::uint32_t>{ 0U, 1U, 2U, 0U, 2U, 3U })));
        geometry->setRevision(1U);
    }
    GeometryFacts                        geometry_facts;
    std::vector<vine::vsg::ChannelFacts> channel_storage;
    ASSERT_EQ(buildGeometryFacts(*geometry, geometry_facts, channel_storage), FactMiss::None);

    std::unique_ptr<ContentPipeline> pipelines =
        pipelineFor(geometry_facts, program_facts.shaders, program_facts.abi);
    ASSERT_NE(pipelines, nullptr) << "the quad's two channels are what the program declares";

    // The declared set: the material's block from the arena, and the texture's images at binding 1.
    const vine::vsg::BlockDescriptors::SampledBinding maps[] = {
        vine::vsg::BlockDescriptors::SampledBinding{ 1U, map.view, map.sampler }
    };
    std::unique_ptr<BlockDescriptors> declared =
        BlockDescriptors::forAbi(program_facts.abi, 0U, created.device, *storage, maps);
    ASSERT_NE(declared, nullptr);
    BlockDescriptors* declared_sets[] = { declared.get() };

    const vine::intrusive_ptr<Material> material(new Material());
    material->setTexture(texture);
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

    VariantPool   pool;
    StateRegistry registry(pool);
    StreamUploads uploads;
    ContentDraw   draws(*pipelines, pool,
                        vine::vsg::detail::fetchDynamicStateEntryPoints(created.device->vk(),
                                                                       created.instance->vk()));

    storage->beginFrame();
    // The half declares the VARIANT it serves: this drawable's material carries a texture, so it is drawn
    // with the textured variant of the program (see api/ProgramVariant - a pass only draws a command
    // through a half whose variant is the one its material and geometry ask for).
    const ContentPass::Scope::Entry halves[]{ ContentPass::Scope::Entry{
        vine::vsg::core::DrawKind::Content, program.get(), program_facts.revision, geometry_facts.layout,
        pipelines.get(), &draws, textured_variant } };
    ContentPass::Scope scope;
    scope.entries    = halves;
    scope.registry   = &registry;
    scope.storage    = storage.get();
    scope.block_sets = declared_sets;
    scope.uploads    = &uploads;
    ContentPass content(scope, diagnostics);

    const std::vector<std::byte> view_block(288U, std::byte{ 0 });
    ::vsg::ref_ptr<::vsg::Node>  content_node;
    ASSERT_TRUE(content.record(frame.passes[0], facts, target->shape().compatibility(), {}, view_block,
                               content_node));
    ASSERT_TRUE(diagnostics.clean()) << "nothing is refused: the map is the cache's image";

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

    // The four quadrants of the picture are the texture's four texels. The engine's device clip inverts Y
    // (vsg's projection does, and the recorder folds it), so the texture's first row is the picture's
    // BOTTOM row: v = 0 is the quad's world -Y edge, which lands at the picture's last rows.
    // The sampler is the cache's own (linear), so a probe at a quadrant's centre reads that texel plus a
    // few percent of its neighbour - which is why the tolerance is a texel's worth of bleed and not 1.
    const auto near = [](std::uint8_t byte, double expected) {
        return std::abs(static_cast<double>(byte) - expected) <= 16.0;
    };
    const auto probe = [&](int x, int y) { return target->probe().pixel(x, y); };
    const Rgba8 top_left     = probe(static_cast<int>(kSize) / 4, static_cast<int>(kSize) / 4);
    const Rgba8 top_right    = probe(static_cast<int>(kSize) * 3 / 4, static_cast<int>(kSize) / 4);
    const Rgba8 bottom_left  = probe(static_cast<int>(kSize) / 4, static_cast<int>(kSize) * 3 / 4);
    const Rgba8 bottom_right = probe(static_cast<int>(kSize) * 3 / 4, static_cast<int>(kSize) * 3 / 4);
    EXPECT_TRUE(near(bottom_left.r, 255.0) && near(bottom_left.g, 0.0) && near(bottom_left.b, 0.0))
        << "the texture's first texel is red, got (" << static_cast<int>(bottom_left.r) << ", "
        << static_cast<int>(bottom_left.g) << ", " << static_cast<int>(bottom_left.b)
        << ") - black means the upload never ran";
    EXPECT_TRUE(near(bottom_right.r, 0.0) && near(bottom_right.g, 255.0) && near(bottom_right.b, 0.0))
        << "its second texel is green, got (" << static_cast<int>(bottom_right.r) << ", "
        << static_cast<int>(bottom_right.g) << ", " << static_cast<int>(bottom_right.b) << ")";
    EXPECT_TRUE(near(top_left.r, 0.0) && near(top_left.g, 0.0) && near(top_left.b, 255.0))
        << "the second row's first texel is blue, got (" << static_cast<int>(top_left.r) << ", "
        << static_cast<int>(top_left.g) << ", " << static_cast<int>(top_left.b) << ")";
    EXPECT_TRUE(near(top_right.r, 255.0) && near(top_right.g, 255.0) && near(top_right.b, 255.0))
        << "and its second is white, got (" << static_cast<int>(top_right.r) << ", "
        << static_cast<int>(top_right.g) << ", " << static_cast<int>(top_right.b) << ")";
}

TEST(ContentPassTest, ACubeTextureIsSampledByTheDirectionTheFragmentComputes)
{
    // A cube map's six faces are six LAYERS of one image, and the layer a fragment reads is chosen by its
    // direction - so the sampler's kind, the view's kind and the layer ORDER all have to be right at once,
    // and a wrong layer order is SILENT: the byte count is the same either way, every copy region stays
    // inside the image, and the picture simply shows the wrong face. It is therefore asserted by pixels.
    //
    // The fragment turns its UV pair into a direction (x and y over the quad, z fixed at 0.5): the
    // picture's centre looks along +Z, its left/right edges along -X/+X and its top/bottom along +/-Y,
    // each far enough from the diagonal ties for the face choice to be unambiguous. Every face is 1x1 and
    // a different colour, so the five probes read five different faces.
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

    // The cube: six faces of 4x4 texels, in Vulkan's layer order (CubeMap::Face), each a solid distinct
    // colour. The size is not decoration: a 1x1 face sampled with the cache's LINEAR filter bleeds across
    // the cube's seams (the taps reach the neighbouring face), so a face's colour is only pure inside it.
    constexpr int kFaceSize = 4;
    const vine::intrusive_ptr<vine::graphics::CubeMap> cube(
        new vine::graphics::CubeMap(kFaceSize, vine::imaging::PixelFormat::Rgba8Unorm));
    const std::uint8_t face_colors[6][4]{ { 255U, 0U, 0U, 255U },      // PosX: red
                                          { 0U, 255U, 0U, 255U },      // NegX: green
                                          { 0U, 0U, 255U, 255U },      // PosY: blue
                                          { 255U, 255U, 0U, 255U },    // NegY: yellow
                                          { 255U, 0U, 255U, 255U },    // PosZ: magenta
                                          { 0U, 255U, 255U, 255U } };  // NegZ: cyan
    for (int face = 0; face < cube->faceCount(); ++face) {
        auto image = vine::intrusive_ptr<vine::imaging::Image>(
            new vine::imaging::Image(kFaceSize, kFaceSize, vine::imaging::PixelFormat::Rgba8Unorm));
        const std::span<std::byte> pixels = image->mipData(0);
        for (std::size_t texel = 0; texel < pixels.size() / 4U; ++texel) {
            for (std::size_t byte = 0; byte < 4U; ++byte) {
                pixels[texel * 4U + byte] = static_cast<std::byte>(face_colors[face][byte]);
            }
        }
        cube->setSource(face, vine::intrusive_ptr<const vine::imaging::Image>(image));
    }

    const std::shared_ptr<vine::vsg::MaterialImages> images = vine::vsg::MaterialImages::create();
    ASSERT_NE(images, nullptr);
    vine::vsg::detail::TextureReject reason = vine::vsg::detail::TextureReject::Ok;
    const vine::vsg::SamplerImage    map    = images->acquire(cube.get(), reason);
    ASSERT_EQ(reason, vine::vsg::detail::TextureReject::Ok) << "a complete cube must be uploadable";
    ASSERT_EQ(map.view->viewType, VK_IMAGE_VIEW_TYPE_CUBE);

    const vine::intrusive_ptr<ShaderProgram> program(new ShaderProgram());
    {
        ShaderStage vertex;
        vertex.type   = ShaderStageType::Vertex;
        vertex.source = vine::String(reinterpret_cast<const char8_t*>(
            "layout(location = 0) in vec3 position;\n"
            "layout(location = 8) in vec2 texcoord;\n"
            "layout(location = 0) out vec2 uv;\n"
            "layout(push_constant) uniform PushConstants { mat4 projection; mat4 modelView; } pc;\n"
            "void main()\n"
            "{\n"
            "    uv = texcoord;\n"
            "    gl_Position = pc.projection * pc.modelView * vec4(position, 1.0);\n"
            "}\n"));
        ShaderStage fragment;
        fragment.type   = ShaderStageType::Fragment;
        fragment.source = vine::String(reinterpret_cast<const char8_t*>(
            "layout(location = 0) in vec2 uv;\n"
            "layout(location = 0) out vec4 outColor;\n"
            "layout(set = 0, binding = 0, std140) uniform VineMaterialBlock\n"
            "{\n"
            "    vec4 ambient; vec4 diffuse; vec4 specular; float shininess;\n"
            "} material;\n"
            "layout(set = 0, binding = 1) uniform samplerCube diffuseMap;\n"
            "void main()\n"
            "{\n"
            "    vec3 direction = normalize(vec3(uv * 2.0 - 1.0, 0.5));\n"
            "    outColor = vec4(texture(diffuseMap, direction).rgb, 1.0);\n"
            "}\n"));
        program->addStage(vertex);
        program->addStage(fragment);
    }
    // The entry is built for the variant the material asks for (it carries a texture): the tables key
    // their program entries by the variant, so an entry built for another text is not this entry.
    vine::vsg::ProgramVariant textured_variant;
    textured_variant.diffuse_map = true;
    ProgramFacts program_facts;
    ASSERT_EQ(buildProgramFacts(*program, textured_variant, program_facts), FactMiss::None);

    const vine::intrusive_ptr<Geometry> geometry(new Geometry());
    {
        const std::vector<float> positions{ -1.0F, -1.0F, 0.0F, 1.0F, -1.0F, 0.0F,
                                            1.0F,  1.0F,  0.0F, -1.0F, 1.0F, 0.0F };
        const std::vector<float> texcoords{ 0.0F, 0.0F, 1.0F, 0.0F, 1.0F, 1.0F, 0.0F, 1.0F };
        geometry->setPositions(
            vine::intrusive_ptr<const vine::Buffer<float>>(new vine::Buffer<float>(positions)));
        geometry->setTexcoords2(
            vine::intrusive_ptr<const vine::Buffer<float>>(new vine::Buffer<float>(texcoords)));
        geometry->setIndices(vine::intrusive_ptr<const vine::Buffer<std::uint32_t>>(
            new vine::Buffer<std::uint32_t>(std::vector<std::uint32_t>{ 0U, 1U, 2U, 0U, 2U, 3U })));
        geometry->setRevision(1U);
    }
    GeometryFacts                        geometry_facts;
    std::vector<vine::vsg::ChannelFacts> channel_storage;
    ASSERT_EQ(buildGeometryFacts(*geometry, geometry_facts, channel_storage), FactMiss::None);

    std::unique_ptr<ContentPipeline> pipelines =
        pipelineFor(geometry_facts, program_facts.shaders, program_facts.abi);
    ASSERT_NE(pipelines, nullptr);

    const vine::vsg::BlockDescriptors::SampledBinding maps[] = {
        vine::vsg::BlockDescriptors::SampledBinding{ 1U, map.view, map.sampler }
    };
    std::unique_ptr<BlockDescriptors> declared =
        BlockDescriptors::forAbi(program_facts.abi, 0U, created.device, *storage, maps);
    ASSERT_NE(declared, nullptr);
    BlockDescriptors* declared_sets[] = { declared.get() };

    const vine::intrusive_ptr<Material> material(new Material());
    material->setTexture(cube);
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

    VariantPool   pool;
    StateRegistry registry(pool);
    StreamUploads uploads;
    ContentDraw   draws(*pipelines, pool,
                        vine::vsg::detail::fetchDynamicStateEntryPoints(created.device->vk(),
                                                                       created.instance->vk()));

    storage->beginFrame();
    // The half declares the VARIANT it serves: this drawable's material carries a texture, so it is drawn
    // with the textured variant of the program (see api/ProgramVariant - a pass only draws a command
    // through a half whose variant is the one its material and geometry ask for).
    const ContentPass::Scope::Entry halves[]{ ContentPass::Scope::Entry{
        vine::vsg::core::DrawKind::Content, program.get(), program_facts.revision, geometry_facts.layout,
        pipelines.get(), &draws, textured_variant } };
    ContentPass::Scope scope;
    scope.entries    = halves;
    scope.registry   = &registry;
    scope.storage    = storage.get();
    scope.block_sets = declared_sets;
    scope.uploads    = &uploads;
    ContentPass content(scope, diagnostics);

    const std::vector<std::byte> view_block(288U, std::byte{ 0 });
    ::vsg::ref_ptr<::vsg::Node>  content_node;
    ASSERT_TRUE(content.record(frame.passes[0], facts, target->shape().compatibility(), {}, view_block,
                               content_node));
    ASSERT_TRUE(diagnostics.clean());

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

    const auto near = [](std::uint8_t byte, double expected) {
        return std::abs(static_cast<double>(byte) - expected) <= 16.0;
    };
    const auto isColor = [&](const Rgba8& pixel, std::uint8_t r, std::uint8_t g, std::uint8_t b,
                             const char* face) {
        return near(pixel.r, r) && near(pixel.g, g) && near(pixel.b, b)
               || (ADD_FAILURE() << "expected " << face << ", got (" << static_cast<int>(pixel.r) << ", "
                                 << static_cast<int>(pixel.g) << ", " << static_cast<int>(pixel.b) << ")",
                   false);
    };
    const int middle = static_cast<int>(kSize) / 2;
    EXPECT_TRUE(isColor(target->probe().pixel(middle, middle), 255U, 0U, 255U, "the +Z face (magenta)"));
    EXPECT_TRUE(isColor(target->probe().pixel(4, middle), 0U, 255U, 0U, "the -X face (green)"));
    EXPECT_TRUE(isColor(target->probe().pixel(static_cast<int>(kSize) - 4, middle), 255U, 0U, 0U,
                        "the +X face (red)"));
    EXPECT_TRUE(isColor(target->probe().pixel(middle, 4), 0U, 0U, 255U, "the +Y face (blue)"));
    EXPECT_TRUE(isColor(target->probe().pixel(middle, static_cast<int>(kSize) - 4), 255U, 255U, 0U,
                        "the -Y face (yellow)"));
}

TEST(ContentPassTest, ARefilledTextureIsUploadedAgainForTheNextDraw)
{
    // A texture that is re-filled keeps its ADDRESS, so only its revision says the pixels changed - and the
    // upload is the viewer's transfer step, which copies what the scene references. This frame draws the
    // same quad twice, with the texture re-filled between the two passes: the first picture is the old
    // texel and the second is the new one. A cache that ignored the revision would draw the old colour
    // twice, and a pipeline that reused the first pass' descriptor would do the same.
    const vine::vsg::DeviceResult created = vine::vsg::createDevice();
    if (!created.ok)
    {
        GTEST_SKIP() << "no Vulkan device available (lavapipe + X11 are needed): " << created.error.as_std_str();
    }

    const auto makeTarget = [&] {
        OffscreenTarget::Layout layout;
        layout.width  = kSize;
        layout.height = kSize;
        return OffscreenTarget::create(created.device, layout);
    };
    std::unique_ptr<OffscreenTarget> first_target  = makeTarget();
    std::unique_ptr<OffscreenTarget> second_target = makeTarget();
    ASSERT_NE(first_target, nullptr);
    ASSERT_NE(second_target, nullptr);

    std::unique_ptr<BlockStorage> storage = BlockStorage::create(created.device, BlockStorage::Layout{});
    ASSERT_NE(storage, nullptr);

    const auto fill = [](vine::graphics::Texture2D& texture, std::uint8_t r, std::uint8_t g, std::uint8_t b) {
        auto image = vine::intrusive_ptr<vine::imaging::Image>(
            new vine::imaging::Image(1, 1, vine::imaging::PixelFormat::Rgba8Unorm));
        const std::span<std::byte> pixels = image->mipData(0);
        pixels[0] = static_cast<std::byte>(r);
        pixels[1] = static_cast<std::byte>(g);
        pixels[2] = static_cast<std::byte>(b);
        pixels[3] = static_cast<std::byte>(255U);
        texture.setImage(vine::intrusive_ptr<const vine::imaging::Image>(image));
    };
    const vine::intrusive_ptr<vine::graphics::Texture2D> texture(
        new vine::graphics::Texture2D(1, 1, vine::imaging::PixelFormat::Rgba8Unorm));
    fill(*texture, 255U, 0U, 0U);

    const std::shared_ptr<vine::vsg::MaterialImages> images = vine::vsg::MaterialImages::create();
    ASSERT_NE(images, nullptr);
    const auto acquire = [&](const char* step) {
        vine::vsg::detail::TextureReject reason = vine::vsg::detail::TextureReject::Ok;
        const vine::vsg::SamplerImage    result = images->acquire(texture.get(), reason);
        EXPECT_EQ(reason, vine::vsg::detail::TextureReject::Ok) << step;
        EXPECT_NE(result.view, nullptr) << step;
        return result;
    };
    const vine::vsg::SamplerImage first_images = acquire("the first fill uploads");

    const vine::intrusive_ptr<ShaderProgram> program(new ShaderProgram());
    {
        ShaderStage vertex;
        vertex.type   = ShaderStageType::Vertex;
        vertex.source = vine::String(reinterpret_cast<const char8_t*>(
            "layout(location = 0) in vec3 position;\n"
            "layout(location = 8) in vec2 texcoord;\n"
            "layout(location = 0) out vec2 uv;\n"
            "layout(push_constant) uniform PushConstants { mat4 projection; mat4 modelView; } pc;\n"
            "void main()\n"
            "{\n"
            "    uv = texcoord;\n"
            "    gl_Position = pc.projection * pc.modelView * vec4(position, 1.0);\n"
            "}\n"));
        ShaderStage fragment;
        fragment.type   = ShaderStageType::Fragment;
        fragment.source = vine::String(reinterpret_cast<const char8_t*>(
            "layout(location = 0) in vec2 uv;\n"
            "layout(location = 0) out vec4 outColor;\n"
            "layout(set = 0, binding = 0, std140) uniform VineMaterialBlock\n"
            "{\n"
            "    vec4 ambient; vec4 diffuse; vec4 specular; float shininess;\n"
            "} material;\n"
            "layout(set = 0, binding = 1) uniform sampler2D diffuseMap;\n"
            "void main() { outColor = vec4(texture(diffuseMap, uv).rgb, 1.0); }\n"));
        program->addStage(vertex);
        program->addStage(fragment);
    }
    // The entry is built for the variant the material asks for (it carries a texture): the tables key
    // their program entries by the variant, so an entry built for another text is not this entry.
    vine::vsg::ProgramVariant textured_variant;
    textured_variant.diffuse_map = true;
    ProgramFacts program_facts;
    ASSERT_EQ(buildProgramFacts(*program, textured_variant, program_facts), FactMiss::None);

    const vine::intrusive_ptr<Geometry> geometry(new Geometry());
    {
        const std::vector<float> positions{ -1.0F, -1.0F, 0.0F, 1.0F, -1.0F, 0.0F,
                                            1.0F,  1.0F,  0.0F, -1.0F, 1.0F, 0.0F };
        const std::vector<float> texcoords{ 0.0F, 0.0F, 1.0F, 0.0F, 1.0F, 1.0F, 0.0F, 1.0F };
        geometry->setPositions(
            vine::intrusive_ptr<const vine::Buffer<float>>(new vine::Buffer<float>(positions)));
        geometry->setTexcoords2(
            vine::intrusive_ptr<const vine::Buffer<float>>(new vine::Buffer<float>(texcoords)));
        geometry->setIndices(vine::intrusive_ptr<const vine::Buffer<std::uint32_t>>(
            new vine::Buffer<std::uint32_t>(std::vector<std::uint32_t>{ 0U, 1U, 2U, 0U, 2U, 3U })));
        geometry->setRevision(1U);
    }
    GeometryFacts                        geometry_facts;
    std::vector<vine::vsg::ChannelFacts> channel_storage;
    ASSERT_EQ(buildGeometryFacts(*geometry, geometry_facts, channel_storage), FactMiss::None);

    std::unique_ptr<ContentPipeline> pipelines =
        pipelineFor(geometry_facts, program_facts.shaders, program_facts.abi);
    ASSERT_NE(pipelines, nullptr);

    // The declared set of the FIRST pass; the second pass gets its own, built from the re-acquired images.
    const vine::vsg::BlockDescriptors::SampledBinding first_maps[] = {
        vine::vsg::BlockDescriptors::SampledBinding{ 1U, first_images.view, first_images.sampler }
    };
    std::unique_ptr<BlockDescriptors> first_declared =
        BlockDescriptors::forAbi(program_facts.abi, 0U, created.device, *storage, first_maps);
    ASSERT_NE(first_declared, nullptr);

    const vine::intrusive_ptr<Material> material(new Material());
    material->setTexture(texture);
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

    const auto targetFacts = [](std::unique_ptr<OffscreenTarget>& which) {
        TargetFacts entry;
        entry.target        = which.get();
        entry.wanted.width  = static_cast<int>(kSize);
        entry.wanted.height = static_cast<int>(kSize);
        entry.wanted.shape.color_formats.push_back(RenderTarget::ColorFormat::RGBA8);
        entry.current = which->instance();
        return entry;
    };
    const std::vector<TargetFacts> target_table{ targetFacts(first_target), targetFacts(second_target) };

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
    recorder.setRenderTarget(first_target.get());
    recorder.setClearPolicy(clear);
    recorder.render(commands, camera.get());
    recorder.endPass();
    recorder.endFrame();

    // Re-fill the texture: the same address, a new revision - the cache rebuilds and the new image is what
    // the second pass' descriptor is built from.
    fill(*texture, 0U, 255U, 0U);
    const vine::vsg::SamplerImage second_images = acquire("the re-fill uploads");
    EXPECT_NE(second_images.view.get(), first_images.view.get()) << "the revision decided the rebuild";

    const vine::vsg::BlockDescriptors::SampledBinding second_maps[] = {
        vine::vsg::BlockDescriptors::SampledBinding{ 1U, second_images.view, second_images.sampler }
    };
    std::unique_ptr<BlockDescriptors> second_declared =
        BlockDescriptors::forAbi(program_facts.abi, 0U, created.device, *storage, second_maps);
    ASSERT_NE(second_declared, nullptr);

    recorder.beginPass(2U);
    recorder.setRenderTarget(second_target.get());
    recorder.setClearPolicy(clear);
    recorder.render(commands, camera.get());
    recorder.endPass();
    recorder.endFrame();

    const CompiledFrame& frame = compiler.compile(recorder.description(), FrameFacts{ target_table });
    ASSERT_EQ(frame.passes.size(), 2U);

    VariantPool   pool;
    StateRegistry first_registry(pool);
    StateRegistry second_registry(pool);
    StreamUploads uploads;
    ContentDraw   first_draws(*pipelines, pool,
                              vine::vsg::detail::fetchDynamicStateEntryPoints(created.device->vk(),
                                                                             created.instance->vk()));
    ContentDraw  second_draws(*pipelines, pool,
                              vine::vsg::detail::fetchDynamicStateEntryPoints(created.device->vk(),
                                                                             created.instance->vk()));
    const std::vector<std::byte> view_block(288U, std::byte{ 0 });

    storage->beginFrame();
    BlockDescriptors* first_sets[]  = { first_declared.get() };
    BlockDescriptors* second_sets[] = { second_declared.get() };
    const auto recordPass = [&](const vine::vsg::core::CompiledPass& pass,
                                std::span<const ContentPass::Scope::Entry> halves,
                                const vine::vsg::core::RenderPassCompatibility& compatibility,
                                std::span<BlockDescriptors*> sets, StateRegistry& registry,
                                ContentPass::Scope& scope) {
        scope.entries    = halves;
        scope.registry   = &registry;
        scope.storage    = storage.get();
        scope.block_sets = sets;
        scope.uploads    = &uploads;
        ContentPass content(scope, diagnostics);
        ::vsg::ref_ptr<::vsg::Node> node;
        EXPECT_TRUE(content.record(pass, facts, compatibility, {}, view_block, node));
        return node;
    };

    ContentPass::Scope first_scope;
    ContentPass::Scope second_scope;
    // Both halves serve the textured variant: the material carries a texture, and that is the variant
    // (see api/ProgramVariant) this drawable is drawn with.
    const ContentPass::Scope::Entry first_halves[]{ ContentPass::Scope::Entry{
        vine::vsg::core::DrawKind::Content, program.get(), program_facts.revision, geometry_facts.layout,
        pipelines.get(), &first_draws, textured_variant } };
    const ContentPass::Scope::Entry second_halves[]{ ContentPass::Scope::Entry{
        vine::vsg::core::DrawKind::Content, program.get(), program_facts.revision, geometry_facts.layout,
        pipelines.get(), &second_draws, textured_variant } };
    ::vsg::ref_ptr<::vsg::Node> first_node = recordPass(
        frame.passes[0], first_halves, first_target->shape().compatibility(),
        std::span<BlockDescriptors*>(first_sets, 1U), first_registry, first_scope);
    ::vsg::ref_ptr<::vsg::Node> second_node = recordPass(
        frame.passes[1], second_halves, second_target->shape().compatibility(),
        std::span<BlockDescriptors*>(second_sets, 1U), second_registry, second_scope);
    ASSERT_TRUE(diagnostics.clean());

    VsgExecutor executor(diagnostics);
    executor.addTarget(first_target.get(), first_target.get());
    executor.addTarget(second_target.get(), second_target.get());
    auto command_graph = ::vsg::CommandGraph::create(created.device, created.queue_family);
    const PassContent packets[]{ PassContent{ frame.passes[0].pass, first_node },
                                 PassContent{ frame.passes[1].pass, second_node } };
    ASSERT_TRUE(executor.record(frame, command_graph, std::span<const PassContent>(packets, 2U)));

    ::vsg::ref_ptr<::vsg::Viewer> viewer = ::vsg::Viewer::create();
    ASSERT_NE(viewer, nullptr);
    viewer->assignRecordAndSubmitTaskAndPresentation(::vsg::CommandGraphs{ command_graph });
    ASSERT_TRUE(viewer->compile());
    viewer->advanceToNextFrame();
    viewer->handleEvents();
    viewer->recordAndSubmit();
    viewer->deviceWaitIdle();

    const auto near = [](std::uint8_t byte, double expected) {
        return std::abs(static_cast<double>(byte) - expected) <= 16.0;
    };
    const int middle = static_cast<int>(kSize) / 2;
    const Rgba8 first_pixel  = first_target->probe().pixel(middle, middle);
    const Rgba8 second_pixel = second_target->probe().pixel(middle, middle);
    EXPECT_TRUE(near(first_pixel.r, 255.0) && near(first_pixel.g, 0.0) && near(first_pixel.b, 0.0))
        << "the first pass draws the first fill (red), got (" << static_cast<int>(first_pixel.r) << ", "
        << static_cast<int>(first_pixel.g) << ", " << static_cast<int>(first_pixel.b) << ")";
    EXPECT_TRUE(near(second_pixel.r, 0.0) && near(second_pixel.g, 255.0) && near(second_pixel.b, 0.0))
        << "the second pass draws the SECOND fill (green), got (" << static_cast<int>(second_pixel.r) << ", "
        << static_cast<int>(second_pixel.g) << ", " << static_cast<int>(second_pixel.b)
        << ") - red here means the revision was ignored";
}

TEST(ContentPassTest, TwoVariantsOfOneProgramAreDrawnInOnePass)
{
    // A program's text is SEVERAL programs: its stages gate declarations and code on `#pragma
    // import_defines` names, so a material with a texture and one without are drawn by the same identity
    // compiled two ways. This pass draws both in ONE pass, through two halves that differ in nothing but
    // their VARIANT - the same program, the same revision, the same vertex layout - so a pass that picked
    // a half by (program, revision, layout) alone would hand the untextured drawable the textured half and
    // paint the texture's colour on both sides of the picture.
    //
    // The program is written the way the engine's own stages are (see builtin_forward.vert): the texcoord
    // input and the sampler sit behind `VINE_DIFFUSE_MAP`, the slot's kind is `#error`-guarded, and the
    // fragment shades the material's own colour when the map is not defined.
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

    // The texture the textured variant samples: one magenta texel, a colour no other node writes.
    const vine::intrusive_ptr<vine::graphics::Texture2D> texture(
        new vine::graphics::Texture2D(1, 1, vine::imaging::PixelFormat::Rgba8Unorm));
    {
        auto image = vine::intrusive_ptr<vine::imaging::Image>(
            new vine::imaging::Image(1, 1, vine::imaging::PixelFormat::Rgba8Unorm));
        const std::span<std::byte> pixels = image->mipData(0);
        pixels[0] = static_cast<std::byte>(255U);
        pixels[1] = static_cast<std::byte>(0U);
        pixels[2] = static_cast<std::byte>(255U);
        pixels[3] = static_cast<std::byte>(255U);
        texture->setImage(vine::intrusive_ptr<const vine::imaging::Image>(image));
    }

    const std::shared_ptr<vine::vsg::MaterialImages> images = vine::vsg::MaterialImages::create();
    ASSERT_NE(images, nullptr);
    vine::vsg::detail::TextureReject map_reason = vine::vsg::detail::TextureReject::Ok;
    const vine::vsg::SamplerImage    map        = images->acquire(texture.get(), map_reason);
    ASSERT_EQ(map_reason, vine::vsg::detail::TextureReject::Ok);

    const vine::intrusive_ptr<ShaderProgram> program(new ShaderProgram());
    {
        ShaderStage vertex;
        vertex.type   = ShaderStageType::Vertex;
        vertex.source = vine::String(reinterpret_cast<const char8_t*>(
            "#pragma import_defines (VINE_DIFFUSE_MAP, VINE_TEXCOORD_UV, VINE_TEXCOORD_CUBE)\n"
            "layout(location = 0) in vec3 position;\n"
            "#ifdef VINE_DIFFUSE_MAP\n"
            "#if defined(VINE_TEXCOORD_CUBE)\n"
            "layout(location = 8) in vec3 texcoord;\n"
            "#elif defined(VINE_TEXCOORD_UV)\n"
            "layout(location = 8) in vec2 texcoord;\n"
            "#else\n"
            "#error a sampled texcoord slot needs one kind: define VINE_TEXCOORD_UV or VINE_TEXCOORD_CUBE\n"
            "#endif\n"
            "#endif\n"
            "layout(location = 0) out vec2 uv;\n"
            "layout(push_constant) uniform PushConstants { mat4 projection; mat4 modelView; } pc;\n"
            "void main()\n"
            "{\n"
            "    gl_Position = pc.projection * pc.modelView * vec4(position, 1.0);\n"
            "#ifdef VINE_DIFFUSE_MAP\n"
            "    uv = texcoord;\n"
            "#else\n"
            "    uv = vec2(0.0);\n"
            "#endif\n"
            "}\n"));
        ShaderStage fragment;
        fragment.type   = ShaderStageType::Fragment;
        fragment.source = vine::String(reinterpret_cast<const char8_t*>(
            "#pragma import_defines (VINE_DIFFUSE_MAP, VINE_TEXCOORD_UV, VINE_TEXCOORD_CUBE)\n"
            "layout(location = 0) in vec2 uv;\n"
            "layout(location = 0) out vec4 outColor;\n"
            "layout(set = 0, binding = 0, std140) uniform VineMaterialBlock\n"
            "{\n"
            "    vec4 ambient; vec4 diffuse; vec4 specular; float shininess;\n"
            "} material;\n"
            "#ifdef VINE_DIFFUSE_MAP\n"
            "layout(set = 0, binding = 1) uniform sampler2D diffuseMap;\n"
            "#endif\n"
            "void main()\n"
            "{\n"
            "#ifdef VINE_DIFFUSE_MAP\n"
            "    outColor = vec4(texture(diffuseMap, uv).rgb, 1.0);\n"
            "#else\n"
            "    outColor = vec4(material.diffuse.rgb, 1.0);\n"
            "#endif\n"
            "}\n"));
        program->addStage(vertex);
        program->addStage(fragment);
    }

    // The two variants' facts: ONE text, two ABIs - the textured one declares the sampler the plain one
    // does not, and each compile asks for its own define list.
    vine::vsg::ProgramVariant textured_variant;
    textured_variant.diffuse_map = true;
    const vine::vsg::ProgramVariant plain_variant;

    ProgramFacts textured_facts;
    ProgramFacts plain_facts;
    ASSERT_EQ(buildProgramFacts(*program, textured_variant, textured_facts), FactMiss::None);
    ASSERT_EQ(buildProgramFacts(*program, plain_variant, plain_facts), FactMiss::None);
    ASSERT_EQ(textured_facts.abi.bindings.size(), plain_facts.abi.bindings.size() + 1U)
        << "the map's declaration is what the define adds";

    const vine::intrusive_ptr<Geometry> geometry(new Geometry());
    {
        const std::vector<float> positions{ -1.0F, -1.0F, 0.0F, 1.0F, -1.0F, 0.0F,
                                            1.0F,  1.0F,  0.0F, -1.0F, 1.0F, 0.0F };
        const std::vector<float> texcoords{ 0.0F, 0.0F, 1.0F, 0.0F, 1.0F, 1.0F, 0.0F, 1.0F };
        geometry->setPositions(
            vine::intrusive_ptr<const vine::Buffer<float>>(new vine::Buffer<float>(positions)));
        geometry->setTexcoords2(
            vine::intrusive_ptr<const vine::Buffer<float>>(new vine::Buffer<float>(texcoords)));
        geometry->setIndices(vine::intrusive_ptr<const vine::Buffer<std::uint32_t>>(
            new vine::Buffer<std::uint32_t>(std::vector<std::uint32_t>{ 0U, 1U, 2U, 0U, 2U, 3U })));
        geometry->setRevision(1U);
    }
    GeometryFacts                        geometry_facts;
    std::vector<vine::vsg::ChannelFacts> channel_storage;
    ASSERT_EQ(buildGeometryFacts(*geometry, geometry_facts, channel_storage), FactMiss::None);

    std::unique_ptr<ContentPipeline> textured_layer =
        pipelineFor(geometry_facts, textured_facts.shaders, textured_facts.abi);
    std::unique_ptr<ContentPipeline> plain_layer =
        pipelineFor(geometry_facts, plain_facts.shaders, plain_facts.abi);
    ASSERT_NE(textured_layer, nullptr);
    ASSERT_NE(plain_layer, nullptr);

    // Each variant's declared set: the material block, and - for the textured one - the map at (0,1).
    const vine::vsg::BlockDescriptors::SampledBinding textured_maps[] = {
        vine::vsg::BlockDescriptors::SampledBinding{ 1U, map.view, map.sampler }
    };
    std::unique_ptr<BlockDescriptors> textured_declared =
        BlockDescriptors::forAbi(textured_facts.abi, 0U, created.device, *storage, textured_maps);
    std::unique_ptr<BlockDescriptors> plain_declared =
        BlockDescriptors::forAbi(plain_facts.abi, 0U, created.device, *storage);
    ASSERT_NE(textured_declared, nullptr);
    ASSERT_NE(plain_declared, nullptr);

    // The two materials: one samples the texture, the other shades its own diffuse.
    const vine::intrusive_ptr<Material> textured_material(new Material());
    textured_material->setTexture(texture);
    const vine::intrusive_ptr<Material> plain_material(new Material());
    plain_material->setDiffuse(vine::Colorf(0.25F, 0.5F, 0.75F, 1.0F));

    MaterialFacts          textured_material_facts;
    MaterialFacts          plain_material_facts;
    std::vector<std::byte> textured_material_storage;
    std::vector<std::byte> plain_material_storage;
    ASSERT_EQ(buildMaterialFacts(textured_material.get(), 1U, textured_material_facts, textured_material_storage),
              FactMiss::None);
    ASSERT_EQ(buildMaterialFacts(plain_material.get(), 1U, plain_material_facts, plain_material_storage),
              FactMiss::None);

    const ProgramFacts  programs[]{ plain_facts, textured_facts };
    const GeometryFacts geometries[]{ geometry_facts };
    const MaterialFacts materials[]{ textured_material_facts, plain_material_facts };
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

    // ONE pass, two drawing calls side by side: the LEFT half is the textured material, the RIGHT half the
    // untextured one.
    RenderCommand textured_command;
    textured_command.geometry = geometry;
    textured_command.material = textured_material;
    textured_command.program  = program;
    RenderCommand plain_command;
    plain_command.geometry = geometry;
    plain_command.material = plain_material;
    plain_command.program  = program;
    const std::vector<RenderCommand> textured_commands{ textured_command };
    const std::vector<RenderCommand> plain_commands{ plain_command };

    recorder.beginFrame(FrameToken{ 1 });
    recorder.beginPass(1U);
    recorder.setRenderTarget(target.get());
    recorder.setClearPolicy(clear);
    recorder.setViewport(0, 0, static_cast<int>(kSize) / 2, static_cast<int>(kSize));
    recorder.render(textured_commands, camera.get());
    recorder.setViewport(static_cast<int>(kSize) / 2, 0, static_cast<int>(kSize) / 2, static_cast<int>(kSize));
    recorder.render(plain_commands, camera.get());
    recorder.endPass();
    recorder.endFrame();

    const CompiledFrame& frame = compiler.compile(recorder.description(), FrameFacts{ target_table });
    ASSERT_EQ(frame.passes.size(), 1U);
    ASSERT_EQ(frame.passes[0].draws.size(), 2U);

    VariantPool   pool;
    StateRegistry registry(pool);
    StreamUploads uploads;
    ContentDraw   textured_draws(*textured_layer, pool,
                                 vine::vsg::detail::fetchDynamicStateEntryPoints(created.device->vk(),
                                                                                created.instance->vk()));
    ContentDraw   plain_draws(*plain_layer, pool,
                              vine::vsg::detail::fetchDynamicStateEntryPoints(created.device->vk(),
                                                                             created.instance->vk()));

    // ONE scope holds BOTH halves - the same program, revision and layout, two variants. The textured half
    // is registered FIRST so that a pass which ignored the variant would use it for both draws.
    const ContentPass::Scope::Entry halves[]{
        ContentPass::Scope::Entry{ vine::vsg::core::DrawKind::Content, program.get(), textured_facts.revision,
                                   geometry_facts.layout, textured_layer.get(), &textured_draws,
                                   textured_variant },
        ContentPass::Scope::Entry{ vine::vsg::core::DrawKind::Content, program.get(), plain_facts.revision,
                                   geometry_facts.layout, plain_layer.get(), &plain_draws, plain_variant }
    };
    BlockDescriptors* declared_sets[] = { textured_declared.get(), plain_declared.get() };

    storage->beginFrame();
    ContentPass::Scope scope;
    scope.entries    = halves;
    scope.registry   = &registry;
    scope.storage    = storage.get();
    scope.block_sets = declared_sets;
    scope.uploads    = &uploads;
    ContentPass content(scope, diagnostics);

    const std::vector<std::byte> view_block(288U, std::byte{ 0 });
    ::vsg::ref_ptr<::vsg::Node>  content_node;
    ASSERT_TRUE(content.record(frame.passes[0], facts, target->shape().compatibility(), {}, view_block,
                               content_node));
    EXPECT_EQ(diagnostics.count(vine::graphics::DiagnosticCategory::ContentSkipped), 0U)
        << "both variants are served by the pass' own halves";

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

    const auto near = [](std::uint8_t byte, double expected) {
        return std::abs(static_cast<double>(byte) - expected) <= 16.0;
    };
    const int middle_row  = static_cast<int>(kSize) / 2;
    const Rgba8 left_half = target->probe().pixel(static_cast<int>(kSize) / 4, middle_row);
    const Rgba8 right_half = target->probe().pixel(static_cast<int>(kSize) * 3 / 4, middle_row);
    EXPECT_TRUE(near(left_half.r, 255.0) && near(left_half.g, 0.0) && near(left_half.b, 255.0))
        << "the textured variant samples the map (magenta), got (" << static_cast<int>(left_half.r) << ", "
        << static_cast<int>(left_half.g) << ", " << static_cast<int>(left_half.b) << ")";
    EXPECT_TRUE(near(right_half.r, 64.0) && near(right_half.g, 128.0) && near(right_half.b, 191.0))
        << "the untextured variant shades the material's diffuse (0.25, 0.5, 0.75), got ("
        << static_cast<int>(right_half.r) << ", " << static_cast<int>(right_half.g) << ", "
        << static_cast<int>(right_half.b)
        << ") - the map's colour here means the drawable was drawn through the OTHER variant's half";
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

TEST(ContentPassTest, TheDeclaredShadowMapBindingCarriesTheMapThePassResolved)
{
    // THE ENGINE'S SHADOW ABI on the content path: the forward program declares `shadow_map` at set 0 /
    // binding 3 (next to the material at 0 and the map at 1) and reads it under the shadow block's switch.
    // Which image that binding carries is decided BY NAME (api/ContentImages): the map the pass' plan
    // RESOLVED - the input whose target states whose shadow it is - and the white stand-in when the plan
    // resolved none, because the engine only samples the map while the block's switch is on.
    //
    // This frame draws the same program three ways, and each picture rules out one of the silent failures:
    //
    //   * the map's own pass clears its DEPTH to 0.25 - a value no other pass in the frame writes;
    //   * over [source, map] the shading must be material * 1.0 (white map) * 0.25 = (0.25, 0.125, 0.125):
    //     (255, 128, 128) would mean the binding never got the map (a stand-in bound where a map was
    //     resolved), and (0, 0, 0) the "first sampleable depth" pick - the source IS a sampleable depth
    //     too, so a picker that walks for one instead of asking the plan takes the far plane;
    //   * over [source] alone the plan resolves no map, and the same text must shine at (255, 128, 128).
    //
    // A program that declares the map NOWHERE, drawn by a pass that resolved one, takes the fourth: it
    // draws (the shadow is not a reason to lose the drawable) and the map it cannot read is REPORTED - a
    // picture the host cannot tell from "the light does not cast" is what that diagnostic exists for.
    const vine::vsg::DeviceResult created = vine::vsg::createDevice();
    if (!created.ok)
    {
        GTEST_SKIP() << "no Vulkan device available (lavapipe + X11 are needed): " << created.error.as_std_str();
    }

    // The map: DEPTH-ONLY and sampleable (the shadow-map shape), cleared to 0.25 by its own pass.
    OffscreenTarget::TargetLayout map_layout;
    map_layout.width            = kSize;
    map_layout.height           = kSize;
    map_layout.color_formats.clear();
    map_layout.depth_format     = RenderTarget::DepthFormat::D32;
    map_layout.depth_sampleable = true;
    map_layout.clear.depth      = true;
    std::unique_ptr<OffscreenTarget> map = OffscreenTarget::create(created.device, map_layout);
    ASSERT_NE(map, nullptr);
    ASSERT_TRUE(map->hasDepth());

    // ... and a second, identical one the pass also samples: sampleable, but its target states NO light, so
    // it is NOT the map (the trap above - two sampleable depths, one of them the map).
    std::unique_ptr<OffscreenTarget> plain_depth = OffscreenTarget::create(created.device, map_layout);
    ASSERT_NE(plain_depth, nullptr);

    OffscreenTarget::Layout shading_layout;
    shading_layout.width  = kSize;
    shading_layout.height = kSize;
    std::unique_ptr<OffscreenTarget> lit   = OffscreenTarget::create(created.device, shading_layout);
    std::unique_ptr<OffscreenTarget> flat  = OffscreenTarget::create(created.device, shading_layout);
    std::unique_ptr<OffscreenTarget> plain = OffscreenTarget::create(created.device, shading_layout);
    ASSERT_NE(lit, nullptr);
    ASSERT_NE(flat, nullptr);
    ASSERT_NE(plain, nullptr);

    // The map's identity and its own statements: the target says whose shadow it is, and how a fragment maps
    // into it (the producer's matrix - the consumer cannot invent it).
    const vine::intrusive_ptr<RenderTarget> map_handle(new RenderTarget());
    const vine::intrusive_ptr<RenderTarget> depth_handle(new RenderTarget());
    const vine::intrusive_ptr<RenderTarget> lit_handle(new RenderTarget());
    const vine::intrusive_ptr<RenderTarget> flat_handle(new RenderTarget());
    const vine::intrusive_ptr<RenderTarget> plain_handle(new RenderTarget());
    const vine::intrusive_ptr<vine::graphics::Light> sun =
        vine::graphics::Light::createDirectional(vine::math::Vec3d(0.0, 0.0, -1.0));
    vine::math::Mat4d light_view_projection;
    light_view_projection(0, 0) = 0.5;
    light_view_projection(1, 1) = 0.5;
    light_view_projection(2, 3) = 0.5;
    light_view_projection(3, 3) = 1.0;
    map_handle->setShadowOf(sun);
    map_handle->setProducerViewProjection(light_view_projection);
    ASSERT_TRUE(map_handle->hasProducerViewProjection());

    const auto make_program = [](const char8_t* fragment) {
        auto program = vine::intrusive_ptr<ShaderProgram>(new ShaderProgram());
        ShaderStage vertex;
        vertex.type   = ShaderStageType::Vertex;
        vertex.source = vine::String(reinterpret_cast<const char8_t*>(
            "layout(location = 0) in vec3 position;\n"
            "layout(push_constant) uniform PushConstants { mat4 projection; mat4 modelView; } pc;\n"
            "void main() { gl_Position = pc.projection * pc.modelView * vec4(position, 1.0); }\n"));
        ShaderStage fragment_stage;
        fragment_stage.type   = ShaderStageType::Fragment;
        fragment_stage.source = vine::String(fragment);
        program->addStage(vertex);
        program->addStage(fragment_stage);
        return program;
    };
    // The engine's set 0: the material at 0 (the shading's colour), the map at 1, and - in the shadowed
    // text - the map at 3. Both fragments multiply by whatever they sample, so a binding that did not arrive
    // cannot hide: the sampled value IS part of the picture.
    const vine::intrusive_ptr<ShaderProgram> shadow_program = make_program(
        u8"layout(location = 0) out vec4 outColor;\n"
        u8"layout(set = 0, binding = 0, std140) uniform VineMaterialBlock\n"
        u8"{\n"
        u8"    vec4 ambient; vec4 diffuse; vec4 specular; float shininess;\n"
        u8"} material;\n"
        u8"layout(set = 0, binding = 1) uniform sampler2D diffuseMap;\n"
        u8"layout(set = 0, binding = 3) uniform sampler2D shadow_map;\n"
        u8"void main() {\n"
        u8"    vec3  albedo = texture(diffuseMap, vec2(0.5, 0.5)).rgb;\n"
        u8"    float mapped = texture(shadow_map, vec2(0.5, 0.5)).r;\n"
        u8"    outColor = vec4(albedo * material.diffuse.rgb * mapped, 1.0);\n"
        u8"}\n");
    const vine::intrusive_ptr<ShaderProgram> unshadowed_program = make_program(
        u8"layout(location = 0) out vec4 outColor;\n"
        u8"layout(set = 0, binding = 0, std140) uniform VineMaterialBlock\n"
        u8"{\n"
        u8"    vec4 ambient; vec4 diffuse; vec4 specular; float shininess;\n"
        u8"} material;\n"
        u8"layout(set = 0, binding = 1) uniform sampler2D diffuseMap;\n"
        u8"void main() {\n"
        u8"    vec3 albedo = texture(diffuseMap, vec2(0.5, 0.5)).rgb;\n"
        u8"    outColor = vec4(albedo * material.diffuse.rgb, 1.0);\n"
        u8"}\n");
    ProgramFacts shadow_facts;
    ProgramFacts unshadowed_facts;
    ASSERT_EQ(buildProgramFacts(*shadow_program, shadow_facts), FactMiss::None);
    ASSERT_EQ(buildProgramFacts(*unshadowed_program, unshadowed_facts), FactMiss::None);
    EXPECT_TRUE(samplesShadowMap(shadow_facts.abi)) << "the text declares the map at set 0 / binding 3";
    EXPECT_FALSE(samplesShadowMap(unshadowed_facts.abi));

    std::unique_ptr<BlockStorage> storage = BlockStorage::create(created.device, BlockStorage::Layout{});
    ASSERT_NE(storage, nullptr);
    const std::shared_ptr<vine::vsg::WhiteImage> white = vine::vsg::WhiteImage::create();
    ASSERT_NE(white, nullptr);

    const vine::intrusive_ptr<Geometry> geometry(new Geometry());
    {
        Triangle triangle;
        geometry->setPositions(triangle.positions);
        geometry->setIndices(triangle.indices);
        geometry->setRevision(1U);
    }
    GeometryFacts                        geometry_facts;
    std::vector<vine::vsg::ChannelFacts> channel_storage;
    ASSERT_EQ(buildGeometryFacts(*geometry, geometry_facts, channel_storage), FactMiss::None);

    const vine::intrusive_ptr<Material> material(new Material());
    material->setDiffuse(vine::Colorf(1.0F, 0.5F, 0.5F, 1.0F));
    MaterialFacts          material_facts;
    std::vector<std::byte> material_storage;
    ASSERT_EQ(buildMaterialFacts(material.get(), 1U, material_facts, material_storage), FactMiss::None);

    const ProgramFacts  programs[]{ shadow_facts, unshadowed_facts };
    const GeometryFacts geometries[]{ geometry_facts };
    const MaterialFacts materials[]{ material_facts };
    ContentFacts        facts;
    facts.programs   = programs;
    facts.geometries = geometries;
    facts.materials  = materials;

    FrameArena    arena{ 128 * 1024 };
    Diagnostics   diagnostics;
    Observe       observe;
    FrameRecorder recorder{ arena, diagnostics, observe };
    FrameCompiler compiler{ arena, diagnostics, observe };

    const vine::intrusive_ptr<vine::graphics::Camera> camera(new vine::graphics::Camera());
    camera->setViewMatrixAsLookAt(vine::math::Vec3d(0.0, 0.0, 1.5), vine::math::Vec3d(0.0, 0.0, 0.0),
                                  vine::math::Vec3d(0.0, 1.0, 0.0));
    camera->setProjectionMatrixAsOrtho(-1.0, 1.0, -1.0, 1.0, 0.5, 4.0);

    const auto targetFacts = [](OffscreenTarget& which, const void* handle) {
        TargetFacts entry;
        entry.target        = handle;
        entry.wanted.width  = static_cast<int>(kSize);
        entry.wanted.height = static_cast<int>(kSize);
        entry.wanted.shape  = which.shape();
        entry.current       = which.instance();
        entry.depth.has_depth = which.hasDepth();
        entry.depth.promotion = which.hasDepth();
        return entry;
    };
    TargetFacts map_facts  = targetFacts(*map, map_handle.get());
    map_facts.shadow.light                 = sun.get();
    map_facts.shadow.has_view_projection   = map_handle->hasProducerViewProjection();
    map_facts.shadow.view_projection       = map_handle->producerViewProjection();
    const std::vector<TargetFacts> target_table{ targetFacts(*plain_depth, depth_handle.get()), map_facts,
                                                 targetFacts(*lit, lit_handle.get()),
                                                 targetFacts(*flat, flat_handle.get()),
                                                 targetFacts(*plain, plain_handle.get()) };

    // Pass 1 publishes the OTHER sampleable depth (cleared to the reverse-Z far plane, 0.0), pass 2 the map
    // (0.25): both are real images in the layout a descriptor declares, and only one of them is the map.
    ClearPolicy depth_clear;
    depth_clear.depth       = true;
    depth_clear.depth_value = 0.0F;
    ClearPolicy map_clear;
    map_clear.depth       = true;
    map_clear.depth_value = 0.25F;
    ClearPolicy clear;
    clear.color          = true;
    clear.color_value[0] = kClear[0];
    clear.color_value[1] = kClear[1];
    clear.color_value[2] = kClear[2];
    clear.color_value[3] = 1.0F;
    clear.depth          = true;
    clear.depth_value    = 0.0F;

    RenderCommand shadow_command;
    shadow_command.geometry = geometry;
    shadow_command.material = material;
    shadow_command.program  = shadow_program;
    RenderCommand unshadowed_command;
    unshadowed_command.geometry = geometry;
    unshadowed_command.material = material;
    unshadowed_command.program  = unshadowed_program;
    const std::vector<RenderCommand> shadow_commands{ shadow_command };
    const std::vector<RenderCommand> unshadowed_commands{ unshadowed_command };
    const RenderTarget*              both_inputs[]{ depth_handle.get(), map_handle.get() };
    const RenderTarget*              depth_only[]{ depth_handle.get() };

    recorder.beginFrame(FrameToken{ 1 });
    recorder.beginPass(1U);
    recorder.setRenderTarget(depth_handle.get());
    recorder.setClearPolicy(depth_clear);
    recorder.endPass();
    recorder.beginPass(2U);
    recorder.setRenderTarget(map_handle.get());
    recorder.setClearPolicy(map_clear);
    recorder.endPass();
    recorder.beginPass(3U);
    recorder.setRenderTarget(lit_handle.get());
    recorder.setClearPolicy(clear);
    recorder.setPassInputs(std::span<const RenderTarget* const>(both_inputs, 2U));
    recorder.render(shadow_commands, camera.get());
    recorder.endPass();
    recorder.beginPass(4U);
    recorder.setRenderTarget(flat_handle.get());
    recorder.setClearPolicy(clear);
    recorder.setPassInputs(std::span<const RenderTarget* const>(depth_only, 1U));
    recorder.render(shadow_commands, camera.get());
    recorder.endPass();
    recorder.beginPass(5U);
    recorder.setRenderTarget(plain_handle.get());
    recorder.setClearPolicy(clear);
    recorder.setPassInputs(std::span<const RenderTarget* const>(both_inputs, 2U));
    recorder.render(unshadowed_commands, camera.get());
    recorder.endPass();
    recorder.endFrame();

    const CompiledFrame& frame = compiler.compile(recorder.description(), FrameFacts{ target_table });
    ASSERT_EQ(frame.passes.size(), 5U);
    ASSERT_EQ(frame.passes[2].shadow.light, static_cast<const void*>(sun.get()))
        << "the plan resolved the map from what the target stated";
    EXPECT_EQ(frame.passes[3].shadow.light, nullptr) << "no map among this pass' inputs";
    for (const auto& pass : frame.passes)
    {
        for (const auto& draw : pass.draws)
        {
            EXPECT_TRUE(draw.camera.present) << "every pass announced the camera its push comes from";
        }
    }

    // The by-name pick: the resolve says which offered image the map is, and it is the SECOND sampleable
    // depth this pass reads - not the first one.
    const std::vector<std::byte> view_block(288U, std::byte{ 0 });
    const InputImages lit_inputs[]{ InputImages{ {}, plain_depth->depthView() },
                                    InputImages{ {}, map->depthView() } };
    const auto shadow_layer = pipelineFor(geometry_facts, shadow_facts.shaders, shadow_facts.abi);
    ASSERT_NE(shadow_layer, nullptr);
    vine::vsg::SamplerImage shadow_image;
    ASSERT_TRUE(shadowImageOf(frame.passes[2], std::span<const InputImages>(lit_inputs, 2U),
                              shadow_layer->depthSampler(), shadow_image));
    EXPECT_EQ(shadow_image.view, map->depthView()) << "the map, not the other sampleable depth";
    EXPECT_EQ(shadow_image.sampler, shadow_layer->depthSampler());

    // The caller's sets, built per pass from what the policy picked: the map where the text names it, the
    // white stand-in where the pass resolved none, and the material's own fallback for `diffuseMap`.
    const vine::vsg::BlockDescriptors::SampledBinding with_map[] = {
        { 1U, white->view(), white->sampler() }, { 3U, shadow_image.view, shadow_image.sampler }
    };
    const vine::vsg::BlockDescriptors::SampledBinding with_stand_in[] = {
        { 1U, white->view(), white->sampler() }, { 3U, white->view(), white->sampler() }
    };
    const vine::vsg::BlockDescriptors::SampledBinding map_only[] = {
        { 1U, white->view(), white->sampler() }
    };
    std::unique_ptr<BlockDescriptors> declares_map =
        BlockDescriptors::forAbi(shadow_facts.abi, 0U, created.device, *storage, with_map);
    std::unique_ptr<BlockDescriptors> declares_stand_in =
        BlockDescriptors::forAbi(shadow_facts.abi, 0U, created.device, *storage, with_stand_in);
    std::unique_ptr<BlockDescriptors> declares_none =
        BlockDescriptors::forAbi(unshadowed_facts.abi, 0U, created.device, *storage, map_only);
    ASSERT_NE(declares_map, nullptr);
    ASSERT_NE(declares_stand_in, nullptr);
    ASSERT_NE(declares_none, nullptr);
    BlockDescriptors* map_sets[]{ declares_map.get() };
    BlockDescriptors* stand_in_sets[]{ declares_stand_in.get() };
    BlockDescriptors* none_sets[]{ declares_none.get() };

    VariantPool   pool;
    StreamUploads uploads;
    const auto    entry_points =
        vine::vsg::detail::fetchDynamicStateEntryPoints(created.device->vk(), created.instance->vk());
    ContentDraw shadow_draws(*shadow_layer, pool, entry_points);
    std::unique_ptr<ContentPipeline> unshadowed_layer =
        pipelineFor(geometry_facts, unshadowed_facts.shaders, unshadowed_facts.abi);
    ASSERT_NE(unshadowed_layer, nullptr);
    ContentDraw unshadowed_draws(*unshadowed_layer, pool, entry_points);
    const ContentPass::Scope::Entry shadow_halves[]{ ContentPass::Scope::Entry{
        vine::vsg::core::DrawKind::Content, shadow_program.get(), shadow_facts.revision, geometry_facts.layout,
        shadow_layer.get(), &shadow_draws } };
    const ContentPass::Scope::Entry unshadowed_halves[]{ ContentPass::Scope::Entry{
        vine::vsg::core::DrawKind::Content, unshadowed_program.get(), unshadowed_facts.revision,
        geometry_facts.layout, unshadowed_layer.get(), &unshadowed_draws } };

    storage->beginFrame();
    // One scope per pass, and each carries the half ITS command names: the two programs declare different
    // sets (the shadowed one names the map), so a scope that offered both to one pass would serve the wrong
    // set's descriptors to whichever half the lookup did not pick.
    const auto recordPass = [&](const vine::vsg::core::CompiledPass& pass,
                                std::span<const ContentPass::Scope::Entry> halves,
                                const vine::vsg::core::RenderPassCompatibility& compatibility,
                                std::span<const InputImages> inputs, std::span<BlockDescriptors* const> sets,
                                StateRegistry& registry) {
        ContentPass::Scope scope;
        scope.entries    = halves;
        scope.registry   = &registry;
        scope.storage    = storage.get();
        scope.block_sets = sets;
        scope.uploads    = &uploads;
        ContentPass content(scope, diagnostics);
        ::vsg::ref_ptr<::vsg::Node> node;
        EXPECT_TRUE(content.record(pass, facts, compatibility, inputs, view_block, node))
            << "the declared set is the one the text names";
        return node;
    };
    StateRegistry lit_registry(pool);
    StateRegistry flat_registry(pool);
    StateRegistry plain_registry(pool);
    ::vsg::ref_ptr<::vsg::Node> lit_node =
        recordPass(frame.passes[2], shadow_halves, lit->shape().compatibility(),
                   std::span<const InputImages>(lit_inputs, 2U), map_sets, lit_registry);
    const InputImages flat_inputs[]{ InputImages{ {}, plain_depth->depthView() } };
    ::vsg::ref_ptr<::vsg::Node> flat_node =
        recordPass(frame.passes[3], shadow_halves, flat->shape().compatibility(),
                   std::span<const InputImages>(flat_inputs, 1U), stand_in_sets, flat_registry);
    EXPECT_TRUE(diagnostics.clean()) << "a pass without a resolved map binds the white stand-in, and says nothing";

    // The program that cannot read the map: the draw is recorded, and the shadow it will not shade is said
    // once (the picture below is the proof the draw survived).
    const std::uint64_t before_report =
        diagnostics.count(vine::graphics::DiagnosticCategory::UnsupportedRequest);
    ::vsg::ref_ptr<::vsg::Node> plain_node =
        recordPass(frame.passes[4], unshadowed_halves, plain->shape().compatibility(),
                   std::span<const InputImages>(lit_inputs, 2U), none_sets, plain_registry);
    EXPECT_EQ(diagnostics.count(vine::graphics::DiagnosticCategory::UnsupportedRequest), before_report + 1U)
        << "the map the pass resolved and the program cannot read is reported";
    EXPECT_EQ(diagnostics.count(vine::graphics::DiagnosticCategory::ContentSkipped), 0U)
        << "and the drawable is NOT lost over it";

    VsgExecutor executor(diagnostics);
    executor.addTarget(depth_handle.get(), plain_depth.get());
    executor.addTarget(map_handle.get(), map.get());
    executor.addTarget(lit_handle.get(), lit.get());
    executor.addTarget(flat_handle.get(), flat.get());
    executor.addTarget(plain_handle.get(), plain.get());
    auto command_graph = ::vsg::CommandGraph::create(created.device, created.queue_family);
    command_graph->addChild(white->fill());
    const PassContent packets[]{ PassContent{ frame.passes[2].pass, lit_node },
                                 PassContent{ frame.passes[3].pass, flat_node },
                                 PassContent{ frame.passes[4].pass, plain_node } };
    ASSERT_TRUE(executor.record(frame, command_graph, std::span<const PassContent>(packets, 3U)));

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
    const Rgba8 over_the_map = lit->probe().pixel(static_cast<int>(kSize) / 2, static_cast<int>(kSize) / 2);
    EXPECT_TRUE(near(over_the_map.r, 0.25) && near(over_the_map.g, 0.125) && near(over_the_map.b, 0.125))
        << "the map the pass resolved must be read where the text declares it, got ("
        << static_cast<int>(over_the_map.r) << ", " << static_cast<int>(over_the_map.g) << ", "
        << static_cast<int>(over_the_map.b) << ") - (255, 128, 128) means the stand-in was bound instead, "
                                                      "(0, 0, 0) the OTHER sampleable depth";

    const Rgba8 without_a_map = flat->probe().pixel(static_cast<int>(kSize) / 2, static_cast<int>(kSize) / 2);
    EXPECT_TRUE(near(without_a_map.r, 1.0) && near(without_a_map.g, 0.5) && near(without_a_map.b, 0.5))
        << "a pass that resolved no map shades through the white stand-in, got ("
        << static_cast<int>(without_a_map.r) << ", " << static_cast<int>(without_a_map.g) << ", "
        << static_cast<int>(without_a_map.b) << ")";

    const Rgba8 unshadowed = plain->probe().pixel(static_cast<int>(kSize) / 2, static_cast<int>(kSize) / 2);
    EXPECT_TRUE(near(unshadowed.r, 1.0) && near(unshadowed.g, 0.5) && near(unshadowed.b, 0.5))
        << "a program that declares no map still draws, got (" << static_cast<int>(unshadowed.r) << ", "
        << static_cast<int>(unshadowed.g) << ", " << static_cast<int>(unshadowed.b) << ")";
}

TEST(ContentPassTest, TheEnginesScreenLightingShadesTheGbufferThroughTheMapItsTextDeclares)
{
    // THE ENGINE'S OWN SCREEN LIGHTING, end to end. `BuiltinShaders::shadowedDeferredLightProgram` is the text
    // the engine's deferred renderer draws its lighting pass with, and it declares its shadow ABI by hand:
    // the map at binding 5 (the source's four colours take 0..3 and its depth would take 4) and its block at
    // 6. This frame draws it over a four-attachment G-buffer, twice - once with a map cleared to the far
    // plane (nothing casts: the sun reaches every fragment) and once cleared to 1.0 (a caster at the near
    // plane: the sun is scaled away) - plus the engine's UNSHADOWED variant over the same G-buffer.
    //
    // The G-buffer has NO depth attachment, so the source's depth is not sampleable and the set has NOTHING
    // at binding 4: a layer that just appended the inputs' textures would bind the map one binding too low
    // and the shader would read undefined data where its text hard-codes 5.
    //
    // The numbers: albedo (0.5, 0.25, 1.0), view normal (0, 0, 1), view position (0, 0, -2), an ambient light
    // (0.1, 0.1, 0.1) and ONE directional sun (0.4, 0.4, 0) whose view direction is (0, 0, -1) with the camera
    // at +Z - so ndl = 1 and the specular term is zero (the G-buffer's specular is black). Lit is
    // (0.25, 0.125, 0.1) and shadowed is (0.05, 0.025, 0.1): the sun's half of the picture IS the map.
    //
    // The writer's normal attachment carries the material's SHININESS in its alpha, the engine's own
    // G-buffer convention (`builtin_gbuffer.frag` writes `clamp(shininess / 256, 0, 1)` there), and this
    // material's shininess is ZERO (a legitimate one - the SDK's default is 32, which the L2 measured as
    // "an always-on blend scaled every stored normal to 12.5% of its value"). Zero is the case that shows
    // the rule: an alpha of 0 blended over the attachment's transparent-black clear leaves the WHOLE
    // normal at 0, the shading reads it as a direction it cannot normalize, and the sun disappears -
    // which is what this picture asserted before the delivery learned to write several attachments
    // unblended (see makeDynamicStateCommand's opaque rule).
    const vine::vsg::DeviceResult created = vine::vsg::createDevice();
    if (!created.ok)
    {
        GTEST_SKIP() << "no Vulkan device available (lavapipe + X11 are needed): " << created.error.as_std_str();
    }

    // The G-buffer: the engine's four attachments AND their formats (`defaultGbufferTarget`): albedo and
    // specular are RGBA8, while the view normal and the view position are RGBA16F - an eight-bit attachment
    // clamps the position's negative z (the view's own axis) to zero, which the lighting reads as its
    // "nothing was drawn" background.
    OffscreenTarget::TargetLayout gbuffer_layout;
    gbuffer_layout.width         = kSize;
    gbuffer_layout.height        = kSize;
    gbuffer_layout.color_formats = { RenderTarget::ColorFormat::RGBA8, RenderTarget::ColorFormat::RGBA16F,
                                     RenderTarget::ColorFormat::RGBA8, RenderTarget::ColorFormat::RGBA16F };
    std::unique_ptr<OffscreenTarget> gbuffer = OffscreenTarget::create(created.device, gbuffer_layout);
    ASSERT_NE(gbuffer, nullptr);
    ASSERT_EQ(gbuffer->colorAttachmentCount(), 4U);

    // The maps: depth-only and sampleable (the shadow-map shape), each CLEARED by its own draw-less pass
    // below - a map whose clearing pass is not recorded compares against whatever the image happened to
    // hold, so both are part of the recorded frame.
    OffscreenTarget::TargetLayout map_layout;
    map_layout.width            = kSize;
    map_layout.height           = kSize;
    map_layout.color_formats.clear();
    map_layout.depth_format     = RenderTarget::DepthFormat::D32;
    map_layout.depth_sampleable = true;
    map_layout.clear.depth      = true;
    std::unique_ptr<OffscreenTarget> lit_map       = OffscreenTarget::create(created.device, map_layout);
    std::unique_ptr<OffscreenTarget> shadowed_map  = OffscreenTarget::create(created.device, map_layout);
    ASSERT_NE(lit_map, nullptr);
    ASSERT_NE(shadowed_map, nullptr);

    OffscreenTarget::Layout shade_layout;
    shade_layout.width  = kSize;
    shade_layout.height = kSize;
    std::unique_ptr<OffscreenTarget> lit      = OffscreenTarget::create(created.device, shade_layout);
    std::unique_ptr<OffscreenTarget> shadowed = OffscreenTarget::create(created.device, shade_layout);
    std::unique_ptr<OffscreenTarget> flat     = OffscreenTarget::create(created.device, shade_layout);
    std::unique_ptr<OffscreenTarget> plain    = OffscreenTarget::create(created.device, shade_layout);
    ASSERT_NE(lit, nullptr);
    ASSERT_NE(shadowed, nullptr);
    ASSERT_NE(flat, nullptr);
    ASSERT_NE(plain, nullptr);

    const vine::intrusive_ptr<RenderTarget> gbuffer_handle(new RenderTarget());
    const vine::intrusive_ptr<RenderTarget> lit_map_handle(new RenderTarget());
    const vine::intrusive_ptr<RenderTarget> shadowed_map_handle(new RenderTarget());
    const vine::intrusive_ptr<RenderTarget> lit_handle(new RenderTarget());
    const vine::intrusive_ptr<RenderTarget> shadowed_handle(new RenderTarget());
    const vine::intrusive_ptr<RenderTarget> flat_handle(new RenderTarget());
    const vine::intrusive_ptr<RenderTarget> plain_handle(new RenderTarget());

    const vine::intrusive_ptr<vine::graphics::Light> sun =
        vine::graphics::Light::createDirectional(vine::math::Vec3d(0.0, 0.0, -1.0));
    sun->setColor(vine::Colorf(0.4F, 0.4F, 0.0F, 1.0F));
    sun->setIntensity(1.0F);
    sun->setCastShadow(true);
    const vine::intrusive_ptr<vine::graphics::Light> ambient = vine::graphics::Light::createAmbient();
    ambient->setColor(vine::Colorf(0.1F, 0.1F, 0.1F, 1.0F));
    ambient->setIntensity(1.0F);

    // The producer's matrix: x/y from the world position and a constant light-space depth, so every fragment
    // of the receiver lands inside the map's rectangle with the same comparison value (see ShadowBlockTest).
    vine::math::Mat4d light_view_projection;   // a diagonal of ones is the base
    light_view_projection(0, 0) = 0.5;
    light_view_projection(1, 1) = 0.5;
    light_view_projection(2, 2) = 0.0;   // the row keeps NO position: the light-space depth is constant
    light_view_projection(2, 3) = 0.5;   // and it starts AT the depth the shader's `frag` reads as 0.25
    light_view_projection(3, 3) = 1.0;
    lit_map_handle->setShadowOf(sun);
    lit_map_handle->setProducerViewProjection(light_view_projection);
    shadowed_map_handle->setShadowOf(sun);
    shadowed_map_handle->setProducerViewProjection(light_view_projection);
    ASSERT_TRUE(lit_map_handle->hasProducerViewProjection());

    const vine::intrusive_ptr<vine::graphics::Camera> camera(new vine::graphics::Camera());
    camera->setViewMatrixAsLookAt(vine::math::Vec3d(0.0, 0.0, 5.0), vine::math::Vec3d(0.0, 0.0, 0.0),
                                  vine::math::Vec3d(0.0, 1.0, 0.0));

    // The G-buffer writer: a CONTENT program that writes all four attachments from a clip-space triangle. Its
    // text declares NOTHING (no blocks, no samplers, no push), so the content path's own arrangement is the
    // whole interface it needs. Its outputs are the engine's own four (albedo / normal with the shininess
    // in alpha / specular / position), so what the lighting pass below reads is the shape a real G-buffer
    // has.
    const vine::intrusive_ptr<ShaderProgram> writer_program(new ShaderProgram());
    {
        ShaderStage vertex;
        vertex.type   = ShaderStageType::Vertex;
        vertex.source = vine::String(reinterpret_cast<const char8_t*>(
            "layout(location = 0) in vec3 position;\n"
            "void main() { gl_Position = vec4(position.xy, 0.5, 1.0); }\n"));
        ShaderStage fragment;
        fragment.type   = ShaderStageType::Fragment;
        fragment.source = vine::String(reinterpret_cast<const char8_t*>(
            "layout(location = 0) out vec4 albedo_out;\n"
            "layout(location = 1) out vec4 normal_out;\n"
            "layout(location = 2) out vec4 specular_out;\n"
            "layout(location = 3) out vec4 position_out;\n"
            "void main() {\n"
            "    albedo_out   = vec4(0.5, 0.25, 1.0, 1.0);\n"
            "    normal_out   = vec4(0.0, 0.0, 1.0, 0.0);\n"
            "    specular_out = vec4(0.0, 0.0, 0.0, 1.0);\n"
            "    position_out = vec4(0.0, 0.0, -2.0, 1.0);\n"
            "}\n"));
        writer_program->addStage(vertex);
        writer_program->addStage(fragment);
    }
    ProgramFacts writer_facts;
    ASSERT_EQ(buildProgramFacts(*writer_program, writer_facts), FactMiss::None);

    // The engine's own lighting programs, both variants - servable only because the layer's set follows what
    // their texts declare.
    const vine::intrusive_ptr<ShaderProgram> shadowed_program = vine::graphics::shadowedDeferredLightProgram();
    const vine::intrusive_ptr<ShaderProgram> unshadowed_program = vine::graphics::deferredLightProgram();
    ASSERT_NE(shadowed_program, nullptr);
    ASSERT_NE(unshadowed_program, nullptr);
    ProgramFacts shadowed_facts;
    ProgramFacts unshadowed_facts;
    ASSERT_EQ(buildScreenProgramFacts(*shadowed_program, shadowed_facts), FactMiss::None);
    ASSERT_EQ(buildScreenProgramFacts(*unshadowed_program, unshadowed_facts), FactMiss::None);

    // The layers: the writer with FOUR colour attachments, the two screen programs from their declarations.
    const ContentPipeline::VertexBinding   binding{ 0U, sizeof(float) * 3U, false };
    const ContentPipeline::VertexAttribute attribute{ 0U, 0U, VK_FORMAT_R32G32B32_SFLOAT, 0U };
    ContentPipeline::Settings              writer_settings;
    writer_settings.color_attachments = 4U;
    std::unique_ptr<ContentPipeline> writer_layer =
        ContentPipeline::create(writer_facts.abi, std::span<const ContentPipeline::VertexBinding>(&binding, 1U),
                                std::span<const ContentPipeline::VertexAttribute>(&attribute, 1U),
                                writer_facts.shaders, writer_settings);
    ASSERT_NE(writer_layer, nullptr);
    std::unique_ptr<ContentPipeline> shadowed_layer =
        ContentPipeline::createScreen(shadowed_facts.abi, shadowed_facts.shaders);
    std::unique_ptr<ContentPipeline> unshadowed_layer =
        ContentPipeline::createScreen(unshadowed_facts.abi, unshadowed_facts.shaders);
    ASSERT_NE(shadowed_layer, nullptr) << "the engine's shadowed lighting program must be servable";
    ASSERT_NE(unshadowed_layer, nullptr);

    std::unique_ptr<BlockStorage> storage = BlockStorage::create(created.device, BlockStorage::Layout{});
    ASSERT_NE(storage, nullptr);

    const vine::intrusive_ptr<Geometry> geometry(new Geometry());
    {
        const std::vector<float> positions{ -1.0F, -1.0F, 0.0F, 3.0F, -1.0F, 0.0F, -1.0F, 3.0F, 0.0F };
        geometry->setPositions(
            vine::intrusive_ptr<const vine::Buffer<float>>(new vine::Buffer<float>(positions)));
        geometry->setIndices(vine::intrusive_ptr<const vine::Buffer<std::uint32_t>>(
            new vine::Buffer<std::uint32_t>(std::vector<std::uint32_t>{ 0U, 1U, 2U })));
        geometry->setRevision(1U);
    }
    GeometryFacts                        geometry_facts;
    std::vector<vine::vsg::ChannelFacts> channel_storage;
    ASSERT_EQ(buildGeometryFacts(*geometry, geometry_facts, channel_storage), FactMiss::None);

    const vine::intrusive_ptr<Material> material(new Material());
    MaterialFacts                       material_facts;
    std::vector<std::byte>              material_storage;
    ASSERT_EQ(buildMaterialFacts(material.get(), 1U, material_facts, material_storage), FactMiss::None);

    const ProgramFacts  programs[]{ writer_facts, shadowed_facts, unshadowed_facts };
    const GeometryFacts geometries[]{ geometry_facts };
    const MaterialFacts materials[]{ material_facts };
    ContentFacts        facts;
    facts.programs   = programs;
    facts.geometries = geometries;
    facts.materials  = materials;

    FrameArena    arena{ 128 * 1024 };
    Diagnostics   diagnostics;
    Observe       observe;
    FrameRecorder recorder{ arena, diagnostics, observe };
    FrameCompiler compiler{ arena, diagnostics, observe };

    const auto targetFacts = [](OffscreenTarget& which, const void* handle) {
        TargetFacts entry;
        entry.target          = handle;
        entry.wanted.width    = static_cast<int>(kSize);
        entry.wanted.height   = static_cast<int>(kSize);
        entry.wanted.shape    = which.shape();
        entry.current         = which.instance();
        entry.depth.has_depth = which.hasDepth();
        entry.depth.promotion = which.hasDepth();
        return entry;
    };
    TargetFacts lit_map_facts      = targetFacts(*lit_map, lit_map_handle.get());
    TargetFacts shadowed_map_facts = targetFacts(*shadowed_map, shadowed_map_handle.get());
    lit_map_facts.shadow.light              = sun.get();
    lit_map_facts.shadow.has_view_projection = true;
    lit_map_facts.shadow.view_projection     = light_view_projection;
    shadowed_map_facts.shadow.light              = sun.get();
    shadowed_map_facts.shadow.has_view_projection = true;
    shadowed_map_facts.shadow.view_projection     = light_view_projection;
    const std::vector<TargetFacts> target_table{ targetFacts(*gbuffer, gbuffer_handle.get()),
                                                 lit_map_facts,
                                                 shadowed_map_facts,
                                                 targetFacts(*lit, lit_handle.get()),
                                                 targetFacts(*shadowed, shadowed_handle.get()),
                                                 targetFacts(*flat, flat_handle.get()),
                                                 targetFacts(*plain, plain_handle.get()) };

    ClearPolicy shade_clear;
    shade_clear.color          = true;
    shade_clear.color_value[0] = kClear[0];
    shade_clear.color_value[1] = kClear[1];
    shade_clear.color_value[2] = kClear[2];
    shade_clear.color_value[3] = 1.0F;
    shade_clear.depth          = true;
    shade_clear.depth_value    = 0.0F;
    ClearPolicy lit_map_clear;
    lit_map_clear.depth       = true;
    lit_map_clear.depth_value = 0.0F;   // the far plane: nothing casts, the sun reaches the fragment
    ClearPolicy shadowed_map_clear;
    shadowed_map_clear.depth       = true;
    shadowed_map_clear.depth_value = 1.0F;   // a caster at the near plane: the fragment is behind it

    RenderCommand writer_command;
    writer_command.geometry = geometry;
    writer_command.material = material;
    writer_command.program  = writer_program;
    const std::vector<RenderCommand> writer_commands{ writer_command };
    const vine::graphics::Light* const shading_lights[]{ ambient.get(), sun.get() };
    const RenderTarget*              gbuffer_input[]{ gbuffer_handle.get() };
    const RenderTarget*              gbuffer_and_lit_map_inputs[]{ gbuffer_handle.get(), lit_map_handle.get() };
    const RenderTarget*              gbuffer_and_shadowed_map_inputs[]{ gbuffer_handle.get(),
                                                                        shadowed_map_handle.get() };

    recorder.beginFrame(FrameToken{ 1 });
    recorder.beginPass(1U);
    recorder.setRenderTarget(gbuffer_handle.get());
    recorder.setClearPolicy(shade_clear);
    recorder.render(writer_commands, camera.get());
    recorder.endPass();
    recorder.beginPass(2U);
    recorder.setRenderTarget(lit_map_handle.get());
    recorder.setClearPolicy(lit_map_clear);
    recorder.endPass();
    recorder.beginPass(3U);
    recorder.setRenderTarget(shadowed_map_handle.get());
    recorder.setClearPolicy(shadowed_map_clear);
    recorder.endPass();
    recorder.beginPass(4U);
    recorder.setRenderTarget(lit_handle.get());
    recorder.setClearPolicy(shade_clear);
    recorder.setPassInputs(std::span<const RenderTarget* const>(gbuffer_and_lit_map_inputs, 2U));
    recorder.setLights(std::span<const vine::graphics::Light* const>(shading_lights, 2U));
    recorder.drawScreenProgram(gbuffer_handle.get(), shadowed_program.get(), camera.get());
    recorder.endPass();
    recorder.beginPass(5U);
    recorder.setRenderTarget(shadowed_handle.get());
    recorder.setClearPolicy(shade_clear);
    recorder.setPassInputs(std::span<const RenderTarget* const>(gbuffer_and_shadowed_map_inputs, 2U));
    recorder.setLights(std::span<const vine::graphics::Light* const>(shading_lights, 2U));
    recorder.drawScreenProgram(gbuffer_handle.get(), shadowed_program.get(), camera.get());
    recorder.endPass();
    recorder.beginPass(6U);
    recorder.setRenderTarget(flat_handle.get());
    recorder.setClearPolicy(shade_clear);
    recorder.setPassInputs(std::span<const RenderTarget* const>(gbuffer_input, 1U));
    recorder.setLights(std::span<const vine::graphics::Light* const>(shading_lights, 2U));
    recorder.drawScreenProgram(gbuffer_handle.get(), unshadowed_program.get(), camera.get());
    recorder.endPass();
    recorder.beginPass(7U);
    recorder.setRenderTarget(plain_handle.get());
    recorder.setClearPolicy(shade_clear);
    recorder.setPassInputs(std::span<const RenderTarget* const>(gbuffer_and_lit_map_inputs, 2U));
    recorder.setLights(std::span<const vine::graphics::Light* const>(shading_lights, 2U));
    recorder.drawScreenProgram(gbuffer_handle.get(), unshadowed_program.get(), camera.get());
    recorder.endPass();
    recorder.endFrame();

    const CompiledFrame& frame = compiler.compile(recorder.description(), FrameFacts{ target_table });
    ASSERT_EQ(frame.passes.size(), 7U);
    EXPECT_EQ(frame.passes[3].shadow.light, static_cast<const void*>(sun.get()))
        << "the lit pass resolved the map its target states";
    EXPECT_EQ(frame.passes[4].shadow.light, static_cast<const void*>(sun.get()));
    EXPECT_EQ(frame.passes[5].shadow.light, nullptr) << "the flat pass declared no map";
    EXPECT_EQ(frame.passes[6].shadow.light, static_cast<const void*>(sun.get()));

    VariantPool   pool;
    StreamUploads uploads;
    const auto    entry_points =
        vine::vsg::detail::fetchDynamicStateEntryPoints(created.device->vk(), created.instance->vk());
    ContentDraw writer_draws(*writer_layer, pool, entry_points);
    ContentDraw shadowed_draws(*shadowed_layer, pool, entry_points);
    ContentDraw unshadowed_draws(*unshadowed_layer, pool, entry_points);
    const ContentPass::Scope::Entry writer_halves[]{ ContentPass::Scope::Entry{
        vine::vsg::core::DrawKind::Content, writer_program.get(), writer_facts.revision, geometry_facts.layout,
        writer_layer.get(), &writer_draws } };
    const ContentPass::Scope::Entry shadowed_halves[]{ ContentPass::Scope::Entry{
        vine::vsg::core::DrawKind::Screen, shadowed_program.get(), shadowed_facts.revision, {},
        shadowed_layer.get(), &shadowed_draws } };
    const ContentPass::Scope::Entry unshadowed_halves[]{ ContentPass::Scope::Entry{
        vine::vsg::core::DrawKind::Screen, unshadowed_program.get(), unshadowed_facts.revision, {},
        unshadowed_layer.get(), &unshadowed_draws } };
    const std::vector<std::byte> view_block(288U, std::byte{ 0 });
    storage->beginFrame();
    // One scope per pass, and each carries the half ITS call draws with - the writer is a content half, the
    // two lighting programs are screen halves, and a screen half's set is the pass' own (see ContentPass).
    std::vector<std::unique_ptr<StateRegistry>> registries;
    registries.reserve(frame.passes.size());
    for (std::size_t index = 0; index < frame.passes.size(); ++index) {
        registries.push_back(std::make_unique<StateRegistry>(pool));
    }
    const auto recordPass = [&](const vine::vsg::core::CompiledPass& pass,
                                std::span<const ContentPass::Scope::Entry> halves,
                                const vine::vsg::core::RenderPassCompatibility& compatibility,
                                std::span<const InputImages> inputs, StateRegistry& registry) {
        ContentPass::Scope scope;
        scope.entries    = halves;
        scope.registry   = &registry;
        scope.storage    = storage.get();
        scope.uploads    = &uploads;
        ContentPass content(scope, diagnostics);
        ::vsg::ref_ptr<::vsg::Node> node;
        EXPECT_TRUE(content.record(pass, facts, compatibility, inputs, view_block, node));
        return node;
    };
    std::vector<::vsg::ref_ptr<::vsg::ImageView>> gbuffer_views;
    for (std::uint32_t index = 0; index < gbuffer->colorAttachmentCount(); ++index) {
        gbuffer_views.push_back(gbuffer->colorView(index));
    }
    const InputImages gbuffer_only[]{ InputImages{ gbuffer_views, {} } };
    const InputImages gbuffer_and_lit_map[]{ InputImages{ gbuffer_views, {} },
                                             InputImages{ {}, lit_map->depthView() } };
    const InputImages gbuffer_and_shadowed_map[]{ InputImages{ gbuffer_views, {} },
                                                  InputImages{ {}, shadowed_map->depthView() } };
    ::vsg::ref_ptr<::vsg::Node> gbuffer_node =
        recordPass(frame.passes[0], writer_halves, gbuffer->shape().compatibility(), {}, *registries[0]);
    // The maps' own passes carry no draws - they CLEAR the map to the depth their measurement needs - and a
    // frame whose clearing passes are not recorded compares against whatever the image happened to hold.
    const std::span<const ContentPass::Scope::Entry> no_halves;
    const std::span<const InputImages>               no_inputs;
    ::vsg::ref_ptr<::vsg::Node>                      lit_map_node =
        recordPass(frame.passes[1], no_halves, lit_map->shape().compatibility(), no_inputs, *registries[1]);
    ::vsg::ref_ptr<::vsg::Node> shadowed_map_node =
        recordPass(frame.passes[2], no_halves, shadowed_map->shape().compatibility(), no_inputs,
                   *registries[2]);
    ::vsg::ref_ptr<::vsg::Node> lit_node =
        recordPass(frame.passes[3], shadowed_halves, lit->shape().compatibility(),
                   std::span<const InputImages>(gbuffer_and_lit_map, 2U), *registries[3]);
    ::vsg::ref_ptr<::vsg::Node> shadowed_node =
        recordPass(frame.passes[4], shadowed_halves, shadowed->shape().compatibility(),
                   std::span<const InputImages>(gbuffer_and_shadowed_map, 2U), *registries[4]);
    ::vsg::ref_ptr<::vsg::Node> flat_node =
        recordPass(frame.passes[5], unshadowed_halves, flat->shape().compatibility(),
                   std::span<const InputImages>(gbuffer_only, 1U), *registries[5]);
    const std::uint64_t before_report =
        diagnostics.count(vine::graphics::DiagnosticCategory::UnsupportedRequest);
    ::vsg::ref_ptr<::vsg::Node> plain_node =
        recordPass(frame.passes[6], unshadowed_halves, plain->shape().compatibility(),
                   std::span<const InputImages>(gbuffer_and_lit_map, 2U), *registries[6]);
    EXPECT_EQ(diagnostics.count(vine::graphics::DiagnosticCategory::UnsupportedRequest), before_report + 1U)
        << "the map the pass resolved and the program cannot read is reported";
    EXPECT_EQ(diagnostics.count(vine::graphics::DiagnosticCategory::ContentSkipped), 0U)
        << "nothing is refused: the engine's own programs, both variants, must be servable";

    VsgExecutor executor(diagnostics);
    executor.addTarget(gbuffer_handle.get(), gbuffer.get());
    executor.addTarget(lit_map_handle.get(), lit_map.get());
    executor.addTarget(shadowed_map_handle.get(), shadowed_map.get());
    executor.addTarget(lit_handle.get(), lit.get());
    executor.addTarget(shadowed_handle.get(), shadowed.get());
    executor.addTarget(flat_handle.get(), flat.get());
    executor.addTarget(plain_handle.get(), plain.get());
    auto command_graph = ::vsg::CommandGraph::create(created.device, created.queue_family);
    const PassContent packets[]{ PassContent{ frame.passes[0].pass, gbuffer_node },
                                 PassContent{ frame.passes[1].pass, lit_map_node },
                                 PassContent{ frame.passes[2].pass, shadowed_map_node },
                                 PassContent{ frame.passes[3].pass, lit_node },
                                 PassContent{ frame.passes[4].pass, shadowed_node },
                                 PassContent{ frame.passes[5].pass, flat_node },
                                 PassContent{ frame.passes[6].pass, plain_node } };
    ASSERT_TRUE(executor.record(frame, command_graph, std::span<const PassContent>(packets, 7U)));

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
    const Rgba8 lit_pixel = lit->probe().pixel(static_cast<int>(kSize) / 2, static_cast<int>(kSize) / 2);
    EXPECT_TRUE(near(lit_pixel.r, 0.25) && near(lit_pixel.g, 0.125) && near(lit_pixel.b, 0.1))
        << "the engine's lighting over a map of 0.0 must shade albedo * (ambient + sun), got ("
        << static_cast<int>(lit_pixel.r) << ", " << static_cast<int>(lit_pixel.g) << ", "
        << static_cast<int>(lit_pixel.b) << ")";

    const Rgba8 shadowed_pixel =
        shadowed->probe().pixel(static_cast<int>(kSize) / 2, static_cast<int>(kSize) / 2);
    EXPECT_TRUE(near(shadowed_pixel.r, 0.05) && near(shadowed_pixel.g, 0.025) && near(shadowed_pixel.b, 0.1))
        << "a map of 1.0 must scale the sun away (ambient only), got (" << static_cast<int>(shadowed_pixel.r)
        << ", " << static_cast<int>(shadowed_pixel.g) << ", " << static_cast<int>(shadowed_pixel.b)
        << ") - the lit value here would mean the map never reached binding 5 or the block never reached 6";

    const Rgba8 flat_pixel = flat->probe().pixel(static_cast<int>(kSize) / 2, static_cast<int>(kSize) / 2);
    EXPECT_TRUE(near(flat_pixel.r, 0.25) && near(flat_pixel.g, 0.125) && near(flat_pixel.b, 0.1))
        << "the unshadowed variant over the same G-buffer shades the same lit value, got ("
        << static_cast<int>(flat_pixel.r) << ", " << static_cast<int>(flat_pixel.g) << ", "
        << static_cast<int>(flat_pixel.b) << ")";

    const Rgba8 plain_pixel = plain->probe().pixel(static_cast<int>(kSize) / 2, static_cast<int>(kSize) / 2);
    EXPECT_TRUE(near(plain_pixel.r, 0.25) && near(plain_pixel.g, 0.125) && near(plain_pixel.b, 0.1))
        << "a program that cannot read the map still draws, got (" << static_cast<int>(plain_pixel.r) << ", "
        << static_cast<int>(plain_pixel.g) << ", " << static_cast<int>(plain_pixel.b) << ")";
}

TEST(ContentPassTest, TheStoresTablesDrawTheFrameThePlanDescribes)
{
    // The tables a recording reads are PRODUCED (see api/ContentStore): the host tracks its live objects,
    // and the store answers for exactly what the frame's plan names, at the revision each object is at
    // NOW. This pass draws through tables nobody built by hand - and it draws ONE program twice, in one
    // pass, with two variants whose PUSH DECLARATIONS differ (`VINE_DIFFUSE_MAP` gates the push constant
    // block itself), so the entry a drawable is served decides whether its matrices exist at all:
    //
    //   * the PLAIN command is collected FIRST (its half is the right of the picture), so the store builds
    //     the plain variant's entry first. A lookup that ignored the variant would hand the TEXTURED draw
    //     that entry - which declares no push range - and the textured shader would read an unset push:
    //     the left half would stay the clear colour.
    //
    // Everything else is the shape the two-variant case uses: the camera's push lands the quad exactly
    // where the plain branch's fixed position does, so the two halves differ only in what they shade.
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

    const vine::intrusive_ptr<vine::graphics::Texture2D> texture(
        new vine::graphics::Texture2D(1, 1, vine::imaging::PixelFormat::Rgba8Unorm));
    {
        auto image = vine::intrusive_ptr<vine::imaging::Image>(
            new vine::imaging::Image(1, 1, vine::imaging::PixelFormat::Rgba8Unorm));
        const std::span<std::byte> pixels = image->mipData(0);
        pixels[0]                          = static_cast<std::byte>(255U);
        pixels[1]                          = static_cast<std::byte>(0U);
        pixels[2]                          = static_cast<std::byte>(255U);
        pixels[3]                          = static_cast<std::byte>(255U);
        texture->setImage(vine::intrusive_ptr<const vine::imaging::Image>(image));
    }

    const std::shared_ptr<vine::vsg::MaterialImages> images = vine::vsg::MaterialImages::create();
    ASSERT_NE(images, nullptr);
    vine::vsg::detail::TextureReject map_reason = vine::vsg::detail::TextureReject::Ok;
    const vine::vsg::SamplerImage    map        = images->acquire(texture.get(), map_reason);
    ASSERT_EQ(map_reason, vine::vsg::detail::TextureReject::Ok);

    const vine::intrusive_ptr<ShaderProgram> program(new ShaderProgram());
    {
        ShaderStage vertex;
        vertex.type   = ShaderStageType::Vertex;
        vertex.source = vine::String(reinterpret_cast<const char8_t*>(
            "#pragma import_defines (VINE_DIFFUSE_MAP, VINE_TEXCOORD_UV, VINE_TEXCOORD_CUBE)\n"
            "layout(location = 0) in vec3 position;\n"
            "#ifdef VINE_DIFFUSE_MAP\n"
            "#if defined(VINE_TEXCOORD_CUBE)\n"
            "layout(location = 8) in vec3 texcoord;\n"
            "#elif defined(VINE_TEXCOORD_UV)\n"
            "layout(location = 8) in vec2 texcoord;\n"
            "#else\n"
            "#error a sampled texcoord slot needs one kind: define VINE_TEXCOORD_UV or VINE_TEXCOORD_CUBE\n"
            "#endif\n"
            "layout(push_constant) uniform PushConstants { mat4 projection; mat4 modelView; } pc;\n"
            "#endif\n"
            "layout(location = 0) out vec2 uv;\n"
            "void main()\n"
            "{\n"
            "#ifdef VINE_DIFFUSE_MAP\n"
            "    gl_Position = pc.projection * pc.modelView * vec4(position, 1.0);\n"
            "    uv = texcoord;\n"
            "#else\n"
            "    gl_Position = vec4(position.xy, 0.5, 1.0);\n"
            "    uv = vec2(0.0);\n"
            "#endif\n"
            "}\n"));
        ShaderStage fragment;
        fragment.type   = ShaderStageType::Fragment;
        fragment.source = vine::String(reinterpret_cast<const char8_t*>(
            "#pragma import_defines (VINE_DIFFUSE_MAP, VINE_TEXCOORD_UV, VINE_TEXCOORD_CUBE)\n"
            "layout(location = 0) in vec2 uv;\n"
            "layout(location = 0) out vec4 outColor;\n"
            "layout(set = 0, binding = 0, std140) uniform VineMaterialBlock\n"
            "{\n"
            "    vec4 ambient; vec4 diffuse; vec4 specular; float shininess;\n"
            "} material;\n"
            "#ifdef VINE_DIFFUSE_MAP\n"
            "layout(set = 0, binding = 1) uniform sampler2D diffuseMap;\n"
            "#endif\n"
            "void main()\n"
            "{\n"
            "#ifdef VINE_DIFFUSE_MAP\n"
            "    outColor = vec4(texture(diffuseMap, uv).rgb, 1.0);\n"
            "#else\n"
            "    outColor = vec4(material.diffuse.rgb, 1.0);\n"
            "#endif\n"
            "}\n"));
        program->addStage(vertex);
        program->addStage(fragment);
    }

    const vine::intrusive_ptr<Geometry> geometry(new Geometry());
    {
        const std::vector<float> positions{ -1.0F, -1.0F, 0.0F, 1.0F, -1.0F, 0.0F,
                                            1.0F,  1.0F,  0.0F, -1.0F, 1.0F, 0.0F };
        const std::vector<float> texcoords{ 0.0F, 0.0F, 1.0F, 0.0F, 1.0F, 1.0F, 0.0F, 1.0F };
        geometry->setPositions(
            vine::intrusive_ptr<const vine::Buffer<float>>(new vine::Buffer<float>(positions)));
        geometry->setTexcoords2(
            vine::intrusive_ptr<const vine::Buffer<float>>(new vine::Buffer<float>(texcoords)));
        geometry->setIndices(vine::intrusive_ptr<const vine::Buffer<std::uint32_t>>(
            new vine::Buffer<std::uint32_t>(std::vector<std::uint32_t>{ 0U, 1U, 2U, 0U, 2U, 3U })));
        geometry->setRevision(1U);
    }

    const vine::intrusive_ptr<Material> textured_material(new Material());
    textured_material->setTexture(texture);
    const vine::intrusive_ptr<Material> plain_material(new Material());
    plain_material->setDiffuse(vine::Colorf(0.25F, 0.5F, 0.75F, 1.0F));

    // The host hands the store its live content, and nothing else: everything below reads its tables.
    vine::vsg::ContentStore store;
    store.track(geometry);
    store.track(program);
    store.track(textured_material);
    store.track(plain_material);

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

    RenderCommand textured_command;
    textured_command.geometry = geometry;
    textured_command.material = textured_material;
    textured_command.program  = program;
    RenderCommand plain_command;
    plain_command.geometry = geometry;
    plain_command.material = plain_material;
    plain_command.program  = program;

    // The plain command FIRST, so its variant's entry is the first one the store builds (see the case note).
    recorder.beginFrame(FrameToken{ 1 });
    recorder.beginPass(1U);
    recorder.setRenderTarget(target.get());
    recorder.setClearPolicy(clear);
    recorder.setViewport(static_cast<int>(kSize) / 2, 0, static_cast<int>(kSize) / 2, static_cast<int>(kSize));
    recorder.render(std::vector<RenderCommand>{ plain_command }, camera.get());
    recorder.setViewport(0, 0, static_cast<int>(kSize) / 2, static_cast<int>(kSize));
    recorder.render(std::vector<RenderCommand>{ textured_command }, camera.get());
    recorder.endPass();
    recorder.endFrame();

    const CompiledFrame& frame = compiler.compile(recorder.description(), FrameFacts{ target_table });
    ASSERT_EQ(frame.passes.size(), 1U);
    ASSERT_EQ(frame.passes[0].draws.size(), 2U);

    vine::vsg::core::FrameTimeline   timeline;
    vine::vsg::core::RetirementQueue retirement(3U);
    const ContentFacts&              facts = store.tablesFor(frame, timeline, retirement);
    ASSERT_EQ(store.programEntries(), 2U) << "one program, two texts: the store built both variants";
    ASSERT_EQ(store.builds(), 5U) << "the geometry, the two materials and the two variants";

    vine::vsg::ProgramVariant textured_variant;
    textured_variant.diffuse_map = true;
    const vine::vsg::ProgramVariant plain_variant;
    const auto textured_entry =
        findProgram(facts, vine::vsg::core::ProgramRef{ program.get(), program->revision() }, textured_variant);
    const auto plain_entry =
        findProgram(facts, vine::vsg::core::ProgramRef{ program.get(), program->revision() }, plain_variant);
    ASSERT_TRUE(textured_entry.found());
    ASSERT_TRUE(plain_entry.found());
    EXPECT_FALSE(textured_entry.entry->abi.pushes.empty()) << "the textured text declares the push it reads";
    EXPECT_TRUE(plain_entry.entry->abi.pushes.empty()) << "the plain text declares none, and must be served none";

    const auto geometry_entry = findGeometry(facts, geometry.get(), geometry->revision());
    ASSERT_TRUE(geometry_entry.found());
    ASSERT_TRUE(channelsMatchLayout(*geometry_entry.entry));

    // The two layers are built from THE STORE'S OWN entries - the production loop, spelled out.
    std::unique_ptr<ContentPipeline> textured_layer =
        pipelineFor(*geometry_entry.entry, textured_entry.entry->shaders, textured_entry.entry->abi);
    std::unique_ptr<ContentPipeline> plain_layer =
        pipelineFor(*geometry_entry.entry, plain_entry.entry->shaders, plain_entry.entry->abi);
    ASSERT_NE(textured_layer, nullptr);
    ASSERT_NE(plain_layer, nullptr);

    const vine::vsg::BlockDescriptors::SampledBinding textured_maps[] = {
        vine::vsg::BlockDescriptors::SampledBinding{ 1U, map.view, map.sampler }
    };
    std::unique_ptr<BlockDescriptors> textured_declared =
        BlockDescriptors::forAbi(textured_entry.entry->abi, 0U, created.device, *storage, textured_maps);
    std::unique_ptr<BlockDescriptors> plain_declared =
        BlockDescriptors::forAbi(plain_entry.entry->abi, 0U, created.device, *storage);
    ASSERT_NE(textured_declared, nullptr);
    ASSERT_NE(plain_declared, nullptr);

    VariantPool   pool;
    StateRegistry registry(pool);
    StreamUploads uploads;
    ContentDraw   textured_draws(*textured_layer, pool,
                                 vine::vsg::detail::fetchDynamicStateEntryPoints(created.device->vk(),
                                                                                created.instance->vk()));
    ContentDraw   plain_draws(*plain_layer, pool,
                              vine::vsg::detail::fetchDynamicStateEntryPoints(created.device->vk(),
                                                                             created.instance->vk()));

    const ContentPass::Scope::Entry halves[]{
        ContentPass::Scope::Entry{ vine::vsg::core::DrawKind::Content, program.get(), textured_entry.entry->revision,
                                   geometry_entry.entry->layout, textured_layer.get(), &textured_draws,
                                   textured_variant },
        ContentPass::Scope::Entry{ vine::vsg::core::DrawKind::Content, program.get(), plain_entry.entry->revision,
                                   geometry_entry.entry->layout, plain_layer.get(), &plain_draws, plain_variant }
    };
    BlockDescriptors* declared_sets[] = { textured_declared.get(), plain_declared.get() };

    storage->beginFrame();
    ContentPass::Scope scope;
    scope.entries    = halves;
    scope.registry   = &registry;
    scope.storage    = storage.get();
    scope.block_sets = declared_sets;
    scope.uploads    = &uploads;
    ContentPass content(scope, diagnostics);

    const std::vector<std::byte> view_block(288U, std::byte{ 0 });
    ::vsg::ref_ptr<::vsg::Node>  content_node;
    ASSERT_TRUE(content.record(frame.passes[0], facts, target->shape().compatibility(), {}, view_block,
                               content_node));
    EXPECT_EQ(diagnostics.count(vine::graphics::DiagnosticCategory::ContentSkipped), 0U)
        << "both drawables are served from the store's tables";

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

    const auto near = [](std::uint8_t byte, double expected) {
        return std::abs(static_cast<double>(byte) - expected) <= 16.0;
    };
    const int   middle_row = static_cast<int>(kSize) / 2;
    const Rgba8 left_half  = target->probe().pixel(static_cast<int>(kSize) / 4, middle_row);
    const Rgba8 right_half = target->probe().pixel(static_cast<int>(kSize) * 3 / 4, middle_row);
    EXPECT_TRUE(near(left_half.r, 255.0) && near(left_half.g, 0.0) && near(left_half.b, 255.0))
        << "the textured variant samples the map (magenta), got (" << static_cast<int>(left_half.r) << ", "
        << static_cast<int>(left_half.g) << ", " << static_cast<int>(left_half.b)
        << ") - the clear colour here means its push was never written (another variant's ABI was served)";
    EXPECT_TRUE(near(right_half.r, 64.0) && near(right_half.g, 128.0) && near(right_half.b, 191.0))
        << "the untextured variant shades the material's diffuse (0.25, 0.5, 0.75), got ("
        << static_cast<int>(right_half.r) << ", " << static_cast<int>(right_half.g) << ", "
        << static_cast<int>(right_half.b) << ")";
}
