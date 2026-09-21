/**
 * @brief The window as a composition surface: a scene pass and a full-screen overlay in ONE frame, on ONE
 * clear, next to an off-screen pass whose ENGINE shape equals the window's.
 *
 * Three claims, and each of them is a picture rather than a counter - the counters the fixture does check
 * (`pipelines()`, the bind counts) exist only to tell "the right thing was recorded" from "nothing was", which
 * a black window would also explain:
 *
 *   * a full-screen OVERLAY draws into the window (the engine's own screen-copy program, sampling the
 *     picture an earlier pass produced) while the scene stays visible outside its rectangle. That is the
 *     full-screen ABI inside vsg's OWN render pass - a different render-pass family from every off-screen
 *     target - which is the claim M4a/M4b could only make off-screen;
 *   * the window is cleared ONCE, by its FIRST pass. The overlay pass announces a different clear colour on
 *     purpose: with one clear the window keeps the first pass' colour everywhere the overlay does not cover,
 *     and a per-pass clear would wipe the scene;
 *   * an off-screen pass whose shape the ENGINE cannot tell from the window's pass gets a variant of its own
 *     (`pipelines() == 2`), because the two render passes are not compatible: the window's surface is an sRGB
 *     format while the off-screen target is a linear one, and only the DEVICE formats can say so. This is the
 *     claim the M3d-3c record registered as missing, and what M4c found instead is that the obvious fix - one
 *     view per family - does NOT hold: the first version of this fixture shared ONE variant between the two
 *     passes and the validation layer reported `VUID-vkCmdDrawIndexed-renderPass-02684` (the off-screen
 *     compiled VkPipeline bound in the window's render pass). The picture looked right throughout - that is
 *     what undefined behaviour may look like - so this claim's evidence is the layer, and the pixels below
 *     are the evidence for the other two.
 *
 * B carries the depth format the window's traits ask for, so the two passes really are the same shape to the
 * engine (colour format, depth format, samples): without that, the third claim would be about nothing - two
 * obviously different targets whose keys differ anyway.
 */

#include <gtest/gtest.h>

#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

#if !defined(_WIN32)
#    include <xcb/xcb.h>
#endif

#include <vsg/app/CommandGraph.h>
#include <vsg/app/Viewer.h>

#include <vine/Buffer.hpp>
#include <vine/graphics/BuiltinShaders.hpp>
#include <vine/graphics/Camera.hpp>
#include <vine/graphics/Geometry.hpp>
#include <vine/graphics/Material.hpp>
#include <vine/graphics/RenderCommand.hpp>
#include <vine/graphics/RenderTarget.hpp>
#include <vine/graphics/ShaderProgram.hpp>

#include "TestHostWindow.hpp"

#include <vine/math/Vector3.hpp>

#include <vsg/state/ImageView.h>

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
#include <vine/vsg/core/Observe.hpp>
#include <vine/vsg/core/FrameCompiler.hpp>
#include <vine/vsg/core/FrameRecorder.hpp>
#include <vine/vsg/core/StateRegistry.hpp>
#include <vine/vsg/core/Streams.hpp>
#include <vine/vsg/core/VariantPool.hpp>

using vine::graphics::Geometry;
using vine::graphics::Material;
using vine::graphics::RenderCommand;
using vine::graphics::RenderTarget;
using vine::graphics::ShaderProgram;
using vine::graphics::ShaderStage;
using vine::graphics::ShaderStageType;
using vine::vsg::api::probePhysicalDevices;
using vine::vsg::api::Session;
using vine::vsg::api::SessionOptions;
using vine::vsg::BlockDescriptors;
using vine::vsg::BlockStorage;
using vine::vsg::buildGeometryFacts;
using vine::vsg::buildMaterialFacts;
using vine::vsg::buildProgramFacts;
using vine::vsg::buildScreenProgramFacts;
using vine::vsg::buildViewBlock;
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
using vine::vsg::WindowTarget;
using vine::vsg::core::ClearPolicy;
using vine::vsg::core::CompiledFrame;
using vine::vsg::core::Diagnostics;
using vine::vsg::core::DrawKind;
using vine::vsg::core::FrameArena;
using vine::vsg::core::FrameCompiler;
using vine::vsg::core::FrameFacts;
using vine::vsg::core::FrameRecorder;
using vine::vsg::core::StateRegistry;
using vine::vsg::core::TargetFacts;
using vine::vsg::core::TargetShape;
using vine::vsg::core::VariantPool;

