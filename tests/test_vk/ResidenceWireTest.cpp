/**
 * @brief The residence table WIRED INTO THE DEVICE: a key another driver is building is copied for this frame and
 *        thrown away, a key this driver won is published when its frame's fence proves the fill, and a released key
 *        leaves the table with its entry (see ResidencyClaims and `.ai/design/graphics-sessions.md` §2.4, §3.1.4).
 *
 * WHY THIS FILE EXISTS. `ResidencyClaimsTest` pins the state machine with two invented drivers. This file pins what the
 * DEVICE does with it (T4): a frame that meets an `Uploading` key still draws its picture from a temporary copy, pays
 * exactly the geometry's two streams for it, and leaves nothing behind; two real sessions first drawing one geometry
 * upload it at most twice (one shared build plus at most one copy) and then draw it from ONE entry; a build the pool
 * refused is withdrawn instead of published, so the key is never left `Uploading` (`P1`) and never reported
 * `Resident` while unusable; and a released geometry is forgotten, so the next frame may build it again.
 *
 * HOW THE OTHER DRIVER IS PLAYED. The deterministic cases hold a claim through `Device::residence()` - the same door
 * `TargetClaimFrameTest` uses for a target lease - so "another driver is in the middle of this key" is a fact the
 * test states rather than a race it hopes for. The two-session case uses two real threads and asserts the bound that
 * holds whatever the interleaving: one claim, one publish, at most one copy.
 */

#include <gtest/gtest.h>

#include <array>
#include <atomic>
#include <cstdint>
#include <memory>
#include <optional>
#include <thread>
#include <vector>

#include <vine/Buffer.hpp>
#include <vine/Colorf.hpp>
#include <vine/String.hpp>
#include <vine/graphics/CameraSnapshot.hpp>
#include <vine/graphics/FrameDescription.hpp>
#include <vine/graphics/Geometry.hpp>
#include <vine/graphics/Primitive.hpp>
#include <vine/graphics/RenderBackendRegistry.hpp>
#include <vine/graphics/DrawItem.hpp>
#include <vine/graphics/RenderFrameExecutor.hpp>
#include <vine/graphics/RenderTarget.hpp>
#include <vine/graphics/ShaderProgram.hpp>
#include <vine/math/Matrix4x4.hpp>

#include <vine/backend_vk/internal/Device.hpp>
#include <vine/backend_vk/internal/MemoryAllocator.hpp>
#include <vine/backend_vk/internal/ResidencyClaims.hpp>

#include "PlanTestSupport.hpp"
namespace
{

using vn::Colorf;
using vn::intrusive_ptr;
using vn::String;
using vn::graphics::AttachmentPolicy;
using vn::graphics::CameraSnapshot;
using vn::graphics::ColorReadback;
using vn::graphics::DeviceDesc;
using vn::graphics::DevicePresentation;
using vn::graphics::DrawItem;
using vn::graphics::DrawKind;
using vn::graphics::DrawPacket;
using vn::graphics::FrameDescription;
using vn::graphics::Geometry;
using vn::graphics::PassDraw;
using vn::graphics::RenderFrameExecutor;
using vn::graphics::RenderTarget;
using vn::graphics::ShaderProgram;
using vn::graphics::ShaderStage;
using vn::graphics::ShaderStageType;
using vn::graphics::Topology;

using vn::backend_vk::MemoryStats;
using vn::backend_vk::ResidencyClaims;

constexpr int kTargetSize = 32;

/// @brief The clip-space triangle every case draws (see ContentDrawTest: z = 0.5 is in front of the reverse-Z far).
///
/// @param vertex_count How many vec3 positions the buffer holds; the first three are the triangle (the index span
///                     reads those, so a bigger stream only makes the upload bigger - see the refused-build case).
[[nodiscard]] intrusive_ptr<Geometry> makeTriangle(const std::size_t vertex_count = 3U)
{
    std::vector<float> positions((vertex_count < 3U ? 3U : vertex_count) * 3U, 0.0F);
    positions[0] = -0.75F;
    positions[1] = -0.75F;
    positions[2] = 0.5F;
    positions[3] = 0.75F;
    positions[4] = -0.75F;
    positions[5] = 0.5F;
    positions[6] = 0.0F;
    positions[7] = 0.75F;
    positions[8] = 0.5F;
    std::vector<std::uint32_t> indices{ 0U, 1U, 2U };

    auto geometry = intrusive_ptr<Geometry>(new Geometry());
    geometry->setPositions(intrusive_ptr<const vn::Buffer<float>>(new vn::Buffer<float>(std::move(positions))));

    vn::graphics::Primitive primitive;
    primitive.topology = Topology::Triangles;
    primitive.indices  = vn::graphics::Primitive::IndexSpan::whole(
        intrusive_ptr<const vn::Buffer<std::uint32_t>>(new vn::Buffer<std::uint32_t>(std::move(indices))));
    geometry->clearPrimitives();
    geometry->addPrimitive(primitive);
    geometry->bumpRevision();
    return geometry;
}

/// @brief A 32x32 RGBA8 target (no depth: these cases are about streams, not about the depth policy).
[[nodiscard]] intrusive_ptr<RenderTarget> makeTarget(const char8_t* name)
{
    auto target = intrusive_ptr<RenderTarget>(new RenderTarget());
    target->setName(String(name));
    target->attachColor(RenderTarget::ColorFormat::RGBA8);
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

/// @brief A program that ignores every block and writes red: the picture says "this frame drew" and nothing else.
[[nodiscard]] intrusive_ptr<ShaderProgram> redProgram()
{
    constexpr const char8_t* kVertex = u8R"(
#version 450
layout(location = 0) in vec3 in_position;
void main()
{
    gl_Position = vec4(in_position, 1.0);
}
)";
    constexpr const char8_t* kFragment = u8R"(
#version 450
layout(location = 0) out vec4 out_color;
void main()
{
    out_color = vec4(1.0, 0.0, 0.0, 1.0);
}
)";

