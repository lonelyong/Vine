/**
 * @brief End-to-end evidence: a triangle drawn through the WHOLE rewritten stack reaches the target's pixels.
 *
 * This is the first phase where every layer built so far has to work together, and the assertions are the ones
 * the design promises a phase must make - pixels AND counters:
 *
 *   `StreamUploads`      one shared bind per stream (the geometry is uploaded once)
 *   `BlockStorage`       the frame's blocks (view, draw, material) written into mapped memory
 *   `BlockDescriptors`   one set, three dynamic offsets, chosen per draw
 *   `ContentPipeline`    the pipeline for the identity (compiled by the viewer's traversal)
 *   `ContentDraw`        the command graph: pipeline, dynamic block, descriptor bind, geometry, draw
 *   `OffscreenTarget`    the pixels, read back headlessly
 *
 * The second case is the one that proves the DYNAMIC OFFSETS are what selects a draw's block: one frame, two
 * triangles, two draw blocks - each triangle moves exactly as its own block says, and the space between them
 * is still the clear colour. A picture with both triangles in the same place (or neither) would mean the
 * offsets, the descriptor set or the recording were wrong, none of which a counter alone can tell apart.
 *
 * No window and no display server: a device created by `api::Device`, an off-screen target, and lavapipe when
 * no GPU is present.
 */

#include <gtest/gtest.h>

#include <array>
#include <cstddef>
#include <cstdlib>
#include <cstdint>
#include <cstring>
#include <span>
#include <vector>

#include <vsg/app/CommandGraph.h>
#include <vsg/app/Viewer.h>
#include <vsg/core/Array.h>
#include <vsg/vk/Device.h>

#include <vine/vsg/VsgVulkanEntryPoints.hpp>
#include <vine/vsg/api/BlockDescriptors.hpp>
#include <vine/vsg/api/BlockStorage.hpp>
#include <vine/vsg/api/ContentDraw.hpp>
#include <vine/vsg/api/ContentPipeline.hpp>
#include <vine/vsg/api/Device.hpp>
#include <vine/vsg/api/OffscreenTarget.hpp>
#include <vine/vsg/api/StreamUploads.hpp>
#include <vine/vsg/core/StateRegistry.hpp>
#include <vine/vsg/core/Streams.hpp>
#include <vine/vsg/core/VariantPool.hpp>

using vine::vsg::BlockDescriptors;
using vine::vsg::BlockStorage;
using vine::vsg::ContentDraw;
using vine::vsg::ContentPipeline;
using vine::vsg::OffscreenTarget;
using vine::vsg::StreamUploads;
using vine::vsg::ViewportRect;
using vine::vsg::core::PixelProbe;
using vine::vsg::core::PipelineKey;
using vine::vsg::core::StateRegistry;
using vine::vsg::core::StreamKey;
using vine::vsg::core::StreamKind;
using vine::vsg::core::VariantPool;