namespace
{

constexpr int           kWidth  = 128;
constexpr int           kHeight = 96;
constexpr std::uint32_t kSourceSize = 64U;  // the off-screen targets' extent

/// @brief The scene's clear (linear (0, 0, 0.25)): the colour the FIRST window pass owns.
constexpr float kSceneClear[4]{ 0.0F, 0.0F, 0.25F, 1.0F };

/// @brief The overlay pass' own clear (linear (0, 0.25, 0)): it must NOT reach the window.
constexpr float kOverlayClear[4]{ 0.0F, 0.25F, 0.0F, 1.0F };

/// @brief The off-screen picture the overlay samples (linear (0.25, 0, 0)).
constexpr float kPictureClear[4]{ 0.25F, 0.0F, 0.0F, 1.0F };

/// @brief The picture-in-picture rectangle: to the RIGHT of the triangle the scene draws into the left quarter.
constexpr vine::graphics::Viewport kPictureInPicture{ 64, 16, 48, 48 };

/// @brief Whether a byte is the 8-bit image of a linear colour value, in whichever colour space the surface
/// stores it (an sRGB swapchain encodes a linear write).
bool isColourByte(std::uint8_t byte, double linear)
{
    const double encoded = linear <= 0.0031308 ? 12.92 * linear : 1.055 * std::pow(linear, 1.0 / 2.4) - 0.055;
    const double value   = static_cast<double>(byte);
    return std::abs(value - 255.0 * linear) <= 6.0 || std::abs(value - 255.0 * encoded) <= 6.0;
}

/// @brief Whether a pixel is @p red / @p green / @p blue (see isColourByte; the READER puts the bytes in
/// this order - the window hands them back in its surface's order).
bool isColour(const std::array<std::uint8_t, 3>& pixel, double red, double green, double blue)
{
    return isColourByte(pixel[0], red) && isColourByte(pixel[1], green) && isColourByte(pixel[2], blue);
}

}  // namespace

#if !defined(_WIN32)