    auto program = intrusive_ptr<ShaderProgram>(new ShaderProgram());
    program->setName(u8"residence_red");
    program->addStage(ShaderStage{ ShaderStageType::Vertex, String(kVertex), String(u8"main") });
    program->addStage(ShaderStage{ ShaderStageType::Fragment, String(kFragment), String(u8"main") });
    return program;
}

/// @brief One scene draw that NAMES its program (no default program, so the frame is self-contained).
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

/// @brief A frame with ONE pass into @p target: a clear, then @p draw when it is engaged.
[[nodiscard]] FrameDescription passFrame(RenderTarget* target, std::optional<DrawPacket> draw)
{
    FrameDescription frame;

    FrameDescription::Pass entry;
    entry.pass  = vn::test::framePassForTest();
    entry.order = 0;

    PassDraw pass;
    pass.target       = target;
    pass.attachment_policy        = AttachmentPolicy{};
    pass.attachment_policy->color = Colorf(0.0F, 0.0F, 0.0F, 1.0F);
    if (draw.has_value()) {
        pass.packets.push_back(std::move(*draw));
    }
    entry.draw = std::move(pass);

    frame.passes.push_back(std::move(entry));
    return frame;
}

[[nodiscard]] FrameDescription clearingFrame(RenderTarget* target)
{
    return passFrame(target, std::nullopt);
}

[[nodiscard]] FrameDescription drawingFrame(RenderTarget* target, DrawPacket draw)
{
    return passFrame(target, std::move(draw));
}

/// @brief The centre pixel as RGBA bytes.
[[nodiscard]] std::array<int, 4> centreOf(const std::vector<std::uint8_t>& pixels)
{
    const std::size_t offset = (static_cast<std::size_t>(kTargetSize / 2) * kTargetSize + kTargetSize / 2) * 4U;
    return { static_cast<int>(pixels[offset]), static_cast<int>(pixels[offset + 1U]), static_cast<int>(pixels[offset + 2U]),
             static_cast<int>(pixels[offset + 3U]) };
}

/// @brief Asserts the centre pixel is the red the program writes.
void expectRed(const std::vector<std::uint8_t>& pixels, const char* what)
{
    const std::array<int, 4> centre = centreOf(pixels);
    EXPECT_GE(centre[0], 254) << what << ": the frame did not draw with the copy it was given";
    EXPECT_LE(centre[1], 1) << what;
    EXPECT_LE(centre[2], 1) << what;
}

} // namespace

