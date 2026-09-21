/**
 * @brief Content reaching the frame THROUGH a session, proven by the window's own pixels.
 *
 * The off-screen phase proves the stack's picture; this one proves the OTHER half of the contract: that a
 * session renders what a caller attaches to it, in its frame, on its window - and that the pixels really
 * leave the swapchain. It is the path a real backend uses (the host gives a window, the session owns the
 * device, callers draw into the content root), so the evidence is taken from the window itself: after a few
 * committed frames, one pixel in the middle of the window is read back through X11.
 *
 * The two facts the case gates on are different in kind, and both are needed:
 *
 *   * the picture: the triangle's colour is in the window, and the window is not uniformly that colour (a
 *     frame that painted everything green would pass a sloppier check);
 *   * the frame's health: frames were committed, none of them idled the device, and the content root is the
 *     session's own node (the structure, not just the pixels).
 *
 * X11 only, and it SKIPS rather than fails without a display, a device or an X connection: what it checks is
 * this backend's behaviour, not the machine's configuration.
 */

#include <gtest/gtest.h>

#include <array>
#include <cstdint>
#include <cstdlib>
#include <cstring>

#if !defined(_WIN32)
#    include <xcb/xcb.h>
#endif

#include <vsg/app/CommandGraph.h>
#include <vsg/app/Viewer.h>
#include <vsg/commands/Command.h>
#include <vsg/vk/Device.h>

#include <vine/vsg/VsgVulkanEntryPoints.hpp>
#include <vine/vsg/api/BlockDescriptors.hpp>
#include <vine/vsg/api/BlockStorage.hpp>
#include <vine/vsg/api/ContentDraw.hpp>
#include <vine/vsg/api/ContentPipeline.hpp>
#include <vine/vsg/api/DeviceProbe.hpp>
#include <vine/vsg/api/OffscreenTarget.hpp>
#include <vine/vsg/api/Session.hpp>
#include <vine/vsg/api/SessionContent.hpp>
#include <vine/vsg/api/StreamUploads.hpp>
#include <vine/vsg/core/Diagnostics.hpp>
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
using vine::vsg::api::Session;
using vine::vsg::api::SessionOptions;
using vine::vsg::api::probePhysicalDevices;
using vine::vsg::core::StateRegistry;
using vine::vsg::core::StreamKey;
using vine::vsg::core::StreamKind;
using vine::vsg::core::VariantPool;

#if !defined(_WIN32)

namespace
{

constexpr int kWidth  = 128;
constexpr int kHeight = 96;

/// @brief A window the TEST owns, so the session can adopt it as the host's and the test can read it back.
class HostWindow
{
  public:
    HostWindow(xcb_connection_t* connection, xcb_screen_t* screen, int width, int height)
      : connection_(connection)
    {
        window_                      = xcb_generate_id(connection_);
        const std::uint32_t mask     = XCB_CW_BACK_PIXEL | XCB_CW_EVENT_MASK;
        const std::uint32_t values[] = { screen->black_pixel,
                                         XCB_EVENT_MASK_STRUCTURE_NOTIFY | XCB_EVENT_MASK_EXPOSURE };
        xcb_create_window(connection_, XCB_COPY_FROM_PARENT, window_, screen->root, 0, 0,
                          static_cast<std::uint16_t>(width), static_cast<std::uint16_t>(height), 0,
                          XCB_WINDOW_CLASS_INPUT_OUTPUT, screen->root_visual, mask, values);
        xcb_map_window(connection_, window_);
        xcb_flush(connection_);
    }

    HostWindow(const HostWindow&)            = delete;
    HostWindow& operator=(const HostWindow&) = delete;

    ~HostWindow()
    {
        xcb_destroy_window(connection_, window_);
        xcb_flush(connection_);
    }

    /** @brief The handle in the form the backend passes it around. */
    [[nodiscard]] void* handle() const noexcept
    {
        return reinterpret_cast<void*>(static_cast<std::uintptr_t>(window_));
    }

    /** @brief Reads one pixel back out of the window (XGetImage's server-side equivalent).
     *
     * The bytes of a TrueColor pixel are returned in the server's order; the case's assertions are written so
     * they do not depend on it (a pure green pixel has the same middle byte in both orders).
     *
     * @param x Column to read.
     * @param y Row to read.
     * @return The first three bytes of the pixel.
     */
    [[nodiscard]] std::array<std::uint8_t, 3> pixel(int x, int y) const
    {
        const xcb_get_image_cookie_t cookie =
            xcb_get_image(connection_, XCB_IMAGE_FORMAT_Z_PIXMAP, window_, static_cast<std::int16_t>(x),
                          static_cast<std::int16_t>(y), 1U, 1U, ~0U);
        xcb_get_image_reply_t* reply = xcb_get_image_reply(connection_, cookie, nullptr);
        if (reply == nullptr) {
            return { 0U, 0U, 0U };
        }
        const std::uint8_t* data = xcb_get_image_data(reply);
        const std::array<std::uint8_t, 3> rgb{ data[0], data[1], data[2] };
        std::free(reply);
        return rgb;
    }

