/**
 * @brief Borrowed depth: a second target drawing against the depth an earlier pass wrote (see
 * `.ai/design/vsg-reimplementation.md` D6, `core::depthPlan`, milestone M3c).
 *
 * Three facts interact here - promotion (the host wants a sampleable depth), borrowing (a target uses another
 * target's depth image) and preservation (a pass LOADs the depth an earlier pass wrote) - and all three change
 * the same thing: the attachment's initial layout and load op. That is why the plan is a core function of the
 * facts rather than a flag the API layer keeps, and why this file has two halves:
 *
 *   * the FACTS half, on a real device but without pixels: a borrower reports `borrowed`, never `sampleable`
 *     and never `preserve`; lending revokes the lender's promotion (its depth is now LOADed by a pass) and the
 *     revocation disappears with the borrower; a source without a depth or with another format is refused;
 *   * the PIXELS half, which is the only thing that can prove the depth is actually SHARED: the lender draws a
 *     near triangle, the borrower draws two far triangles - one exactly where the lender's is (it must be
 *     rejected by the depth the lender wrote) and one elsewhere (it must appear). A borrower that had its own
 *     cleared depth would draw BOTH, and a borrower that drew nothing at all would be caught by the second
 *     triangle.
 */

#include <gtest/gtest.h>

#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <span>
#include <vector>

#include <vsg/app/CommandGraph.h>
#include <vsg/app/Viewer.h>
#include <vsg/core/Array.h>
#include <vsg/core/Exception.h>
#include <vsg/vk/Device.h>

#include <vine/vsg/VsgVulkanEntryPoints.hpp>
#include <vine/vsg/api/BlockDescriptors.hpp>
#include <vine/vsg/api/BlockStorage.hpp>
#include <vine/vsg/api/ContentDraw.hpp>
#include <vine/vsg/api/ContentPipeline.hpp>
#include <vine/vsg/api/Device.hpp>
#include <vine/vsg/api/OffscreenTarget.hpp>
#include <vine/vsg/api/StreamUploads.hpp>
#include <vine/vsg/core/DepthProbe.hpp>
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

/// @brief The clear colour both passes use (0, 0, 0.25), so "nothing was drawn here" is distinguishable from
///        both triangle colours.
constexpr float kClear[4]{ 0.0F, 0.0F, 0.25F, 1.0F };

/// @brief The shape under test: one colour attachment and a depth attachment.
OffscreenTarget::TargetLayout depthLayout(bool sampleable)
{
    OffscreenTarget::TargetLayout layout;
    layout.width            = kSize;
    layout.height           = kSize;
    layout.color_formats    = { RenderTarget::ColorFormat::RGBA8 };
    layout.depth_format     = RenderTarget::DepthFormat::D32;
    layout.depth_sampleable = sampleable;
    layout.clear.color      = true;
    for (std::size_t index = 0; index < 4U; ++index) {
        layout.clear.color_value[index] = kClear[index];
    }
    return layout;
}

/**
 * @brief The block bytes one draw reads: the model matrix (placing x, y and the DEPTH) then the colour.
 *
 * The depth is placed by the matrix rather than by the geometry: the vertex stage multiplies the position, so
 * one triangle mesh can be drawn near and far without a second vertex buffer - which is what makes "the same
 * triangle at two depths" a two-line difference instead of a second upload.
 */