TEST(VkResidenceWireTest, AKeyAnotherDriverIsFillingIsCopiedForThisFrameAndDiscarded)
{
    auto backend = vn::graphics::RenderBackendRegistry::instance().create(u8"vk", DeviceDesc{ DevicePresentation::None, nullptr });
    ASSERT_NE(backend, nullptr);
    if (!backend->initialize()) {
        GTEST_SKIP() << "this machine has no Vulkan device the backend can come up on";
    }
    auto* const device = dynamic_cast<vn::backend_vk::Device*>(backend.get());
    ASSERT_NE(device, nullptr) << "the registry hands out the vk backend this case is about";

    const intrusive_ptr<RenderTarget>  target    = makeTarget(u8"residence_copy");
    const intrusive_ptr<ShaderProgram> program   = redProgram();
    const intrusive_ptr<Geometry>      warmup    = makeTriangle();
    const intrusive_ptr<Geometry>      geometry  = makeTriangle();

    // THE RING, THE TARGET AND THE 1x1 STAND-IN ARE NOT THE GEOMETRY'S COST: a first drawing frame builds them, so the
    // frames this case measures pay for the geometry's two streams alone (F4's "at most two uploads" is about those).
    // It also leaves ONE key resident (`warmup`), which is why the numbers below start from a claim and a resident one.
    (void)backend->executeFrame(drawingFrame(target.get(), programDraw(warmup, program)), nullptr);
    ASSERT_EQ(backend->diagnosticCount(vn::graphics::DiagnosticCategory::ContentSkipped), 0U);
    ASSERT_EQ(device->residence().numbers().resident, 1U) << "the warm-up frame published nothing";

    const vn::graphics::GeometryDescriptionPtr description = geometry->description();

    // THE OTHER DRIVER, PLAYED HERE (see the file note): it claimed the key and is still filling it.
    ResidencyClaims::Decision other = device->residence().admit(description.get(), 9999U);
    ASSERT_EQ(other.kind, ResidencyClaims::Decision::Kind::Build) << "the key was not absent: this case would prove nothing";

    // FRAME ONE: the picture must be right although the cache holds no entry for the key - the frame was given a
    // temporary copy, not the other driver's half fill and not a wait for its frame.
    (void)backend->executeFrame(drawingFrame(target.get(), programDraw(geometry, program)), nullptr);
    const ColorReadback read = backend->readColorImage(target.get(), 0);
    ASSERT_TRUE(read.ok());
    expectRed(read.values, "the frame that met an Uploading key");

    ResidencyClaims::Numbers numbers = device->residence().numbers();
    EXPECT_EQ(numbers.copies, 1U) << "the frame did not take the Copy answer";
    EXPECT_EQ(numbers.claims, 2U) << "the frame claimed a key another driver is building (only the warm-up and the test may)";
    EXPECT_EQ(numbers.uploading, 1U) << "the other driver's claim is gone";
    EXPECT_EQ(numbers.resident, 1U) << "the copy was published as if it were the shared entry (only the warm-up's is)";
    EXPECT_EQ(device->residence().stateOf(description.get()), ResidencyClaims::State::Uploading);

    // FRAME TWO, MEASURED: the copy is exactly the geometry's two streams, and NOTHING of it survives the frame.
    const MemoryStats before = device->memoryStats();
    (void)backend->executeFrame(drawingFrame(target.get(), programDraw(geometry, program)), nullptr);
    const MemoryStats after = device->memoryStats();
    EXPECT_EQ(after.allocations - before.allocations, 2U) << "a copied frame uploaded something other than the geometry's two streams";
    EXPECT_EQ(after.used_bytes, before.used_bytes) << "the copy was not discarded: its bytes are still live";
    EXPECT_EQ(device->residence().numbers().copies, 2U) << "the second frame did not copy too";

    // THE OTHER DRIVER ABANDONS ITS FILL: the key returns to Absent (P1's prevention), so the next frame may build it.
    other.claim.withdraw();
    EXPECT_EQ(device->residence().stateOf(description.get()), ResidencyClaims::State::Absent);

    // FRAME THREE: this driver now wins the key and publishes it - the claim survives the frame and becomes Resident
    // when the fence proves the fill complete (not when the head returns).
    (void)backend->executeFrame(drawingFrame(target.get(), programDraw(geometry, program)), nullptr);
    numbers = device->residence().numbers();
    EXPECT_EQ(numbers.claims, 3U) << "the frame that should have won the key did not claim it";
    EXPECT_EQ(numbers.published, 2U) << "the frame that won the key did not publish it";
    EXPECT_EQ(numbers.resident, 2U) << "the published key is not resident beside the warm-up's";
    EXPECT_EQ(numbers.uploading, 0U) << "a published key was left Uploading";
    EXPECT_EQ(device->residence().stateOf(description.get()), ResidencyClaims::State::Resident);
    EXPECT_GT(device->memoryStats().used_bytes, before.used_bytes) << "a published entry's streams must be resident";
    const ColorReadback published_read = backend->readColorImage(target.get(), 0);
    ASSERT_TRUE(published_read.ok());
    expectRed(published_read.values, "the frame that built the shared entry");

    // FRAME FOUR, STEADY: a resident key is USED - nothing is claimed, nothing is copied, nothing is uploaded.
    const MemoryStats steady = device->memoryStats();
    (void)backend->executeFrame(drawingFrame(target.get(), programDraw(geometry, program)), nullptr);
    EXPECT_EQ(device->memoryStats().allocations, steady.allocations) << "a steady frame uploaded the geometry again";
    EXPECT_EQ(device->residence().numbers().copies, 2U);
    EXPECT_EQ(device->residence().numbers().claims, 3U);

    backend->shutdown();
}