namespace
{

constexpr std::uint32_t kSize = 64U;

/// @brief The block bytes one draw reads: a 4x4 matrix then the parameter vector (the engine's draw ABI).
std::vector<std::byte> drawBlock(float translate_x, float translate_y, float opacity)
{
    // Column-major, the layout GLSL expects: translation lives in m[12] / m[13].
    const std::array<float, 16> matrix{ 1.0F, 0.0F, 0.0F, 0.0F,  0.0F, 1.0F, 0.0F, 0.0F,
                                        0.0F, 0.0F, 1.0F, 0.0F,  translate_x, translate_y, 0.0F, 1.0F };
    std::vector<std::byte>      block(80U, std::byte{ 0 });
    std::memcpy(block.data(), matrix.data(), sizeof(matrix));
    const std::array<float, 4> params{ opacity, 0.0F, 0.0F, 0.0F };
    std::memcpy(block.data() + sizeof(matrix), params.data(), sizeof(params));
    return block;
}

/// @brief A view block's worth of bytes (the shader here does not read them; the region is still written).
std::vector<std::byte> viewBlock()
{
    return std::vector<std::byte>(288U, std::byte{ 0 });
}

/// @brief The material block's bytes (64 bytes; this shader reads the draw block only).
std::vector<std::byte> materialBlock()
{
    return std::vector<std::byte>(64U, std::byte{ 0 });
}

/// @brief A shader pair: the vertex stage places the triangle by the DRAW block's matrix, the fragment is green.
ContentPipeline::Shaders contentShaders()
{
    ContentPipeline::Shaders shaders;
    shaders.vertex = "#version 450\n"
                     "layout(location = 0) in vec3 position;\n"
                     "layout(set = 0, binding = 1, std140) uniform VineDrawBlock { mat4 model; vec4 params; } draw;\n"
                     "void main() { gl_Position = draw.model * vec4(position, 1.0); }\n";
    shaders.fragment = "#version 450\n"
                       "layout(location = 0) out vec4 outColor;\n"
                       "layout(set = 0, binding = 1, std140) uniform VineDrawBlock { mat4 model; vec4 params; } draw;\n"
                       "void main() { outColor = vec4(0.0, 1.0, 0.0, draw.params.x); }\n";
    return shaders;
}

/// @brief The triangle every case draws: small enough that two of them do not overlap.
::vsg::ref_ptr<::vsg::vec3Array> trianglePositions()
{
    auto positions  = ::vsg::vec3Array::create(3U);
    (*positions)[0] = ::vsg::vec3(-0.4F, -0.4F, 0.0F);
    (*positions)[1] = ::vsg::vec3(0.4F, -0.4F, 0.0F);
    (*positions)[2] = ::vsg::vec3(0.0F, 0.4F, 0.0F);
    return positions;
}

::vsg::ref_ptr<::vsg::uintArray> triangleIndices()
{
    auto indices  = ::vsg::uintArray::create(3U);
    (*indices)[0] = 0U;
    (*indices)[1] = 1U;
    (*indices)[2] = 2U;
    return indices;
}

/// @brief What one run of the stack needs, all created on one device.
struct Fixture
{
    static constexpr float kClear[4]{ 0.0F, 0.0F, 0.25F, 1.0F };

    vine::vsg::DeviceResult                created;
    std::unique_ptr<OffscreenTarget>       target;
    std::unique_ptr<BlockStorage>          storage;
    std::unique_ptr<BlockDescriptors>      descriptors;
    std::unique_ptr<ContentPipeline>       pipelines;
    std::unique_ptr<StreamUploads>         uploads;
    std::unique_ptr<ContentDraw>           recorder;
    VariantPool                            pool;
    std::unique_ptr<StateRegistry>         registry;
    ::vsg::ref_ptr<::vsg::Viewer>          viewer;

    static int material_identity;

    bool build()
    {
        created = vine::vsg::createDevice();
        if (!created.ok) {
            return false;
        }
        target = OffscreenTarget::create(created.device, OffscreenTarget::Layout{ kSize, kSize,
                                                                                  { kClear[0], kClear[1], kClear[2],
                                                                                    kClear[3] } });
        if (target == nullptr) {
            return false;
        }
        storage = BlockStorage::create(created.device, BlockStorage::Layout{});
        if (storage == nullptr) {
            return false;
        }
        const ContentPipeline::Shaders shader_pair = contentShaders();
        vine::vsg::ProgramAbi            abi;
        if (vine::vsg::scanProgramAbi(shader_pair.vertex, shader_pair.fragment, {}, abi) != vine::vsg::FactMiss::None) {
            return false;
        }
        descriptors = BlockDescriptors::forAbi(abi, 0U, created.device, *storage);
        if (descriptors == nullptr) {
            return false;
        }

        const ContentPipeline::VertexBinding   binding{ 0U, sizeof(float) * 3U, false };
        const ContentPipeline::VertexAttribute attribute{ 0U, 0U, VK_FORMAT_R32G32B32_SFLOAT, 0U };
        pipelines = ContentPipeline::create(abi,
                                            std::span<const ContentPipeline::VertexBinding>(&binding, 1U),
                                            std::span<const ContentPipeline::VertexAttribute>(&attribute, 1U),
                                            shader_pair);
        if (pipelines == nullptr) {
            return false;
        }

        uploads = std::make_unique<StreamUploads>();
        // The three extension entry points, exactly as a session would fetch them: without them the dynamic
        // command skips the polygon-mode and blend calls, and the pipeline's create-info values would be the
        // picture - which is not what the phase is testing.
        recorder = std::make_unique<ContentDraw>(*pipelines, pool,
                                                 vine::vsg::detail::fetchDynamicStateEntryPoints(created.device->vk(),
                                                                                                 created.instance->vk()));
        registry = std::make_unique<StateRegistry>(pool);
        viewer   = ::vsg::Viewer::create();
        return viewer != nullptr;
    }

