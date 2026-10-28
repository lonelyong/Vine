/**
 * @brief Multisampled targets, and the resolve that makes them a picture (see `.ai/design/graphics-rendering.md`).
 *
 * The sample count is a TARGET's property, and this is the file that proves what that means on a device: a
 * multisampled target draws into multisampled images and RESOLVES its colour into a single-sample image, and
 * that resolved image is what a later pass samples and what a readback copies. The evidence is the picture
 * itself, because it is the only thing that distinguishes a pass that multisampled from one that did not:
 *
 *   * at four samples the pixels a triangle's edge passes through hold a MIX of the triangle's colour and
 *     the clear colour - that is the whole point of the feature, and no single-sample pass can produce it
 *     (its fragments either cover a pixel or they do not);
 *   * at one sample those same pixels hold one colour or the other, and the colour readback still works -
 *     the resolve must not change what a single-sample target reads back, or every existing picture would
 *     silently change with this feature;
 *   * the DEPTH is not resolved (this slice does not do it), so a multisampled target refuses its depth
 *     readback with `Multisampled` rather than pretending the copy could happen.
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

#include <vine/graphics/RenderTarget.hpp>
#include <vine/graphics/backend/Readback.hpp>
#include <vine/graphics/backend/StateRegistry.hpp>
#include <vine/graphics/backend/Streams.hpp>
#include <vine/graphics/backend/VariantPool.hpp>
#include <vine/vsg/internal/BlockDescriptors.hpp>
#include <vine/vsg/internal/BlockStorage.hpp>
#include <vine/vsg/internal/ContentDraw.hpp>
#include <vine/vsg/internal/ContentPipeline.hpp>
#include <vine/vsg/internal/Device.hpp>
#include <vine/vsg/internal/HostTargets.hpp>
#include <vine/vsg/internal/OffscreenTarget.hpp>
#include <vine/vsg/internal/StreamUploads.hpp>
#include <vine/vsg/support/VsgVulkanEntryPoints.hpp>

namespace
{

using vn::graphics::RenderTarget;
using vn::graphics::backend::DepthProbe;
using vn::graphics::backend::PixelProbe;
using vn::graphics::backend::ReadbackKind;
using vn::graphics::backend::Rgba8;
using vn::graphics::backend::StateRegistry;
using vn::graphics::backend::StreamKey;
using vn::graphics::backend::StreamKind;
using vn::graphics::backend::VariantPool;
using vn::vsg::BlockDescriptors;
using vn::vsg::BlockStorage;
using vn::vsg::ContentDraw;
using vn::vsg::ContentPipeline;
using vn::vsg::OffscreenTarget;
using vn::vsg::StreamUploads;
using vn::vsg::ViewportRect;

constexpr std::uint32_t kSize = 64U;

/// @brief The pass' clear colour (0.5, 0.25, 0, 1) - chosen away from the shader's green so a blend of the two
///        is unmistakable, and away from black so "nothing was written" is not an option.
constexpr float kClear[4]{ 0.5F, 0.25F, 0.0F, 1.0F };

/// @brief The target under test: one colour attachment, one depth attachment, @p samples samples per pixel.
OffscreenTarget::TargetLayout msaaLayout(std::uint32_t samples)
{
    OffscreenTarget::TargetLayout layout;
    layout.width         = kSize;
    layout.height        = kSize;
    layout.color_formats = { RenderTarget::ColorFormat::RGBA8 };
    layout.depth_format  = RenderTarget::DepthFormat::D32;
    layout.samples       = samples;
    layout.clear.color   = true;
    for (std::size_t index = 0; index < 4U; ++index) {
        layout.clear.color_value[index] = kClear[index];
    }
    return layout;
}

std::vector<std::byte> drawBlock()
{
    const std::array<float, 16> matrix{ 1.0F, 0.0F, 0.0F, 0.0F, 0.0F, 1.0F, 0.0F, 0.0F, 0.0F, 0.0F, 1.0F, 0.0F, 0.0F, 0.0F, 0.0F, 1.0F };
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

/// @brief One fragment output: opaque green. The SAME shader is used at both sample counts, so a difference
///        between the two pictures cannot come from the program.
ContentPipeline::Shaders greenShaders()
{
    ContentPipeline::Shaders shaders;
    shaders.vertex   = "#version 450\n"
                       "layout(location = 0) in vec3 position;\n"
                       "void main() { gl_Position = vec4(position.x, position.y, 0.5, 1.0); }\n";
    shaders.fragment = "#version 450\n"
                       "layout(location = 0) out vec4 outColor;\n"
                       "void main() { outColor = vec4(0.0, 1.0, 0.0, 1.0); }\n";
    return shaders;
}

/// @brief A large triangle: its edges cross many pixels of a 64x64 target, which is what a sample count has to
///        show up in.
::vsg::ref_ptr<::vsg::vec3Array> trianglePositions()
{
    auto positions  = ::vsg::vec3Array::create(3U);
    (*positions)[0] = ::vsg::vec3(-0.9F, -0.9F, 0.0F);
    (*positions)[1] = ::vsg::vec3(0.9F, -0.9F, 0.0F);
    (*positions)[2] = ::vsg::vec3(0.0F, 0.9F, 0.0F);
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

std::span<const float> scalarsOf(const ::vsg::ref_ptr<::vsg::vec3Array>& array) noexcept
{
    return array == nullptr ? std::span<const float>{} : std::span<const float>(reinterpret_cast<const float*>(array->data()), array->size() * 3U);
}

std::span<const std::uint32_t> indicesOf(const ::vsg::ref_ptr<::vsg::uintArray>& array) noexcept
{
    return array == nullptr ? std::span<const std::uint32_t>{} : std::span<const std::uint32_t>(array->data(), array->size());
}

/// @brief What a case needs: a target of a given sample count and the stack that can draw into it.
struct Fixture {
    vn::vsg::DeviceResult             created;
    std::unique_ptr<OffscreenTarget>  target;
    std::unique_ptr<BlockStorage>     storage;
    std::unique_ptr<BlockDescriptors> descriptors;
    std::unique_ptr<ContentPipeline>  pipelines;
    std::unique_ptr<StreamUploads>    uploads;
    std::unique_ptr<ContentDraw>      recorder;
    VariantPool                       pool;
    std::unique_ptr<StateRegistry>    registry;
    ::vsg::ref_ptr<::vsg::Viewer>     viewer;
    std::uint32_t                     samples{ 1 };

    static int material_identity;

    bool build(std::uint32_t sample_count)
    {
        created = vn::vsg::createDevice();
        if (!created.ok) {
            return false;
        }
        return buildOn(created, sample_count);
    }

    /// @brief Builds this fixture on @p device: the DEVICE is a scarce resource in this suite (a build holds
    ///        a fixed number of them at once), so a case that needs two targets makes one device and shares it.
    bool buildOn(const vn::vsg::DeviceResult& device, std::uint32_t sample_count)
    {
        samples = sample_count;
        created = device;
        target  = OffscreenTarget::create(created.device, msaaLayout(samples));
        if (target == nullptr || target->colorAttachmentCount() != 1U || !target->hasDepth()) {
            return false;
        }
        storage = BlockStorage::create(created.device, BlockStorage::Layout{});
        if (storage == nullptr) {
            return false;
        }
        const ContentPipeline::Shaders shader_pair = greenShaders();
        vn::vsg::ProgramAbi            abi;
        if (vn::vsg::scanProgramAbi(shader_pair.vertex, shader_pair.fragment, {}, abi) != vn::vsg::FactMiss::None) {
            return false;
        }
        descriptors = BlockDescriptors::forAbi(abi, 0U, created.device, *storage);
        if (descriptors == nullptr) {
            return false;
        }

        const ContentPipeline::VertexBinding   binding{ 0U, sizeof(float) * 3U, false };
        const ContentPipeline::VertexAttribute attribute{ 0U, 0U, VK_FORMAT_R32G32B32_SFLOAT, 0U };
        ContentPipeline::Settings              settings;
        settings.color_attachments = 1U;
        pipelines                  = ContentPipeline::create(abi,
                                                             std::span<const ContentPipeline::VertexBinding>(&binding, 1U),
                                                             std::span<const ContentPipeline::VertexAttribute>(&attribute, 1U),
                                                             shader_pair,
                                                             settings);
        if (pipelines == nullptr) {
            return false;
        }

        uploads  = std::make_unique<StreamUploads>();
        recorder = std::make_unique<ContentDraw>(*pipelines, pool, vn::vsg::detail::fetchDynamicStateEntryPoints(created.device->vk(), created.instance->vk()));
        registry = std::make_unique<StateRegistry>(pool);
        viewer   = ::vsg::Viewer::create();
        return viewer != nullptr;
    }

    /// @brief Records one draw of the triangle, compiled for THIS target's sample count.
    ::vsg::ref_ptr<::vsg::Node> triangle()
    {
        storage->beginFrame();
        const auto view     = storage->writeView(viewBlock());
        const auto block    = storage->writeDraw(drawBlock());
        const auto material = storage->writeMaterial(&material_identity, 1U, materialBlock());
        if (!view.valid || !block.valid) {
            return {};
        }

        const auto vertex_bind = uploads->acquireVertex(positionKey(), scalarsOf(trianglePositions()));
        const auto index_bind  = uploads->acquireIndex(indexKey(), indicesOf(triangleIndices()));
        if (vertex_bind.bind == nullptr || index_bind.bind == nullptr) {
            return {};
        }

        ContentDraw::Draw draw;
        static int        program                                    = 0;
        draw.key.program                                             = &program;
        draw.key.revision                                            = 1U;
        draw.key.vertex_layout.canonical_mask                        = 0x1U;
        // The count is part of a pipeline's identity: the pipeline the API accepts for a multisampled pass has
        // to state the same count its attachments do, and the key is how a pass says which one it needs.
        draw.key.compatibility.samples                               = samples;
        const ::vsg::ref_ptr<::vsg::BindDescriptorSet> block_binds[] = {
            descriptors->bind(pipelines->layout(), BlockDescriptors::Offsets{ view.offset, block.offset, material.offset })
        };
        if (!descriptors->shape().empty()) {
            draw.blocks = block_binds;
        }
        draw.vertex_binds = std::span<const ::vsg::ref_ptr<::vsg::BindVertexBuffers>>(&vertex_bind.bind, 1U);
        draw.index        = index_bind.bind;
        draw.viewport     = ViewportRect{ 0.0F, 0.0F, static_cast<float>(kSize), static_cast<float>(kSize) };
        draw.index_count  = 3U;
        return recorder->record(*registry, draw);
    }

    /// @brief Compiles the graph and renders one frame, copying the colour attachment back.
    bool frame(const std::vector<::vsg::ref_ptr<::vsg::Node>>& content)
    {
        for (const auto& node : content) {
            target->renderGraph()->addChild(node);
        }
        auto command_graph = ::vsg::CommandGraph::create(created.device, created.queue_family);
        command_graph->addChild(target->renderGraph());
        command_graph->addChild(target->capture(0U));
        // Both attachments are copied: a depth case that recorded only the colour copy would read an
        // unrecorded buffer and call the target unreadable.
        if (const ::vsg::ref_ptr<::vsg::Node> depth_copy = target->captureDepth()) {
            command_graph->addChild(depth_copy);
        }
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

bool near(int value, int expected)
{
    return std::abs(value - expected) <= 2;
}

/// @brief Whether @p pixel is the pass' clear colour (0.5, 0.25, 0).
bool isClearColor(const Rgba8& pixel)
{
    return near(pixel.r, 128) && near(pixel.g, 64) && near(pixel.b, 0) && near(pixel.a, 255);
}

/// @brief Whether @p pixel is the shader's green.
bool isGreen(const Rgba8& pixel)
{
    return pixel.r < 10U && pixel.g > 245U && pixel.b < 10U && pixel.a > 245U;
}

/// @brief How many pixels of @p probe are a MIX - neither the clear colour nor the triangle's.
///
/// This is the number a sample count is visible in: a single-sample pass covers a pixel or it does not, so its
/// edges are a stair of pure colours; a multisampled pass gives the pixels an edge passes through one sample
/// per fragment coverage, which blends the two colours.
std::size_t mixedPixelCount(const PixelProbe& probe)
{
    std::size_t mixed = 0U;
    for (int y = 0; y < probe.height(); ++y) {
        for (int x = 0; x < probe.width(); ++x) {
            const Rgba8 pixel = probe.pixel(x, y);
            if (!isClearColor(pixel) && !isGreen(pixel)) {
                ++mixed;
            }
        }
    }
    return mixed;
}

} // namespace

TEST(MsaaTargetTest, FourSamplesBlendThePixelsATriangleEdgePassesThrough)
{
    Fixture single;
    if (!single.build(1U)) {
        GTEST_SKIP() << "no device satisfies the device-floor requirements";
    }
    const auto single_triangle = single.triangle();
    ASSERT_NE(single_triangle, nullptr) << "the draw must be recordable";
    ASSERT_TRUE(single.frame({ single_triangle }));

    Fixture four;
    ASSERT_TRUE(four.buildOn(single.created, 4U)) << "the same device must build a four-sample target";
    EXPECT_EQ(four.target->layout().samples, 4U) << "the count is the target's own, and the layout reports it";
    const auto four_triangle = four.triangle();
    ASSERT_NE(four_triangle, nullptr) << "a pipeline must compile for the four-sample pass";
    ASSERT_TRUE(four.frame({ four_triangle }));

    const PixelProbe sharp = single.target->probe(0U);
    ASSERT_TRUE(sharp.valid());
    EXPECT_EQ(mixedPixelCount(sharp), 0U) << "a single-sample pass covers a pixel or it does not: its edges are a stair of pure colours";

    const PixelProbe smooth = four.target->probe(0U);
    ASSERT_TRUE(smooth.valid());
    EXPECT_EQ(smooth.width(), static_cast<int>(kSize));
    EXPECT_EQ(smooth.height(), static_cast<int>(kSize));
    EXPECT_GT(mixedPixelCount(smooth), 0U) << "the pixels the triangle's edges pass through hold a blend of the two colours - which is what "
                                              "multisampling is, and the only evidence that the count reached the pipeline AND the images";

    // The interior and the exterior are still the colours they were: the resolve averages SAMPLES, it does not
    // wash the whole picture out.
    const int center = static_cast<int>(kSize) / 4;
    EXPECT_TRUE(isGreen(smooth.pixel(center, center))) << "the inside of the triangle is still the shader's green";
    EXPECT_TRUE(isClearColor(smooth.pixel(1, 1))) << "a corner outside it is still the pass' clear colour";
}

TEST(MsaaTargetTest, AFourSampleTargetsDepthIsTheResolvedPicture)
{
    Fixture single;
    if (!single.build(1U)) {
        GTEST_SKIP() << "no device satisfies the device-floor requirements";
    }
    const auto single_triangle = single.triangle();
    ASSERT_NE(single_triangle, nullptr);
    ASSERT_TRUE(single.frame({ single_triangle }));

    Fixture four;
    ASSERT_TRUE(four.buildOn(single.created, 4U));
    const auto four_triangle = four.triangle();
    ASSERT_NE(four_triangle, nullptr);
    ASSERT_TRUE(four.frame({ four_triangle }));

    // COLOUR and DEPTH are both readable, and both are the RESOLVED single-sample images (see
    // OffscreenTarget::Attachments::picture / depthPicture): the sample count never reaches a copy.
    EXPECT_TRUE(four.target->probe(0U).valid()) << "the resolved colour image is what a capture copies";

    const DepthProbe sharp  = single.target->depthProbe();
    const DepthProbe smooth = four.target->depthProbe();
    ASSERT_TRUE(sharp.valid()) << "a single-sample depth reads back";
    ASSERT_TRUE(smooth.valid()) << "a four-sample depth reads back through its RESOLVED image";
    EXPECT_EQ(smooth.width(), static_cast<int>(kSize));

    // The resolve mode is `sample0`, so DEPTH TAKES NO INTERMEDIATE VALUES - unlike colour, whose edge pixels
    // blend. That is the whole difference between the two attachments' resolves, and it is what makes turning
    // multisampling on unable to change the depth a later pass tests against: every pixel holds either the
    // triangle's depth (the shader writes clip z = 0.5, and the API's NDC z is already [0, 1], so the window
    // depth is 0.5) or the clear value (0, the reverse-Z far plane).
    const auto is_one_of_the_two = [](const DepthProbe& probe) {
        std::size_t other = 0U;
        for (int y = 0; y < probe.height(); ++y) {
            for (int x = 0; x < probe.width(); ++x) {
                const float value    = probe.depthAt(x, y);
                const bool  triangle = std::abs(value - 0.5F) < 0.01F;
                const bool  cleared  = std::abs(value) < 0.01F;
                if (!triangle && !cleared) {
                    ++other;
                }
            }
        }
        return other;
    };
    EXPECT_EQ(is_one_of_the_two(sharp), 0U);
    EXPECT_EQ(is_one_of_the_two(smooth), 0U) << "a depth resolve is not a colour resolve: sample0 picks a sample's value, it does not average";

    // ... and the VALUES agree where the picture does: the interior is the triangle's depth in both, the
    // exterior is the clear value in both.
    const int center = static_cast<int>(kSize) / 4;
    EXPECT_NEAR(smooth.depthAt(center, center), sharp.depthAt(center, center), 0.01F);
    EXPECT_NEAR(smooth.depthAt(1, 1), sharp.depthAt(1, 1), 0.01F);
}

/*
 * @brief What the host's table answers for the descriptions a device can and cannot build - through `ensure`,
 *        which is the door the backend uses.
 *
 * TWO refusals and they are about different facts, both answered BEFORE anything is created (so they are the
 * description's, not a build that failed): a count above the device's limit, and a MULTISAMPLED BORROWER (what
 * it would attach is the lender's single-sample picture while its own pass draws at another count). Everything
 * else a multisampled target asks for is legal now that depth is resolved: promoting its own depth and lending
 * it to a single-sample pass are the two cases below that MUST build - they are the control that makes the
 * refusal above mean "this combination", not "multisampling".
 */
