/**
 * @brief The last DevicePresentation the backend was missing: `Probe`, a device that brings up its own window.
 *
 * WHAT THIS PINS. `DeviceDesc{}` is the SDK's DEFAULT, and its presentation is `Probe`: a host that asks for a
 * backend and nothing else lets the DEVICE own the surface (see RenderDevice). Before this case the vk backend
 * answered that with `presentation == false` and a warning; now the device creates a window of its own, presents
 * through a target made for it, and destroys the window with the session.
 *
 * WHY THERE IS NO PIXEL ASSERTION HERE. The window's pixels belong to the display server, and this machine's
 * instrument cannot read them back even for a plain XCB fill (the M5 window case says so and skips that half). What
 * the case CAN measure is the lifecycle the SDK promises: a target comes out of an ask that names no window, the
 * frames through it reach the present call, a second ask is refused, and the session rebuilds (which is the only
 * way the window's destruction is observable from outside).
 *
 * A machine without a display cannot serve a Probe device at all: `initialize()` then answers false with a
 * diagnostic (the reference backend behaves the same), so the case skips.
 */

#include <gtest/gtest.h>

#include <array>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <memory>
#include <thread>
#include <vector>

#include <vine/Buffer.hpp>
#include <vine/Color.hpp>
#include <vine/String.hpp>
#include <vine/graphics/CameraSnapshot.hpp>
#include <vine/graphics/DepthMode.hpp>
#include <vine/graphics/FrameDescription.hpp>
#include <vine/graphics/Geometry.hpp>
#include <vine/graphics/PresentTarget.hpp>
#include <vine/graphics/Primitive.hpp>
#include <vine/graphics/RenderBackendRegistry.hpp>
#include <vine/graphics/RenderCommand.hpp>
#include <vine/graphics/RenderPass.hpp>
#include <vine/graphics/RenderTarget.hpp>
#include <vine/graphics/ShaderProgram.hpp>
#include <vine/graphics/StateResolution.hpp>
#include <vine/math/Matrix4x4.hpp>

#include "PlanTestSupport.hpp"

using vn::Colorf;
using vn::String;
using vn::graphics::CameraSnapshot;
using vn::graphics::ClearPolicy;
using vn::graphics::DepthMode;
using vn::graphics::DeviceDesc;
using vn::graphics::DevicePresentation;
using vn::graphics::DrawDescription;
using vn::graphics::FrameDescription;
using vn::graphics::Geometry;
using vn::graphics::PassDraw;
using vn::graphics::PrepareReport;
using vn::graphics::PresentOutcome;
using vn::graphics::Primitive;
using vn::graphics::RenderBackendRegistry;
using vn::graphics::RenderCommand;
using vn::graphics::RenderTarget;
using vn::graphics::ShaderProgram;
using vn::graphics::ShaderStage;
using vn::graphics::ShaderStageType;
using vn::graphics::SurfaceTargetResult;
using vn::graphics::Topology;
using vn::intrusive_ptr;

namespace
{

/// @brief The M4+ content ABI: camera push (two matrices) and a fragment that writes one colour.
constexpr const char8_t* kVertexStage = u8R"(
#version 450
layout(location = 0) in vec3 in_position;
layout(push_constant) uniform PushConstants
{
    mat4 projection;
    mat4 modelView;
} vn_pc;
void main()
{
    gl_Position = vn_pc.projection * vn_pc.modelView * vec4(in_position, 1.0);
}
)";

constexpr const char8_t* kGreenFragment = u8R"(
#version 450
layout(location = 0) out vec4 out_color;
void main()
{
    out_color = vec4(0.0, 1.0, 0.0, 1.0);
}
)";

[[nodiscard]] intrusive_ptr<ShaderProgram> makeProgram()
{
    auto program = intrusive_ptr<ShaderProgram>(new ShaderProgram());
    program->setName(u8"vk_probe_window");
    program->addStage(ShaderStage{ ShaderStageType::Vertex, String(kVertexStage), String(u8"main") });
    program->addStage(ShaderStage{ ShaderStageType::Fragment, String(kGreenFragment), String(u8"main") });
    return program;
}

/// @brief Builds a triangle in clip space, indexed, as the one primitive of its geometry.
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

/// @brief A frame with ONE window pass (target null = the default framebuffer the frame is presented through).
[[nodiscard]] FrameDescription windowFrame()
{
    const intrusive_ptr<ShaderProgram> program  = makeProgram();
    const intrusive_ptr<Geometry>      geometry = makeTriangle();

    RenderCommand command(geometry, nullptr, vn::math::Mat4d{});
    command.program             = program;
    command.program_description = program->description();

    auto commands = std::make_shared<std::vector<RenderCommand>>();
    commands->push_back(std::move(command));

    CameraSnapshot camera;
    camera.present    = true;
    camera.view       = vn::math::Mat4d{};
    camera.projection = vn::math::Mat4d{};

    DrawDescription draw;
    draw.kind     = DrawDescription::Kind::Scene;
    draw.camera   = camera;
    draw.commands = commands;

    FrameDescription frame;
    FrameDescription::Pass entry;
    entry.pass = vn::test::framePassForTest();
    PassDraw pass;
    pass.target       = nullptr; // the window the frame is presented through
    pass.clear        = ClearPolicy{};
    pass.clear->color = Colorf(1.0F, 0.0F, 0.0F, 1.0F);
    pass.state.depth  = DepthMode::TestAndWrite;
    pass.draws.push_back(std::move(draw));
    entry.draw = std::move(pass);
    frame.passes.push_back(std::move(entry));
    return frame;
}

