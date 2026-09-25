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
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <cstring>

#if !defined(_WIN32)
#    include <xcb/xcb.h>
#endif

#include "TestHostWindow.hpp"

#include <vsg/app/CommandGraph.h>
#include <vsg/app/Viewer.h>
#include <vsg/commands/Command.h>
#include <vsg/vk/Device.h>

#include <vine/vsg/VsgVulkanEntryPoints.hpp>
#include <vine/vsg/api/BlockDescriptors.hpp>
#include <vine/vsg/api/BlockStorage.hpp>
#include <vine/vsg/api/ContentDraw.hpp>
#include <vine/vsg/api/ContentFacts.hpp>
#include <vine/vsg/api/ContentPass.hpp>
#include <vine/vsg/api/ContentPipeline.hpp>
#include <vine/vsg/api/ContentSources.hpp>
#include <vine/vsg/api/DeviceProbe.hpp>
#include <vine/vsg/api/GeometryFacts.hpp>
#include <vine/vsg/api/OffscreenTarget.hpp>
#include <vine/vsg/api/Session.hpp>
#include <vine/vsg/api/SessionContent.hpp>
#include <vine/vsg/api/StreamUploads.hpp>
#include <vine/vsg/api/ViewBlock.hpp>
#include <vine/vsg/api/VsgExecutor.hpp>
#include <vine/vsg/api/WindowTarget.hpp>
#include <vine/vsg/core/Diagnostics.hpp>
#include <vine/vsg/core/FrameCompiler.hpp>
#include <vine/vsg/core/FrameRecorder.hpp>
#include <vine/vsg/core/StateRegistry.hpp>
#include <vine/vsg/core/Streams.hpp>
#include <vine/vsg/core/VariantPool.hpp>

#include <vine/Buffer.hpp>
#include <vine/graphics/Camera.hpp>
#include <vine/graphics/Geometry.hpp>
#include <vine/graphics/Material.hpp>
#include <vine/graphics/RenderCommand.hpp>
#include <vine/graphics/ShaderAbi.hpp>
#include <vine/graphics/ShaderProgram.hpp>

using vn::graphics::Geometry;
using vn::graphics::Material;
using vn::graphics::RenderCommand;
using vn::graphics::ShaderProgram;
using vn::graphics::ShaderStage;
using vn::graphics::ShaderStageType;
using vn::vsg::BlockDescriptors;
using vn::vsg::BlockStorage;
using vn::vsg::buildGeometryFacts;
using vn::vsg::buildMaterialFacts;
using vn::vsg::buildProgramFacts;
using vn::vsg::buildViewBlock;
using vn::vsg::ContentDraw;
using vn::vsg::ContentFacts;
using vn::vsg::ContentPass;
using vn::vsg::ContentPipeline;
using vn::vsg::FactMiss;
using vn::vsg::GeometryFacts;
using vn::vsg::MaterialFacts;
using vn::vsg::OffscreenTarget;
using vn::vsg::PassContent;
using vn::vsg::ProgramFacts;
using vn::vsg::StreamUploads;
using vn::vsg::ViewportRect;
using vn::vsg::VsgExecutor;
using vn::vsg::WindowTarget;
using vn::vsg::api::Session;
using vn::vsg::api::SessionOptions;
using vn::vsg::api::probePhysicalDevices;
using vn::vsg::core::ClearPolicy;
using vn::vsg::core::CompiledFrame;
using vn::vsg::core::FrameArena;
using vn::vsg::core::FrameCompiler;
using vn::vsg::core::FrameFacts;
using vn::vsg::core::FrameRecorder;
using vn::vsg::core::StateRegistry;
using vn::vsg::core::StreamKey;
using vn::vsg::core::StreamKind;
using vn::vsg::core::TargetFacts;
using vn::vsg::core::TargetShape;
using vn::vsg::core::VariantPool;

#if !defined(_WIN32)