std::vector<std::byte> drawBlock(float z, float translate_x, float green, float red)
{
    const std::array<float, 16> matrix{ 1.0F, 0.0F, 0.0F, 0.0F, 0.0F, 1.0F, 0.0F, 0.0F,
                                        0.0F, 0.0F, 1.0F, 0.0F, translate_x, 0.0F, z, 1.0F };
    std::vector<std::byte>      block(80U, std::byte{ 0 });
    std::memcpy(block.data(), matrix.data(), sizeof(matrix));
    const std::array<float, 4> params{ green, red, 0.0F, 0.0F };
    std::memcpy(block.data() + sizeof(matrix), params.data(), sizeof(params));
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

/// @brief A shader pair: the draw block places the triangle (including its depth), the colour comes from the
///        draw block's parameters (green, red).
ContentPipeline::Shaders depthShaders()
{
    ContentPipeline::Shaders shaders;
    shaders.vertex = "#version 450\n"
                     "layout(location = 0) in vec3 position;\n"
                     "layout(set = 0, binding = 1) uniform DrawBlock { mat4 model; vec4 params; } draw;\n"
                     "void main() { gl_Position = draw.model * vec4(position, 1.0); }\n";
    shaders.fragment = "#version 450\n"
                       "layout(location = 0) out vec4 outColor;\n"
                       "layout(set = 0, binding = 1) uniform DrawBlock { mat4 model; vec4 params; } draw;\n"
                       "void main() { outColor = vec4(draw.params.y, draw.params.x, 0.0, 1.0); }\n";
    return shaders;
}

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

/// @brief The device plus everything needed to record draws into it (shared by both targets).
struct Stack
{
    vine::vsg::DeviceResult            created;
    std::unique_ptr<BlockStorage>      storage;
    std::unique_ptr<BlockDescriptors>  descriptors;
    std::unique_ptr<ContentPipeline>   pipelines;
    std::unique_ptr<StreamUploads>     uploads;
    std::unique_ptr<ContentDraw>       recorder;
    VariantPool                        pool;
    std::unique_ptr<StateRegistry>     registry;
    ::vsg::ref_ptr<::vsg::Viewer>      viewer;

    static int material_identity;

    bool build()
    {
        vine::vsg::DeviceOptions options;
        options.validation = true;  // the shared depth is also a layout question, so the layers are on
        created            = vine::vsg::createDevice(options);
        if (!created.ok) {
            return false;
        }
        if (!created.validation) {
            // The instrument is asked for and its absence is reported, not silently traded for a weaker set of
            // assertions: the layer is a separate package (see the repo's own lavapipe gate, which warns and
            // carries on). The pixels and the counters below are still asserted, which is the evidence that
            // does not depend on it.
            std::cerr << "[warning] VK_LAYER_KHRONOS_validation is not available: this phase's evidence is "
                         "two-way (counters and pixels)" << std::endl;
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
        pipelines = ContentPipeline::create(descriptors->layout(),
                                            std::span<const ContentPipeline::VertexBinding>(&binding, 1U),
                                            std::span<const ContentPipeline::VertexAttribute>(&attribute, 1U),
                                            depthShaders());
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

    /// @brief Records one triangle: at depth @p z, translated in x, coloured (green, red).
    ::vsg::ref_ptr<::vsg::Node> triangle(float z, float translate_x, float green, float red)
    {
        storage->beginFrame();
        const auto view     = storage->writeView(viewBlock());
        const auto block    = storage->writeDraw(drawBlock(z, translate_x, green, red));
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

int Stack::material_identity = 0;

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

bool isRed(const Rgba8& pixel)
{
    return pixel.r > 200U && pixel.g < 60U && pixel.b < 60U;
}

bool isGreen(const Rgba8& pixel)
{
    return pixel.g > 200U && pixel.r < 60U && pixel.b < 60U;
}

bool isClear(const Rgba8& pixel)
{
    const auto near = [](std::uint8_t value, int expected) {
        return std::abs(static_cast<int>(value) - expected) <= 2;
    };
    return near(pixel.r, 0) && near(pixel.g, 0) && near(pixel.b, 64) && near(pixel.a, 255);
}

/// @brief The centroid of a triangle whose local half-width is 0.4 (y of the centroid is a third of it).
Pixel centroidOf(float translate_x)
{
    return pixelAt(translate_x, -0.4F / 3.0F);
}

// Vulkan's normalised device coordinates are ALREADY the depth: a vertex at gl_Position.z = z is tested and
// written at depth z under the default viewport (minDepth 0, maxDepth 1). The OpenGL arithmetic
// (z * 0.5 + 0.5) does NOT apply, and getting it wrong is invisible in a colour picture: a "far" triangle
// written at z = -0.5 is simply below the far plane (0.0 here) and gets rejected EVERYWHERE, which looks
// exactly like a depth buffer that occludes it. Measured, not assumed: see the depth assertions below.
constexpr float kFarZ  = 0.1F;   ///< Depth 0.1: in front of the clear (0.0), behind the lender's 0.5.
constexpr float kNearZ = 0.5F;   ///< Depth 0.5: nearer, which is what reverse-Z's GREATER comparison reads.

}  // namespace

TEST(SharedDepthTest, ABorrowedDepthIsNeverSampleableAndRevokesTheLendersPromotion)
{
    const auto created = vine::vsg::createDevice();
    if (!created.ok) {
        GTEST_SKIP() << "no device satisfies the device-floor requirements";
    }
    const auto& device = created.device;

    // The lender owns a depth and asked for it to be sampleable: that is the promotion.
    auto lender = OffscreenTarget::create(device, depthLayout(/*sampleable*/ true));
    ASSERT_NE(lender, nullptr);
    ASSERT_TRUE(lender->hasDepth());
    EXPECT_TRUE(lender->depth().has_depth);
    EXPECT_TRUE(lender->depth().sampleable) << "asked for and nothing preserves it (yet)";
    EXPECT_FALSE(lender->depth().borrowed);
    EXPECT_EQ(lender->depthSource(), nullptr);

    // A colour-only target has nothing to lend, and a different depth format cannot be borrowed: both are
    // refused before anything is built.
    auto colour_only = OffscreenTarget::create(device, OffscreenTarget::TargetLayout{ kSize, kSize });
    ASSERT_NE(colour_only, nullptr) << "a colour-only target is a valid target (it just has no depth to lend)";
    EXPECT_EQ(OffscreenTarget::create(device, depthLayout(false), colour_only.get()), nullptr)
        << "borrowing a depth from a target that has none must be refused, not built";
    OffscreenTarget::TargetLayout refused = depthLayout(false);
    refused.depth_format                  = RenderTarget::DepthFormat::D24;
    EXPECT_EQ(OffscreenTarget::create(device, refused, lender.get()), nullptr)
        << "formats are part of the pass description: a mismatched borrow is refused";

    auto borrower = OffscreenTarget::create(device, depthLayout(false), lender.get());
    ASSERT_NE(borrower, nullptr);
    EXPECT_TRUE(borrower->hasDepth());
    EXPECT_EQ(borrower->depthSource(), lender.get());
    EXPECT_TRUE(borrower->depth().borrowed);
    EXPECT_FALSE(borrower->depth().sampleable) << "the policy is the lender's, not the borrower's";
    EXPECT_FALSE(borrower->depth().preserve) << "a borrower does not promise the lender's depth to a shader";

    // The borrower's pass LOADs the lender's depth, and that is exactly a depth-preserving pass: the lender's
    // promotion is revoked for as long as the loan lasts.
    EXPECT_TRUE(lender->depth().preserve);
    EXPECT_FALSE(lender->depth().sampleable) << "a LOAD pass revokes the promotion";

    // Readback of a shared depth is PER TARGET: each target copies the image into a buffer of its own, so the
    // lender's capture says nothing about the borrower's - a borrower whose own copy never ran would otherwise
    // answer a probe with whatever its buffer held.
    const vine::vsg::core::ReadbackRequest depth_request{ vine::vsg::core::ReadbackKind::Depth, 0U };
    EXPECT_EQ(static_cast<int>(lender->readbackResult(depth_request).refusal),
              static_cast<int>(vine::vsg::core::ReadbackRefusal::NotCaptured));
    EXPECT_EQ(static_cast<int>(borrower->readbackResult(depth_request).refusal),
              static_cast<int>(vine::vsg::core::ReadbackRefusal::NotCaptured));
    EXPECT_TRUE(lender->captureDepth() != nullptr);
    EXPECT_EQ(static_cast<int>(lender->readbackResult(depth_request).refusal),
              static_cast<int>(vine::vsg::core::ReadbackRefusal::None));
    EXPECT_EQ(static_cast<int>(borrower->readbackResult(depth_request).refusal),
              static_cast<int>(vine::vsg::core::ReadbackRefusal::NotCaptured))
        << "the lender's copy-back is the lender's: the borrower's probe reads the borrower's buffer";
    EXPECT_FALSE(borrower->depthProbe().valid());
    EXPECT_TRUE(borrower->captureDepth() != nullptr) << "the borrower gets its own destination and copy";
    EXPECT_EQ(static_cast<int>(borrower->readbackResult(depth_request).refusal),
              static_cast<int>(vine::vsg::core::ReadbackRefusal::None));

    borrower.reset();
    EXPECT_TRUE(lender->depth().sampleable) << "the revocation goes away with the borrower";
    EXPECT_FALSE(lender->depth().preserve);
}

TEST(SharedDepthTest, TheBorrowerSeesTheDepthTheLenderWrote)
{
    Stack stack;
    try {
        if (!stack.build()) {
            GTEST_SKIP() << "no device satisfies the device-floor requirements";
        }
    }
    catch (const ::vsg::Exception& error) {
        FAIL() << "build threw a vsg exception: " << error.message << " (VkResult " << error.result << ")";
    }
    const auto& device = stack.created.device;

    std::unique_ptr<OffscreenTarget> lender;
    std::unique_ptr<OffscreenTarget> borrower;
    try {
        lender   = OffscreenTarget::create(device, depthLayout(/*sampleable*/ false));
        borrower = lender != nullptr ? OffscreenTarget::create(device, depthLayout(/*sampleable*/ false), lender.get())
                                     : nullptr;
    }
    catch (const ::vsg::Exception& error) {
        FAIL() << "target create threw a vsg exception: " << error.message << " (VkResult " << error.result << ")";
    }
    ASSERT_NE(lender, nullptr);
    ASSERT_NE(borrower, nullptr) << "the shared-depth target must be creatable";

    // The lender draws one NEAR red triangle on the left; the borrower draws two FAR green triangles - one
    // exactly where the lender's is (the shared depth must reject it) and one on the right (which must appear).
    const auto lender_triangle   = stack.triangle(kNearZ, -0.4F, 0.0F, 1.0F);
    const auto borrower_rejected = stack.triangle(kFarZ, -0.4F, 1.0F, 0.0F);
    const auto borrower_visible  = stack.triangle(kFarZ, 0.4F, 1.0F, 0.0F);
    ASSERT_NE(lender_triangle, nullptr);
    ASSERT_NE(borrower_rejected, nullptr);
    ASSERT_NE(borrower_visible, nullptr);

    lender->renderGraph()->addChild(lender_triangle);
    (void)0;
    borrower->renderGraph()->addChild(borrower_rejected);
    borrower->renderGraph()->addChild(borrower_visible);

    // One command graph, in order: the lender's pass (which writes the depth), then the borrower's (which
    // LOADs it), then both readbacks. The borrower's pass must run after the lender's - that is the producer /
    // consumer edge the shared image carries.
    auto command_graph = ::vsg::CommandGraph::create(device, stack.created.queue_family);
    command_graph->addChild(lender->renderGraph());
    command_graph->addChild(lender->capture());
    command_graph->addChild(borrower->renderGraph());
    command_graph->addChild(borrower->capture());
    // The depth copy reads the SAME image the borrower's pass just wrote into, and it runs after both passes:
    // what it holds is therefore the state the two passes left behind, which is what the assertions read.
    command_graph->addChild(lender->captureDepth());
    // A BORROWER's depth copy reads the same shared image through its own destination buffer: that is what the
    // readback of a borrowed depth means, and the assertions below read it back through the borrower to say so.
    command_graph->addChild(borrower->captureDepth());
    try {
        stack.viewer->assignRecordAndSubmitTaskAndPresentation(::vsg::CommandGraphs{ command_graph });
    }
    catch (const ::vsg::Exception& error) {
        FAIL() << "assign threw a vsg exception: " << error.message << " (VkResult " << error.result << ")";
    }
    try {
        ASSERT_TRUE(stack.viewer->compile());
    }
    catch (const ::vsg::Exception& error) {
        FAIL() << "compile threw a vsg exception: " << error.message << " (VkResult " << error.result << ")";
    }
    stack.viewer->advanceToNextFrame();
    stack.viewer->handleEvents();
    try {
        stack.viewer->recordAndSubmit();
        stack.viewer->deviceWaitIdle();
    }
    catch (const ::vsg::Exception& error) {
        FAIL() << "submit threw a vsg exception: " << error.message << " (VkResult " << error.result << ")";
    }

    const PixelProbe lender_pixels   = lender->probe();
    const PixelProbe borrower_pixels = borrower->probe();
    ASSERT_TRUE(lender_pixels.valid());
    ASSERT_TRUE(borrower_pixels.valid());

    const Pixel shared = centroidOf(-0.4F);
    const Pixel own    = centroidOf(0.4F);

    EXPECT_TRUE(isRed(lender_pixels.pixel(shared.x, shared.y))) << "the lender drew its near triangle";
    EXPECT_TRUE(isClear(lender_pixels.pixel(own.x, own.y))) << "and nothing else";

    EXPECT_TRUE(isClear(borrower_pixels.pixel(shared.x, shared.y)))
        << "the borrower's far triangle stood where the lender's near one already wrote depth: the shared depth "
           "test must reject it. A borrower with a depth of its own would paint this pixel green";
    const Rgba8 own_pixel    = borrower_pixels.pixel(own.x, own.y);
    const Rgba8 shared_pixel = borrower_pixels.pixel(shared.x, shared.y);
    EXPECT_TRUE(isGreen(own_pixel)) << "borrower(" << own.x << "," << own.y << ") = r" << int(own_pixel.r) << " g"
                                    << int(own_pixel.g) << " b" << int(own_pixel.b) << " a" << int(own_pixel.a)
                                    << " | shared = r" << int(shared_pixel.r) << " g" << int(shared_pixel.g) << " b"
                                    << int(shared_pixel.b)
                                    << " | the borrower's second triangle must be visible: 'the depth is shared' and "
                                       "'the borrower draws nothing' are different pictures";
    // The depth is the other half of the evidence, and the half a colour picture cannot give: "the triangle
    // is hidden" and "the depth buffer is empty" look the same in pixels. These numbers say which pass wrote
    // what into the image both targets share.
    const vine::vsg::core::DepthProbe depth = lender->depthProbe();
    if (!depth.valid()) {
        GTEST_SKIP() << "the depth attachment of this device cannot be read back";
    }
    EXPECT_EQ(depth.width(), static_cast<int>(kSize));
    EXPECT_NEAR(depth.depthAt(shared.x, shared.y), kNearZ, 0.01F)
        << "the lender's depth is still there once the borrower's pass has finished: the borrower tested "
           "against THIS value, not against a clear of its own";
    EXPECT_NEAR(depth.depthAt(own.x, own.y), kFarZ, 0.01F)
        << "the borrower's visible triangle wrote its own depth into the shared image";
    EXPECT_NEAR(depth.depthAt(1, 1), 0.0F, 0.01F)
        << "and the clear (the reverse-Z far plane) survives where neither triangle is";
    EXPECT_EQ(depth.countNear(0.0F, 0.01F) + depth.countNear(kNearZ, 0.01F) + depth.countNear(kFarZ, 0.01F),
              static_cast<std::size_t>(kSize) * kSize)
        << "every texel is one of exactly three values: a fourth would mean something wrote depth that no "
           "draw of this frame asked for";

    // The borrower reads the SAME image back: its own copy-back node copies the lender's attachment, so the
    // numbers are the two passes' writes in one picture. A borrower that owned a depth of its own would answer
    // with its clear value where the lender's near triangle is.
    const vine::vsg::core::DepthProbe borrowed_depth = borrower->depthProbe();
    if (!borrowed_depth.valid()) {
        GTEST_SKIP() << "the shared depth cannot be read back through the borrower on this device";
    }
    EXPECT_EQ(borrowed_depth.width(), static_cast<int>(kSize));
    EXPECT_NEAR(borrowed_depth.depthAt(shared.x, shared.y), kNearZ, 0.01F)
        << "where the borrower's own far triangle was rejected, the value is the LENDER's near one: the "
           "borrower's readback is the shared image, not a depth of its own";
    EXPECT_NEAR(borrowed_depth.depthAt(own.x, own.y), kFarZ, 0.01F)
        << "and where the borrower's triangle was visible, its far value - one image, written by two passes";
    EXPECT_NEAR(borrowed_depth.depthAt(1, 1), 0.0F, 0.01F)
        << "the shared clear survives where neither drew";

    EXPECT_EQ(stack.recorder->draws(), 3U) << "three draws were recorded: one lender, two borrower";
    EXPECT_EQ(stack.recorder->refusals(), 0U);
}
