/**
 * @brief The target lease at the FRAME level: a pass whose target another driver is drawing into is skipped, and the
 *        device says so once per episode (see TargetTable and `.ai/design/graphics-sessions.md` §2.5).
 *
 * WHY THIS FILE EXISTS. `TargetTableTest` pins the state machine; this one pins what the DEVICE does with a lost
 * claim: the pass is skipped, the REST of the frame still runs (a one-pass frame makes the skip observable as "nothing
 * was recorded"), and the sentence arrives ONCE - a second frame that loses the same target is the same episode and is
 * not re-reported. S6's public `createFrameDriver()` is what makes this a two-engine story; until then a lease taken by
 * the test plays the other driver.
 */

#include <gtest/gtest.h>

#include <algorithm>
#include <memory>
#include <vector>

#include <vine/graphics/CameraSnapshot.hpp>
#include <vine/graphics/FrameDescription.hpp>
#include <vine/graphics/Geometry.hpp>
#include <vine/graphics/Primitive.hpp>
#include <vine/graphics/RenderBackend.hpp>
#include <vine/graphics/RenderBackendRegistry.hpp>
#include <vine/graphics/DrawItem.hpp>
#include <vine/graphics/RenderState.hpp>
#include <vine/graphics/RenderTarget.hpp>
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
using vn::graphics::DrawPacket;
using vn::graphics::FrameDescription;
using vn::graphics::Geometry;
using vn::graphics::PassDraw;
using vn::graphics::PresentOutcome;
using vn::graphics::Primitive;
using vn::graphics::RenderBackendRegistry;
using vn::graphics::DrawItem;
using vn::graphics::RenderDiagnostic;
using vn::graphics::RenderTarget;
using vn::graphics::Topology;
using vn::graphics::DrawKind;

constexpr int kTargetSize = 32;

/// @brief A 32x32 RGBA8 + D24 target (its shape is irrelevant here; its IDENTITY is what is claimed).
[[nodiscard]] intrusive_ptr<RenderTarget> makeTarget()
{
    auto target = intrusive_ptr<RenderTarget>(new RenderTarget());
    target->setName(String(u8"vk_claim_target"));
    target->attachColor(RenderTarget::ColorFormat::RGBA8);
    target->attachDepth(RenderTarget::DepthFormat::D24);
    target->setSize(kTargetSize, kTargetSize);
    return target;
}

/// @brief A triangle in clip space; the pass is skipped before it is drawn, so the content is only a valid shape.
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

/// @brief An identity camera, so the triangle's clip-space positions are the picture.
[[nodiscard]] CameraSnapshot identityCamera()
{
    CameraSnapshot camera;
    camera.present    = true;
    camera.view       = vn::math::Mat4d{};
    camera.projection = vn::math::Mat4d{};
    camera.eye        = vn::math::Vec3d( 0.0, 0.0, 0.0 );
    return camera;
}

/// @brief One scene draw with one command (it names a geometry and no program: the pass never gets to draw it).
[[nodiscard]] DrawPacket makeDraw()
{
    auto commands = std::make_shared<std::vector<DrawItem>>();
    commands->push_back(DrawItem(makeTriangle(), nullptr, vn::math::Mat4d{}));

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
    pass.clear        = ClearPolicy{};
    pass.clear->color = Colorf(0.0F, 0.0F, 0.0F, 1.0F);
    pass.packets.push_back(std::move(draw));
    entry.draw = std::move(pass);

    frame.passes.push_back(std::move(entry));
    return frame;
}

/// @brief How many claim-conflict sentences @p reported holds.
[[nodiscard]] std::size_t conflictsIn(const std::vector<RenderDiagnostic>& reported)
{
    return static_cast<std::size_t>(
        std::count_if(reported.begin(), reported.end(), [](const RenderDiagnostic& diagnostic) {
            return diagnostic.code.std_u8str_view() == u8"vk.target.claim_conflict";
        }));
}

} // namespace