namespace
{

constexpr int kWidth  = 128;
constexpr int kHeight = 96;

/// @brief The compatibility half of a pipeline's identity, from the shape a target really has.

/// @brief Whether a pixel is the fragment shader's pure green, whichever byte order the server uses.
bool isGreen(const std::array<std::uint8_t, 3>& pixel)
{
    return pixel[1] > 200U && pixel[0] < 60U && pixel[2] < 60U;
}

/// @brief Whether a byte is the 8-bit image of a linear colour value, in whichever colour space the window's
/// surface stores it (an sRGB swapchain encodes a linear write, and the display server hands the encoded
/// bytes back).
///
/// The tolerance is small ON PURPOSE: the two images of a value are far enough apart that the next value up
/// the shader encodes (0.5 linear is 128, the plan's 0.25 clear is 137 encoded) must not be able to answer
/// for it, or a pixel that never left the clear colour would pass a claim about the shading.
bool isColourByte(std::uint8_t byte, double linear)
{
    const double encoded = linear <= 0.0031308 ? 12.92 * linear : 1.055 * std::pow(linear, 1.0 / 2.4) - 0.055;
    const double value   = static_cast<double>(byte);
    return std::abs(value - 255.0 * linear) <= 6.0 || std::abs(value - 255.0 * encoded) <= 6.0;
}

/// @brief Whether a pixel is the plan's clear colour: linear (0, 0.25, 0), in either colour space.
bool isPlanClear(const std::array<std::uint8_t, 3>& pixel)
{
    return isColourByte(pixel[1], 0.25) && isColourByte(pixel[0], 0.0) && isColourByte(pixel[2], 0.0);
}

/// @brief The shader pair and the triangle the content draws (the same shape the off-screen phase uses).
ContentPipeline::Shaders shaders()
{
    ContentPipeline::Shaders pair;
    pair.vertex = "#version 450\n"
                  "layout(location = 0) in vec3 position;\n"
                  "layout(set = 0, binding = 1, std140) uniform VineDrawBlock { mat4 model; vec4 params; } draw;\n"
                  "void main() { gl_Position = draw.model * vec4(position, 1.0); }\n";
    pair.fragment = "#version 450\n"
                    "layout(location = 0) out vec4 outColor;\n"
                    "layout(set = 0, binding = 1, std140) uniform VineDrawBlock { mat4 model; vec4 params; } draw;\n"
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
    const TestXConnection connection_owner(connection);  // closed at scope end (see TestXConnection)
    if (connection == nullptr || xcb_connection_has_error(connection) != 0) {
        GTEST_SKIP() << "no X display to create a host window on";
    }
    xcb_screen_t* screen = xcb_setup_roots_iterator(xcb_get_setup(connection)).data;
    ASSERT_NE(screen, nullptr);

    TestHostWindow window(connection, screen, kWidth, kHeight);

    vn::vsg::core::Diagnostics diagnostics;
    diagnostics.setSink([](const vn::graphics::RenderDiagnostic& diagnostic) {
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
    const auto root   = vn::vsg::detail::SessionContentAccess::root(session);
    const auto device = vn::vsg::detail::SessionContentAccess::device(session);
    ASSERT_NE(root, nullptr) << "the session must expose the node it renders";
    ASSERT_NE(device, nullptr);

    // The content stack, built on the SESSION's device (objects from another device are unusable here).
    auto storage = BlockStorage::create(device, BlockStorage::Layout{});
    ASSERT_NE(storage, nullptr);
    const ContentPipeline::Shaders program_shaders = shaders();
    vn::vsg::ProgramAbi            program_abi;
    ASSERT_EQ(vn::vsg::scanProgramAbi(program_shaders.vertex, program_shaders.fragment, {}, program_abi),
              FactMiss::None);
    auto descriptors = BlockDescriptors::forAbi(program_abi, 0U, device, *storage);
    ASSERT_NE(descriptors, nullptr);

    const ContentPipeline::VertexBinding   binding{ 0U, sizeof(float) * 3U, false };
    const ContentPipeline::VertexAttribute attribute{ 0U, 0U, VK_FORMAT_R32G32B32_SFLOAT, 0U };
    auto pipelines = ContentPipeline::create(program_abi,
                                             std::span<const ContentPipeline::VertexBinding>(&binding, 1U),
                                             std::span<const ContentPipeline::VertexAttribute>(&attribute, 1U),
                                             program_shaders);
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
                           vn::vsg::detail::fetchDynamicStateEntryPoints(device->vk(), device->getInstance()->vk()));

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
    const ::vsg::ref_ptr<::vsg::BindDescriptorSet> block_binds[] = {
        descriptors->bind(pipelines->layout(),
                          BlockDescriptors::Offsets{ view.offset, block.offset, material.offset })
    };
    draw.blocks       = block_binds;
    draw.vertex_binds = std::span<const ::vsg::ref_ptr<::vsg::BindVertexBuffers>>(&vertex_bind.bind, 1U);
    draw.index        = index_bind.bind;
    draw.viewport     = ViewportRect{ 0.0F, 0.0F, static_cast<float>(kWidth), static_cast<float>(kHeight) };
    draw.index_count  = 3U;

    const auto content = recorder.record(registry, draw);
    ASSERT_NE(content, nullptr) << "the draw must record";

    root->addChild(content);
    ASSERT_TRUE(vn::vsg::detail::SessionContentAccess::recompile(session))
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
                                 << ", " << static_cast<int>(centre[1]) << ", " << static_cast<int>(centre[2])
                                 << ") read-error " << static_cast<int>(window.readError())
                                 << " (0 = the read was served and the display path had not delivered the "
                                    "picture yet; non-zero = the server refused it, see HostWindowReadTest)";
    EXPECT_FALSE(isGreen(corner)) << "a window that is green everywhere would pass a sloppier check";

    session.shutdown();
}

TEST(SessionContentTest, APlanDrivenFrameReachesTheWindowAndTheViewBlockItsShadersRead)
{
    // The other half of the session's job, and the one the whole collect -> compile -> execute split exists
    // for: a caller drives the RECORDING (passes, clear policy, camera), the plan is compiled, the content
    // layer records what the plan names, and the executor turns the plan into the window's graph. The frame's
    // books stay the session's: exactly one present, and the timeline moves once.
    //
    // The shader pair is chosen so that the view block itself is observable: the vertex stage multiplies its
    // positions by `view_proj` (a transposed or misplaced matrix moves the triangle), and the fragment stage
    // encodes two of the block's other fields where the pixels can be read - the picture's extent (frame.y/z,
    // against the extent the test knows) and the camera's world position (cam_pos.x). The camera looks at
    // (0.5, 0, 0) from (0.5, 0, 1.5), so the view pushes the triangle into the LEFT quarter of the window:
    // the right quarter can only be the plan's clear.
    if (std::getenv("DISPLAY") == nullptr) {
        GTEST_SKIP() << "no window system";
    }
    const auto probed_devices = probePhysicalDevices();
    if (!probed_devices.ok || probed_devices.usableCount() == 0) {
        GTEST_SKIP() << "no device satisfies the requirements";
    }

    int               screen_index = 0;
    xcb_connection_t* connection   = xcb_connect(nullptr, &screen_index);
    const TestXConnection connection_owner(connection);  // closed at scope end (see TestXConnection)
    if (connection == nullptr || xcb_connection_has_error(connection) != 0) {
        GTEST_SKIP() << "no X display to create a host window on";
    }
    xcb_screen_t* screen = xcb_setup_roots_iterator(xcb_get_setup(connection)).data;
    ASSERT_NE(screen, nullptr);

    TestHostWindow window(connection, screen, kWidth, kHeight);

    vn::vsg::core::Diagnostics diagnostics;
    diagnostics.setSink([](const vn::graphics::RenderDiagnostic& diagnostic) {
        std::printf("[session-plan] diagnostic: severity=%d category=%d message=%s\n",
                    static_cast<int>(diagnostic.severity), static_cast<int>(diagnostic.category),
                    std::string(reinterpret_cast<const char*>(diagnostic.message.data()),
                                diagnostic.message.size()).c_str());
    });
    Session        session;
    SessionOptions options;
    options.width         = kWidth;
    options.height        = kHeight;
    options.native_handle = window.handle();
    options.validation    = true;
    ASSERT_TRUE(session.initialize(options, diagnostics));

    WindowTarget* window_target = vn::vsg::detail::SessionContentAccess::windowTarget(session);
    ASSERT_NE(window_target, nullptr) << "the session wraps its window as the frame's default framebuffer";
    const auto device = vn::vsg::detail::SessionContentAccess::device(session);
    ASSERT_NE(device, nullptr);

    // The content stack, built on the SESSION's device.
    auto storage = BlockStorage::create(device, BlockStorage::Layout{});
    ASSERT_NE(storage, nullptr);

    const vn::intrusive_ptr<ShaderProgram> program(new ShaderProgram());
    {
        ShaderStage vertex;
        vertex.type   = ShaderStageType::Vertex;
        vertex.source = vn::String(reinterpret_cast<const char8_t*>(
            "layout(location = 0) in vec3 position;\n"
            "layout(set = 0, binding = 0, std140) uniform VineViewBlock {\n"
            "    mat4 view; mat4 inv_view; mat4 proj; mat4 view_proj; vec4 cam_pos; vec4 frame; } vb;\n"
            "void main() { gl_Position = vb.view_proj * vec4(position, 1.0); }\n"));
        ShaderStage fragment;
        fragment.type   = ShaderStageType::Fragment;
        fragment.source = vn::String(reinterpret_cast<const char8_t*>(
            "layout(location = 0) out vec4 outColor;\n"
            "layout(set = 0, binding = 0, std140) uniform VineViewBlock {\n"
            "    mat4 view; mat4 inv_view; mat4 proj; mat4 view_proj; vec4 cam_pos; vec4 frame; } vb;\n"
            "void main() { outColor = vec4(vb.frame.y / 128.0, vb.cam_pos.x, vb.frame.z / 96.0, 1.0); }\n"));
        program->addStage(vertex);
        program->addStage(fragment);
    }

    ProgramFacts program_facts;
    ASSERT_EQ(buildProgramFacts(*program, program_facts), FactMiss::None);

    // The block set the program's OWN declarations need (the view block at set 0 / binding 0).
    auto descriptors = BlockDescriptors::forAbi(program_facts.abi, 0U, device, *storage);
    ASSERT_NE(descriptors, nullptr);
    BlockDescriptors* content_blocks[] = { descriptors.get() };

    const ContentPipeline::VertexBinding   binding{ 0U, sizeof(float) * 3U, false };
    const ContentPipeline::VertexAttribute attribute{ 0U, 0U, VK_FORMAT_R32G32B32_SFLOAT, 0U };
    auto pipelines = ContentPipeline::create(program_facts.abi,
                                             std::span<const ContentPipeline::VertexBinding>(&binding, 1U),
                                             std::span<const ContentPipeline::VertexAttribute>(&attribute, 1U),
                                             program_facts.shaders);
    ASSERT_NE(pipelines, nullptr) << "the view-block shader pair must compile";

    const vn::intrusive_ptr<Geometry> geometry(new Geometry());
    const vn::intrusive_ptr<vn::Buffer<float>> positions_buffer = vn::intrusive_ptr<vn::Buffer<float>>(
        new vn::Buffer<float>(std::vector<float>{ -0.4F, -0.4F, 0.5F, 0.4F, -0.4F, 0.5F, 0.0F, 0.6F, 0.5F }));
    const vn::intrusive_ptr<vn::Buffer<std::uint32_t>> indices_buffer =
        vn::intrusive_ptr<vn::Buffer<std::uint32_t>>(
            new vn::Buffer<std::uint32_t>(std::vector<std::uint32_t>{ 0U, 1U, 2U }));
    geometry->setPositions(positions_buffer);
    geometry->setIndices(indices_buffer);
    geometry->setRevision(1U);

    const vn::intrusive_ptr<Material> material(new Material());
    material->setDiffuse(vn::Colorf(0.2F, 0.3F, 0.4F, 1.0F));

    GeometryFacts                        geometry_facts;
    std::vector<vn::vsg::ChannelFacts> channel_storage;
    ASSERT_EQ(buildGeometryFacts(*geometry, geometry_facts, channel_storage), FactMiss::None);
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

    VariantPool   pool;
    StateRegistry registry(pool);
    StreamUploads uploads;
    ContentDraw   draws(*pipelines, pool,
                        vn::vsg::detail::fetchDynamicStateEntryPoints(device->vk(), device->getInstance()->vk()));

    // The camera: looking at (0.5, 0, 0) from (0.5, 0, 1.5), with an orthographic window of [-1, 1] squared.
    // The look-at puts the triangle 0.5 to the LEFT of the window's centre; the fragment stage reports the
    // camera's x (0.5) in a colour byte.
    const vn::intrusive_ptr<vn::graphics::Camera> camera(new vn::graphics::Camera());
    camera->setViewMatrixAsLookAt(vn::math::Vec3d(0.5, 0.0, 1.5), vn::math::Vec3d(0.5, 0.0, 0.0),
                                  vn::math::Vec3d(0.0, 1.0, 0.0));
    camera->setProjectionMatrixAsOrtho(-1.0, 1.0, -1.0, 1.0, 0.5, 4.0);

    RenderCommand command;
    command.geometry = geometry;
    command.material = material;
    command.program  = program;
    const std::vector<RenderCommand> commands{ command };

    ClearPolicy clear;
    clear.color          = true;
    clear.color_value[0] = 0.0F;
    clear.color_value[1] = 0.25F;  // the plan's clear: a green the shader's colour cannot be
    clear.color_value[2] = 0.0F;
    clear.color_value[3] = 1.0F;

    // The plan: one pass into the default framebuffer, whose frame token is the SESSION's.
    FrameArena             arena{ 64 * 1024 };
    vn::vsg::core::Observe observe;
    FrameRecorder          recorder{ arena, diagnostics, observe };
    FrameCompiler          compiler{ arena, diagnostics, observe };

    const vn::vsg::core::FrameToken token = session.beginFrame();
    ASSERT_TRUE(token) << "the session opens the frame the plan belongs to";
    recorder.beginFrame(token);
    recorder.beginPass(7U);
    recorder.setRenderTarget(nullptr);  // the default framebuffer
    recorder.setClearPolicy(clear);
    recorder.render(commands, camera.get());
    recorder.endPass();
    recorder.endFrame();

    const TargetFacts window_facts = window_target->facts();
    const CompiledFrame& frame =
        compiler.compile(recorder.description(), FrameFacts{ std::span<const TargetFacts>(&window_facts, 1U) });
    ASSERT_EQ(frame.passes.size(), 1U);
    ASSERT_EQ(frame.passes[0].color_attachments, 1U) << "the plan resolved the window's shape from its facts";
    ASSERT_EQ(frame.passes[0].draws.size(), 1U);
    ASSERT_TRUE(frame.passes[0].draws[0].camera.present) << "the camera the host announced is in the plan";

    // The content layer, with the view block built for THIS pass from the plan's camera and the session's
    // frame clock - the extent is the window's, which is the picture the shading reconstructs from.
    storage->beginFrame();
    const ContentPass::Scope::Entry halves[]{ ContentPass::Scope::Entry{
        vn::vsg::core::DrawKind::Content, program.get(), program_facts.revision, geometry_facts.layout,
        pipelines.get(), &draws } };
    ContentPass::Scope scope;
    scope.entries    = halves;
    scope.registry   = &registry;
    scope.storage    = storage.get();
    scope.block_sets = content_blocks;
    scope.uploads    = &uploads;
    ContentPass content(scope, diagnostics);

    const std::uint32_t target_width  = window_target->width();
    const std::uint32_t target_height = window_target->height();
    const vn::graphics::VineViewBlock view_block =
        buildViewBlock(frame.passes[0].draws[0].camera, session.frameSeconds(), target_width, target_height);
    const std::span<const std::byte> view_bytes =
        std::as_bytes(std::span<const vn::graphics::VineViewBlock>(&view_block, 1U));

    ::vsg::ref_ptr<::vsg::Node> content_node;
    ASSERT_TRUE(content.record(frame.passes[0], facts, window_target->shape().compatibility(), {}, view_bytes,
                               content_node));
    EXPECT_TRUE(diagnostics.clean()) << "every identity is in the tables and the inputs match the plan";

    // The executor: the window is its default-framebuffer target, and the plan's single pass is recorded into
    // the graph the session hands out for this frame.
    VsgExecutor executor(diagnostics);
    executor.setWindow(window_target);
    const auto command_graph = vn::vsg::detail::SessionContentAccess::makeFrameGraph(session);
    ASSERT_NE(command_graph, nullptr);

    const PassContent packet{ frame.passes[0].pass, content_node };
    ASSERT_TRUE(executor.record(frame, command_graph, std::span<const PassContent>(&packet, 1U)));
    EXPECT_EQ(executor.skipped(), 0U) << "the executor serves the default framebuffer through the window target";
    ASSERT_EQ(executor.recorded().size(), 1U);
    EXPECT_EQ(executor.recorded()[0], 7U);

    ASSERT_TRUE(vn::vsg::detail::SessionContentAccess::assignFrameGraphs(session, ::vsg::CommandGraphs{ command_graph }));
    ASSERT_TRUE(session.commitFrame());

    // The frame's books: ONE present for the frame, the timeline moved once, no device wait anywhere.
    EXPECT_EQ(session.framesPresented(), 1U) << "the plan's frame IS the session's frame: it presents once";
    EXPECT_EQ(session.timeline().submittedFrame(), 1U);
    EXPECT_EQ(session.deviceWaits(), 0U);

    // Two more presents of the SAME graph: the graph the plan built stays where it is, and the picture it
    // drew is presented again. The reason is the read-back, not the rendering: a presentation is copied into
    // the window by the display server asynchronously, so the image a test reads straight after the FIRST
    // present can still be the window's untouched backing store - while a PLAN-FREE frame would repaint the
    // window with the session's own graph (measured: the read then answers the window's own background with
    // `read-error 0`; see TestHostWindow::waitForPixel). Moving `assignFrameGraphs` inside the loop is what
    // keeps the picture: the assignment is consumed by the commit that follows it.
    for (int settle = 0; settle < 2; ++settle)
    {
        ASSERT_TRUE(session.beginFrame());
        ASSERT_TRUE(vn::vsg::detail::SessionContentAccess::assignFrameGraphs(
            session, ::vsg::CommandGraphs{ command_graph }));
        ASSERT_TRUE(session.commitFrame());
    }

    // The picture, from the window itself. Byte order is the display server's and the surface's colour space
    // is the platform's, so every colour claim is written against the value it means (see isColourByte) and
    // the two outer bytes are treated as a set rather than as named channels.
    //
    // WHERE THE TRIANGLE IS, and why these pixels: the camera looks at (0.5, 0, 0) from 1.5 units out with an
    // orthographic window of [-1, 1] squared, and the block carries the matrices in the DEVICE's clip
    // convention (reverse-Z, y-down). World x in [-0.4, 0.4] therefore lands in the left half, the apex
    // (world y = 0.6) at the TOP: at the apex's column (x = 32) the triangle covers rows ~19 to ~67, so row 22
    // is inside it and row 72 is not - while an unfolded (or y-mirrored) block would cover ~29 to ~77 there,
    // which turns both probes around. A picture whose clip space did not match the window's reverse-Z depth
    // (cleared to 0.0 under GREATER) would show the plan's clear everywhere instead.
    const auto drawn       = window.pixel(kWidth / 4, kHeight / 2);      // inside the triangle
    const auto apex_side   = window.pixel(kWidth / 4, 22);               // inside, above the middle
    const auto below_base  = window.pixel(kWidth / 4, 72);               // outside, below the base
    const auto clear_pixel = window.pixel(3 * kWidth / 4, kHeight / 2);  // the triangle never reaches here
    const auto corner      = window.pixel(2, 2);

    EXPECT_TRUE(isColourByte(drawn[1], 0.5))
        << "cam_pos.x (0.5) reached the fragment stage, got (" << static_cast<int>(drawn[0]) << ", "
        << static_cast<int>(drawn[1]) << ", " << static_cast<int>(drawn[2]) << ")";
    EXPECT_TRUE(isColourByte(drawn[0], 1.0)) << "the picture's width (frame.y = 128, the window's own) reached "
                                                "the fragment stage";
    EXPECT_TRUE(isColourByte(drawn[2], 1.0)) << "and so did its height (frame.z = 96)";

    EXPECT_TRUE(isColourByte(apex_side[1], 0.5)) << "the triangle's apex points UP in the window: a pixel "
                                                    "above the middle, inside the triangle, is shaded";
    EXPECT_TRUE(isPlanClear(below_base)) << "and one below its base is not: got ("
                                         << static_cast<int>(below_base[0]) << ", "
                                         << static_cast<int>(below_base[1]) << ", "
                                         << static_cast<int>(below_base[2]) << ")";

    EXPECT_TRUE(isPlanClear(clear_pixel))
        << "the right quarter is the plan's clear, so the view really moved the triangle: got ("
        << static_cast<int>(clear_pixel[0]) << ", " << static_cast<int>(clear_pixel[1]) << ", "
        << static_cast<int>(clear_pixel[2]) << ") read-error " << static_cast<int>(window.readError());
    EXPECT_TRUE(isPlanClear(corner)) << "the swapchain image was cleared to the plan's colour";

    session.shutdown();
}

#endif  // !defined(_WIN32)