    /// @brief Records one draw of the triangle, placed by @p translate_x / @p translate_y.
    ::vsg::ref_ptr<::vsg::Node> drawAt(float translate_x, float translate_y)
    {
        storage->beginFrame();
        const auto view     = storage->writeView(viewBlock());
        const auto block    = storage->writeDraw(drawBlock(translate_x, translate_y, 1.0F));
        const auto material = storage->writeMaterial(&material_identity, 1U, materialBlock());
        if (!view.valid || !block.valid) {
            return {};
        }

        const auto vertex_bind = uploads->acquireVertex(positionKey(), trianglePositions());
        const auto index_bind  = uploads->acquireIndex(indexKey(), triangleIndices());
        if (vertex_bind.bind == nullptr || index_bind.bind == nullptr) {
            return {};
        }

        ContentDraw::Draw draw;
        static int       program = 0;
        draw.key.program                      = &program;
        draw.key.revision                     = 1U;
        draw.key.vertex_layout.canonical_mask = 0x1U;
        draw.key.compatibility.samples        = 1U;
        const ::vsg::ref_ptr<::vsg::BindDescriptorSet> block_binds[] = {
            descriptors->bind(pipelines->layout(), BlockDescriptors::Offsets{ view.offset, block.offset,
                                                                               material.offset })
        };
        draw.blocks       = block_binds;
        draw.vertex_binds = std::span<const ::vsg::ref_ptr<::vsg::BindVertexBuffers>>(&vertex_bind.bind, 1U);
        draw.index        = index_bind.bind;
        draw.viewport     = ViewportRect{ 0.0F, 0.0F, static_cast<float>(kSize), static_cast<float>(kSize) };
        draw.index_count  = 3U;
        return recorder->record(*registry, draw);
    }

    /// @brief Compiles the graph and renders one frame, returning the pixels.
    PixelProbe frame(const std::vector<::vsg::ref_ptr<::vsg::Node>>& content)
    {
        for (const auto& node : content) {
            target->renderGraph()->addChild(node);
        }
        auto command_graph = ::vsg::CommandGraph::create(created.device, created.queue_family);
        command_graph->addChild(target->renderGraph());
        command_graph->addChild(target->capture());
        viewer->assignRecordAndSubmitTaskAndPresentation(::vsg::CommandGraphs{ command_graph });
        if (!viewer->compile()) {
            return PixelProbe(0, 0, {});
        }
        viewer->advanceToNextFrame();
        viewer->handleEvents();
        viewer->recordAndSubmit();
        viewer->deviceWaitIdle();
        return target->probe();
    }

    static StreamKey positionKey()
    {
        static int model_buffer = 0;
        StreamKey  key;
        key.kind       = StreamKind::Vertex;
        key.location   = 0U;
        key.components = 3U;
        key.buffer     = &model_buffer;
        key.revision   = 1U;
        key.count      = 9U;
        return key;
    }

    static StreamKey indexKey()
    {
        static int model_indices = 0;
        StreamKey  key;
        key.kind       = StreamKind::Index;
        key.location   = 0U;
        key.components = 1U;
        key.buffer     = &model_indices;
        key.revision   = 1U;
        key.count      = 3U;
        return key;
    }
};

int Fixture::material_identity = 0;

/// @brief The pixel a point in normalised device coordinates lands on (Vulkan's y points down).
struct Pixel
{
    int x;
    int y;
};

Pixel pixelAt(float x_ndc, float y_ndc)
{
    return { static_cast<int>((x_ndc + 1.0F) * 0.5F * static_cast<float>(kSize)),
             static_cast<int>((y_ndc + 1.0F) * 0.5F * static_cast<float>(kSize)) };
}

bool isGreen(const vine::vsg::core::Rgba8& pixel)
{
    return pixel.g > 200U && pixel.r < 60U && pixel.b < 60U;
}

/// @brief Whether @p pixel is the target's clear colour (0, 0, 0.25) within a UNORM step.
bool isClear(const vine::vsg::core::Rgba8& pixel)
{
    const auto near = [](std::uint8_t value, int expected) {
        return std::abs(static_cast<int>(value) - expected) <= 2;
    };
    return near(pixel.r, 0) && near(pixel.g, 0) && near(pixel.b, 64) && near(pixel.a, 255);
}

}  // namespace