TEST(TargetClaimFrameTest, AFrameThatLostItsTargetSkipsThatPassAndSaysSoOncePerEpisode)
{
    auto backend = RenderBackendRegistry::instance().create(u8"vk", DeviceDesc{ DevicePresentation::None, nullptr });
    ASSERT_NE(backend, nullptr);
    if (!backend->initialize()) {
        GTEST_SKIP() << "this machine has no Vulkan device the backend can come up on";
    }
    auto* device = dynamic_cast<vn::backend_vk::Device*>(backend.get());
    ASSERT_NE(device, nullptr);

    std::vector<RenderDiagnostic> reported;
    backend->setDiagnosticSink([&reported](const RenderDiagnostic& diagnostic) { reported.push_back(diagnostic); });

    const intrusive_ptr<RenderTarget> target = makeTarget();

    // THE OTHER DRIVER: a session id the device's own driver does not have, holding the target for a "frame" of its
    // own. Until S6 hands out a second session, this lease IS the second engine.
    auto other = device->targetTable().lease(9999U);
    ASSERT_TRUE(other.claim(target.get()));
    ASSERT_EQ(device->targetTable().holderOf(target.get()), 9999U);

    // THE SAME FRAME TWICE: the pass loses its target both times, and it is SAID once. `refused` counting twice is
    // what separates "reported per episode" from "skipped silently".
    EXPECT_EQ(backend->executeFrame(drawingFrame(target.get(), makeDraw()), nullptr), PresentOutcome::Skipped);
    EXPECT_EQ(backend->executeFrame(drawingFrame(target.get(), makeDraw()), nullptr), PresentOutcome::Skipped);
    EXPECT_EQ(conflictsIn(reported), 1U) << "a driven-out pass was reported once per frame instead of once per episode";
    EXPECT_EQ(device->targetTable().claimNumbers().refused, 2U) << "the second frame did not lose the target";
    EXPECT_EQ(device->targetTable().claimNumbers().live, 1U) << "the other driver's claim vanished";

    // THE OTHER DRIVER FINISHES: its lease releases, the next frame runs its pass, and the episode ends.
    other = vn::backend_vk::TargetTable::Lease{};
    EXPECT_FALSE(device->targetTable().holderOf(target.get()).has_value());
    (void)backend->executeFrame(drawingFrame(target.get(), makeDraw()), nullptr);
    EXPECT_EQ(conflictsIn(reported), 1U);

    // A NEW CONFLICT IS A NEW EPISODE: the same target taken again earns one more sentence, not silence.
    auto again = device->targetTable().lease(9999U);
    ASSERT_TRUE(again.claim(target.get()));
    (void)backend->executeFrame(drawingFrame(target.get(), makeDraw()), nullptr);
    EXPECT_EQ(conflictsIn(reported), 2U) << "the conflict episode never ended after a clean frame";

    again = vn::backend_vk::TargetTable::Lease{};
    backend->shutdown();
}

TEST(TargetClaimFrameTest, APreparationThatLostItsTargetIsSkippedAndReportedToo)
{
    // A WARM-UP BUILDS AND WRITES TARGETS (see Device::prepareFrame), so it takes the same lease. Without this, a
    // warm-up could rebuild a target underneath the frame another driver is drawing into.
    auto backend = RenderBackendRegistry::instance().create(u8"vk", DeviceDesc{ DevicePresentation::None, nullptr });
    ASSERT_NE(backend, nullptr);
    if (!backend->initialize()) {
        GTEST_SKIP() << "this machine has no Vulkan device the backend can come up on";
    }
    auto* device = dynamic_cast<vn::backend_vk::Device*>(backend.get());
    ASSERT_NE(device, nullptr);

    std::vector<RenderDiagnostic> reported;
    backend->setDiagnosticSink([&reported](const RenderDiagnostic& diagnostic) { reported.push_back(diagnostic); });

    const intrusive_ptr<RenderTarget> target = makeTarget();
    const FrameDescription            frame  = drawingFrame(target.get(), makeDraw());

    auto other = device->targetTable().lease(9999U);
    ASSERT_TRUE(other.claim(target.get()));

    const vn::graphics::PrepareReport blocked = backend->prepareFrame(frame);
    EXPECT_EQ(conflictsIn(reported), 1U) << "a preparation that lost its target said nothing";

    other = vn::backend_vk::TargetTable::Lease{};
    const vn::graphics::PrepareReport allowed = backend->prepareFrame(frame);
    EXPECT_EQ(conflictsIn(reported), 1U) << "a clean preparation re-reported the old conflict";

    (void)blocked;
    (void)allowed;
    backend->shutdown();
}