  private:
    xcb_connection_t* connection_;
    xcb_window_t      window_{ 0 };
};

/// @brief Whether a pixel is the fragment shader's pure green, whichever byte order the server uses.
bool isGreen(const std::array<std::uint8_t, 3>& pixel)
{
    return pixel[1] > 200U && pixel[0] < 60U && pixel[2] < 60U;
}

/// @brief The shader pair and the triangle the content draws (the same shape the off-screen phase uses).
ContentPipeline::Shaders shaders()
{
    ContentPipeline::Shaders pair;
    pair.vertex = "#version 450\n"
                  "layout(location = 0) in vec3 position;\n"
                  "layout(set = 0, binding = 1) uniform DrawBlock { mat4 model; vec4 params; } draw;\n"
                  "void main() { gl_Position = draw.model * vec4(position, 1.0); }\n";
    pair.fragment = "#version 450\n"
                    "layout(location = 0) out vec4 outColor;\n"
                    "layout(set = 0, binding = 1) uniform DrawBlock { mat4 model; vec4 params; } draw;\n"
                    "void main() { outColor = vec4(0.0, 1.0, 0.0, draw.params.x); }\n";
    return pair;
}

int   program_identity = 0;
int   model_vertices   = 0;
int   model_indices    = 0;
int   material_identity = 0;

/// @brief A draw block whose matrix is the identity (the triangle stays where its positions put it).
std::vector<std::byte> drawBlock()
{
    const std::array<float, 16> identity{ 1.0F, 0.0F, 0.0F, 0.0F, 0.0F, 1.0F, 0.0F, 0.0F,
                                          0.0F, 0.0F, 1.0F, 0.0F, 0.0F, 0.0F, 0.0F, 1.0F };
    std::vector<std::byte>      block(80U, std::byte{ 0 });
    std::memcpy(block.data(), identity.data(), sizeof(identity));
    const std::array<float, 4> params{ 1.0F, 0.0F, 0.0F, 0.0F };
    std::memcpy(block.data() + sizeof(identity), params.data(), sizeof(params));
    return block;
}

}  // namespace