TEST(VkResidenceWireTest, TwoDriversFirstDrawingOneGeometryShareOneEntryAndAtMostOneCopy)
{
    // T4's two-session half, with REAL sessions on two threads: whatever the interleaving, exactly one driver wins
    // the key, at most one competitor builds a copy, and both pictures are their own.
    auto backend = vn::graphics::RenderBackendRegistry::instance().create(u8"vk", DeviceDesc{ DevicePresentation::None, nullptr });
    ASSERT_NE(backend, nullptr);
    if (!backend->initialize()) {
        GTEST_SKIP() << "this machine has no Vulkan device the backend can come up on";
    }
    auto* const device = dynamic_cast<vn::backend_vk::Device*>(backend.get());
    ASSERT_NE(device, nullptr);

    RenderFrameExecutor* const second = device->createFrameDriver();
    ASSERT_NE(second, nullptr) << "the device handed out no second driver";

    const intrusive_ptr<ShaderProgram> program  = redProgram();
    const intrusive_ptr<Geometry>      geometry = makeTriangle();
    const intrusive_ptr<RenderTarget>  target_a = makeTarget(u8"residence_race_a");
    const intrusive_ptr<RenderTarget>  target_b = makeTarget(u8"residence_race_b");

    // THE TARGETS COME FIRST: a clear-only pass builds the attachments without drawing, so the drawing frames below
    // are the only frames that can build the geometry.
    (void)backend->executeFrame(clearingFrame(target_a.get()), nullptr);
    (void)second->executeFrame(clearingFrame(target_b.get()), nullptr);

    const FrameDescription draw_a = drawingFrame(target_a.get(), programDraw(geometry, program));
    const FrameDescription draw_b = drawingFrame(target_b.get(), programDraw(geometry, program));

    std::atomic<int>  ready{ 0 };
    std::atomic<bool> go{ false };
    std::thread       first_driver([&]() {
        ready.fetch_add(1);
        while (!go.load()) {
            std::this_thread::yield();
        }
        (void)backend->executeFrame(draw_a, nullptr);
    });
    std::thread other_driver([&]() {
        ready.fetch_add(1);
        while (!go.load()) {
            std::this_thread::yield();
        }
        (void)second->executeFrame(draw_b, nullptr);
    });
    while (ready.load() != 2) {
        std::this_thread::yield();
    }
    go.store(true);
    first_driver.join();
    other_driver.join();

    // BOTH PICTURES ARE RIGHT: a copy and a shared entry are equally valid sources, and neither ever reads the other
    // driver's half fill (that would be a picture that is not red, or a validation fault).
    const ColorReadback first_read = backend->readColorImage(target_a.get(), 0);
    ASSERT_TRUE(first_read.ok());
    expectRed(first_read.values, "the first driver");
    const ColorReadback second_read = backend->readColorImage(target_b.get(), 0);
    ASSERT_TRUE(second_read.ok());
    expectRed(second_read.values, "the second driver");

    // THE BOUND, WHATEVER THE INTERLEAVING: one claim won, one fill published, at most one copy built - so the same
    // geometry was uploaded at most twice, which is T4's acceptance criterion at the device level.
    const ResidencyClaims::Numbers numbers = device->residence().numbers();
    EXPECT_EQ(numbers.claims, 1U) << "two drivers both won the same key";
    EXPECT_EQ(numbers.published, 1U) << "the winning driver did not publish";
    EXPECT_EQ(numbers.resident, 1U);
    EXPECT_EQ(numbers.uploading, 0U) << "a key was left Uploading after both frames completed";
    EXPECT_LE(numbers.copies, 1U) << "more than one temporary copy was built";

    // AND BOTH DRIVERS NOW DRAW FROM THAT ONE ENTRY: a steady frame on each uploads nothing.
    (void)backend->executeFrame(draw_a, nullptr);
    ASSERT_TRUE(backend->frameReport().uploads.has_value()) << "the device keeps no per-frame uploads?";
    EXPECT_EQ(backend->frameReport().uploads->stream_uploads, 0U) << "the first driver uploaded a geometry the device already holds";
    EXPECT_EQ(backend->frameReport().uploads->texture_uploads, 0U);
    (void)second->executeFrame(draw_b, nullptr);
    ASSERT_TRUE(second->frameReport().uploads.has_value());
    EXPECT_EQ(second->frameReport().uploads->stream_uploads, 0U) << "the second driver uploaded a geometry the device already holds";
    EXPECT_EQ(second->frameReport().uploads->texture_uploads, 0U);

    device->releaseFrameDriver(*second);
    backend->shutdown();
}

