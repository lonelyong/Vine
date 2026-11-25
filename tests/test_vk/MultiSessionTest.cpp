/**
 * @brief TWO frame drivers on one device: each shades with its own default program, each keeps its own picture, and
 *        a driver borrowed before `initialize()` comes up with the device (see `RenderDevice::createFrameDriver`).
 *
 * WHY THIS FILE EXISTS. S6a introduced the seam and deliberately answered the device's own driver. S6b makes the device
 * hand out a REAL second driver - its own command machinery, its own frame scratch, its own default program - while the
 * DEVICE still runs one frame body at a time (the caches/allocator/queue are shared; see `Impl::frame_mutex`). These
 * cases pin what that shape promises: two different programs never cross (T8), two threads each drive a driver and the
 * pictures stay their own (T10), and a driver borrowed before the creator brings the device up is valid afterwards
 * (R1).
 */

#include <gtest/gtest.h>

#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <future>
#include <memory>
#include <thread>
#include <vector>

#include <vine/Buffer.hpp>
#include <vine/Colorf.hpp>
#include <vine/String.hpp>
#include <vine/graphics/CameraSnapshot.hpp>
#include <vine/graphics/FrameDescription.hpp>
#include <vine/graphics/Geometry.hpp>
#include <vine/graphics/Material.hpp>
#include <vine/graphics/Primitive.hpp>
#include <vine/graphics/RenderBackend.hpp>
#include <vine/graphics/RenderBackendRegistry.hpp>
#include <vine/graphics/DrawItem.hpp>
#include <vine/graphics/RenderState.hpp>
#include <vine/graphics/RenderTarget.hpp>
#include <vine/graphics/ShaderProgram.hpp>
#include <vine/graphics/Texture.hpp>
#include <vine/imaging/Image.hpp>
#include <vine/imaging/PixelFormat.hpp>
#include <vine/math/Matrix4x4.hpp>

#include <vine/backend_vk/internal/Device.hpp>
#include <vine/backend_vk/internal/PassTarget.hpp>
#include <vine/backend_vk/internal/TargetObjects.hpp>

#include "PlanTestSupport.hpp"

namespace
{

using vn::Colorf;
using vn::intrusive_ptr;
using vn::String;
using vn::graphics::AttachmentPolicy;
using vn::graphics::CameraSnapshot;
using vn::graphics::DeviceDesc;
using vn::graphics::DevicePresentation;
using vn::graphics::DrawItem;
using vn::graphics::DrawKind;
using vn::graphics::DrawPacket;
using vn::graphics::FrameDescription;
using vn::graphics::Geometry;
using vn::graphics::Material;
using vn::graphics::PassDraw;
using vn::graphics::PresentOutcome;
using vn::graphics::Primitive;
using vn::graphics::RenderBackendRegistry;
using vn::graphics::RenderFrameExecutor;
using vn::graphics::RenderTarget;
using vn::graphics::ShaderProgram;
using vn::graphics::ShaderStage;
using vn::graphics::ShaderStageType;
using vn::graphics::Texture2D;
using vn::graphics::TextureSlot;
using vn::graphics::Topology;

constexpr int kTargetSize = 32;

/// @brief The clip-space triangle every case draws (see ContentDrawTest: z = 0.5 is in front of the reverse-Z far).
[[nodiscard]] intrusive_ptr<Geometry> makeTriangle()
{
    std::vector<float> positions{
        -0.75F, -0.75F, 0.5F,
        0.75F,  -0.75F, 0.5F,
        0.0F,   0.75F,  0.5F,
    };
    std::vector<std::uint32_t> indices{ 0U, 1U, 2U };

    auto geometry = intrusive_ptr<Geometry>(new Geometry());
    geometry->setPositions(intrusive_ptr<const vn::Buffer<float>>(new vn::Buffer<float>(std::move(positions))));

    Primitive primitive;
    primitive.topology = Topology::Triangles;
    primitive.indices  = Primitive::IndexSpan::whole(intrusive_ptr<const vn::Buffer<std::uint32_t>>(new vn::Buffer<std::uint32_t>(std::move(indices))));
    geometry->clearPrimitives();
    geometry->addPrimitive(primitive);
    geometry->bumpRevision();
    return geometry;
}

/// @brief A 32x32 RGBA8 + D24 target (the pass' depth policy tests, so the target carries depth).
[[nodiscard]] intrusive_ptr<RenderTarget> makeTarget(const char8_t* name)
{
    auto target = intrusive_ptr<RenderTarget>(new RenderTarget());
    target->setName(String(name));
    target->attachColor(RenderTarget::ColorFormat::RGBA8);
    target->attachDepth(RenderTarget::DepthFormat::D24);
    target->setSize(kTargetSize, kTargetSize);
    return target;
}

/// @brief A camera whose matrices are identities, so the triangle's clip-space positions ARE the picture.
[[nodiscard]] CameraSnapshot identityCamera()
{
    CameraSnapshot camera;
    camera.present    = true;
    camera.view       = vn::math::Mat4d{};
    camera.projection = vn::math::Mat4d{};
    camera.eye        = vn::math::Vec3d( 0.0, 0.0, 0.0 );
    return camera;
}

/// @brief A program that ignores every block and writes @p colour: the only difference between the two drivers.
[[nodiscard]] intrusive_ptr<ShaderProgram> solidProgram(const char8_t* name, const char8_t* fragment_body)
{
    constexpr const char8_t* kVertex = u8R"(
#version 450
layout(location = 0) in vec3 in_position;
void main()
{
    gl_Position = vec4(in_position, 1.0);
}
)";

    auto program = intrusive_ptr<ShaderProgram>(new ShaderProgram());
    program->setName(String(name));
    program->addStage(ShaderStage{ ShaderStageType::Vertex, String(kVertex), String(u8"main") });
    program->addStage(ShaderStage{ ShaderStageType::Fragment, String(fragment_body), String(u8"main") });
    return program;
}

[[nodiscard]] intrusive_ptr<ShaderProgram> redProgram()
{
    return solidProgram(u8"solid_red", u8R"(
#version 450
layout(location = 0) out vec4 out_color;
void main()
{
    out_color = vec4(1.0, 0.0, 0.0, 1.0);
}
)");
}

[[nodiscard]] intrusive_ptr<ShaderProgram> greenProgram()
{
    return solidProgram(u8"solid_green", u8R"(
#version 450
layout(location = 0) out vec4 out_color;
void main()
{
    out_color = vec4(0.0, 1.0, 0.0, 1.0);
}
)");
}

/// @brief One scene draw with one command that names NO program: the driver's default is what shades it.
[[nodiscard]] DrawPacket programlessDraw(const intrusive_ptr<Geometry>& geometry)
{
    auto commands = std::make_shared<std::vector<DrawItem>>();
    commands->push_back(DrawItem(geometry, nullptr, vn::math::Mat4d{}));

    DrawPacket draw;
    draw.kind     = DrawKind::Scene;
    draw.view.camera   = identityCamera();
    draw.commands = commands;
    return draw;
}

/// @brief One scene draw whose command names @p material (and no program: the driver's default shades it).
[[nodiscard]] DrawPacket materialDraw(const intrusive_ptr<Geometry>& geometry, const intrusive_ptr<Material>& material)
{
    auto commands = std::make_shared<std::vector<DrawItem>>();
    DrawItem command(geometry, nullptr, vn::math::Mat4d{});
    command.material             = material;
    command.material_description = material->description();
    commands->push_back(std::move(command));

    DrawPacket draw;
    draw.kind     = DrawKind::Scene;
    draw.view.camera   = identityCamera();
    draw.commands = commands;
    return draw;
}

/// @brief A material with a complete 1x1 base-colour texture: its first frame records an upload, which is the point.
[[nodiscard]] intrusive_ptr<Material> texturedMaterial(const char8_t* name)
{
    auto material = intrusive_ptr<Material>(new Material());
    material->setName(String(name));

    auto image = intrusive_ptr<vn::imaging::Image>(new vn::imaging::Image(1, 1, vn::imaging::PixelFormat::Rgba8Unorm));
    const std::span<std::byte> texel = image->mipData(0);
    texel[0] = std::byte{ 255U };
    texel[1] = std::byte{ 0U };
    texel[2] = std::byte{ 0U };
    texel[3] = std::byte{ 255U };

    auto texture = intrusive_ptr<Texture2D>(new Texture2D(1, 1, vn::imaging::PixelFormat::Rgba8Unorm));
    texture->setImage(image);
    material->setTexture(TextureSlot::BaseColor, texture);
    return material;
}

/// @brief A frame with ONE pass whose draw packets are issued in order (a good draw, then a refusing one).
[[nodiscard]] FrameDescription drawingFrameWith(RenderTarget* target, std::vector<DrawPacket> draws)
{
    FrameDescription frame;

    FrameDescription::Pass entry;
    entry.pass  = vn::test::framePassForTest();
    entry.order = 0;

    PassDraw pass;
    pass.target       = target;
    pass.attachment_policy        = AttachmentPolicy{};
    pass.attachment_policy->color = Colorf(0.0F, 0.0F, 0.0F, 1.0F);
    pass.packets        = std::move(draws);
    entry.draw        = std::move(pass);

    frame.passes.push_back(std::move(entry));
    return frame;
}

