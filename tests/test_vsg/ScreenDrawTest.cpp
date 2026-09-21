/**
 * @brief The full-screen drawing call: the engine's two descriptor ABIs meet in one place, and a pixel says
 * which one was bound.
 *
 * The cases run the CONTENT STACK (no plan, no session): an off-screen target holds a picture, and a
 * full-screen draw copies it into another target through the SDK's own screen programs - the shipped GLSL, not
 * a text written for this test. That is what makes the claim worth something: `BuiltinShaders::screenCopyProgram`
 * declares its sampler as `layout(binding = i) uniform sampler2D`, with no set qualifier, so if the rewrite put
 * its sampled inputs anywhere but set 0 the engine's own screen programs would compile against a layout their
 * bindings do not match.
 *
 * What only a pixel can report, and why these two:
 *
 *   * a copy lands inside the VIEWPORT rectangle (picture-in-picture): the area outside it keeps the
 *     destination's clear colour, so a draw that ignored the rectangle, or drew the triangle at the wrong
 *     scale, shows up as a whole-target picture instead;
 *   * binding i reads ATTACHMENT i: the source is an MRT target whose two attachments hold different colours,
 *     and the program compiled for binding 1 must read the second one - a binding that answered with
 *     attachment 0 would produce the other colour with every counter and validation check green.
 *
 * WHY THE SECOND CASE PAINTS ITS SOURCE WITH CONTENT instead of using the target's clear: an MRT target's
 * extra attachments are cleared to TRANSPARENT BLACK (see core/ClearPlan), and this engine blends with
 * SRC_ALPHA / ONE_MINUS_SRC_ALPHA, so a copy of a transparent attachment leaves the destination untouched -
 * indistinguishable from "nothing was drawn". A source that means something has to be shaded, which is also
 * what a real screen pass reads.
 *
 * A content draw and a full-screen draw differ in exactly this: geometry and blocks versus three generated
 * vertices and one sampled set. The counters the fixture checks (`screen_draws`, `input_binds`) exist to tell
 * "recorded the right thing" apart from "recorded nothing", which a black picture would also explain.
 */

#include <gtest/gtest.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <vector>

#include <vsg/app/CommandGraph.h>
#include <vsg/app/Viewer.h>
#include <vsg/core/Array.h>
#include <vsg/state/DescriptorImage.h>
#include <vsg/state/DescriptorSet.h>

#include <vine/graphics/BuiltinShaders.hpp>
#include <vine/graphics/RenderTarget.hpp>
#include <vine/graphics/ShaderProgram.hpp>

#include <vine/vsg/VsgVulkanEntryPoints.hpp>
#include <vine/vsg/api/BlockDescriptors.hpp>
#include <vine/vsg/api/BlockStorage.hpp>
#include <vine/vsg/api/ContentDraw.hpp>
#include <vine/vsg/api/ContentPipeline.hpp>
#include <vine/vsg/api/ContentSources.hpp>
#include <vine/vsg/api/Device.hpp>
#include <vine/vsg/api/OffscreenTarget.hpp>
#include <vine/vsg/api/StreamUploads.hpp>
#include <vine/vsg/core/PixelProbe.hpp>
#include <vine/vsg/core/Streams.hpp>

using vine::graphics::RenderTarget;
using vine::vsg::BlockDescriptors;
using vine::vsg::BlockStorage;
using vine::vsg::buildScreenProgramFacts;
using vine::vsg::ContentDraw;
using vine::vsg::ContentPipeline;
using vine::vsg::FactMiss;
using vine::vsg::OffscreenTarget;
using vine::vsg::ProgramFacts;
using vine::vsg::StreamUploads;
using vine::vsg::ViewportRect;
using vine::vsg::core::DrawKind;
using vine::vsg::core::PixelProbe;
using vine::vsg::core::Rgba8;
using vine::vsg::core::StateRegistry;
using vine::vsg::core::StreamKey;
using vine::vsg::core::StreamKind;
using vine::vsg::core::VariantPool;

