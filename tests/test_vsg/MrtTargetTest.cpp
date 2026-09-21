/**
 * @brief Multi-attachment targets and the clear policy that goes with them (see `.ai/design/vsg-reimplementation.md`
 * D6 and milestone M3).
 *
 * The clear plan is pinned device-free in `ClearPlanTest`; what these cases add is the two things only a
 * device can say:
 *
 *   * the plan's decisions actually reach the pass: attachment 0 comes back as the pass' clear colour and an
 *     extra attachment comes back as TRANSPARENT BLACK - not as the pass' colour, which is the wrong picture
 *     that looks entirely plausible in a single-attachment test;
 *   * a second attachment is a real render target: a fragment shader writing two outputs leaves green in one
 *     and red in the other, so a readback that happened to copy attachment 0 twice is caught.
 *
 * The second point is why the shader has two outputs at all: with MRT, an attachment the fragment stage does
 * not write is UNDEFINED (not "the clear colour"), so the only honest evidence is a shader that writes both.
 */

#include <gtest/gtest.h>

#include <array>
#include <cstddef>
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

using vine::graphics::RenderTarget;
using vine::vsg::BlockDescriptors;
using vine::vsg::BlockStorage;
using vine::vsg::ContentDraw;
using vine::vsg::ContentPipeline;
using vine::vsg::OffscreenTarget;
using vine::vsg::StreamUploads;
using vine::vsg::ViewportRect;
using vine::vsg::core::PixelProbe;
using vine::vsg::core::Rgba8;
using vine::vsg::core::StateRegistry;
using vine::vsg::core::StreamKey;
using vine::vsg::core::StreamKind;
using vine::vsg::core::VariantPool;