TEST(VkResidenceWireTest, ARefusedBuildIsWithdrawnInsteadOfPublished)
{
    // P1 AT THE DEVICE: a build the pool refused must return its key to Absent. Publishing it would call a broken
    // entry "resident" (and C4 says only a usable entry may be used); leaving it Uploading would make every driver
    // copy the key forever. The mutation this case kills: publish (or simply keep) the claim of a failed fill.
    auto backend = vn::graphics::RenderBackendRegistry::instance().create(u8"vk", DeviceDesc{ DevicePresentation::None, nullptr });
    ASSERT_NE(backend, nullptr);
    if (!backend->initialize()) {
        GTEST_SKIP() << "this machine has no Vulkan device the backend can come up on";
    }
    auto* const device = dynamic_cast<vn::backend_vk::Device*>(backend.get());
    ASSERT_NE(device, nullptr);

    const intrusive_ptr<ShaderProgram> program = redProgram();
    const intrusive_ptr<RenderTarget>  target  = makeTarget(u8"residence_refused");

    // A GEOMETRY WHOSE STREAMS NEED A NEW POOL BLOCK: `vkAllocateMemory` runs when the pool creates a BLOCK, not when
    // it carves a slice, so the injected refusal has to ask for something the open host-visible block cannot hold
    // (the same lever `VkMemoryFailureTest` uses for a target). 20 MiB is above the pool's 16 MiB first fill.
    constexpr std::size_t         kBigVertexCount = (20U * 1024U * 1024U) / (3U * sizeof(float));
    const intrusive_ptr<Geometry> big_geometry    = makeTriangle(kBigVertexCount);

    // THE RING, THE TARGET AND THE STAND-IN COME FIRST, so the refusal below is the geometry's own and nothing else.
    (void)backend->executeFrame(drawingFrame(target.get(), programDraw(makeTriangle(), program)), nullptr);
    ASSERT_EQ(backend->diagnosticCount(vn::graphics::DiagnosticCategory::ContentSkipped), 0U);

    const vn::graphics::GeometryDescriptionPtr description = big_geometry->description();

    int refusals = 0;
    device->setMemoryBlockCreator([&refusals](VkDevice, const VkMemoryAllocateInfo&, VkDeviceMemory*) {
        ++refusals;
        return VK_ERROR_OUT_OF_DEVICE_MEMORY;
    });
    (void)backend->executeFrame(drawingFrame(target.get(), programDraw(big_geometry, program)), nullptr);
    EXPECT_GT(refusals, 0) << "the injection was never reached: the case proved nothing";

    EXPECT_EQ(device->residence().stateOf(description.get()), ResidencyClaims::State::Absent)
        << "a refused build was left Uploading or published as usable";
    EXPECT_EQ(device->residence().numbers().uploading, 0U) << "a refused build left its key Uploading (P1)";
    EXPECT_EQ(device->residence().numbers().resident, 1U) << "a refused build was published (only the warm-up's key is)";
    EXPECT_EQ(backend->diagnosticCount(vn::graphics::DiagnosticCategory::ContentSkipped), 1U)
        << "the refused stream was not reported to the host";

    // THE HOST LETS GO AND THE POOL RECOVERS: the release forgets the failed entry, and the next frame builds it.
    device->setMemoryBlockCreator({});
    ASSERT_TRUE(backend->releaseContent(big_geometry.get()));
    EXPECT_EQ(device->residence().stateOf(description.get()), ResidencyClaims::State::Absent);
    (void)backend->executeFrame(drawingFrame(target.get(), programDraw(big_geometry, program)), nullptr);
    EXPECT_EQ(device->residence().numbers().published, 2U) << "the rebuilt key was not published beside the warm-up's";
    EXPECT_EQ(device->residence().stateOf(description.get()), ResidencyClaims::State::Resident);
    EXPECT_EQ(device->residence().numbers().uploading, 0U);

    backend->shutdown();
}