TEST(MsaaTargetTest, ADescriptionTheDeviceCannotBuildIsRefusedWithTheReasonItCannot)
{
    const vn::vsg::DeviceResult created = vn::vsg::createDevice();
    if (!created.ok) {
        GTEST_SKIP() << "no device satisfies the device-floor requirements";
    }

    vn::vsg::HostTargets targets;
    targets.setMaxSamples(4U);

    const vn::intrusive_ptr<RenderTarget> too_many(new RenderTarget());
    too_many->attachColor(RenderTarget::ColorFormat::RGBA8);
    too_many->setSize(16, 16);
    too_many->setSamples(8U);
    EXPECT_EQ(targets.ensure(*too_many, created.device).state, vn::vsg::HostTargets::State::SamplesUnsupported)
        << "eight samples is above this table's limit of four";

    const vn::intrusive_ptr<RenderTarget> lender(new RenderTarget());
    lender->attachColor(RenderTarget::ColorFormat::RGBA8);
    lender->attachDepth(RenderTarget::DepthFormat::D32);
    lender->setDepthPromotion(false);
    lender->setSize(16, 16);
    EXPECT_EQ(targets.ensure(*lender, created.device).state, vn::vsg::HostTargets::State::Ready);

    const vn::intrusive_ptr<RenderTarget> borrower(new RenderTarget());
    borrower->attachColor(RenderTarget::ColorFormat::RGBA8);
    borrower->shareDepth(lender);
    borrower->setSize(16, 16);
    borrower->setSamples(4U);
    EXPECT_EQ(targets.ensure(*borrower, created.device).state, vn::vsg::HostTargets::State::SamplesUnsupported)
        << "one depth image cannot be a four-sample attachment for one pass and a single-sample one for "
           "another";

    // A multisampled lender: legal, because a borrower attaches its RESOLVED picture - and the borrower is
    // single-sample, which is the shape the refusal above rules out when it is not.
    lender->setSamples(4U);
    (void)targets.observe(*lender);
    EXPECT_EQ(targets.ensure(*lender, created.device).state, vn::vsg::HostTargets::State::Ready)
        << "a multisampled target lends its resolved depth picture like any other depth";

    borrower->setSamples(1U);
    (void)targets.observe(*borrower);
    EXPECT_EQ(targets.ensure(*borrower, created.device).state, vn::vsg::HostTargets::State::Ready)
        << "a single-sample pass borrowing that picture is the case the resolve made possible";

    // Promoting a multisampled target's OWN depth: legal, and it is the resolved image a shader samples.
    const vn::intrusive_ptr<RenderTarget> promoted(new RenderTarget());
    promoted->attachColor(RenderTarget::ColorFormat::RGBA8);
    promoted->attachDepth(RenderTarget::DepthFormat::D32);
    promoted->setSize(16, 16);
    promoted->setSamples(4U);
    EXPECT_TRUE(promoted->isDepthPromoted()) << "the SDK default asks for a sampleable depth";
    EXPECT_EQ(targets.ensure(*promoted, created.device).state, vn::vsg::HostTargets::State::Ready)
        << "the multisampled image is a draw target only; the RESOLVED one is what promotion makes sampleable";
}