namespace
{

constexpr std::uint32_t kSize = 64U;

/// @brief The pass' clear colour (0.5, 0.25, 0, 1): nothing about it is black, so an extra attachment painted
///        with the pass' colour cannot be mistaken for one that was cleared to transparent black.
constexpr float kClear[4]{ 0.5F, 0.25F, 0.0F, 1.0F };

/// @brief The engine-side shape under test: two colour attachments and a depth attachment.
OffscreenTarget::TargetLayout mrtLayout()
{
    OffscreenTarget::TargetLayout layout;
    layout.width         = kSize;
    layout.height        = kSize;
    layout.color_formats = { RenderTarget::ColorFormat::RGBA8, RenderTarget::ColorFormat::RGBA8 };
    layout.depth_format  = RenderTarget::DepthFormat::D32;
    layout.clear.color   = true;
    for (std::size_t index = 0; index < 4U; ++index) {
        layout.clear.color_value[index] = kClear[index];
    }
    return layout;
}

/// @brief The block bytes one draw reads: a 4x4 matrix then the parameter vector (the engine's draw ABI).
std::vector<std::byte> drawBlock()
{
    const std::array<float, 16> matrix{ 1.0F, 0.0F, 0.0F, 0.0F, 0.0F, 1.0F, 0.0F, 0.0F,
                                        0.0F, 0.0F, 1.0F, 0.0F, 0.0F, 0.0F, 0.0F, 1.0F };
    std::vector<std::byte>      block(80U, std::byte{ 0 });
    std::memcpy(block.data(), matrix.data(), sizeof(matrix));
    return block;
}

std::vector<std::byte> viewBlock()
{
    return std::vector<std::byte>(288U, std::byte{ 0 });
}

std::vector<std::byte> materialBlock()
{
    return std::vector<std::byte>(64U, std::byte{ 0 });
}

/// @brief A shader pair with TWO fragment outputs: location 0 green, location 1 red.
ContentPipeline::Shaders mrtShaders()
{
    ContentPipeline::Shaders shaders;
    shaders.vertex = "#version 450\n"
                     "layout(location = 0) in vec3 position;\n"
                     "void main() { gl_Position = vec4(position.x, position.y, 0.5, 1.0); }\n";
    shaders.fragment = "#version 450\n"
                       "layout(location = 0) out vec4 outColor;\n"
                       "layout(location = 1) out vec4 outExtra;\n"
                       "void main() { outColor = vec4(0.0, 1.0, 0.0, 1.0); outExtra = vec4(1.0, 0.0, 0.0, 1.0); }\n";
    return shaders;
}

::vsg::ref_ptr<::vsg::vec3Array> trianglePositions()
{
    auto positions  = ::vsg::vec3Array::create(3U);
    (*positions)[0] = ::vsg::vec3(-0.6F, -0.6F, 0.0F);
    (*positions)[1] = ::vsg::vec3(0.6F, -0.6F, 0.0F);
    (*positions)[2] = ::vsg::vec3(0.0F, 0.6F, 0.0F);
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

/// @brief What a case needs: an MRT target and the stack that can draw into it.
struct Fixture
{
    vine::vsg::DeviceResult                 created;
    std::unique_ptr<OffscreenTarget>        target;
    std::unique_ptr<BlockStorage>           storage;
    std::unique_ptr<BlockDescriptors>       descriptors;
    std::unique_ptr<ContentPipeline>        pipelines;
    std::unique_ptr<StreamUploads>          uploads;
    std::unique_ptr<ContentDraw>            recorder;
    VariantPool                             pool;
    std::unique_ptr<StateRegistry>          registry;
    ::vsg::ref_ptr<::vsg::Viewer>           viewer;

    static int material_identity;

    bool build()
    {
        created = vine::vsg::createDevice();
        if (!created.ok) {
            return false;
        }
        target = OffscreenTarget::create(created.device, mrtLayout());
        if (target == nullptr || target->colorAttachmentCount() != 2U || !target->hasDepth()) {
            return false;
        }
        storage = BlockStorage::create(created.device, BlockStorage::Layout{});
        if (storage == nullptr) {
            return false;
        }
        descriptors = BlockDescriptors::create(created.device, *storage);
        if (descriptors == nullptr) {
            return false;
        }

        const ContentPipeline::VertexBinding   binding{ 0U, sizeof(float) * 3U, false };
        const ContentPipeline::VertexAttribute attribute{ 0U, 0U, VK_FORMAT_R32G32B32_SFLOAT, 0U };
        ContentPipeline::Settings              settings;
        settings.color_attachments = 2U;  // the pass has two colour attachments, so the blend state declares two
        pipelines                  = ContentPipeline::create(descriptors->layout(),
                                                              std::span<const ContentPipeline::VertexBinding>(&binding, 1U),
                                                              std::span<const ContentPipeline::VertexAttribute>(&attribute, 1U),
                                                              mrtShaders(), settings);
        if (pipelines == nullptr) {
            return false;
        }

        uploads  = std::make_unique<StreamUploads>();
        recorder = std::make_unique<ContentDraw>(*pipelines, pool,
                                                 vine::vsg::detail::fetchDynamicStateEntryPoints(
                                                     created.device->vk(), created.instance->vk()));
        registry = std::make_unique<StateRegistry>(pool);
        viewer   = ::vsg::Viewer::create();
        return viewer != nullptr;
    }

    /// @brief Records one draw of the triangle (with two fragment outputs).
    ::vsg::ref_ptr<::vsg::Node> triangle()
    {
        storage->beginFrame();
        const auto view     = storage->writeView(viewBlock());
        const auto block    = storage->writeDraw(drawBlock());
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
        draw.blocks = descriptors->bind(pipelines->layout(), BlockDescriptors::Offsets{ view.offset, block.offset,
                                                                                        material.offset });
        draw.vertex_binds = std::span<const ::vsg::ref_ptr<::vsg::BindVertexBuffers>>(&vertex_bind.bind, 1U);
        draw.index        = index_bind.bind;
        draw.viewport     = ViewportRect{ 0.0F, 0.0F, static_cast<float>(kSize), static_cast<float>(kSize) };
        draw.index_count  = 3U;
        return recorder->record(*registry, draw);
    }

    /// @brief Compiles the graph and renders one frame.
    bool frame(const std::vector<::vsg::ref_ptr<::vsg::Node>>& content)
    {
        for (const auto& node : content) {
            target->renderGraph()->addChild(node);
        }
        auto command_graph = ::vsg::CommandGraph::create(created.device, created.queue_family);
        command_graph->addChild(target->renderGraph());
        // Both attachments are copied, each into its own destination: `capture(i)` is what makes the probe of
        // attachment i meaningful, and copying only attachment 0 would make the second probe an alias of the
        // first (which reads as "MRT works" while proving nothing).
        command_graph->addChild(target->capture(0U));
        command_graph->addChild(target->capture(1U));
        viewer->assignRecordAndSubmitTaskAndPresentation(::vsg::CommandGraphs{ command_graph });
        if (!viewer->compile()) {
            return false;
        }
        viewer->advanceToNextFrame();
        viewer->handleEvents();
        viewer->recordAndSubmit();
        viewer->deviceWaitIdle();
        return true;
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

/// @brief Whether @p pixel is the pass' clear colour (0.5, 0.25, 0) within a UNORM step.
bool isClearColor(const Rgba8& pixel)
{
    const auto near = [](std::uint8_t value, int expected) {
        return std::abs(static_cast<int>(value) - expected) <= 2;
    };
    return near(pixel.r, 128) && near(pixel.g, 64) && near(pixel.b, 0) && near(pixel.a, 255);
}

/// @brief Whether @p pixel is transparent black - what an extra attachment of a bootstrap pass gets.
bool isTransparentBlack(const Rgba8& pixel)
{
    return pixel.r == 0U && pixel.g == 0U && pixel.b == 0U && pixel.a == 0U;
}

bool isGreen(const Rgba8& pixel)
{
    return pixel.g > 200U && pixel.r < 60U && pixel.b < 60U;
}

bool isRed(const Rgba8& pixel)
{
    return pixel.r > 200U && pixel.g < 60U && pixel.b < 60U;
}

}  // namespace

TEST(MrtTargetTest, ExtraAttachmentsClearToTransparentBlackWhileAttachmentZeroTakesThePolicy)
{
    Fixture fixture;
    if (!fixture.build()) {
        GTEST_SKIP() << "no device satisfies the device-floor requirements";
    }
    ASSERT_TRUE(fixture.frame({})) << "the graph must compile and submit";

    const PixelProbe first  = fixture.target->probe(0U);
    const PixelProbe second = fixture.target->probe(1U);
    ASSERT_TRUE(first.valid());
    ASSERT_TRUE(second.valid());
    EXPECT_EQ(first.width(), static_cast<int>(kSize));
    EXPECT_EQ(second.width(), static_cast<int>(kSize));

    const Rgba8 center = first.pixel(static_cast<int>(kSize) / 2, static_cast<int>(kSize) / 2);
    EXPECT_TRUE(isClearColor(center)) << "attachment 0 is the pass' colour";
    EXPECT_TRUE(first.wholeImageMatches(center)) << "the bootstrap clear covers the whole attachment";

    const Rgba8 extra = second.pixel(static_cast<int>(kSize) / 2, static_cast<int>(kSize) / 2);
    EXPECT_TRUE(isTransparentBlack(extra))
        << "an extra attachment clears to transparent black, NOT to the pass' colour";
    EXPECT_TRUE(second.wholeImageMatches(extra));

    EXPECT_NE(first.pixel(0, 0), second.pixel(0, 0)) << "two attachments, two pictures";
}

TEST(MrtTargetTest, EachAttachmentKeepsItsOwnShaderOutput)
{
    Fixture fixture;
    if (!fixture.build()) {
        GTEST_SKIP() << "no device satisfies the device-floor requirements";
    }
    const auto triangle = fixture.triangle();
    ASSERT_NE(triangle, nullptr) << "the draw must be recordable";
    ASSERT_TRUE(fixture.frame({ triangle }));

    const PixelProbe first  = fixture.target->probe(0U);
    const PixelProbe second = fixture.target->probe(1U);
    ASSERT_TRUE(first.valid());
    ASSERT_TRUE(second.valid());

    const int center = static_cast<int>(kSize) / 2;
    EXPECT_TRUE(isGreen(first.pixel(center, center))) << "attachment 0 holds the first fragment output";
    EXPECT_TRUE(isRed(second.pixel(center, center)))
        << "attachment 1 holds the SECOND fragment output - if it were green, the copy-back had aliased the "
           "two attachments onto one buffer";

    // The depth attachment is present and cleared to the reverse-Z far plane (0.0), which is in front of nothing:
    // the triangle at z = 0.5 (depth 0.75) must survive it, and the corners must still be the clear values.
    EXPECT_TRUE(isClearColor(first.pixel(1, 1))) << "the corners are still the pass' clear colour";
    EXPECT_TRUE(isTransparentBlack(second.pixel(1, 1)));

    EXPECT_EQ(fixture.recorder->draws(), 1U);
    EXPECT_EQ(fixture.recorder->pipeline_binds(), 1U);
}