TEST(ContentStackTest, ATriangleDrawnThroughTheWholeStackReachesTheTarget)
{
    Fixture fixture;
    if (!fixture.build()) {
        GTEST_SKIP() << "no device satisfies the device-floor requirements";
    }

    const auto content = fixture.drawAt(0.0F, 0.0F);
    ASSERT_NE(content, nullptr) << "the recording must produce a command graph";

    const PixelProbe probe = fixture.frame({ content });
    ASSERT_TRUE(probe.valid()) << "the frame must read back";

    // The triangle covers the centre; its corners stay the clear colour.
    const auto centre = probe.pixel(kSize / 2, kSize / 2);
    EXPECT_TRUE(isGreen(centre)) << "the centre must hold the fragment's colour, got ("
                                 << static_cast<int>(centre.r) << ", " << static_cast<int>(centre.g) << ", "
                                 << static_cast<int>(centre.b) << ")";
    const auto corner_a = probe.pixel(1, 1);
    const auto corner_b = probe.pixel(static_cast<int>(kSize) - 2, static_cast<int>(kSize) - 2);
    EXPECT_TRUE(isClear(corner_a)) << "a corner is outside the triangle, got (" << static_cast<int>(corner_a.r)
                                   << ", " << static_cast<int>(corner_a.g) << ", " << static_cast<int>(corner_a.b)
                                   << ", " << static_cast<int>(corner_a.a) << ")";
    EXPECT_TRUE(isClear(corner_b)) << "got (" << static_cast<int>(corner_b.r) << ", " << static_cast<int>(corner_b.g)
                                   << ", " << static_cast<int>(corner_b.b) << ", " << static_cast<int>(corner_b.a)
                                   << ")";

    // The counters the design makes a phase assert: one upload, one pipeline, one bind of each kind.
    EXPECT_EQ(fixture.uploads->uploads(), 2U) << "positions and indices: one upload each";
    EXPECT_EQ(fixture.uploads->aliases(), 0U);
    EXPECT_EQ(fixture.pool.created(), 1U);
    EXPECT_EQ(fixture.pipelines->compiles(), 1U);
    EXPECT_EQ(fixture.recorder->pipeline_binds(), 1U);
    EXPECT_EQ(fixture.recorder->dynamic_commands(), 1U);
    EXPECT_EQ(fixture.storage->materialWrites(), 1U) << "one material block, written once";
}

TEST(ContentStackTest, EachDrawReadsItsOwnBlockThroughTheDynamicOffsets)
{
    Fixture fixture;
    if (!fixture.build()) {
        GTEST_SKIP() << "no device satisfies the device-floor requirements";
    }

    // One frame, two draws, two draw blocks: the left triangle and the right one.
    const auto left  = fixture.drawAt(-0.5F, 0.0F);
    const auto right = fixture.drawAt(0.5F, 0.0F);
    ASSERT_NE(left, nullptr);
    ASSERT_NE(right, nullptr);

    const PixelProbe probe = fixture.frame({ left, right });
    ASSERT_TRUE(probe.valid());

    const Pixel left_point  = pixelAt(-0.5F, 0.0F);
    const Pixel right_point = pixelAt(0.5F, 0.0F);
    const Pixel middle      = pixelAt(0.0F, 0.0F);

    EXPECT_TRUE(isGreen(probe.pixel(left_point.x, left_point.y)))
        << "the first draw's block placed its triangle on the left";
    EXPECT_TRUE(isGreen(probe.pixel(right_point.x, right_point.y)))
        << "the second draw's block placed ITS triangle on the right";
    EXPECT_TRUE(isClear(probe.pixel(middle.x, middle.y)))
        << "the gap between them was never drawn into: the two blocks really did place two triangles";

    // Two draws of one variant: the pipeline is bound once, the dynamic block issued once, and the design's
    // "a pass' command buffer is proportional to its content" shows up here as these two numbers.
    EXPECT_EQ(fixture.pool.created(), 1U);
    EXPECT_EQ(fixture.recorder->draws(), 2U);
    EXPECT_EQ(fixture.recorder->pipeline_binds(), 1U);
    EXPECT_EQ(fixture.recorder->dynamic_commands(), 1U);
}