/*
 * @brief Changing the count on a LIVE target is a REBUILD, and it replaces the images rather than drawing on.
 *
 * The count is part of the shape, so a host that announces a different one gets a plan that says Rebuild and a
 * target that replaces its image set - a target cannot draw multisampled into images built for one sample, and
 * pretending otherwise would be a stale picture nobody could explain. The clear policy and the depth policy are
 * the target's own and survive the rebuild (see OffscreenTarget::layout).
 */
TEST(MsaaTargetTest, RebuildingAtANewCountReplacesTheImages)
{
    const vn::vsg::DeviceResult created = vn::vsg::createDevice();
    if (!created.ok) {
        GTEST_SKIP() << "no device satisfies the device-floor requirements";
    }
    auto target = OffscreenTarget::create(created.device, msaaLayout(1U));
    ASSERT_NE(target, nullptr);
    EXPECT_EQ(target->shape().samples, 1U);
    const ::vsg::ref_ptr<::vsg::RenderGraph> before = target->renderGraph();

    vn::graphics::backend::FrameTimeline   timeline;
    vn::graphics::backend::RetirementQueue queue(3U);

    OffscreenTarget::TargetLayout four = target->layout();
    EXPECT_EQ(four.samples, 1U) << "the layout is what the target was built for";
    four.samples                           = 4U;
    const OffscreenTarget::Rebuilt rebuilt = target->rebuild(four, timeline, queue);
    EXPECT_TRUE(rebuilt.replaced) << "a different count is a different shape: it has to be built, not drawn on";
    EXPECT_EQ(target->shape().samples, 4U);
    EXPECT_EQ(target->layout().samples, 4U);
    EXPECT_NE(target->renderGraph(), before) << "new images and a new framebuffer: a new pass graph";
    EXPECT_EQ(target->layout().depth_sampleable, four.depth_sampleable)
        << "the depth policy is the target's and survives the rebuild (the plan does not carry it)";
    EXPECT_TRUE(target->hasDepth()) << "the depth attachment is rebuilt with the colour ones";

    // And the new images are a picture: an empty frame comes back as the clear colour.
    auto command_graph = ::vsg::CommandGraph::create(created.device, created.queue_family);
    command_graph->addChild(target->renderGraph());
    command_graph->addChild(target->capture(0U));
    auto viewer = ::vsg::Viewer::create();
    viewer->assignRecordAndSubmitTaskAndPresentation(::vsg::CommandGraphs{ command_graph });
    ASSERT_TRUE(viewer->compile());
    viewer->advanceToNextFrame();
    viewer->handleEvents();
    viewer->recordAndSubmit();
    viewer->deviceWaitIdle();

    const PixelProbe probe = target->probe(0U);
    ASSERT_TRUE(probe.valid()) << "the resolved image of a rebuilt target is readable";
    EXPECT_EQ(probe.width(), static_cast<int>(kSize));
    EXPECT_TRUE(isClearColor(probe.pixel(0, 0)));
}
