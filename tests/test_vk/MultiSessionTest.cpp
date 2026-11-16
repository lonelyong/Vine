/**
 * @brief TWO frame drivers on one device: each shades with its own default program, each keeps its own picture, and
 *        a driver borrowed before `initialize()` comes up with the device (see `RenderDevice::createSession`).
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
#include <vine/graphics/RenderCommand.hpp>
#include <vine/graphics/RenderState.hpp>
#include <vine/graphics/RenderTarget.hpp>
#include <vine/graphics/ShaderProgram.hpp>
#include <vine/graphics/Texture.hpp>
#include <vine/imaging/Image.hpp>
#include <vine/imaging/PixelFormat.hpp>
#include <vine/math/Matrix4x4.hpp>

#include <vine/backend_vk/internal/Device.hpp>

#include "PlanTestSupport.hpp"

namespace
{

using vn::Colorf;
using vn::intrusive_ptr;
using vn::String;
using vn::graphics::CameraSnapshot;
using vn::graphics::ClearPolicy;
using vn::graphics::DeviceDesc;
using vn::graphics::DevicePresentation;
using vn::graphics::DrawDescription;
using vn::graphics::FrameDescription;
using vn::graphics::Geometry;
using vn::graphics::Material;
using vn::graphics::PassDraw;
using vn::graphics::PresentOutcome;
using vn::graphics::Primitive;
using vn::graphics::RenderBackendRegistry;
using vn::graphics::RenderCommand;
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
[[nodiscard]] DrawDescription programlessDraw(const intrusive_ptr<Geometry>& geometry)
{
    auto commands = std::make_shared<std::vector<RenderCommand>>();
    commands->push_back(RenderCommand(geometry, nullptr, vn::math::Mat4d{}));

    DrawDescription draw;
    draw.kind     = DrawDescription::Kind::Scene;
    draw.view.camera   = identityCamera();
    draw.commands = commands;
    return draw;
}

/// @brief One scene draw whose command names @p material (and no program: the driver's default shades it).
[[nodiscard]] DrawDescription materialDraw(const intrusive_ptr<Geometry>& geometry, const intrusive_ptr<Material>& material)
{
    auto commands = std::make_shared<std::vector<RenderCommand>>();
    RenderCommand command(geometry, nullptr, vn::math::Mat4d{});
    command.material             = material;
    command.material_description = material->description();
    commands->push_back(std::move(command));

    DrawDescription draw;
    draw.kind     = DrawDescription::Kind::Scene;
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

/// @brief A frame with ONE pass whose drawing calls are issued in order (a good draw, then a refusing one).
[[nodiscard]] FrameDescription drawingFrameWith(RenderTarget* target, std::vector<DrawDescription> draws)
{
    FrameDescription frame;

    FrameDescription::Pass entry;
    entry.pass  = vn::test::framePassForTest();
    entry.order = 0;

    PassDraw pass;
    pass.target       = target;
    pass.clear        = ClearPolicy{};
    pass.clear->color = Colorf(0.0F, 0.0F, 0.0F, 1.0F);
    pass.draws        = std::move(draws);
    entry.draw        = std::move(pass);

    frame.passes.push_back(std::move(entry));
    return frame;
}

/// @brief One scene draw that NAMES its program (so the frame records it without a default program).
[[nodiscard]] DrawDescription programDraw(const intrusive_ptr<Geometry>& geometry, const intrusive_ptr<ShaderProgram>& program)
{
    auto commands = std::make_shared<std::vector<RenderCommand>>();
    RenderCommand command(geometry, nullptr, vn::math::Mat4d{});
    command.program             = program;
    command.program_description = program->description();
    commands->push_back(std::move(command));

    DrawDescription draw;
    draw.kind     = DrawDescription::Kind::Scene;
    draw.view.camera   = identityCamera();
    draw.commands = commands;
    return draw;
}

/// @brief A frame with ONE pass that clears @p target and issues @p draw.
[[nodiscard]] FrameDescription drawingFrame(RenderTarget* target, DrawDescription draw)
{
    FrameDescription frame;

    FrameDescription::Pass entry;
    entry.pass  = vn::test::framePassForTest();
    entry.order = 0;

    PassDraw pass;
    pass.target       = target;
    pass.clear        = ClearPolicy{};
    pass.clear->color = Colorf(0.0F, 0.0F, 0.0F, 1.0F);
    pass.draws.push_back(std::move(draw));
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

    RenderFrameExecutor* const second = device->createSession();
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

    device->releaseSession(*second);
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

    RenderFrameExecutor* const second = device->createSession();
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

    device->releaseSession(*second);
    backend->shutdown();
}

TEST(VkMultiSessionTest, ADriverBorrowedBeforeInitializeComesUpWithTheDevice)
{
    // R1: an engine attaches before its creator brings the device up, so createSession() must answer an object whose
    // machinery is built by initialize().
    auto backend = RenderBackendRegistry::instance().create(u8"vk", DeviceDesc{ DevicePresentation::None, nullptr });
    ASSERT_NE(backend, nullptr);
    auto* const device = dynamic_cast<vn::backend_vk::Device*>(backend.get());
    ASSERT_NE(device, nullptr);

    RenderFrameExecutor* const early = device->createSession();
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

    device->releaseSession(*early);
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

    backend->releaseSession(backend->executor());
    EXPECT_EQ(&backend->executor(), &device->executor()) << "releasing the device's own driver replaced it";

    RenderFrameExecutor* const borrowed = device->createSession();
    ASSERT_NE(borrowed, nullptr);
    device->releaseSession(*borrowed);
    // A released driver is gone and another can be borrowed: the address may be reused (the allocator is free to), so
    // what is asserted is that the NEW driver comes up and works - not where it lives.
    RenderFrameExecutor* const again = device->createSession();
    ASSERT_NE(again, nullptr);
    again->setDefaultContentProgram(greenProgram());
    const intrusive_ptr<RenderTarget> target = makeTarget(u8"after_release");
    (void)again->executeFrame(drawingFrame(target.get(), programlessDraw(makeTriangle())), nullptr);
    std::vector<std::uint8_t> pixels;
    ASSERT_TRUE(backend->readColorBuffer(target.get(), 0, pixels));
    EXPECT_GE(centreOf(pixels)[1], 254) << "a driver borrowed after a release never came up";
    device->releaseSession(*again);

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

    RenderFrameExecutor* const second = device->createSession();
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

    device->releaseSession(*second);
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

    RenderFrameExecutor* const second = device->createSession();
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

    device->releaseSession(*second);
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

    RenderFrameExecutor* const second = device->createSession();
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
    device->releaseSession(*second);
    backend->shutdown();
}

TEST(VkMultiSessionTest, AReleaseDoesNotWaitForTheFrameInFlightAndDoesNotDestroyWhatItUses)
{
    // R6, OBSERVED: a release announced while a frame body is in flight RETURNS while that frame is still inside - and
    // the frame then finishes with what it had already resolved, because the release PARKS the objects instead of
    // destroying them (T6). The frame is held by the diagnostic sink (a refused drawing call reaches the host there),
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
    std::vector<DrawDescription> draws;
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

    RenderFrameExecutor* const second = device->createSession();
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

    device->releaseSession(*second);
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
    std::vector<DrawDescription> draws;
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

    std::vector<DrawDescription> draws;
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