TEST(VkResidenceWireTest, AReleasedGeometryIsForgottenSoTheNextFrameMayBuildItAgain)
{
    // F4's release half: `releaseGeometry` forgets the key WITH the entry. Without the forget the table would call a
    // key usable that the cache no longer holds, and the next frame would have to repair what an announcement broke.
    auto backend = vn::graphics::RenderBackendRegistry::instance().create(u8"vk", DeviceDesc{ DevicePresentation::None, nullptr });
    ASSERT_NE(backend, nullptr);
    if (!backend->initialize()) {
        GTEST_SKIP() << "this machine has no Vulkan device the backend can come up on";
    }
    auto* const device = dynamic_cast<vn::backend_vk::Device*>(backend.get());
    ASSERT_NE(device, nullptr);

    const intrusive_ptr<ShaderProgram> program  = redProgram();
    const intrusive_ptr<Geometry>      geometry = makeTriangle();
    const intrusive_ptr<RenderTarget>  target   = makeTarget(u8"residence_release");
    const vn::graphics::GeometryDescriptionPtr description = geometry->description();

    (void)backend->executeFrame(drawingFrame(target.get(), programDraw(geometry, program)), nullptr);
    ASSERT_EQ(device->residence().stateOf(description.get()), ResidencyClaims::State::Resident);
    ASSERT_EQ(device->residence().numbers().published, 1U);

    ASSERT_TRUE(backend->releaseContent(geometry.get()));
    EXPECT_EQ(device->residence().stateOf(description.get()), ResidencyClaims::State::Absent)
        << "the released key stayed in the table";
    EXPECT_EQ(device->residence().numbers().resident, 0U);

    // THE NEXT FRAME CLAIMS AND BUILDS IT AGAIN: the release forgot the key, so the driver may and does win it.
    (void)backend->executeFrame(drawingFrame(target.get(), programDraw(geometry, program)), nullptr);
    const ResidencyClaims::Numbers numbers = device->residence().numbers();
    EXPECT_EQ(numbers.claims, 2U) << "the released key was not claimable again";
    EXPECT_EQ(numbers.published, 2U) << "the rebuilt key was not published";
    EXPECT_EQ(numbers.resident, 1U);

    const ColorReadback read = backend->readColorImage(target.get(), 0);
    ASSERT_TRUE(read.ok());
    expectRed(read.values, "the frame after the release");

    backend->shutdown();
}