TEST(SessionContentTest, ContentAttachedToTheSessionReachesTheWindowsPixels)
{
    if (std::getenv("DISPLAY") == nullptr) {
        GTEST_SKIP() << "no window system";
    }
    const auto probed_devices = probePhysicalDevices();
    if (!probed_devices.ok || probed_devices.usableCount() == 0) {
        GTEST_SKIP() << "no device satisfies the requirements";
    }

    int               screen_index = 0;
    xcb_connection_t* connection   = xcb_connect(nullptr, &screen_index);
    if (connection == nullptr || xcb_connection_has_error(connection) != 0) {
        GTEST_SKIP() << "no X display to create a host window on";
    }
    xcb_screen_t* screen = xcb_setup_roots_iterator(xcb_get_setup(connection)).data;
    ASSERT_NE(screen, nullptr);

    HostWindow window(connection, screen, kWidth, kHeight);

    vine::vsg::core::Diagnostics diagnostics;
    diagnostics.setSink([](const vine::graphics::RenderDiagnostic& diagnostic) {
        std::printf("[session-content] diagnostic: severity=%d category=%d message=%s\n",
                    static_cast<int>(diagnostic.severity), static_cast<int>(diagnostic.category),
                    std::string(reinterpret_cast<const char*>(diagnostic.message.data()),
                                diagnostic.message.size()).c_str());
    });
    Session                       session;
    SessionOptions                options;
    options.width         = kWidth;
    options.height        = kHeight;
    options.native_handle = window.handle();
    // The layer is asked for here too: this is the phase that compiles content pipelines through the SESSION's
    // device (created by a window, not by api::Device), and a dynamic state the session's device was never
    // given the extension for is exactly what the layer names. Without it, this phase would pass on a
    // device that cannot deliver the pipelines it builds.
    options.validation    = true;
    ASSERT_TRUE(session.initialize(options, diagnostics)) << "the session must adopt the host window";

    // The three facts a drawing layer needs from the session: its content root, its device, a recompile.
    const auto root   = vine::vsg::detail::SessionContentAccess::root(session);
    const auto device = vine::vsg::detail::SessionContentAccess::device(session);
    ASSERT_NE(root, nullptr) << "the session must expose the node it renders";
    ASSERT_NE(device, nullptr);

    // The content stack, built on the SESSION's device (objects from another device are unusable here).
    auto storage = BlockStorage::create(device, BlockStorage::Layout{});
    ASSERT_NE(storage, nullptr);
    auto descriptors = BlockDescriptors::create(device, *storage);
    ASSERT_NE(descriptors, nullptr);

    const ContentPipeline::VertexBinding   binding{ 0U, sizeof(float) * 3U, false };
    const ContentPipeline::VertexAttribute attribute{ 0U, 0U, VK_FORMAT_R32G32B32_SFLOAT, 0U };
    auto pipelines = ContentPipeline::create(descriptors->layout(),
                                             std::span<const ContentPipeline::VertexBinding>(&binding, 1U),
                                             std::span<const ContentPipeline::VertexAttribute>(&attribute, 1U),
                                             shaders());
    ASSERT_NE(pipelines, nullptr) << "the shader pair must compile";

    auto positions  = ::vsg::vec3Array::create(3U);
    // z = 0.5, and the value is not arbitrary: the window's pass HAS a depth attachment, cleared to 0.0 for
    // reverse-Z, and the depth comparison is GREATER - so a triangle lying exactly at the cleared depth is
    // rejected by the depth test and nothing appears. (The off-screen target has no depth attachment, which
    // is why the same shape draws there at any z.) This is the kind of difference only a pixel can report.
    (*positions)[0] = ::vsg::vec3(-0.4F, -0.4F, 0.5F);
    (*positions)[1] = ::vsg::vec3(0.4F, -0.4F, 0.5F);
    (*positions)[2] = ::vsg::vec3(0.0F, 0.4F, 0.5F);
    auto indices  = ::vsg::uintArray::create(3U);
    (*indices)[0] = 0U;
    (*indices)[1] = 1U;
    (*indices)[2] = 2U;

    StreamUploads uploads;
    StreamKey     vertex_key;
    vertex_key.kind       = StreamKind::Vertex;
    vertex_key.location   = 0U;
    vertex_key.components = 3U;
    vertex_key.buffer     = &model_vertices;
    vertex_key.revision   = 1U;
    vertex_key.count      = 9U;
    const auto vertex_bind = uploads.acquireVertex(vertex_key, positions);
    StreamKey  index_key;
    index_key.kind       = StreamKind::Index;
    index_key.components = 1U;
    index_key.buffer     = &model_indices;
    index_key.revision   = 1U;
    index_key.count      = 3U;
    const auto index_bind = uploads.acquireIndex(index_key, indices);
    ASSERT_NE(vertex_bind.bind, nullptr);
    ASSERT_NE(index_bind.bind, nullptr);

    VariantPool   pool;
    StateRegistry registry(pool);
    ContentDraw   recorder(*pipelines, pool,
                           vine::vsg::detail::fetchDynamicStateEntryPoints(device->vk(), device->getInstance()->vk()));

    storage->beginFrame();
    const auto view     = storage->writeView(std::vector<std::byte>(288U, std::byte{ 0 }));
    const auto block    = storage->writeDraw(drawBlock());
    const auto material = storage->writeMaterial(&material_identity, 1U, std::vector<std::byte>(64U, std::byte{ 0 }));
    ASSERT_TRUE(view.valid);
    ASSERT_TRUE(block.valid);

    ContentDraw::Draw draw;
    draw.key.program                      = &program_identity;
    draw.key.revision                     = 1U;
    draw.key.vertex_layout.canonical_mask = 0x1U;
    draw.key.compatibility.samples        = 1U;
    draw.blocks = descriptors->bind(pipelines->layout(), BlockDescriptors::Offsets{ view.offset, block.offset,
                                                                                    material.offset });
    draw.vertex_binds = std::span<const ::vsg::ref_ptr<::vsg::BindVertexBuffers>>(&vertex_bind.bind, 1U);
    draw.index        = index_bind.bind;
    draw.viewport     = ViewportRect{ 0.0F, 0.0F, static_cast<float>(kWidth), static_cast<float>(kHeight) };
    draw.index_count  = 3U;

    const auto content = recorder.record(registry, draw);
    ASSERT_NE(content, nullptr) << "the draw must record";

    root->addChild(content);
    ASSERT_TRUE(vine::vsg::detail::SessionContentAccess::recompile(session))
        << "content attached after the session came up has to be compiled";

    for (int frame = 0; frame < 3; ++frame) {
        ASSERT_TRUE(session.beginFrame());
        ASSERT_TRUE(session.commitFrame());
    }

    EXPECT_GE(session.framesPresented(), 3U);
    EXPECT_EQ(session.deviceWaits(), 0U) << "a frame that draws content must not idle the device";

    // The picture, from the window itself: the triangle's colour in the middle, and a window that is not
    // uniformly that colour.
    const auto centre = window.pixel(kWidth / 2, kHeight / 2 + kHeight / 8);
    const auto corner = window.pixel(2, 2);
    // The recording itself, so a missing picture can be told apart from a missing draw.
    EXPECT_EQ(recorder.draws(), 1U);
    EXPECT_EQ(recorder.pipeline_binds(), 1U);
    EXPECT_EQ(recorder.dynamic_commands(), 1U);

    EXPECT_TRUE(isGreen(centre)) << "the window's centre must hold the content, got (" << static_cast<int>(centre[0])
                                 << ", " << static_cast<int>(centre[1]) << ", " << static_cast<int>(centre[2]) << ")";
    EXPECT_FALSE(isGreen(corner)) << "a window that is green everywhere would pass a sloppier check";

    session.shutdown();
}

#endif  // !defined(_WIN32)