/// @brief One frame with ONE pass that clears @p target to @p colour and draws nothing.
[[nodiscard]] FrameDescription clearingFrame(RenderTarget* target, const Colorf& colour)
{
    FrameDescription frame;

    FrameDescription::Pass entry;
    entry.pass  = vn::test::framePassForTest();
    entry.order = 0;

    PassDraw pass;
    pass.target       = target;
    pass.attachment_policy        = AttachmentPolicy{};
    pass.attachment_policy->color = colour;
    entry.draw        = std::move(pass);

    frame.passes.push_back(std::move(entry));
    return frame;
}

/// @brief One scene draw that NAMES its program (so the frame records it without a default program).
[[nodiscard]] DrawPacket programDraw(const intrusive_ptr<Geometry>& geometry, const intrusive_ptr<ShaderProgram>& program)
{
    auto commands = std::make_shared<std::vector<DrawItem>>();
    DrawItem command(geometry, nullptr, vn::math::Mat4d{});
    command.program             = program;
    command.program_description = program->description();
    commands->push_back(std::move(command));

    DrawPacket draw;
    draw.kind     = DrawKind::Scene;
    draw.view.camera   = identityCamera();
    draw.commands = commands;
    return draw;
}

/// @brief A frame with ONE pass that clears @p target and issues @p draw.
[[nodiscard]] FrameDescription drawingFrame(RenderTarget* target, DrawPacket draw)
{
    FrameDescription frame;

    FrameDescription::Pass entry;
    entry.pass  = vn::test::framePassForTest();
    entry.order = 0;

    PassDraw pass;
    pass.target       = target;
    pass.attachment_policy        = AttachmentPolicy{};
    pass.attachment_policy->color = Colorf(0.0F, 0.0F, 0.0F, 1.0F);
    pass.packets.push_back(std::move(draw));
    entry.draw = std::move(pass);

    frame.passes.push_back(std::move(entry));
    return frame;
}

/// @brief The centre pixel as RGBA bytes.
[[nodiscard]] std::array<int, 4> centreOf(const std::vector<std::uint8_t>& pixels)
{
    const std::size_t offset = (static_cast<std::size_t>(kTargetSize / 2) * kTargetSize + kTargetSize / 2) * 4U;
    return { static_cast<int>(pixels[offset]), static_cast<int>(pixels[offset + 1U]), static_cast<int>(pixels[offset + 2U]),
             static_cast<int>(pixels[offset + 3U]) };
}

} // namespace

TEST(VkMultiSessionTest, TwoDriversShadeWithTheirOwnDefaultProgram)
{
    auto backend = RenderBackendRegistry::instance().create(u8"vk", DeviceDesc{ DevicePresentation::None, nullptr });
    ASSERT_NE(backend, nullptr);
    if (!backend->initialize()) {
        GTEST_SKIP() << "this machine has no Vulkan device the backend can come up on";
    }
    auto* const device = dynamic_cast<vn::backend_vk::Device*>(backend.get());
    ASSERT_NE(device, nullptr);

    RenderFrameExecutor* const second = device->createFrameDriver();
    ASSERT_NE(second, nullptr) << "the device handed out no second driver";
    EXPECT_NE(second, &device->executor());

    // T8: the default program is the DRIVER's setting, so two drivers on one device shade program-less content
    // differently - the whole point of moving the declaration to RenderFrameExecutor.
    device->setDefaultContentProgram(redProgram());
    second->setDefaultContentProgram(greenProgram());

    const intrusive_ptr<Geometry> geometry = makeTriangle();
    {
        // The device's own driver, through the device (the shape a direct caller uses).
        const intrusive_ptr<RenderTarget> target = makeTarget(u8"multi_first");
        (void)backend->executeFrame(drawingFrame(target.get(), programlessDraw(geometry)), nullptr);
        std::vector<std::uint8_t> pixels;
        ASSERT_TRUE(backend->readColorBuffer(target.get(), 0, pixels));
        const std::array<int, 4> centre = centreOf(pixels);
        EXPECT_GE(centre[0], 254) << "the first driver's red program did not shade its frame";
        EXPECT_LE(centre[1], 1);
        EXPECT_LE(centre[2], 1);
    }

    {
        // The second driver, driven directly: its own default program, its own frame.
        const intrusive_ptr<RenderTarget> target = makeTarget(u8"multi_second");
        (void)second->executeFrame(drawingFrame(target.get(), programlessDraw(geometry)), nullptr);
        std::vector<std::uint8_t> pixels;
        ASSERT_TRUE(backend->readColorBuffer(target.get(), 0, pixels));
        const std::array<int, 4> centre = centreOf(pixels);
        EXPECT_GE(centre[1], 254) << "the second driver's green program did not shade its frame";
        EXPECT_LE(centre[0], 1);
        EXPECT_LE(centre[2], 1);
    }

    device->releaseFrameDriver(*second);
    backend->shutdown();
}

TEST(VkMultiSessionTest, TwoDriversOnTwoThreadsEachKeepTheirOwnPicture)
{
    auto backend = RenderBackendRegistry::instance().create(u8"vk", DeviceDesc{ DevicePresentation::None, nullptr });
    ASSERT_NE(backend, nullptr);
    if (!backend->initialize()) {
        GTEST_SKIP() << "this machine has no Vulkan device the backend can come up on";
    }
    auto* const device = dynamic_cast<vn::backend_vk::Device*>(backend.get());
    ASSERT_NE(device, nullptr);

    RenderFrameExecutor* const second = device->createFrameDriver();
    ASSERT_NE(second, nullptr);
    device->setDefaultContentProgram(redProgram());
    second->setDefaultContentProgram(greenProgram());

    const intrusive_ptr<Geometry>     geometry = makeTriangle();
    const intrusive_ptr<RenderTarget> red_target   = makeTarget(u8"thread_red");
    const intrusive_ptr<RenderTarget> green_target = makeTarget(u8"thread_green");

    // EACH DRIVER'S OWN TEXTURE: its first frame records a copy and its staging belongs to that driver's batch
    // (ContentImages::BatchId), so the frame that finishes first must not free the other's staging (S6c-2).
    const intrusive_ptr<Material> red_material   = texturedMaterial(u8"thread_red_texture");
    const intrusive_ptr<Material> green_material = texturedMaterial(u8"thread_green_texture");

    constexpr int kFrames = 6;
    std::thread   red_driver([&backend, &geometry, &red_target, &red_material]() {
        for (int frame = 0; frame < kFrames; ++frame) {
            (void)backend->executeFrame(drawingFrame(red_target.get(), materialDraw(geometry, red_material)), nullptr);
        }
    });
    std::thread green_driver([second, &geometry, &green_target, &green_material]() {
        for (int frame = 0; frame < kFrames; ++frame) {
            (void)second->executeFrame(drawingFrame(green_target.get(), materialDraw(geometry, green_material)), nullptr);
        }
    });
    red_driver.join();
    green_driver.join();

    std::vector<std::uint8_t> red_pixels;
    std::vector<std::uint8_t> green_pixels;
    ASSERT_TRUE(backend->readColorBuffer(red_target.get(), 0, red_pixels));
    ASSERT_TRUE(backend->readColorBuffer(green_target.get(), 0, green_pixels));
    const std::array<int, 4> red   = centreOf(red_pixels);
    const std::array<int, 4> green = centreOf(green_pixels);
    EXPECT_GE(red[0], 254) << "the two drivers' pictures were mixed (red lost its own colour)";
    EXPECT_LE(red[1], 1);
    EXPECT_GE(green[1], 254) << "the two drivers' pictures were mixed (green lost its own colour)";
    EXPECT_LE(green[0], 1);

    device->releaseFrameDriver(*second);
    backend->shutdown();
}

TEST(VkMultiSessionTest, ADriverBorrowedBeforeInitializeComesUpWithTheDevice)
{
    // R1: an engine attaches before its creator brings the device up, so createFrameDriver() must answer an object whose
    // machinery is built by initialize().
    auto backend = RenderBackendRegistry::instance().create(u8"vk", DeviceDesc{ DevicePresentation::None, nullptr });
    ASSERT_NE(backend, nullptr);
    auto* const device = dynamic_cast<vn::backend_vk::Device*>(backend.get());
    ASSERT_NE(device, nullptr);

    RenderFrameExecutor* const early = device->createFrameDriver();
    ASSERT_NE(early, nullptr) << "a device that is not up yet refused to borrow a driver (R1)";

    if (!backend->initialize()) {
        GTEST_SKIP() << "this machine has no Vulkan device the backend can come up on";
    }
    early->setDefaultContentProgram(greenProgram());

    const intrusive_ptr<Geometry>     geometry = makeTriangle();
    const intrusive_ptr<RenderTarget> target   = makeTarget(u8"early_driver");
    (void)early->executeFrame(drawingFrame(target.get(), programlessDraw(geometry)), nullptr);

    std::vector<std::uint8_t> pixels;
    ASSERT_TRUE(backend->readColorBuffer(target.get(), 0, pixels));
    const std::array<int, 4> centre = centreOf(pixels);
    EXPECT_GE(centre[1], 254) << "the driver borrowed before initialize() never came up";

    device->releaseFrameDriver(*early);
    backend->shutdown();
}