TEST(WindowCompositionTest, TheWindowIsClearedOnceAndCarriesASceneAndAFullScreenOverlay)
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

    TestHostWindow window(connection, screen, kWidth, kHeight);

    Diagnostics diagnostics;
    diagnostics.setSink([](const vine::graphics::RenderDiagnostic& diagnostic) {
        std::printf("[window-composition] diagnostic: severity=%d category=%d message=%s\n",
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

    WindowTarget* window_target = vine::vsg::detail::SessionContentAccess::windowTarget(session);
    ASSERT_NE(window_target, nullptr);
    const auto device = vine::vsg::detail::SessionContentAccess::device(session);
    ASSERT_NE(device, nullptr);

    // The two off-screen targets: A is the picture the overlay samples (a plain clear), B is the target whose
    // pass has the SAME engine shape as a window pass - same colour format, same depth format, same samples -
    // which is exactly the case whose render passes are nevertheless NOT compatible (see the file note).
    OffscreenTarget::Layout picture_layout;
    picture_layout.width  = kSourceSize;
    picture_layout.height = kSourceSize;
    for (std::size_t index = 0; index < 4U; ++index) {
        picture_layout.clear_color[index] = kPictureClear[index];
    }
    std::unique_ptr<OffscreenTarget> picture = OffscreenTarget::create(device, picture_layout);
    ASSERT_NE(picture, nullptr);

    OffscreenTarget::TargetLayout shared_layout;
    shared_layout.width         = kSourceSize;
    shared_layout.height        = kSourceSize;
    shared_layout.color_formats = { RenderTarget::ColorFormat::RGBA8 };
    // The depth format the WINDOW's traits asked for, taken from the window itself: this is what makes B's
    // engine-side shape the window's equal, so the case is about the two render passes that the engine's
    // vocabulary calls the same thing and the DEVICE formats tell apart.
    const TargetShape window_shape = window_target->shape();
    ASSERT_EQ(window_shape.color_formats.size(), 1U);
    EXPECT_EQ(window_shape.color_formats.front(), RenderTarget::ColorFormat::RGBA8)
        << "the case's pixel expectations are written for an 8-bit surface";
    ASSERT_TRUE(window_shape.depth_format.has_value()) << "a window with a depth attachment is what the case is for";
    shared_layout.depth_format = window_shape.depth_format;
    shared_layout.clear.color  = true;
    for (std::size_t index = 0; index < 4U; ++index) {
        shared_layout.clear.color_value[index] = kSceneClear[index];
    }
    std::unique_ptr<OffscreenTarget> shared_target = OffscreenTarget::create(device, shared_layout);
    ASSERT_NE(shared_target, nullptr);
    ASSERT_TRUE(shared_target->hasDepth());

    // The content stack the scene uses (one layer, one recorder, one registry per PASS).
    auto storage     = BlockStorage::create(device, BlockStorage::Layout{});
    ASSERT_NE(storage, nullptr);
    auto descriptors = BlockDescriptors::create(device, *storage);
    ASSERT_NE(descriptors, nullptr);

    const vine::intrusive_ptr<ShaderProgram> program(new ShaderProgram());
    {
        ShaderStage vertex;
        vertex.type   = ShaderStageType::Vertex;
        vertex.source = vine::String(reinterpret_cast<const char8_t*>(
            "layout(location = 0) in vec3 position;\n"
            "layout(set = 0, binding = 0) uniform ViewBlock {\n"
            "    mat4 view; mat4 inv_view; mat4 proj; mat4 view_proj; vec4 cam_pos; vec4 frame; } vb;\n"
            "void main() { gl_Position = vb.view_proj * vec4(position, 1.0); }\n"));
        ShaderStage fragment;
        fragment.type   = ShaderStageType::Fragment;
        fragment.source = vine::String(reinterpret_cast<const char8_t*>(
            "layout(location = 0) out vec4 outColor;\n"
            "layout(set = 0, binding = 0) uniform ViewBlock {\n"
            "    mat4 view; mat4 inv_view; mat4 proj; mat4 view_proj; vec4 cam_pos; vec4 frame; } vb;\n"
            "void main() { outColor = vec4(vb.frame.y / 128.0, vb.cam_pos.x, vb.frame.z / 96.0, 1.0); }\n"));
        program->addStage(vertex);
        program->addStage(fragment);
    }

    ProgramFacts program_facts;
    ASSERT_EQ(buildProgramFacts(*program, program_facts), FactMiss::None);

    const ContentPipeline::VertexBinding   binding{ 0U, sizeof(float) * 3U, false };
    const ContentPipeline::VertexAttribute attribute{ 0U, 0U, VK_FORMAT_R32G32B32_SFLOAT, 0U };
    auto content_layer = ContentPipeline::create(descriptors->layout(),
                                                 std::span<const ContentPipeline::VertexBinding>(&binding, 1U),
                                                 std::span<const ContentPipeline::VertexAttribute>(&attribute, 1U),
                                                 program_facts.shaders);
    ASSERT_NE(content_layer, nullptr);

    // The screen layer: the engine's own copy program over the picture.
    const vine::intrusive_ptr<ShaderProgram> screen_program(vine::graphics::screenCopyProgram(0));
    ASSERT_NE(screen_program, nullptr);
    ProgramFacts screen_facts;
    ASSERT_EQ(buildScreenProgramFacts(*screen_program, screen_facts), FactMiss::None);
    auto screen_layer = ContentPipeline::createScreen(screen_facts.shaders);
    ASSERT_NE(screen_layer, nullptr);

    VariantPool   content_pool;
    VariantPool   screen_pool;
    StreamUploads uploads;
    const auto    entry_points = vine::vsg::detail::fetchDynamicStateEntryPoints(device->vk(), device->getInstance()->vk());
    ContentDraw   content_draws(*content_layer, content_pool, entry_points);
    ContentDraw   overlay_draws(*screen_layer, screen_pool, entry_points);

    // The scene: a triangle whose fragment stage encodes the view block, at the same camera the earlier
    // session case uses - the view pushes it into the LEFT quarter of the picture.
    const vine::intrusive_ptr<Geometry> geometry(new Geometry());
    geometry->setPositions(vine::intrusive_ptr<vine::Buffer<float>>(
        new vine::Buffer<float>(std::vector<float>{ -0.4F, -0.4F, 0.5F, 0.4F, -0.4F, 0.5F, 0.0F, 0.6F, 0.5F })));
    geometry->setIndices(vine::intrusive_ptr<vine::Buffer<std::uint32_t>>(
        new vine::Buffer<std::uint32_t>(std::vector<std::uint32_t>{ 0U, 1U, 2U })));
    geometry->setRevision(1U);

    GeometryFacts                        geometry_facts;
    std::vector<vine::vsg::ChannelFacts> channel_storage;
    ASSERT_EQ(buildGeometryFacts(*geometry, geometry_facts, channel_storage), FactMiss::None);

    const vine::intrusive_ptr<Material> material(new Material());
    material->setDiffuse(vine::Colorf(0.2F, 0.3F, 0.4F, 1.0F));
    MaterialFacts          material_facts;
    std::vector<std::byte> material_storage;
    ASSERT_EQ(buildMaterialFacts(material.get(), 1U, material_facts, material_storage), FactMiss::None);

    const ProgramFacts  programs[]   = { program_facts, screen_facts };
    const GeometryFacts geometries[] = { geometry_facts };
    const MaterialFacts materials[]  = { material_facts };
    ContentFacts        facts;
    facts.programs   = programs;
    facts.geometries = geometries;
    facts.materials  = materials;

    const vine::intrusive_ptr<vine::graphics::Camera> camera(new vine::graphics::Camera());
    camera->setViewMatrixAsLookAt(vine::math::Vec3d(0.5, 0.0, 1.5), vine::math::Vec3d(0.5, 0.0, 0.0),
                                  vine::math::Vec3d(0.0, 1.0, 0.0));
    camera->setProjectionMatrixAsOrtho(-1.0, 1.0, -1.0, 1.0, 0.5, 4.0);
    const std::vector<RenderCommand> commands = [&] {
        RenderCommand command;
        command.geometry = geometry;
        command.material = material;
        command.program  = program;
        return std::vector<RenderCommand>{ command };
    }();

    // The identity the plan names the targets by: the SDK handles the engine announces them with.
    const vine::intrusive_ptr<RenderTarget> picture_handle(new RenderTarget());
    const vine::intrusive_ptr<RenderTarget> shared_handle(new RenderTarget());

    // The facts the plan resolves against, taken FROM the targets: a target's shape carries the engine's
    // formats and the device ones, and the plan compares wanted with current - hand-building the wanted shape
    // without the device formats would read as "the target's shape changed" and ask for a rebuild.
    TargetFacts picture_facts;
    picture_facts.target        = picture_handle.get();
    picture_facts.wanted.width  = static_cast<int>(kSourceSize);
    picture_facts.wanted.height = static_cast<int>(kSourceSize);
    picture_facts.wanted.shape  = picture->shape();
    picture_facts.current       = picture->instance();

    TargetFacts shared_facts;
    shared_facts.target                = shared_handle.get();
    shared_facts.wanted.width          = static_cast<int>(kSourceSize);
    shared_facts.wanted.height         = static_cast<int>(kSourceSize);
    shared_facts.wanted.shape          = shared_target->shape();
    shared_facts.current               = shared_target->instance();

    const std::vector<TargetFacts> target_table = { picture_facts, shared_facts, window_target->facts() };

    // FOUR passes: the picture (cleared), the off-screen scene pass whose ENGINE shape equals the window's, the
    // window's scene pass (the FIRST window pass - it owns the clear) and the window's overlay pass.
    FrameArena    arena{ 64 * 1024 };
    vine::vsg::core::Observe observe;
    FrameRecorder recorder{ arena, diagnostics, observe };
    FrameCompiler compiler{ arena, diagnostics, observe };

    ClearPolicy scene_clear;
    scene_clear.color          = true;
    scene_clear.color_value[0] = kSceneClear[0];
    scene_clear.color_value[1] = kSceneClear[1];
    scene_clear.color_value[2] = kSceneClear[2];
    scene_clear.color_value[3] = 1.0F;

    ClearPolicy overlay_clear;
    overlay_clear.color          = true;
    overlay_clear.color_value[0] = kOverlayClear[0];
    overlay_clear.color_value[1] = kOverlayClear[1];
    overlay_clear.color_value[2] = kOverlayClear[2];
    overlay_clear.color_value[3] = 1.0F;

    ClearPolicy picture_clear;
    picture_clear.color          = true;
    picture_clear.color_value[0] = kPictureClear[0];
    picture_clear.color_value[1] = kPictureClear[1];
    picture_clear.color_value[2] = kPictureClear[2];
    picture_clear.color_value[3] = 1.0F;

    std::vector<RenderTarget*> picture_inputs{ picture_handle.get() };

    ASSERT_TRUE(recorder.beginFrame(session.beginFrame()));
    ASSERT_TRUE(recorder.beginPass(1U));  // the picture
    ASSERT_TRUE(recorder.setRenderTarget(picture_handle.get()));
    ASSERT_TRUE(recorder.setClearPolicy(picture_clear));
    ASSERT_TRUE(recorder.endPass());

    ASSERT_TRUE(recorder.beginPass(2U));  // the off-screen pass that shares the window's variant
    ASSERT_TRUE(recorder.setRenderTarget(shared_handle.get()));
    ASSERT_TRUE(recorder.setClearPolicy(scene_clear));
    ASSERT_TRUE(recorder.render(commands, camera.get()));
    ASSERT_TRUE(recorder.endPass());

    ASSERT_TRUE(recorder.beginPass(3U));  // the window's scene pass
    ASSERT_TRUE(recorder.setRenderTarget(nullptr));
    ASSERT_TRUE(recorder.setClearPolicy(scene_clear));
    ASSERT_TRUE(recorder.render(commands, camera.get()));
    ASSERT_TRUE(recorder.endPass());

    ASSERT_TRUE(recorder.beginPass(4U));  // the window's overlay pass
    ASSERT_TRUE(recorder.setRenderTarget(nullptr));
    ASSERT_TRUE(recorder.setClearPolicy(overlay_clear));  // announced, and NOT applied: the window clears once
    ASSERT_TRUE(recorder.setPassInputs(picture_inputs));
    ASSERT_TRUE(recorder.setViewport(kPictureInPicture.x, kPictureInPicture.y, kPictureInPicture.width,
                                     kPictureInPicture.height));
    ASSERT_TRUE(recorder.drawScreenProgram(picture_handle.get(), screen_program.get(), camera.get()));
    ASSERT_TRUE(recorder.endPass());
    ASSERT_TRUE(recorder.endFrame());

    const CompiledFrame& frame = compiler.compile(recorder.description(), FrameFacts{ target_table });
    ASSERT_EQ(frame.passes.size(), 4U) << "a pass that only clears is still a pass (the picture's producer)";
    const auto pass_of = [&frame](std::uint32_t id) -> const vine::vsg::core::CompiledPass* {
        for (const auto& pass : frame.passes) {
            if (pass.pass == id) {
                return &pass;
            }
        }
        return nullptr;
    };
    const auto* overlay = pass_of(4U);
    ASSERT_NE(overlay, nullptr);
    ASSERT_EQ(overlay->draws.size(), 1U);
    EXPECT_EQ(overlay->draws[0].kind, DrawKind::Screen);
    ASSERT_EQ(overlay->inputs.size(), 1U);
    EXPECT_EQ(overlay->inputs[0].target, static_cast<const void*>(picture_handle.get()));

    // The three records the passes need, each with the state its own pass owns: one registry per pass, because
    // the registry answers "is this variant already bound?" and a pipeline bound in another pass is not.
    storage->beginFrame();

    const auto state_pass = [&](std::uint32_t id) -> const vine::vsg::core::CompiledPass& {
        const auto* found = pass_of(id);
        static const vine::vsg::core::CompiledPass empty{};
        return found != nullptr ? *found : empty;
    };
    const auto view_block_for = [&](std::uint32_t pass_id, std::uint32_t width, std::uint32_t height) {
        const auto  block = buildViewBlock(state_pass(pass_id).draws[0].camera, session.frameSeconds(), width, height);
        return std::vector<std::byte>(reinterpret_cast<const std::byte*>(&block),
                                      reinterpret_cast<const std::byte*>(&block) + sizeof(block));
    };

    StateRegistry scene_registry(content_pool);
    StateRegistry window_registry(content_pool);
    const auto    shared_view = view_block_for(2U, kSourceSize, kSourceSize);
    const auto    window_view =
        view_block_for(3U, static_cast<std::uint32_t>(kWidth), static_cast<std::uint32_t>(kHeight));

    const ContentPass::Scope::Entry content_halves[]{
        ContentPass::Scope::Entry{ DrawKind::Content, program.get(), program_facts.revision, geometry_facts.layout,
                                   content_layer.get(), &content_draws } };

    const auto* shared_pass = pass_of(2U);
    ASSERT_NE(shared_pass, nullptr);
    ContentPass::Scope shared_scope;
    shared_scope.entries     = content_halves;
    shared_scope.registry    = &scene_registry;
    shared_scope.storage     = storage.get();
    shared_scope.descriptors = descriptors.get();
    shared_scope.uploads     = &uploads;
    ContentPass shared_content(shared_scope, diagnostics);
    ::vsg::ref_ptr<::vsg::Node> shared_node;
    ASSERT_TRUE(shared_content.record(*shared_pass, facts, shared_target->shape().compatibility(),
                                      std::span<const InputImages>{}, shared_view, shared_node));

    const auto* window_pass = pass_of(3U);
    ASSERT_NE(window_pass, nullptr);
    ContentPass::Scope window_scope;
    window_scope.entries     = content_halves;
    window_scope.registry    = &window_registry;
    window_scope.storage     = storage.get();
    window_scope.descriptors = descriptors.get();
    window_scope.uploads     = &uploads;
    ContentPass window_content(window_scope, diagnostics);
    ::vsg::ref_ptr<::vsg::Node> window_node;
    ASSERT_TRUE(window_content.record(*window_pass, facts, window_target->shape().compatibility(),
                                      std::span<const InputImages>{}, window_view, window_node));

    StateRegistry          screen_registry(screen_pool);
    const ContentPass::Scope::Entry screen_halves[]{
        ContentPass::Scope::Entry{ DrawKind::Screen, screen_program.get(), screen_facts.revision, {},
                                   screen_layer.get(), &overlay_draws } };
    ContentPass::Scope screen_scope;
    screen_scope.entries     = screen_halves;
    screen_scope.registry    = &screen_registry;
    screen_scope.storage     = storage.get();
    screen_scope.descriptors = descriptors.get();
    screen_scope.uploads     = &uploads;
    ContentPass overlay_content(screen_scope, diagnostics);

    const ::vsg::ref_ptr<::vsg::ImageView> picture_colors[] = { picture->colorView(0) };
    ASSERT_NE(picture_colors[0], nullptr);
    const InputImages overlay_images[] = {
        InputImages{ std::span<const ::vsg::ref_ptr<::vsg::ImageView>>(picture_colors, 1U) } };
    ::vsg::ref_ptr<::vsg::Node> overlay_node;
    ASSERT_TRUE(overlay_content.record(*overlay, facts, window_target->shape().compatibility(), overlay_images,
                                       window_view, overlay_node));

    // One variant per render-pass family: the pool would have said "one" if the key had stayed with the
    // engine's vocabulary, and that is exactly the variant that got bound in the wrong render pass.
    EXPECT_EQ(content_layer->pipelines(), 2U)
        << "an sRGB surface and a linear RGBA8 target are one engine shape and two render passes";
    EXPECT_EQ(content_draws.draws(), 2U);
    EXPECT_EQ(content_draws.pipeline_binds(), 2U) << "one bind per PASS: each pass has its own registry";
    EXPECT_EQ(overlay_draws.screen_draws(), 1U);
    EXPECT_EQ(overlay_draws.input_binds(), 1U);

    // The executor places all four passes (the window twice, in plan order) and one frame is presented.
    VsgExecutor executor(diagnostics);
    executor.setWindow(window_target);
    executor.addTarget(picture_handle.get(), picture.get());
    executor.addTarget(shared_handle.get(), shared_target.get());

    const auto command_graph = vine::vsg::detail::SessionContentAccess::makeFrameGraph(session);
    ASSERT_NE(command_graph, nullptr);
    const PassContent packets[] = { PassContent{ 1U, nullptr },   PassContent{ 2U, shared_node },
                                    PassContent{ 3U, window_node }, PassContent{ 4U, overlay_node } };
    ASSERT_TRUE(executor.record(frame, command_graph, packets));
    EXPECT_EQ(executor.skipped(), 0U);
    const std::span<const vine::vsg::core::PassId> placed = executor.recorded();
    EXPECT_EQ(std::vector<vine::vsg::core::PassId>(placed.begin(), placed.end()),
              std::vector<vine::vsg::core::PassId>({ 1U, 2U, 3U, 4U }))
        << "the executor places the passes in the plan's order, not in the order they were announced";

    ASSERT_TRUE(vine::vsg::detail::SessionContentAccess::assignFrameGraphs(session, ::vsg::CommandGraphs{ command_graph }));
    ASSERT_TRUE(session.commitFrame());
    EXPECT_EQ(session.framesPresented(), 1U);

    // Two plan-free frames: the presentation lands asynchronously, so the read-back needs the picture to have
    // been through the display server (see the session case). They also re-record the off-screen passes and
    // their copies, which is what makes this fixture the one that found the copy-back's cross-frame ordering
    // gap (a second frame writing the same destination buffer with no declared dependency; see
    // OffscreenTarget's capture).
    for (int settle = 0; settle < 2; ++settle) {
        ASSERT_TRUE(session.beginFrame());
        ASSERT_TRUE(session.commitFrame());
    }

    // The window hands its bytes back in the SURFACE's own order, and the checks below are written in the
    // colour's own terms: a BGR surface (B8G8R8A8, the format this swapchain picked) returns [B, G, R], so the
    // reader puts a pixel into (red, green, blue) once, here. A check written in the wrong order would compare
    // red against blue, and the colours are chosen so that the mistake cannot hide - which is not a stylistic
    // choice: the triangle is symmetric in red and blue, so a swapped read of IT looks perfectly fine.
    const bool blue_first = window_shape.device_color_formats.size() == 1U &&
                            (window_shape.device_color_formats.front() == VK_FORMAT_B8G8R8A8_UNORM ||
                             window_shape.device_color_formats.front() == VK_FORMAT_B8G8R8A8_SRGB);
    const auto pixel_at = [&](int x, int y) {
        const std::array<std::uint8_t, 3> bytes = window.pixel(x, y);
        return blue_first ? std::array<std::uint8_t, 3>{ bytes[2], bytes[1], bytes[0] } : bytes;
    };

    // The scene's triangle: the shader's own encoding (frame.y/128, cam_pos.x, frame.z/96) = (1, 0.5, 1), in the
    // left quarter - the window pass' content.
    const auto triangle = pixel_at(kWidth / 4, kHeight / 2);
    EXPECT_TRUE(isColour(triangle, 1.0, 0.5, 1.0))
        << "the window's scene pass drew through its own variant, got (" << static_cast<int>(triangle[0])
        << ", " << static_cast<int>(triangle[1]) << ", " << static_cast<int>(triangle[2]) << ")";

    // Outside everything: the first window pass' clear. The green of the OVERLAY pass must not appear - that is
    // what "the window is cleared once" means for the picture.
    const auto background = pixel_at(2, 2);
    EXPECT_TRUE(isColour(background, 0.0, 0.0, 0.25))
        << "the window keeps its FIRST pass' clear, got (" << static_cast<int>(background[0]) << ", "
        << static_cast<int>(background[1]) << ", " << static_cast<int>(background[2]) << ")";
    EXPECT_FALSE(isColourByte(background[1], 0.25)) << "the overlay pass' clear would be green here";

    // Inside the overlay's rectangle: the picture the overlay sampled, opaque (the copy's alpha is 1).
    const auto overlay_pixel = pixel_at(kPictureInPicture.x + kPictureInPicture.width / 2,
                                        kPictureInPicture.y + kPictureInPicture.height / 2);
    EXPECT_TRUE(isColour(overlay_pixel, 0.25, 0.0, 0.0))
        << "the overlay copied the picture into its rectangle, got (" << static_cast<int>(overlay_pixel[0])
        << ", " << static_cast<int>(overlay_pixel[1]) << ", " << static_cast<int>(overlay_pixel[2]) << ")";

    session.shutdown();
}

#endif  // !defined(_WIN32)