namespace
{

constexpr std::uint32_t kSize = 64U;

/// @brief The source's first attachment (0.25, 0, 0, 1): a red nothing else in the frame uses.
constexpr float kSourceClear[4]{ 0.25F, 0.0F, 0.0F, 1.0F };

/// @brief The destination's clear (0, 0, 0.25, 1): a blue that is neither the source's colour nor black.
constexpr float kDestinationClear[4]{ 0.0F, 0.0F, 0.25F, 1.0F };

/// @brief The rectangle the full-screen draw covers, in pixels: a picture-in-picture quarter.
constexpr ViewportRect kPictureInPicture{ 8.0F, 8.0F, 16.0F, 16.0F };

bool isSourceRed(const Rgba8& pixel)
{
    return std::abs(static_cast<int>(pixel.r) - 64) <= 2 && pixel.g <= 2 && pixel.b <= 2 && pixel.a >= 253;
}

bool isDestinationBlue(const Rgba8& pixel)
{
    return pixel.r <= 2 && pixel.g <= 2 && std::abs(static_cast<int>(pixel.b) - 64) <= 2 && pixel.a >= 253;
}

bool isContentGreen(const Rgba8& pixel)
{
    return pixel.r <= 2 && std::abs(static_cast<int>(pixel.g) - 64) <= 2 && pixel.b <= 2 && pixel.a >= 253;
}

/// @brief The shader pair that paints an MRT source: attachment 0 red, attachment 1 green, both opaque.
ContentPipeline::Shaders mrtShaders()
{
    ContentPipeline::Shaders shaders;
    shaders.vertex = "#version 450\n"
                     "layout(location = 0) in vec3 position;\n"
                     "void main() { gl_Position = vec4(position.x, position.y, 0.5, 1.0); }\n";
    shaders.fragment = "#version 450\n"
                       "layout(location = 0) out vec4 first;\n"
                       "layout(location = 1) out vec4 second;\n"
                       "void main() { first = vec4(0.25, 0.0, 0.0, 1.0);\n"
                       "              second = vec4(0.0, 0.25, 0.0, 1.0); }\n";
    return shaders;
}

std::vector<std::byte> zeroedBlock(std::size_t bytes)
{
    return std::vector<std::byte>(bytes, std::byte{ 0 });
}

/// @brief The device, both targets and one full-screen layer, all built from one real device.
class Fixture
{
  public:
    /// @param source_attachments Colour attachments the source offers (1 = cleared, 2 = painted by content).
    /// @param sampled_attachment  The attachment the screen program samples: it names the BINDING, which the
    ///                            engine's full-screen ABI defines as the attachment index.
    bool build(std::uint32_t source_attachments, int sampled_attachment)
    {
        created = vine::vsg::createDevice();
        if (!created.ok) {
            return false;
        }

        OffscreenTarget::TargetLayout source_layout;
        source_layout.width   = kSize;
        source_layout.height  = kSize;
        source_layout.clear.color = true;
        for (std::size_t index = 0; index < 4U; ++index) {
            source_layout.clear.color_value[index] = kSourceClear[index];
        }
        source_layout.color_formats.assign(source_attachments, RenderTarget::ColorFormat::RGBA8);
        source = OffscreenTarget::create(created.device, source_layout);
        if (source == nullptr) {
            return false;
        }

        OffscreenTarget::Layout destination_layout{ kSize, kSize,
                                                    { kDestinationClear[0], kDestinationClear[1],
                                                      kDestinationClear[2], kDestinationClear[3] } };
        destination = OffscreenTarget::create(created.device, destination_layout);
        if (destination == nullptr) {
            return false;
        }

        // The program the pass draws through: the SDK's own screen copy, whose fragment stage names the binding
        // (see the file note). buildScreenProgramFacts composes it with the engine's full-screen vertex stage.
        program = vine::graphics::screenCopyProgram(sampled_attachment);
        if (program == nullptr) {
            return false;
        }
        ProgramFacts facts;
        if (buildScreenProgramFacts(*program, facts) != FactMiss::None) {
            return false;
        }
        pipelines = ContentPipeline::createScreen(facts.shaders);
        if (pipelines == nullptr) {
            return false;
        }
        program_identity = facts.program;
        program_revision = facts.revision;

        recorder = std::make_unique<ContentDraw>(*pipelines, pool,
                                                 vine::vsg::detail::fetchDynamicStateEntryPoints(created.device->vk(),
                                                                                                 created.instance->vk()));
        registry = std::make_unique<StateRegistry>(pool);
        viewer   = ::vsg::Viewer::create();
        if (viewer == nullptr) {
            return false;
        }

        if (source_attachments > 1U) {
            return buildContentStack();
        }
        return true;
    }