TEST(VkMultiSessionTest, ReleasingTheDevicesOwnDriverIsANoOpAndAnotherDriverCanBeBorrowed)
{
    auto backend = RenderBackendRegistry::instance().create(u8"vk", DeviceDesc{ DevicePresentation::None, nullptr });
    ASSERT_NE(backend, nullptr);
    if (!backend->initialize()) {
        GTEST_SKIP() << "this machine has no Vulkan device the backend can come up on";
    }
    auto* const device = dynamic_cast<vn::backend_vk::Device*>(backend.get());
    ASSERT_NE(device, nullptr);

    backend->releaseFrameDriver(backend->executor());
    EXPECT_EQ(&backend->executor(), &device->executor()) << "releasing the device's own driver replaced it";

    RenderFrameExecutor* const borrowed = device->createFrameDriver();
    ASSERT_NE(borrowed, nullptr);
    device->releaseFrameDriver(*borrowed);
    // A released driver is gone and another can be borrowed: the address may be reused (the allocator is free to), so
    // what is asserted is that the NEW driver comes up and works - not where it lives.
    RenderFrameExecutor* const again = device->createFrameDriver();
    ASSERT_NE(again, nullptr);
    again->setDefaultContentProgram(greenProgram());
    const intrusive_ptr<RenderTarget> target = makeTarget(u8"after_release");
    (void)again->executeFrame(drawingFrame(target.get(), programlessDraw(makeTriangle())), nullptr);
    std::vector<std::uint8_t> pixels;
    ASSERT_TRUE(backend->readColorBuffer(target.get(), 0, pixels));
    EXPECT_GE(centreOf(pixels)[1], 254) << "a driver borrowed after a release never came up";
    device->releaseFrameDriver(*again);

    backend->shutdown();
}

TEST(VkMultiSessionTest, AHostVerbWaitsForTheOtherDriversFrameInsteadOfRacingIt)
{
    // WITH SEVERAL DRIVERS, "between frames" IS NOT A MOMENT THE HOST CAN NAME. The device therefore serialises every
    // verb that touches state a frame body touches (readback, release, the default program) on the same device lock
    // the body takes - so a host call from one thread WAITS instead of racing a frame running on another.
    auto backend = RenderBackendRegistry::instance().create(u8"vk", DeviceDesc{ DevicePresentation::None, nullptr });
    ASSERT_NE(backend, nullptr);
    if (!backend->initialize()) {
        GTEST_SKIP() << "this machine has no Vulkan device the backend can come up on";
    }
    auto* const device = dynamic_cast<vn::backend_vk::Device*>(backend.get());
    ASSERT_NE(device, nullptr);

    RenderFrameExecutor* const second = device->createFrameDriver();
    ASSERT_NE(second, nullptr);
    device->setDefaultContentProgram(redProgram());
    second->setDefaultContentProgram(greenProgram());

    const intrusive_ptr<Geometry>     geometry = makeTriangle();
    const intrusive_ptr<RenderTarget> own_target  = makeTarget(u8"verbs_own");
    const intrusive_ptr<RenderTarget> busy_target = makeTarget(u8"verbs_busy");

    // The own driver builds its target once (the readback below needs something to read).
    (void)backend->executeFrame(drawingFrame(own_target.get(), programlessDraw(geometry)), nullptr);

    constexpr int kFrames = 30;
    std::thread   driver([second, &geometry, &busy_target]() {
        for (int frame = 0; frame < kFrames; ++frame) {
            (void)second->executeFrame(drawingFrame(busy_target.get(), programlessDraw(geometry)), nullptr);
            std::this_thread::yield();
        }
    });

    // THE HOST VERBS, from this thread, while that frame body runs: none may fail or crash.
    int releases = 0;
    for (int attempt = 0; attempt < 10; ++attempt) {
        std::vector<std::uint8_t> pixels;
        EXPECT_TRUE(backend->readColorBuffer(own_target.get(), 0, pixels))
            << "a readback raced the frame in flight (it should have waited)";
        EXPECT_EQ(pixels.size(), static_cast<std::size_t>(kTargetSize) * kTargetSize * 4U);
        releases += device->releaseContent(geometry.get()) ? 1 : 0;
        std::this_thread::yield();
    }
    driver.join();
    EXPECT_GT(releases, 0) << "no release was ever accepted while another driver framed";

    // The busy driver still draws after the releases (its next frame re-uploads what was let go).
    (void)second->executeFrame(drawingFrame(busy_target.get(), programlessDraw(geometry)), nullptr);
    std::vector<std::uint8_t> pixels;
    ASSERT_TRUE(backend->readColorBuffer(busy_target.get(), 0, pixels));
    EXPECT_GE(centreOf(pixels)[1], 254) << "a release during the other driver's frame broke its next picture";

    device->releaseFrameDriver(*second);
    backend->shutdown();
}

TEST(VkMultiSessionTest, TheDefaultProgramMayChangeWhileAnotherDriverFrames)
{
    auto backend = RenderBackendRegistry::instance().create(u8"vk", DeviceDesc{ DevicePresentation::None, nullptr });
    ASSERT_NE(backend, nullptr);
    if (!backend->initialize()) {
        GTEST_SKIP() << "this machine has no Vulkan device the backend can come up on";
    }
    auto* const device = dynamic_cast<vn::backend_vk::Device*>(backend.get());
    ASSERT_NE(device, nullptr);

    RenderFrameExecutor* const second = device->createFrameDriver();
    ASSERT_NE(second, nullptr);
    device->setDefaultContentProgram(redProgram());
    second->setDefaultContentProgram(greenProgram());

    const intrusive_ptr<Geometry>     geometry = makeTriangle();
    const intrusive_ptr<RenderTarget> own_target  = makeTarget(u8"switch_own");
    const intrusive_ptr<RenderTarget> busy_target = makeTarget(u8"switch_busy");

    constexpr int kFrames = 20;
    std::thread   driver([second, &geometry, &busy_target]() {
        for (int frame = 0; frame < kFrames; ++frame) {
            (void)second->executeFrame(drawingFrame(busy_target.get(), programlessDraw(geometry)), nullptr);
        }
    });

    // The device's own driver is NOT the one framing, so its setting may change while the other runs; the write waits
    // for the body in flight (see Impl::frame_mutex) and the next frame uses the new program.
    device->setDefaultContentProgram(greenProgram());
    driver.join();

    (void)backend->executeFrame(drawingFrame(own_target.get(), programlessDraw(geometry)), nullptr);
    std::vector<std::uint8_t> pixels;
    ASSERT_TRUE(backend->readColorBuffer(own_target.get(), 0, pixels));
    EXPECT_GE(centreOf(pixels)[1], 254) << "the live program switch was lost while the other driver framed";

    device->releaseFrameDriver(*second);
    backend->shutdown();
}