TEST(VkResidenceWireTest, AVertexBufferThatAnnouncesANewRevisionIsUploadedAgain)
{
    // THE ANNOUNCEMENT THAT DECIDES A RE-UPLOAD IS THE BUFFER'S, not the geometry's: `Geometry::setRevision` is about the
    // SHAPE (which channels, where each reads) and `Buffer::setRevision` - the `Edit` scope's own announcement - is about
    // the BYTES (see Geometry.hpp's setRevision, GeometryDescription's note, and Buffer's class comment). A host that
    // animates vertices edits the buffer and announces there; the geometry's revision does not move at all.
    //
    // WHAT THIS PINS: the frame after that edit draws the NEW bytes. The cache keys a geometry entry by its DESCRIPTION
    // (identity + geometry revision), so a stream whose buffer announced has to be detached (replaced entry, old buffers
    // parked) and refilled - otherwise a vertex edit would stay invisible, which is the defect this case was written for.
    auto backend = vn::graphics::RenderBackendRegistry::instance().create(u8"vk", DeviceDesc{ DevicePresentation::None, nullptr });
    ASSERT_NE(backend, nullptr);
    if (!backend->initialize()) {
        GTEST_SKIP() << "this machine has no Vulkan device the backend can come up on";
    }
    auto* const device = dynamic_cast<vn::backend_vk::Device*>(backend.get());
    ASSERT_NE(device, nullptr);

    // THE BYTES ARE THE TEST'S OWN HANDLE (the geometry borrows them), which is what lets the case edit them in place.
    auto position_buffer = intrusive_ptr<vn::Buffer<float>>(new vn::Buffer<float>(std::vector<float>{
        -0.75F, -0.75F, 0.5F, 0.75F, -0.75F, 0.5F, 0.0F, 0.75F, 0.5F }));
    auto geometry = intrusive_ptr<Geometry>(new Geometry());
    geometry->setPositions(position_buffer);
    vn::graphics::Primitive primitive;
    primitive.topology = Topology::Triangles;
    primitive.indices  = vn::graphics::Primitive::IndexSpan::whole(
        intrusive_ptr<const vn::Buffer<std::uint32_t>>(new vn::Buffer<std::uint32_t>(std::vector<std::uint32_t>{ 0U, 1U, 2U })));
    geometry->clearPrimitives();
    geometry->addPrimitive(primitive);
    geometry->bumpRevision(); // the SHAPE announcement, once, before any frame

    const intrusive_ptr<ShaderProgram> program = redProgram();
    const intrusive_ptr<RenderTarget>  target  = makeTarget(u8"residence_revision");
    const vn::graphics::GeometryDescriptionPtr description = geometry->description();

    (void)backend->executeFrame(drawingFrame(target.get(), programDraw(geometry, program)), nullptr);
    const ColorReadback before = backend->readColorImage(target.get(), 0);
    ASSERT_TRUE(before.ok());
    expectRed(before.values, "the frame before the edit");

    // THE EDIT: every vertex moves right, out of the target, announced ON THE BUFFER when the scope ends. The geometry
    // revision, the description and the verdict of the residence table all stay what they were.
    {
        auto edit = position_buffer->edit();
        float* const values = edit.data();
        for (std::size_t vertex = 0U; vertex < 3U; ++vertex) {
            values[vertex * 3U] += 2.5F; // x of each vertex: the triangle leaves the centre
        }
    }
    EXPECT_EQ(geometry->revision(), description->revision()) << "a byte edit must not need a shape announcement";

    (void)backend->executeFrame(drawingFrame(target.get(), programDraw(geometry, program)), nullptr);
    const ColorReadback after = backend->readColorImage(target.get(), 0);
    ASSERT_TRUE(after.ok());
    const std::array<int, 4> centre = centreOf(after.values);
    EXPECT_LE(centre[0], 1) << "the frame after the edit still drew the bytes uploaded first (red at the centre)";
    EXPECT_LE(centre[1], 1);
    EXPECT_LE(centre[2], 1);

    // THE ENTRY WAS NOT REBUILT AS A NEW KEY: the edit is a stream replacement inside a resident geometry, so the table
    // keeps one claim and one publish - a second build would mean a byte edit had become a shape change.
    const ResidencyClaims::Numbers numbers = device->residence().numbers();
    EXPECT_EQ(numbers.claims, 1U) << "a byte edit claimed the geometry again";
    EXPECT_EQ(numbers.resident, 1U);
    EXPECT_EQ(device->residence().stateOf(description.get()), ResidencyClaims::State::Resident);

    backend->shutdown();
}