    /// @brief The sampler set: binding i is the source's attachment i, exactly as the ABI says.
    ::vsg::ref_ptr<::vsg::BindDescriptorSet> samplerSet()
    {
        const auto set_layout = pipelines->sampledSetLayout(source->colorAttachmentCount(), 0U);
        const auto sampler    = pipelines->inputSampler();
        if (set_layout == nullptr || sampler == nullptr) {
            return {};
        }

        ::vsg::Descriptors descriptors;
        for (std::uint32_t attachment = 0; attachment < source->colorAttachmentCount(); ++attachment) {
            auto view = source->colorView(attachment);
            if (view == nullptr) {
                return {};
            }
            descriptors.push_back(::vsg::DescriptorImage::create(
                ::vsg::ImageInfoList{ ::vsg::ImageInfo::create(sampler, view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL) },
                attachment, 0U, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER));
        }
        auto set = ::vsg::DescriptorSet::create(set_layout, descriptors);
        if (set == nullptr) {
            return {};
        }
        return ::vsg::BindDescriptorSet::create(VK_PIPELINE_BIND_POINT_GRAPHICS,
                                               pipelines->layoutFor(source->colorAttachmentCount(), 0U), 0U, set);
    }

    /// @brief Records the full-screen draw of the source into the destination, inside @p viewport.
    ::vsg::ref_ptr<::vsg::Node> draw(const ViewportRect& viewport)
    {
        const auto samplers = samplerSet();
        if (samplers == nullptr) {
            return {};
        }

        ContentDraw::ScreenDraw full_screen;
        full_screen.key.kind                  = DrawKind::Screen;
        full_screen.key.program               = program_identity;
        full_screen.key.revision              = program_revision;
        full_screen.key.compatibility.samples = 1U;
        full_screen.key.sampled_color_count   = source->colorAttachmentCount();
        full_screen.dynamic.depth             = vine::graphics::DepthMode::Disabled;  // a composite on top
        full_screen.samplers                  = samplers;
        full_screen.viewport                  = viewport;
        full_screen.color_attachments         = destination->colorAttachmentCount();
        return recorder->recordScreen(*registry, full_screen);
    }

    /// @brief Records the CONTENT draw that paints the MRT source (attachment 0 red, attachment 1 green).
    bool paintMrtSource()
    {
        storage->beginFrame();
        const auto view     = storage->writeView(zeroedBlock(288U));
        const auto block    = storage->writeDraw(zeroedBlock(80U));
        const auto material = storage->writeMaterial(&material_identity, 1U, zeroedBlock(64U));
        if (!view.valid || !block.valid) {
            return false;
        }

        auto positions  = ::vsg::vec3Array::create(3U);
        (*positions)[0] = ::vsg::vec3(-0.9F, -0.9F, 0.0F);
        (*positions)[1] = ::vsg::vec3(0.9F, -0.9F, 0.0F);
        (*positions)[2] = ::vsg::vec3(0.0F, 0.9F, 0.0F);
        auto indices    = ::vsg::uintArray::create(3U);
        (*indices)[0]   = 0U;
        (*indices)[1]   = 1U;
        (*indices)[2]   = 2U;

        StreamKey vertex_key;
        vertex_key.kind       = StreamKind::Vertex;
        vertex_key.location   = 0U;
        vertex_key.components = 3U;
        vertex_key.buffer     = &vertex_identity;
        vertex_key.revision   = 1U;
        vertex_key.count      = 9U;
        StreamKey index_key;
        index_key.kind       = StreamKind::Index;
        index_key.components = 1U;
        index_key.buffer     = &index_identity;
        index_key.revision   = 1U;
        index_key.count      = 3U;

        const auto vertex_bind = uploads->acquireVertex(vertex_key, positions);
        const auto index_bind  = uploads->acquireIndex(index_key, indices);
        if (vertex_bind.bind == nullptr || index_bind.bind == nullptr) {
            return false;
        }

        ContentDraw::Draw draw;
        draw.key.program                      = &content_program;
        draw.key.revision                     = 1U;
        draw.key.vertex_layout.canonical_mask = 0x1U;
        draw.key.compatibility.samples        = 1U;
        draw.blocks                           = content_descriptors->bind(
            content_pipelines->layout(),
            BlockDescriptors::Offsets{ view.offset, block.offset, material.offset });
        draw.vertex_binds      = std::span<const ::vsg::ref_ptr<::vsg::BindVertexBuffers>>(&vertex_bind.bind, 1U);
        draw.index             = index_bind.bind;
        draw.viewport          = ViewportRect{ 0.0F, 0.0F, static_cast<float>(kSize), static_cast<float>(kSize) };
        draw.index_count       = 3U;
        draw.color_attachments = 2U;  // the source's pass writes two attachments; the blend command covers both
        if (draw.blocks == nullptr) {
            return false;
        }

        const auto content = content_recorder->record(*content_registry, draw);
        if (content == nullptr) {
            return false;
        }
        source->renderGraph()->addChild(content);
        return true;
    }