TEST(VkMultiSessionTest, AFrameBodyDoesNotWaitForAnotherDriversFrame)
{
    // THE OBSERVABLE FACT S6c-2 CHANGES: two frame bodies overlap. The proof here is a frame that BLOCKS while another
    // driver's frame runs to completion - the diagnostic sink is the one place a test can hold a frame still without
    // holding any device lock. If the device still ran one body at a time, the second frame could not return until
    // the first was released; the bounded wait below turns that into a failure instead of a hang.
    auto backend = RenderBackendRegistry::instance().create(u8"vk", DeviceDesc{ DevicePresentation::None, nullptr });
    ASSERT_NE(backend, nullptr);
    if (!backend->initialize()) {
        GTEST_SKIP() << "this machine has no Vulkan device the backend can come up on";
    }
    auto* const device = dynamic_cast<vn::backend_vk::Device*>(backend.get());
    ASSERT_NE(device, nullptr);

    RenderFrameExecutor* const second = device->createFrameDriver();
    ASSERT_NE(second, nullptr);

    // The SECOND driver shades program-less content; the device's own driver has NO default program, so its
    // program-less draw is a refusal - and a refusal is what reaches the sink.
    second->setDefaultContentProgram(greenProgram());

    const intrusive_ptr<Geometry>     geometry      = makeTriangle();
    const intrusive_ptr<RenderTarget> warm_target   = makeTarget(u8"overlap_warm");
    const intrusive_ptr<RenderTarget> blocked_target = makeTarget(u8"overlap_blocked");

    // Warm the second driver: its frame then finds its target, its streams and its pipeline, so it allocates nothing
    // and takes no lock the blocked frame holds.
    (void)second->executeFrame(drawingFrame(warm_target.get(), programlessDraw(geometry)), nullptr);

    std::atomic<bool> sink_entered{ false };
    std::atomic<bool> second_finished{ false };
    std::atomic<bool> second_finished_inside{ false };

    backend->setDiagnosticSink([&](const vn::graphics::RenderDiagnostic& diagnostic) {
        if (diagnostic.category != vn::graphics::DiagnosticCategory::ContentSkipped) {
            return;
        }
        if (sink_entered.exchange(true)) {
            return; // only the first refusal blocks
        }
        // WAIT, BOUNDED, FOR THE OTHER DRIVER'S FRAME. Five seconds is orders of magnitude more than a frame takes; a
        // serialised device would spend all of it here and fail the assertion below. THE WITNESS IS SET HERE, while
        // this frame is still inside its body: "the other frame finished DURING this one", not "at some point".
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
        while (std::chrono::steady_clock::now() < deadline) {
            if (second_finished.load(std::memory_order_acquire)) {
                second_finished_inside.store(true, std::memory_order_relaxed);
                return;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
    });

    RenderFrameExecutor* const first = &device->executor();
    std::thread blocker([&]() { (void)first->executeFrame(drawingFrame(blocked_target.get(), programlessDraw(geometry)), nullptr); });

    // Wait until the blocking frame is INSIDE its report (bounded, so a device that never reports fails rather than
    // hangs).
    const auto enter_deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (!sink_entered.load(std::memory_order_acquire) && std::chrono::steady_clock::now() < enter_deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    ASSERT_TRUE(sink_entered.load()) << "the blocking frame never reached its report";

    std::thread other([&]() {
        (void)second->executeFrame(drawingFrame(warm_target.get(), programlessDraw(geometry)), nullptr);
        // RELEASE THE BLOCKER: the sink records whether this return happened while that frame was still inside.
        second_finished.store(true, std::memory_order_release);
    });

    other.join();
    blocker.join();

    EXPECT_TRUE(second_finished_inside.load()) << "the second driver's frame could only run after the first one ended";

    // AND THE SECOND DRIVER'S PICTURE IS STILL ITS OWN after that overlap.
    std::vector<std::uint8_t> pixels;
    ASSERT_TRUE(backend->readColorBuffer(warm_target.get(), 0, pixels));
    const std::array<int, 4> centre = centreOf(pixels);
    EXPECT_GE(centre[1], 254) << "the overlapped frame did not draw its program's green";
    EXPECT_LE(centre[0], 1);
    EXPECT_LE(centre[2], 1);

    backend->setDiagnosticSink({});
    device->releaseFrameDriver(*second);
    backend->shutdown();
}

TEST(VkMultiSessionTest, AReleaseDoesNotWaitForTheFrameInFlightAndDoesNotDestroyWhatItUses)
{
    // R6, OBSERVED: a release announced while a frame body is in flight RETURNS while that frame is still inside - and
    // the frame then finishes with what it had already resolved, because the release PARKS the objects instead of
    // destroying them (T6). The frame is held by the diagnostic sink (a refused draw packet reaches the host there),
    // so the release is attempted at a moment the body beyond doubt still holds its share of the device.
    auto backend = RenderBackendRegistry::instance().create(u8"vk", DeviceDesc{ DevicePresentation::None, nullptr });
    ASSERT_NE(backend, nullptr);
    if (!backend->initialize()) {
        GTEST_SKIP() << "this machine has no Vulkan device the backend can come up on";
    }
    auto* const device = dynamic_cast<vn::backend_vk::Device*>(backend.get());
    ASSERT_NE(device, nullptr);

    const intrusive_ptr<Geometry>      geometry = makeTriangle();
    const intrusive_ptr<ShaderProgram> program  = redProgram();
    const intrusive_ptr<RenderTarget>  blocked_target = makeTarget(u8"release_blocked");
    const intrusive_ptr<RenderTarget>  spare_target   = makeTarget(u8"release_spare");

    // Build the spare target so there is a built target to release (a release of something never built answers "already
    // true" and would prove nothing).
    (void)backend->executeFrame(drawingFrame(spare_target.get(), programDraw(geometry, program)), nullptr);

    std::atomic<bool> sink_entered{ false };
    std::atomic<bool> release_done{ false };

    backend->setDiagnosticSink([&](const vn::graphics::RenderDiagnostic& diagnostic) {
        if (diagnostic.category != vn::graphics::DiagnosticCategory::ContentSkipped) {
            return;
        }
        if (sink_entered.exchange(true)) {
            return; // only the first refusal holds the frame
        }
        // HOLD THE FRAME, BOUNDED, UNTIL THE RELEASE HAS HAPPENED. The five seconds are a failure detector, not a
        // schedule: the release below is expected in milliseconds.
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
        while (!release_done.load(std::memory_order_acquire) && std::chrono::steady_clock::now() < deadline) {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
    });

    // The frame draws ONE good call (recorded with the handles the head resolved) and then a call with no program (the
    // refusal that reaches the sink) - so the release below targets objects the frame has already resolved.
    std::vector<DrawPacket> draws;
    draws.push_back(programDraw(geometry, program));
    draws.push_back(programlessDraw(geometry));

    PresentOutcome outcome = PresentOutcome::Lost;
    std::thread    blocker([&]() { outcome = backend->executeFrame(drawingFrameWith(blocked_target.get(), std::move(draws)), nullptr); });

    const auto enter_deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (!sink_entered.load(std::memory_order_acquire) && std::chrono::steady_clock::now() < enter_deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    ASSERT_TRUE(sink_entered.load()) << "the frame never reached its refusal";

    // THE RELEASES, ON A THREAD WITH A DEADLINE: if any of them waits for the frame body, the wait below times out and
    // the test fails - a serialised release cannot hang the suite, it fails it.
    std::promise<void> released;
    std::future<void>  released_future = released.get_future();
    std::thread        releaser([&]() {
        (void)backend->releaseContent(program.get());
        (void)backend->releaseContent(geometry.get());
        backend->releaseRenderTarget(spare_target.get());
        released.set_value();
    });

    const std::future_status status = released_future.wait_for(std::chrono::seconds(2));

    // ALWAYS UNBLOCK AND JOIN, whatever the deadline said: a test that returned here would leave a frame and a releaser
    // running against a device it is about to shut down.
    release_done.store(true, std::memory_order_release);
    blocker.join();
    releaser.join();

    EXPECT_EQ(status, std::future_status::ready) << "a release waited for the frame in flight (R6)";
    EXPECT_EQ(outcome, PresentOutcome::Skipped) << "the frame did not finish after its content was released";

    // THE FRAME DREW WITH WHAT IT HAD RESOLVED: the release parked the program and the geometry, it did not pull them
    // out from under the command buffer that names them.
    std::vector<std::uint8_t> pixels;
    ASSERT_TRUE(backend->readColorBuffer(blocked_target.get(), 0, pixels));
    const std::array<int, 4> centre = centreOf(pixels);
    EXPECT_GE(centre[0], 254) << "the frame lost the content a release announced while it was in flight";
    EXPECT_LE(centre[1], 1);

    // NO DEVICE IDLE WAS TAKEN BY THE RELEASES: they were parked, not run under a counted stop. The readback above is
    // this test's ONE counted stop (S7: a readback waits for the frames in flight, and says so).
    const std::size_t waits = device->retirementNumbers().device_waits;
    EXPECT_EQ(waits, 1U) << "a release could not be parked and idled the device (the readback is the only counted stop)";
    // AND THE PARK STILL HOLDS THE OBJECTS: the frame that named them submitted after the release, so the watermark is
    // not past their retire point yet.
    EXPECT_GE(device->retirementNumbers().pending, 1U) << "the released objects were destroyed while the frame was in flight";

    // THE SPARE TARGET IS GONE (the release answered as if it were never built), and the device still frames.
    std::vector<std::uint8_t> spare_pixels;
    EXPECT_FALSE(backend->readColorBuffer(spare_target.get(), 0, spare_pixels)) << "the released target was still readable";
    EXPECT_EQ(device->retirementNumbers().device_waits, waits) << "a refused readback never stops the device";

    backend->setDiagnosticSink({});
    backend->shutdown();
}

TEST(VkMultiSessionTest, EachDriversTargetReadsBackItsOwnLastCompletedFrame)
{
    // T7 / R7: two drivers frame their own targets in an interleaved order, and a readback between those frames answers
    // THE TARGET'S OWN last completed frame. The readback is a DEVICE verb (S7): its command machinery is the staging
    // copy's own, so "which session framed last" cannot change what it reads.
    auto backend = RenderBackendRegistry::instance().create(u8"vk", DeviceDesc{ DevicePresentation::None, nullptr });
    ASSERT_NE(backend, nullptr);
    if (!backend->initialize()) {
        GTEST_SKIP() << "this machine has no Vulkan device the backend can come up on";
    }
    auto* const device = dynamic_cast<vn::backend_vk::Device*>(backend.get());
    ASSERT_NE(device, nullptr);

    RenderFrameExecutor* const second = device->createFrameDriver();
    ASSERT_NE(second, nullptr);
    device->setDefaultContentProgram(redProgram());
    second->setDefaultContentProgram(greenProgram());

    const intrusive_ptr<Geometry>     geometry    = makeTriangle();
    const intrusive_ptr<RenderTarget> own_target  = makeTarget(u8"readback_own");
    const intrusive_ptr<RenderTarget> busy_target = makeTarget(u8"readback_other");

    const std::size_t waits_before = device->retirementNumbers().device_waits;

    constexpr int kRounds = 4;
    for (int round = 0; round < kRounds; ++round) {
        // BOTH DRIVERS FRAME BETWEEN THE OTHER'S READBACK, so a readback that answered "whatever was drawn last"
        // would show the other driver's colour in every round.
        const PresentOutcome first = backend->executeFrame(drawingFrame(own_target.get(), programlessDraw(geometry)), nullptr);
        const PresentOutcome next  = second->executeFrame(drawingFrame(busy_target.get(), programlessDraw(geometry)), nullptr);
        ASSERT_EQ(first, PresentOutcome::Skipped);
        ASSERT_EQ(next, PresentOutcome::Skipped);

        std::vector<std::uint8_t> own_pixels;
        ASSERT_TRUE(backend->readColorBuffer(own_target.get(), 0, own_pixels)) << "round " << round;
        const std::array<int, 4> own = centreOf(own_pixels);
        EXPECT_GE(own[0], 254) << "round " << round << ": the own target lost its red";
        EXPECT_LE(own[1], 1);
        EXPECT_LE(own[2], 1);

        std::vector<std::uint8_t> other_pixels;
        ASSERT_TRUE(backend->readColorBuffer(busy_target.get(), 0, other_pixels)) << "round " << round;
        const std::array<int, 4> other = centreOf(other_pixels);
        EXPECT_GE(other[1], 254) << "round " << round << ": a readback answered another target's frame";
        EXPECT_LE(other[0], 1);
        EXPECT_LE(other[2], 1);
    }

    // EVERY READBACK IS ONE COUNTED DEVICE STOP (F7): the wait the verb takes for the frames in flight is the number.
    EXPECT_EQ(device->retirementNumbers().device_waits, waits_before + 2U * kRounds)
        << "a readback's wait for the frames in flight is not counted";

    device->releaseFrameDriver(*second);
    backend->shutdown();
}

TEST(VkMultiSessionTest, AReadbackFromInsideAFrameIsRefusedInsteadOfWaitingForIt)
{
    // A DIAGNOSTIC SINK RUNS INSIDE THE FRAME BODY. A readback there would wait for the frame it is itself inside -
    // the deadlock the frame-body depth exists to name - so the verb answers `Failed` at once, touches no GPU state and
    // touches no host vector. The frame then finishes: the refusal is an answer, not a stall.
    auto backend = RenderBackendRegistry::instance().create(u8"vk", DeviceDesc{ DevicePresentation::None, nullptr });
    ASSERT_NE(backend, nullptr);
    if (!backend->initialize()) {
        GTEST_SKIP() << "this machine has no Vulkan device the backend can come up on";
    }

    const intrusive_ptr<Geometry>      geometry = makeTriangle();
    const intrusive_ptr<ShaderProgram> program  = redProgram();
    const intrusive_ptr<RenderTarget>  target   = makeTarget(u8"inside_read");

    std::atomic<bool>            asked{ false };
    std::atomic<bool>            refused{ false };
    vn::graphics::ReadbackResult reason{ vn::graphics::ReadbackResult::Ok };
    std::size_t                  handed{ 1234U };

    backend->setDiagnosticSink([&](const vn::graphics::RenderDiagnostic& diagnostic) {
        if (diagnostic.category != vn::graphics::DiagnosticCategory::ContentSkipped) {
            return;
        }
        if (asked.exchange(true)) {
            return; // only the first refusal asks
        }
        std::vector<std::uint8_t> pixels;
        refused.store(!backend->readColorBuffer(target.get(), 0, pixels, &reason));
        handed = pixels.size();
    });

    // ONE GOOD CALL (the target is built and the frame is inside its body) and one program-less call, whose refusal
    // reaches the sink.
    std::vector<DrawPacket> draws;
    draws.push_back(programDraw(geometry, program));
    draws.push_back(programlessDraw(geometry));
    (void)backend->executeFrame(drawingFrameWith(target.get(), std::move(draws)), nullptr);

    EXPECT_TRUE(asked.load()) << "the frame's refusal never reached the sink";
    EXPECT_TRUE(refused.load()) << "a readback answered from inside the frame body (it should have been refused)";
    EXPECT_EQ(reason, vn::graphics::ReadbackResult::Failed) << "the inside-frame refusal means the verb did not run";
    EXPECT_EQ(handed, 0U) << "a refused readback must not touch the host's vector";

    backend->setDiagnosticSink({});
    backend->shutdown();
}

TEST(VkMultiSessionTest, AReadbackWaitsForTheFrameInFlightAndCountsTheWait)
{
    // F7: the readback waits for the frames in flight (that IS the completion evidence) and the wait is COUNTED. The
    // frame is held inside its sink, so the readback below is beyond doubt waiting for it; the bounded future turns
    // "did not wait" and "waited forever" into the same failure instead of a hang.
    auto backend = RenderBackendRegistry::instance().create(u8"vk", DeviceDesc{ DevicePresentation::None, nullptr });
    ASSERT_NE(backend, nullptr);
    if (!backend->initialize()) {
        GTEST_SKIP() << "this machine has no Vulkan device the backend can come up on";
    }
    auto* const device = dynamic_cast<vn::backend_vk::Device*>(backend.get());
    ASSERT_NE(device, nullptr);

    const intrusive_ptr<Geometry>      geometry = makeTriangle();
    const intrusive_ptr<ShaderProgram> program  = redProgram();
    const intrusive_ptr<RenderTarget>  target   = makeTarget(u8"waiting_read");

    // THE TARGET IS BUILT AND PAINTED RED BEFORE THE HELD FRAME, so the pixels below say the held frame ran.
    ASSERT_EQ(backend->executeFrame(drawingFrame(target.get(), programDraw(geometry, program)), nullptr), PresentOutcome::Skipped);

    // THE SNAPSHOT COMES BEFORE THE FRAME IS HELD: retirementNumbers() is itself a host verb that waits for the frames
    // in flight, so asking it from behind a held frame would wait for the sink below.
    const std::size_t waits_before = device->retirementNumbers().device_waits;

    std::atomic<bool> sink_entered{ false };
    std::atomic<bool> release_done{ false };
    backend->setDiagnosticSink([&](const vn::graphics::RenderDiagnostic& diagnostic) {
        if (diagnostic.category != vn::graphics::DiagnosticCategory::ContentSkipped) {
            return;
        }
        if (sink_entered.exchange(true)) {
            return; // only the first refusal holds the frame
        }
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
        while (!release_done.load(std::memory_order_acquire) && std::chrono::steady_clock::now() < deadline) {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
    });

    std::vector<DrawPacket> draws;
    draws.push_back(programDraw(geometry, program));
    draws.push_back(programlessDraw(geometry)); // the refusal that holds the frame
    const FrameDescription blocked = drawingFrameWith(target.get(), std::move(draws));

    std::thread blocker([&]() { (void)backend->executeFrame(blocked, nullptr); });
    const auto  enter_deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (!sink_entered.load(std::memory_order_acquire) && std::chrono::steady_clock::now() < enter_deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    ASSERT_TRUE(sink_entered.load()) << "the frame never reached its refusal";

    std::promise<void>        read_done;
    std::future<void>         read_future = read_done.get_future();
    std::vector<std::uint8_t> pixels;
    bool                      read_ok = false;
    std::thread               reader([&]() {
        read_ok = backend->readColorBuffer(target.get(), 0, pixels);
        read_done.set_value();
    });

    // THE READBACK MUST STILL BE WAITING: a verb that returned while the frame is inside its body did not wait.
    const std::future_status status = read_future.wait_for(std::chrono::milliseconds(150));

    // ALWAYS UNBLOCK AND JOIN, whatever the deadline said: a test that returned here would leave a frame and a reader
    // running against a device it is about to shut down.
    release_done.store(true, std::memory_order_release);
    blocker.join();
    reader.join();

    EXPECT_EQ(status, std::future_status::timeout) << "the readback did not wait for the frame in flight (F7)";
    ASSERT_TRUE(read_ok) << "the readback failed after the frame it waited for ended";
    const std::array<int, 4> centre = centreOf(pixels);
    EXPECT_GE(centre[0], 254) << "the readback did not see the frame it waited for";
    EXPECT_LE(centre[1], 1);

    EXPECT_EQ(device->retirementNumbers().device_waits, waits_before + 1U)
        << "the readback's wait is one counted device stop";

    backend->setDiagnosticSink({});
    backend->shutdown();
}

TEST(VkMultiSessionTest, AReadbackRacingARebuiltTargetAnswersAValueItCanStandBehind)
{
    // THE NO-WAIT REFUSALS RUN OUTSIDE THE FRAME GATE (see Device::readColorBuffer, phase one), so they read the
    // target while another thread's frame body may be rebuilding it. That is where audit A3-01's race lived:
    // `TargetObjects::fresh` was a plain bool written by the frame body and read by this thread. The device now
    // answers from a VALUE (`TargetTable::peek`, taken under the map lock every writer takes) and the record's one
    // record-path bit is atomic, so every answer below is one the contract allows: "not ready yet", or the picture
    // the last completed frame left. TSan is the witness for the race itself (`VN_TSAN_TARGET=test_vk
    // scripts/tsan_check.sh`); this case is the behavioural half, and the frame loop RESIZES every frame - the path
    // that rewrites the record's own shape in place.
    auto backend = RenderBackendRegistry::instance().create(u8"vk", DeviceDesc{ DevicePresentation::None, nullptr });
    ASSERT_NE(backend, nullptr);
    if (!backend->initialize()) {
        GTEST_SKIP() << "this machine has no Vulkan device the backend can come up on";
    }
    auto* const device = dynamic_cast<vn::backend_vk::Device*>(backend.get());
    ASSERT_NE(device, nullptr);

    RenderFrameExecutor* const second = device->createFrameDriver();
    ASSERT_NE(second, nullptr);

    const intrusive_ptr<RenderTarget> target = makeTarget(u8"readback_rebuilt");
    const Colorf                      colour(0.25F, 0.5F, 0.75F, 1.0F);

    constexpr int     kFrames = 12;
    std::atomic<bool> done{ false };
    std::thread       driver([second, &target, &done, colour]() {
        for (int frame = 0; frame < kFrames; ++frame) {
            // TWO EXTENTS, ONE SHAPE: a resize keeps the render pass and replaces every attachment, so the record
            // is rewritten while the reader below is asking about it.
            const int side = (frame % 2 == 0) ? kTargetSize : kTargetSize + 8;
            target->setSize(side, side);
            (void)second->executeFrame(clearingFrame(target.get(), colour), nullptr);
        }
        done.store(true, std::memory_order_release);
    });

    const std::size_t small_bytes = static_cast<std::size_t>(kTargetSize) * kTargetSize * 4U;
    const std::size_t large_bytes = static_cast<std::size_t>(kTargetSize + 8) * (kTargetSize + 8) * 4U;
    std::size_t       reads       = 0;
    std::size_t       successes   = 0;
    while (!done.load(std::memory_order_acquire)) {
        std::vector<std::uint8_t>    pixels;
        vn::graphics::ReadbackResult why = vn::graphics::ReadbackResult::Ok;
        const bool                   got = backend->readColorBuffer(target.get(), 0, pixels, &why);
        ++reads;
        // "Not ready yet" is always a legal answer (a frame may have rebuilt the target a moment ago); every other
        // failure would be a claim about a target that exists, so none may appear here.
        EXPECT_TRUE(got || why == vn::graphics::ReadbackResult::NotReady)
            << "a readback racing a rebuild answered " << static_cast<int>(why);
        if (!got) {
            continue;
        }
        ++successes;
        EXPECT_TRUE(pixels.size() == small_bytes || pixels.size() == large_bytes)
            << "a readback answered with an extent no frame ever drew (" << pixels.size() << " bytes)";
        if (pixels.size() < 4U) {
            continue;
        }
        // THE PICTURE IS THE ONE A FINISHED FRAME LEFT: the clear colour, never a half-built or half-copied image.
        EXPECT_NEAR(pixels[0], 64, 2);
        EXPECT_NEAR(pixels[1], 128, 2);
        EXPECT_NEAR(pixels[2], 191, 2);
        EXPECT_EQ(pixels[3], 255);
    }
    driver.join();

    EXPECT_GT(reads, 0U) << "the readback loop never ran";
    EXPECT_GT(successes, 0U) << "no readback saw a finished frame, so the loop proved nothing";

    device->releaseFrameDriver(*second);
    backend->shutdown();
}

TEST(VkMultiSessionTest, AReleaseCannotTakeAwayTheRecordAPassHasJustResolved)
{
    // R6 MEETS THE CLAIM PATH. A release does not wait for the frames in flight, so it may erase a target's entry at
    // any moment - including the moment after a pass' `ensure` answered. That answer is the RECORD, taken under the
    // same lock the build ran under, and this case pins the two facts the recording path stands on: the answer is the
    // record the table held, and a release announced right after PARKS it instead of emptying the caller's answer.
    // (The claim path used to ask `ensure` for a boolean and then resolve it with a second `lookup`; that second call
    // was one lock late and answered null for exactly this release.)
    auto backend = RenderBackendRegistry::instance().create(u8"vk", DeviceDesc{ DevicePresentation::None, nullptr });
    ASSERT_NE(backend, nullptr);
    if (!backend->initialize()) {
        GTEST_SKIP() << "this machine has no Vulkan device the backend can come up on";
    }
    auto* const device = dynamic_cast<vn::backend_vk::Device*>(backend.get());
    ASSERT_NE(device, nullptr);

    const intrusive_ptr<RenderTarget>                    target = makeTarget(u8"ensure_answers_the_record");
    const std::shared_ptr<vn::backend_vk::TargetObjects> built  = device->targetTable().ensure(*target);
    ASSERT_NE(built, nullptr) << "the table did not build the target";
    EXPECT_TRUE(built->built()) << "a build that answered a record left it unbuilt";

    // THE RELEASE IS ANNOUNCED NOW: the entry leaves the map at once (a later frame rebuilds it) ...
    EXPECT_TRUE(device->targetTable().release(target.get()));
    EXPECT_EQ(device->targetTable().lookup(target.get()), nullptr) << "the entry survived its own release";
    // ... and the answer this call already gave still names WHOLE objects, which is what a recording pass keeps using.
    // The park owns it from here and destroys it only once the watermark has passed every frame that could name it.
    EXPECT_TRUE(built->built()) << "the release destroyed the record it parked";
    EXPECT_TRUE(built->hasDepth()) << "the parked record lost its depth half";

    backend->shutdown();
}

TEST(VkMultiSessionTest, AReleaseRacingTheFramesClaimsNeverLeavesAPassWithoutATarget)
{
    // THE INTERLEAVING IS NOT FORCED (the window the fix removed is a lock acquisition, and no hook sits on it), so
    // what this case requires is that EVERY interleaving has a legal answer: while one thread drives frames into the
    // target, another releases it over and over. A frame that lost the race rebuilds what the release erased (a
    // rebuild is the claim holder's verb), a release that lands mid-frame parks the images instead of destroying them,
    // and the last frame still draws the picture. A pass handed a half-resolved record would crash or draw nothing.
    auto backend = RenderBackendRegistry::instance().create(u8"vk", DeviceDesc{ DevicePresentation::None, nullptr });
    ASSERT_NE(backend, nullptr);
    if (!backend->initialize()) {
        GTEST_SKIP() << "this machine has no Vulkan device the backend can come up on";
    }

    const intrusive_ptr<Geometry>      geometry = makeTriangle();
    const intrusive_ptr<ShaderProgram> program  = redProgram();
    const intrusive_ptr<RenderTarget>  target   = makeTarget(u8"release_race");

    std::atomic<bool>        stop{ false };
    std::atomic<std::size_t> frames{ 0 };

    std::thread driver([&]() {
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(1500);
        while (!stop.load(std::memory_order_acquire) && std::chrono::steady_clock::now() < deadline) {
            const PresentOutcome outcome = backend->executeFrame(drawingFrame(target.get(), programDraw(geometry, program)), nullptr);
            EXPECT_EQ(outcome, PresentOutcome::Skipped) << "a frame into a released-and-rebuilt target answered " << static_cast<int>(outcome);
            frames.fetch_add(1, std::memory_order_release);
        }
    });

    std::size_t releases = 0;
    while (frames.load(std::memory_order_acquire) < 20U && releases < 200000U) {
        backend->releaseRenderTarget(target.get());
        ++releases;
    }
    stop.store(true, std::memory_order_release);
    driver.join();

    EXPECT_GT(frames.load(), 0U) << "the driver never ran a frame";
    EXPECT_GT(releases, 0U) << "the release loop never ran";

    // THE LAST FRAME STILL DRAWS: the target is a description, and a release only changes what is resident.
    ASSERT_EQ(backend->executeFrame(drawingFrame(target.get(), programDraw(geometry, program)), nullptr), PresentOutcome::Skipped);
    std::vector<std::uint8_t> pixels;
    ASSERT_TRUE(backend->readColorBuffer(target.get(), 0, pixels));
    const std::array<int, 4> centre = centreOf(pixels);
    EXPECT_GE(centre[0], 254) << "the frame after the race lost its picture";
    EXPECT_LE(centre[1], 1);

    backend->shutdown();
}

TEST(VkMultiSessionTest, APassHandleKeepsItsTargetAliveAfterARelease)
{
    // THE HANDLE OWNS WHAT IT DRAWS INTO (see PassTarget). A release does not wait for the frames in flight (R6): it
    // erases the entry and parks the record, and the park's act asks TWO questions before it destroys - has the
    // submitted work completed, and does any value still name the record (see Impl::releaseLater). This case pins the
    // second half by holding the only other share: the queue's turn comes at every frame end, and it must DEFER while
    // the handle lives.
    auto backend = RenderBackendRegistry::instance().create(u8"vk", DeviceDesc{ DevicePresentation::None, nullptr });
    ASSERT_NE(backend, nullptr);
    if (!backend->initialize()) {
        GTEST_SKIP() << "this machine has no Vulkan device the backend can come up on";
    }
    auto* const device = dynamic_cast<vn::backend_vk::Device*>(backend.get());
    ASSERT_NE(device, nullptr);

    const intrusive_ptr<Geometry>      geometry = makeTriangle();
    const intrusive_ptr<ShaderProgram> program  = redProgram();
    const intrusive_ptr<RenderTarget>  target   = makeTarget(u8"handle_keeps_its_target");
    const intrusive_ptr<RenderTarget>  other    = makeTarget(u8"park_driver");

    std::shared_ptr<vn::backend_vk::TargetObjects> record = device->targetTable().ensure(*target);
    ASSERT_NE(record, nullptr) << "the table did not build the target";
    vn::backend_vk::PassTarget handle = vn::backend_vk::PassTarget::forTarget(record);
    EXPECT_TRUE(handle.valid());
    record.reset(); // the handle is now the only owner the host can see

    device->releaseRenderTarget(target.get());
    // THE PARK ASKS AT EVERY FRAME END AND DEFERS WHILE THE HANDLE LIVES: the queue stays non-empty, because its turn
    // found a value that still names the record.
    for (int frame = 0; frame < 3; ++frame) {
        ASSERT_EQ(backend->executeFrame(drawingFrame(other.get(), programDraw(geometry, program)), nullptr), PresentOutcome::Skipped);
    }
    EXPECT_GE(device->retirementNumbers().pending, 1U) << "the park destroyed a record a handle still names";

    // THE HANDLE STILL NAMES A LIVE RECORD: this read is the witness (a borrowed handle made it ASan's
    // heap-use-after-free), and the snapshot it was built with is untouched.
    EXPECT_FALSE(handle.holdsPicture());
    EXPECT_EQ(handle.extent().width, static_cast<std::uint32_t>(kTargetSize));
    EXPECT_EQ(handle.colorCount(), 1U);

    // AND WHEN THE LAST VALUE DROPS IT, THE NEXT TURN DESTROYS IT: nothing defers it any more.
    handle = vn::backend_vk::PassTarget{};
    for (int frame = 0; frame < 4 && device->retirementNumbers().pending != 0U; ++frame) {
        ASSERT_EQ(backend->executeFrame(drawingFrame(other.get(), programDraw(geometry, program)), nullptr), PresentOutcome::Skipped);
    }
    EXPECT_EQ(device->retirementNumbers().pending, 0U) << "the record was never destroyed after its last holder let go";

    backend->shutdown();
}

TEST(VkMultiSessionTest, AFrameKeepsTheSourceItReadsUntilTheFrameEnds)
{
    // A FRAME OWNS WHAT IT NAMES, NOT ONLY WHAT IT DRAWS INTO (see FrameRecording::read_records). Off-screen frame
    // bodies OVERLAP: while one driver records, another may complete a frame - and that frame end runs the park. A
    // host release of the source in between must not destroy images the recording frame is still going to submit, so
    // the frame keeps the record it resolved. The instrument is the RECORD'S SHARE COUNT, read here without taking the
    // body gate (retirementNumbers() is a host verb and would wait out the frame this case exists to hold).
    //
    // AND THE DRAIN IS THE SECOND WITNESS (backlog T61): once that frame ends, the parked record must be released
    // while the IDLE driver is still registered - the watermark counts the device's submissions, so a driver that
    // stops framing holds nothing back.
    auto backend = RenderBackendRegistry::instance().create(u8"vk", DeviceDesc{ DevicePresentation::None, nullptr });
    ASSERT_NE(backend, nullptr);
    if (!backend->initialize()) {
        GTEST_SKIP() << "this machine has no Vulkan device the backend can come up on";
    }
    auto* const device = dynamic_cast<vn::backend_vk::Device*>(backend.get());
    ASSERT_NE(device, nullptr);
    RenderFrameExecutor* const second = device->createFrameDriver();
    ASSERT_NE(second, nullptr);

    const intrusive_ptr<Geometry>      geometry    = makeTriangle();
    const intrusive_ptr<ShaderProgram> program     = redProgram();
    const intrusive_ptr<ShaderProgram> copy        = solidProgram(u8"session_copy", u8R"(
#version 450
layout(location = 0) out vec4 out_color;
void main()
{
    out_color = vec4(0.0, 0.0, 1.0, 1.0);
}
)");
    const intrusive_ptr<RenderTarget>  source      = makeTarget(u8"frame_reads_this");
    const intrusive_ptr<RenderTarget>  destination = makeTarget(u8"frame_draws_here");
    const intrusive_ptr<RenderTarget>  other       = makeTarget(u8"park_driver");

    // A PICTURE IN THE SOURCE, so the frame has something to read (and the table holds a built record for it).
    ASSERT_EQ(backend->executeFrame(drawingFrame(source.get(), programDraw(geometry, program)), nullptr), PresentOutcome::Skipped);
    std::shared_ptr<vn::backend_vk::TargetObjects> record = device->targetTable().lookup(source.get());
    ASSERT_NE(record, nullptr) << "the table never built the source";

    std::atomic<bool> sink_entered{ false };
    std::atomic<bool> go{ false };
    backend->setDiagnosticSink([&](const vn::graphics::RenderDiagnostic& diagnostic) {
        if (diagnostic.category != vn::graphics::DiagnosticCategory::ContentSkipped || sink_entered.exchange(true)) {
            return;
        }
        // HOLD THE RECORDING FRAME, BOUNDED: the main thread releases the source and lets another frame complete
        // while this one is inside its body.
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
        while (!go.load(std::memory_order_acquire) && std::chrono::steady_clock::now() < deadline) {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
    });

    DrawPacket fullscreen;
    fullscreen.kind                = DrawKind::Fullscreen;
    fullscreen.view.camera         = identityCamera();
    fullscreen.source              = source.get();
    fullscreen.program             = copy;
    fullscreen.program_description = copy->description();

    std::vector<DrawPacket> draws;
    draws.push_back(std::move(fullscreen));
    draws.push_back(programlessDraw(geometry)); // names no program: the refusal holds the frame at the sink

    PresentOutcome outcome = PresentOutcome::Lost;
    std::thread    recorder([&]() { outcome = second->executeFrame(drawingFrameWith(destination.get(), std::move(draws)), nullptr); });

    const auto enter_deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (!sink_entered.load(std::memory_order_acquire) && std::chrono::steady_clock::now() < enter_deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    ASSERT_TRUE(sink_entered.load()) << "the recording frame never reached its refusal";

    // THE RELEASE AND A COMPLETED FRAME, WHILE THE OTHER BODY IS STILL RECORDING: its frame end runs the park's turn,
    // which must keep the record because TWO values still name it - the reading frame's, and this case's own.
    device->releaseRenderTarget(source.get());
    ASSERT_EQ(backend->executeFrame(drawingFrame(other.get(), programDraw(geometry, program)), nullptr), PresentOutcome::Skipped);
    EXPECT_GE(record.use_count(), 3L)
        << "the park let go of a record a frame was still reading (the armed act, the reporting frame and this case each hold one share)";
    EXPECT_TRUE(record->built()) << "the record the reading frame names was destroyed under it";

    go.store(true, std::memory_order_release);
    recorder.join();
    EXPECT_EQ(outcome, PresentOutcome::Skipped) << "the recording frame did not finish";
    backend->setDiagnosticSink({});

    // ONCE THAT FRAME HAS ENDED, only this case's share is left - and the next frame end releases it, with the second
    // driver still registered and idle: nothing about a driver's own framing dates a park any more (T61).
    record.reset();
    for (int frame = 0; frame < 4 && device->retirementNumbers().pending != 0U; ++frame) {
        ASSERT_EQ(backend->executeFrame(drawingFrame(other.get(), programDraw(geometry, program)), nullptr), PresentOutcome::Skipped);
    }
    EXPECT_EQ(device->retirementNumbers().pending, 0U) << "the source was never destroyed after its frame ended (the idle driver must not hold the park back)";
    EXPECT_EQ(device->retirementNumbers().device_waits, 0U) << "a release never stops the device";

    device->releaseFrameDriver(*second);
    backend->shutdown();
}

TEST(VkMultiSessionTest, AReleaseBetweenTheClaimAndTheRecordStillDrawsTheSampledSource)
{
    // THE READ SET IS RESOLVED AT THE CLAIM (`K7`, review 9). A frame CLAIMED the source it samples, but the record
    // itself was looked up AGAIN inside the record loop - per pass and per source, under the target map's lock - so a
    // release landing between the claim and that second call emptied the frame's answer: the copy was refused with
    // `FullscreenNoSource` ("named no source target" - a sentence about the frame's own declaration) and the destination
    // kept its clear. The record is now resolved with the write set and kept by the pass slot, and the record loop
    // takes no target-map lock at all.
    //
    // THE WINDOW IS FORCED, DETERMINISTICALLY: the frame's FIRST packet names no program, so its refusal is reported
    // before the second packet is recorded; the sink holds the recording thread there and another thread releases the
    // source - exactly the interleaving the second lookup lost.
    auto backend = RenderBackendRegistry::instance().create(u8"vk", DeviceDesc{ DevicePresentation::None, nullptr });
    ASSERT_NE(backend, nullptr);
    if (!backend->initialize()) {
        GTEST_SKIP() << "this machine has no Vulkan device the backend can come up on";
    }
    auto* const device = dynamic_cast<vn::backend_vk::Device*>(backend.get());
    ASSERT_NE(device, nullptr);

    const intrusive_ptr<Geometry>      geometry    = makeTriangle();
    const intrusive_ptr<ShaderProgram> program     = redProgram();
    const intrusive_ptr<RenderTarget>  source      = makeTarget(u8"claim_reads_this");
    const intrusive_ptr<RenderTarget>  destination = makeTarget(u8"claim_draws_here");

    // A PICTURE IN THE SOURCE (a red triangle on a black clear), so "the copy sampled it" and "the copy was refused"
    // are different pixels rather than two shades of nothing.
    ASSERT_EQ(backend->executeFrame(drawingFrame(source.get(), programDraw(geometry, program)), nullptr), PresentOutcome::Skipped);

    const intrusive_ptr<ShaderProgram> copy = solidProgram(u8"claim_copy", u8R"(
#version 450
layout(set = 0, binding = 0) uniform sampler2D vn_source;
layout(location = 0) out vec4 out_color;
void main()
{
    out_color = texture(vn_source, vec2(0.5, 0.5));
}
)");

    std::atomic<bool> sink_entered{ false };
    std::atomic<bool> released{ false };
    backend->setDiagnosticSink([&](const vn::graphics::RenderDiagnostic& diagnostic) {
        if (diagnostic.category != vn::graphics::DiagnosticCategory::ContentSkipped || sink_entered.exchange(true)) {
            return;
        }
        // HOLD THE RECORDING FRAME, BOUNDED, until the release has been announced on the other thread: the second
        // packet is recorded after this returns.
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
        while (!released.load(std::memory_order_acquire) && std::chrono::steady_clock::now() < deadline) {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
    });

    DrawPacket copy_draw;
    copy_draw.kind                = DrawKind::Fullscreen;
    copy_draw.view.camera         = identityCamera();
    copy_draw.source              = source.get();
    copy_draw.program             = copy;
    copy_draw.program_description = copy->description();

    std::vector<DrawPacket> draws;
    draws.push_back(programlessDraw(geometry)); // refuses first: its report is the hook this case needs
    draws.push_back(std::move(copy_draw));

    std::thread releaser([&]() {
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
        while (!sink_entered.load(std::memory_order_acquire) && std::chrono::steady_clock::now() < deadline) {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        device->releaseRenderTarget(source.get());
        released.store(true, std::memory_order_release);
    });

    EXPECT_EQ(backend->executeFrame(drawingFrameWith(destination.get(), std::move(draws)), nullptr), PresentOutcome::Skipped);
    releaser.join();
    backend->setDiagnosticSink({});

    ASSERT_TRUE(sink_entered.load()) << "the frame never reported the program-less draw, so the release never happened";
    ASSERT_TRUE(released.load()) << "the release never ran";

    std::vector<std::uint8_t> pixels;
    ASSERT_TRUE(backend->readColorBuffer(destination.get(), 0, pixels));
    const std::array<int, 4> centre = centreOf(pixels);
    EXPECT_GE(centre[0], 254) << "the copy did not sample the source the frame had claimed (its record was lost to the release)";
    EXPECT_LE(centre[1], 1);
    EXPECT_EQ(backend->diagnosticCount(vn::graphics::DiagnosticCategory::ContentSkipped), 1U)
        << "only the program-less draw was skipped: the copy sampled the record the claim resolved";

    backend->shutdown();
}

TEST(VkMultiSessionTest, AFrameKeepsTheContentItResolvedUntilTheFrameEnds)
{
    // THE OTHER HALF OF THE CONTENT WINDOW: one engine RECORDS while another engine's frames END. Every frame end runs
    // the park's turn, so a release announced while this frame records is one turn away from destroying the streams this
    // frame's command buffer already bound - unless the frame keeps the entry it resolved (`FrameBuilds::borrow`).
    //
    // THE EVIDENCE IS THE SANITIZER RUN: with the borrow removed, the parked `VkBuffer` is destroyed at one of the other
    // driver's frame ends and this frame then submits a command buffer naming it (ASan: heap-use-after-free). What the
    // case asserts in a normal build is the legal outcome - the frame runs to completion and still draws its picture.
    auto backend = RenderBackendRegistry::instance().create(u8"vk", DeviceDesc{ DevicePresentation::None, nullptr });
    ASSERT_NE(backend, nullptr);
    if (!backend->initialize()) {
        GTEST_SKIP() << "this machine has no Vulkan device the backend can come up on";
    }
    auto* const device = dynamic_cast<vn::backend_vk::Device*>(backend.get());
    ASSERT_NE(device, nullptr);
    RenderFrameExecutor* const second = device->createFrameDriver();
    ASSERT_NE(second, nullptr);

    const intrusive_ptr<Geometry>      geometry    = makeTriangle();
    const intrusive_ptr<ShaderProgram> program     = redProgram();
    const intrusive_ptr<RenderTarget>  destination = makeTarget(u8"content_draws_here");
    const intrusive_ptr<RenderTarget>  other       = makeTarget(u8"content_park_driver");

    // THE DESTINATION IS BUILT AND BOTH DRIVERS ARE WARM before the hold, so every frame below is a steady frame.
    ASSERT_EQ(backend->executeFrame(clearingFrame(destination.get(), Colorf(0.0F, 0.0F, 0.0F, 1.0F)), nullptr), PresentOutcome::Skipped);
    ASSERT_EQ(second->executeFrame(clearingFrame(destination.get(), Colorf(0.0F, 0.0F, 0.0F, 1.0F)), nullptr), PresentOutcome::Skipped);
    ASSERT_EQ(backend->executeFrame(drawingFrame(other.get(), programDraw(geometry, program)), nullptr), PresentOutcome::Skipped);

    std::atomic<bool> sink_entered{ false };
    std::atomic<bool> go{ false };
    backend->setDiagnosticSink([&](const vn::graphics::RenderDiagnostic& diagnostic) {
        if (diagnostic.category != vn::graphics::DiagnosticCategory::ContentSkipped || sink_entered.exchange(true)) {
            return;
        }
        // HOLD THE RECORDING FRAME, BOUNDED: the main thread releases the geometry and lets frames END while this one is
        // inside its body - which is when the park's turn comes.
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
        while (!go.load(std::memory_order_acquire) && std::chrono::steady_clock::now() < deadline) {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
    });

    std::vector<DrawPacket> draws;
    draws.push_back(programDraw(geometry, program));
    draws.push_back(programlessDraw(geometry)); // names no program: the refusal holds the frame at the sink

    PresentOutcome outcome = PresentOutcome::Lost;
    std::thread    recorder([&]() { outcome = second->executeFrame(drawingFrameWith(destination.get(), std::move(draws)), nullptr); });

    const auto enter_deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (!sink_entered.load(std::memory_order_acquire) && std::chrono::steady_clock::now() < enter_deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    ASSERT_TRUE(sink_entered.load()) << "the recording frame never reached its refusal";

    // THE RELEASE AND THREE COMPLETED FRAMES WHILE THE OTHER BODY IS STILL RECORDING: three submissions are more than the
    // park's window (`slots + 1`), so every parked act's turn comes while the recorder still holds what it resolved.
    EXPECT_TRUE(device->releaseContent(geometry.get())) << "the geometry was not resident to begin with";
    for (int frame = 0; frame < 3; ++frame) {
        ASSERT_EQ(backend->executeFrame(drawingFrame(other.get(), programDraw(makeTriangle(), program)), nullptr), PresentOutcome::Skipped);
    }

    go.store(true, std::memory_order_release);
    recorder.join();
    EXPECT_EQ(outcome, PresentOutcome::Skipped) << "the recording frame did not finish";
    backend->setDiagnosticSink({});

    // THE FRAME THAT RAN BESIDE THE RELEASE STILL DREW: its content is a description, and what it resolved stayed valid
    // at least until it was submitted.
    std::vector<std::uint8_t> pixels;
    ASSERT_TRUE(backend->readColorBuffer(destination.get(), 0, pixels));
    const std::array<int, 4> centre = centreOf(pixels);
    EXPECT_GE(centre[0], 254) << "the recording frame lost its picture";
    EXPECT_LE(centre[1], 1);

    device->releaseFrameDriver(*second);
    backend->shutdown();
}

TEST(VkMultiSessionTest, AReleaseRacingAFramesContentNeverLeavesACommandBufferNamingAFreedBuffer)
{
    // T60: CONTENT IS THE SAME WINDOW THE TARGET HALF CLOSED. A frame head resolves the geometry streams and the
    // pipelines its commands name; a host may release that content at any moment afterwards (`R6`: a release does not
    // wait for the frames in flight). Before the frame kept what it resolved (`ContentCaches::FrameBuilds::borrow`), the
    // park could destroy a `VkBuffer` that a live command buffer had already bound.
    //
    // THE INTERLEAVING IS NOT FORCED - nothing sits on the other side of the cache lock - so what this case requires is
    // that EVERY interleaving has a legal answer: the frames keep answering `Skipped`, and the last one still draws the
    // picture. The witness that the freed buffer is really gone without the fix is the sanitizer run (a `VkBuffer`
    // destroyed under a recording command buffer is a heap-use-after-free in this backend).
    auto backend = RenderBackendRegistry::instance().create(u8"vk", DeviceDesc{ DevicePresentation::None, nullptr });
    ASSERT_NE(backend, nullptr);
    if (!backend->initialize()) {
        GTEST_SKIP() << "this machine has no Vulkan device the backend can come up on";
    }

    const intrusive_ptr<Geometry>      geometry = makeTriangle();
    const intrusive_ptr<ShaderProgram> program  = redProgram();
    const intrusive_ptr<RenderTarget>  target   = makeTarget(u8"content_release_race");

    std::atomic<bool>        stop{ false };
    std::atomic<std::size_t> frames{ 0 };

    // THE FRAME VALUE IS BUILT ONCE, BEFORE THE THREADS: a draw packet reads the content objects it names, and the
    // release loop below drives their descriptions - building one per iteration ON the driver thread is a race in the
    // test, not in the backend.
    const FrameDescription content_frame = drawingFrame(target.get(), programDraw(geometry, program));

    std::thread driver([&]() {
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(1500);
        while (!stop.load(std::memory_order_acquire) && std::chrono::steady_clock::now() < deadline) {
            const PresentOutcome outcome = backend->executeFrame(content_frame, nullptr);
            EXPECT_EQ(outcome, PresentOutcome::Skipped) << "a frame beside a release answered " << static_cast<int>(outcome);
            frames.fetch_add(1, std::memory_order_release);
        }
    });

    std::size_t releases = 0;
    while (frames.load(std::memory_order_acquire) < 20U && releases < 200000U) {
        backend->releaseContent(geometry.get());
        backend->releaseContent(program.get());
        ++releases;
    }
    stop.store(true, std::memory_order_release);
    driver.join();

    EXPECT_GT(frames.load(), 0U) << "the driver never ran a frame";
    EXPECT_GT(releases, 0U) << "the release loop never ran";

    // THE LAST FRAME STILL DRAWS: the content is a description, and a release only changes what is resident - the next
    // frame uploads what left and reuses what compiled.
    ASSERT_EQ(backend->executeFrame(drawingFrame(target.get(), programDraw(geometry, program)), nullptr), PresentOutcome::Skipped);
    std::vector<std::uint8_t> pixels;
    ASSERT_TRUE(backend->readColorBuffer(target.get(), 0, pixels));
    const std::array<int, 4> centre = centreOf(pixels);
    EXPECT_GE(centre[0], 254) << "the frame after the race lost its picture";
    EXPECT_LE(centre[1], 1);

    backend->shutdown();
}
