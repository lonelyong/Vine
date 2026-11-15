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
#include <cstdint>
#include <memory>
#include <thread>
#include <vector>

#include <vine/Buffer.hpp>
#include <vine/Colorf.hpp>
#include <vine/String.hpp>
#include <vine/graphics/CameraSnapshot.hpp>
#include <vine/graphics/FrameDescription.hpp>
#include <vine/graphics/Geometry.hpp>
#include <vine/graphics/Primitive.hpp>
#include <vine/graphics/RenderBackend.hpp>
#include <vine/graphics/RenderBackendRegistry.hpp>
#include <vine/graphics/RenderCommand.hpp>
#include <vine/graphics/RenderState.hpp>
#include <vine/graphics/RenderTarget.hpp>
#include <vine/graphics/ShaderProgram.hpp>
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
using vn::graphics::PassDraw;
using vn::graphics::Primitive;
using vn::graphics::RenderBackendRegistry;
using vn::graphics::RenderCommand;
using vn::graphics::RenderFrameExecutor;
using vn::graphics::RenderTarget;
using vn::graphics::ShaderProgram;
using vn::graphics::ShaderStage;
using vn::graphics::ShaderStageType;
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
    draw.camera   = identityCamera();
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

    constexpr int kFrames = 6;
    std::thread   red_driver([&backend, &geometry, &red_target]() {
        for (int frame = 0; frame < kFrames; ++frame) {
            (void)backend->executeFrame(drawingFrame(red_target.get(), programlessDraw(geometry)), nullptr);
        }
    });
    std::thread green_driver([second, &geometry, &green_target]() {
        for (int frame = 0; frame < kFrames; ++frame) {
            (void)second->executeFrame(drawingFrame(green_target.get(), programlessDraw(geometry)), nullptr);
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