    /// @brief Records one frame: the source's pass, the destination's pass, then the destination's copy.
    PixelProbe frame(::vsg::ref_ptr<::vsg::Node> destination_content)
    {
        auto command_graph = ::vsg::CommandGraph::create(created.device, created.queue_family);
        // The source's pass runs FIRST: it leaves its attachments in SHADER_READ_ONLY (see OffscreenTarget),
        // which is what the samplers below name.
        command_graph->addChild(source->renderGraph());
        destination->renderGraph()->addChild(destination_content);
        command_graph->addChild(destination->renderGraph());
        command_graph->addChild(destination->capture());

        viewer->assignRecordAndSubmitTaskAndPresentation(::vsg::CommandGraphs{ command_graph });
        if (!viewer->compile()) {
            return PixelProbe(0, 0, {});
        }
        viewer->advanceToNextFrame();
        viewer->handleEvents();
        viewer->recordAndSubmit();
        viewer->deviceWaitIdle();
        return destination->probe();
    }

  private:
    /// @brief The content stack a two-attachment source needs (block storage, uploads, its own pipelines).
    bool buildContentStack()
    {
        storage = BlockStorage::create(created.device, BlockStorage::Layout{});
        if (storage == nullptr) {
            return false;
        }
        content_descriptors = BlockDescriptors::create(created.device, *storage);
        if (content_descriptors == nullptr) {
            return false;
        }

        const ContentPipeline::VertexBinding   binding{ 0U, sizeof(float) * 3U, false };
        const ContentPipeline::VertexAttribute attribute{ 0U, 0U, VK_FORMAT_R32G32B32_SFLOAT, 0U };
        ContentPipeline::Settings              settings;
        settings.color_attachments = 2U;  // the source's pass writes two colour attachments
        content_pipelines          = ContentPipeline::create(content_descriptors->layout(),
                                                            std::span<const ContentPipeline::VertexBinding>(&binding, 1U),
                                                            std::span<const ContentPipeline::VertexAttribute>(&attribute, 1U),
                                                            mrtShaders(), settings);
        if (content_pipelines == nullptr) {
            return false;
        }

        uploads          = std::make_unique<StreamUploads>();
        content_recorder = std::make_unique<ContentDraw>(*content_pipelines, content_pool,
                                                         vine::vsg::detail::fetchDynamicStateEntryPoints(
                                                             created.device->vk(), created.instance->vk()));
        content_registry = std::make_unique<StateRegistry>(content_pool);
        return true;
    }

  public:
    vine::vsg::DeviceResult             created;
    std::unique_ptr<OffscreenTarget>    source;
    std::unique_ptr<OffscreenTarget>    destination;
    vine::intrusive_ptr<vine::graphics::ShaderProgram> program;
    const void*                         program_identity{nullptr};
    std::uint64_t                       program_revision{0};
    std::unique_ptr<ContentPipeline>    pipelines;
    VariantPool                         pool;
    std::unique_ptr<ContentDraw>        recorder;
    std::unique_ptr<StateRegistry>      registry;
    ::vsg::ref_ptr<::vsg::Viewer>       viewer;