TEST(VkResidenceWireTest, ARefusedInstanceUploadIsRememberedInsteadOfRetriedEveryFrame)
{
    // REVIEW 19 / Z13. `ensureProgram` and `ensureGeometry` remember a refusal; the instance stream's fill did not, so
    // a broken upload paid an allocation, a copy and a refusal on EVERY frame that drew the command - the report was
    // saved by the episode rule, but the WORK was not. The witness is the pool: with block creation refused, a second
    // frame drawing the same command must not reach vkAllocateMemory at all.
    auto backend = vn::graphics::RenderBackendRegistry::instance().create(u8"vk", DeviceDesc{ DevicePresentation::None, nullptr });
    ASSERT_NE(backend, nullptr);
    if (!backend->initialize()) {
        GTEST_SKIP() << "this machine has no Vulkan device the backend can come up on";
    }
    auto* const device = dynamic_cast<vn::backend_vk::Device*>(backend.get());
    ASSERT_NE(device, nullptr);

    const intrusive_ptr<RenderTarget>  target   = makeTarget(u8"instance_refused");
    const intrusive_ptr<ShaderProgram> program  = redProgram();
    const intrusive_ptr<Geometry>      geometry = makeTriangle();

    const auto instancedFrame = [&](const intrusive_ptr<vn::Buffer<float>>& instances, const std::uint32_t instance_count) {
        DrawItem command(geometry, nullptr, vn::math::Mat4d{});
        command.program             = program;
        command.program_description = program->description();
        command.instance_transforms = vn::BufferSlice<float>::whole(instances);
        command.first_instance      = 0U;
        command.instance_count      = instance_count;

        DrawPacket draw;
        draw.kind        = DrawKind::Scene;
        draw.view.camera = identityCamera();
        draw.commands    = std::make_shared<std::vector<DrawItem>>(std::vector<DrawItem>{ std::move(command) });
        return drawingFrame(target.get(), std::move(draw));
    };

    // THE RING, THE TARGET, THE PROGRAM AND THE INSTANCED VARIANT COME FIRST: one small instanced frame builds all of
    // them, so what the frames below measure is the instance stream's own allocation and nothing else.
    const auto warmup = intrusive_ptr<vn::Buffer<float>>(new vn::Buffer<float>(std::vector<float>(16U, 0.0F)));
    (void)backend->executeFrame(instancedFrame(warmup, 1U), nullptr);
    ASSERT_EQ(backend->diagnosticCount(vn::graphics::DiagnosticCategory::ContentSkipped), 0U);

    // A STREAM BIGGER THAN THE POOL'S FIRST FILL, so its upload really asks the device for a block the injected creator
    // can refuse (the same 20 MiB lever the refused-geometry case above uses).
    constexpr std::size_t kBigBytes = 20U * 1024U * 1024U;
    const auto big = intrusive_ptr<vn::Buffer<float>>(new vn::Buffer<float>(std::vector<float>(kBigBytes / sizeof(float), 0.0F)));
    const std::uint32_t big_count = static_cast<std::uint32_t>(kBigBytes / (16U * sizeof(float)));

    int refusals = 0;
    device->setMemoryBlockCreator([&refusals](VkDevice, const VkMemoryAllocateInfo&, VkDeviceMemory*) {
        ++refusals;
        return VK_ERROR_OUT_OF_DEVICE_MEMORY;
    });
    (void)backend->executeFrame(instancedFrame(big, big_count), nullptr);
    EXPECT_GT(refusals, 0) << "the injection was never reached: the case proved nothing";
    const std::size_t reported = backend->diagnosticCount(vn::graphics::DiagnosticCategory::ContentSkipped);
    EXPECT_EQ(reported, 1U) << "the refused instance stream was not reported once";

    const int after_first = refusals;
    (void)backend->executeFrame(instancedFrame(big, big_count), nullptr);
    EXPECT_EQ(refusals, after_first) << "the refused instance upload was retried: the pool was asked for a block again (Z13)";
    EXPECT_EQ(backend->diagnosticCount(vn::graphics::DiagnosticCategory::ContentSkipped), reported)
        << "only the episode rule was doing the work, while the allocation and the copy were repeated";

    // A NEW REVISION IS NEW BYTES, so the refusal describes the old ones and the upload is attempted again.
    big->bumpRevision();
    (void)backend->executeFrame(instancedFrame(big, big_count), nullptr);
    EXPECT_GT(refusals, after_first) << "a new revision kept the previous revision's refusal";

    device->setMemoryBlockCreator({});
    backend->shutdown();
}