/// @brief Runs @p frame until it presents (a freshly mapped window's first acquires may answer Lost while the
///        surface settles), or answers the last outcome after @p attempts frames.
[[nodiscard]] PresentOutcome presentUntilReady(vn::graphics::RenderBackend& backend, const FrameDescription& frame, vn::raw_ptr<vn::graphics::WindowPresentTarget> target, int attempts)
{
    PresentOutcome outcome = PresentOutcome::Lost;
    for (int attempt = 0; attempt < attempts; ++attempt) {
        outcome = backend.executeFrame(frame, target);
        if (outcome == PresentOutcome::Presented) {
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(25));
    }
    return outcome;
}

} // namespace

TEST(VkProbeTest, AProbeDeviceBringsUpItsOwnWindowAndPresents)
{
    // THE SDK'S DEFAULT DESC is Probe: a host that names no window lets the device own the surface.
    auto backend = RenderBackendRegistry::instance().create(u8"vk", DeviceDesc{});
    ASSERT_NE(backend, nullptr);

    // WHAT IT ANSWERS BEFORE ANY SESSION EXISTS is the CREATION INTENT (see RenderDevice::capabilities).
    EXPECT_TRUE(backend->capabilities().presentation) << "a Probe device is created to present";

    // THE SDK'S ORDER: the host asks for what it wants presented BEFORE initialize(). A null handle is answered by
    // the device's own window, which is brought up by this call.
    SurfaceTargetResult why = SurfaceTargetResult::Ok;
    const vn::raw_ptr<vn::graphics::WindowPresentTarget> target = backend->createSurfaceTarget(nullptr, &why);
    if (target == nullptr) {
        GTEST_SKIP() << "this machine cannot bring a device-owned window up (result " << static_cast<int>(why) << ")";
    }
    ASSERT_EQ(why, SurfaceTargetResult::Ok);

    if (!backend->initialize()) {
        GTEST_SKIP() << "this machine has no display a Probe window can be brought up on";
    }
    EXPECT_TRUE(backend->capabilities().presentation);

    // THE FRAME REACHES THE PRESENT CALL: the picture is the display server's from here, and the OUTCOME is what
    // this case can measure (see the file note on the pixel half).
    EXPECT_EQ(presentUntilReady(*backend, windowFrame(), target, 40), PresentOutcome::Presented)
        << "a frame through the device's own window did not reach the present call";

    // PREPARING A WINDOW FRAME touches neither the swapchain nor the queue (see RenderFrameExecutor::prepareFrame).
    const PrepareReport prepared = backend->prepareFrame(windowFrame(), target);
    EXPECT_TRUE(prepared.prepared()) << "outcome " << static_cast<int>(prepared.outcome);
    EXPECT_EQ(prepared.passes, 1U);
    EXPECT_EQ(prepared.draws, 1U);

    // A SECOND ASK for the device's window is refused: one window carries one live swapchain, and the host already
    // holds the target the first ask answered (see RenderDevice::createSurfaceTarget).
    const vn::raw_ptr<vn::graphics::WindowPresentTarget> again = backend->createSurfaceTarget(nullptr, &why);
    EXPECT_EQ(again, nullptr);
    EXPECT_EQ(why, SurfaceTargetResult::WindowOccupied);

    // THE SWAPCHAIN FOLLOWS THE SURFACE: a resize is another presenting frame, not a new target.
    target->resize(640, 360);
    EXPECT_EQ(presentUntilReady(*backend, windowFrame(), target, 40), PresentOutcome::Presented);

    EXPECT_EQ(backend->diagnosticCount(vn::graphics::DiagnosticCategory::InitFailed), 0U)
        << "the validation layer reported something on the device-owned window path";

    // A REBUILT SESSION MEANS A REBUILT WINDOW: shutdown() destroyed the window with the surface made on it, so
    // bringing the device up again creates a new one (the only way that teardown is observable from outside). The
    // ask comes BEFORE the session again - the SDK's documented order, and the one that answers the target (a
    // device that already brought its window up answers the second ask with WindowOccupied, see above).
    backend->shutdown();
    const vn::raw_ptr<vn::graphics::WindowPresentTarget> rebuilt = backend->createSurfaceTarget(nullptr, &why);
    ASSERT_NE(rebuilt, nullptr) << "the rebuilt session did not bring its window up (result " << static_cast<int>(why) << ")";
    ASSERT_TRUE(backend->initialize()) << "a Probe device has to come up again after shutdown()";
    EXPECT_EQ(presentUntilReady(*backend, windowFrame(), rebuilt, 40), PresentOutcome::Presented);

    backend->shutdown();
}

TEST(VkProbeTest, AProbeDeviceRefusesAWindowItDidNotMake)
{
    auto backend = RenderBackendRegistry::instance().create(u8"vk", DeviceDesc{});
    ASSERT_NE(backend, nullptr);
    if (!backend->initialize()) {
        GTEST_SKIP() << "this machine has no display a Probe window can be brought up on";
    }

    // A HANDLE THE DEVICE DID NOT CREATE is a surface it cannot serve: a Probe device serves the window it brought
    // up, and a host with a window of its own states that with DevicePresentation::Handle (see DeviceDesc).
    SurfaceTargetResult why = SurfaceTargetResult::Ok;
    EXPECT_EQ(backend->createSurfaceTarget(reinterpret_cast<void*>(static_cast<std::uintptr_t>(0x1234U)), &why), nullptr);
    EXPECT_EQ(why, SurfaceTargetResult::UnsupportedSurface);
    EXPECT_GT(backend->diagnosticCount(vn::graphics::DiagnosticCategory::UnsupportedRequest), 0U)
        << "a host that asked for a window it cannot have has to hear it";

    backend->shutdown();
}