    std::unique_ptr<BlockStorage>     storage;
    std::unique_ptr<BlockDescriptors> content_descriptors;
    std::unique_ptr<ContentPipeline>  content_pipelines;
    std::unique_ptr<StreamUploads>    uploads;
    std::unique_ptr<ContentDraw>      content_recorder;
    VariantPool                       content_pool;
    std::unique_ptr<StateRegistry>    content_registry;

    static int material_identity;
    static int vertex_identity;
    static int index_identity;
    static int content_program;
};

int Fixture::material_identity = 0;
int Fixture::vertex_identity   = 0;
int Fixture::index_identity    = 0;
int Fixture::content_program   = 0;

}  // namespace

TEST(ScreenDrawTest, TheShippedScreenCopyProgramLandsInsideTheViewportAndNowhereElse)
{
    Fixture fixture;
    if (!fixture.build(1U, 0)) {
        GTEST_SKIP() << "no device satisfies the requirements";
    }

    const auto content = fixture.draw(kPictureInPicture);
    ASSERT_NE(content, nullptr) << "the full-screen draw must record";

    const PixelProbe pixels = fixture.frame(content);
    ASSERT_TRUE(pixels.valid()) << "the destination must have been copied back";

    // The draw's counters, so "the right thing was recorded" is told apart from "nothing was recorded" (which
    // the picture would also explain).
    EXPECT_EQ(fixture.recorder->draws(), 1U);
    EXPECT_EQ(fixture.recorder->screen_draws(), 1U);
    EXPECT_EQ(fixture.recorder->pipeline_binds(), 1U);
    EXPECT_EQ(fixture.recorder->input_binds(), 1U);

    // Inside the rectangle: the source's picture, 1:1. Outside: the destination's clear, because a full-screen
    // draw covers its rectangle and nothing else.
    const Rgba8 inside  = pixels.pixel(static_cast<int>(kPictureInPicture.x) + 8, static_cast<int>(kPictureInPicture.y) + 8);
    const Rgba8 outside = pixels.pixel(2, 2);
    const Rgba8 right_of_rectangle =
        pixels.pixel(static_cast<int>(kPictureInPicture.x + kPictureInPicture.width) + 4, 12);

    EXPECT_TRUE(isSourceRed(inside))
        << "the copy must land inside the viewport, got (" << static_cast<int>(inside.r) << ", "
        << static_cast<int>(inside.g) << ", " << static_cast<int>(inside.b) << ")";
    EXPECT_TRUE(isDestinationBlue(outside))
        << "and the rest of the target keeps its own colour, got (" << static_cast<int>(outside.r) << ", "
        << static_cast<int>(outside.g) << ", " << static_cast<int>(outside.b) << ")";
    EXPECT_TRUE(isDestinationBlue(right_of_rectangle))
        << "a full-screen triangle that ignored the rectangle would paint here too";
}

TEST(ScreenDrawTest, TheBindingNamesTheAttachmentAndNotTheProgramsFirstOne)
{
    // The source is painted by a content pass: attachment 0 red, attachment 1 green (see the file note for why
    // the clear colour cannot do this). The program is compiled for binding 1, so the copy must produce the
    // SECOND attachment's colour - and exactly one other outcome is possible for a binding that answered with
    // attachment 0, which is what makes the pixel decisive.
    Fixture fixture;
    if (!fixture.build(2U, 1)) {
        GTEST_SKIP() << "no device satisfies the requirements";
    }
    ASSERT_TRUE(fixture.paintMrtSource()) << "the source's content pass must record";

    const auto content =
        fixture.draw(ViewportRect{ 0.0F, 0.0F, static_cast<float>(kSize), static_cast<float>(kSize) });
    ASSERT_NE(content, nullptr);

    const PixelProbe pixels = fixture.frame(content);
    ASSERT_TRUE(pixels.valid());
    ASSERT_EQ(fixture.source->colorAttachmentCount(), 2U);

    const Rgba8 centre = pixels.pixel(static_cast<int>(kSize / 2U), static_cast<int>(kSize / 2U));
    EXPECT_TRUE(isContentGreen(centre))
        << "binding 1 is attachment 1 (green), got (" << static_cast<int>(centre.r) << ", "
        << static_cast<int>(centre.g) << ", " << static_cast<int>(centre.b) << ")";
    EXPECT_FALSE(isSourceRed(centre)) << "attachment 0's red here would mean the binding was ignored";
}
