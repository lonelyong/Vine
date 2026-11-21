/**
 * @brief The vk backend's DEVICE MEMORY POOL: one block per memory type, slices per resource (see MemoryAllocator).
 *
 * WHY THIS FILE EXISTS. `vkAllocateMemory` is a limited resource - the spec's floor for `maxMemoryAllocationCount` is
 * 4096 - and its failure reads like "the GPU is full" while the GPU may be nearly empty. Before this change every
 * resource (attachment, texture, vertex/index stream, the uniform ring, staging, a readback) created its own
 * `VkDeviceMemory`, so the count scaled with the SCENE. The fix is the concept "device memory is a device-level pool"
 * and a change of that shape is only worth anything if it is pinned: a later edit that goes back to a resource per
 * allocation must fail here, not merely look different in a profile.
 *
 * THE WITNESSES ARE THE BACKEND'S OWN (`Device::memoryStats`), the same arrangement `contentCacheCounters` uses for the
 * recorder's cost model: the test asks the pool what it did instead of counting driver calls from outside.
 *
 * The device cases skip on a machine with no Vulkan device, like every device case in this repository.
 */

#include <gtest/gtest.h>

#include <array>
#include <cstdint>
#include <memory>
#include <string_view>
#include <vector>

#include <vine/Buffer.hpp>
#include <vine/Colorf.hpp>
#include <vine/String.hpp>
#include <vine/graphics/CameraSnapshot.hpp>
#include <vine/graphics/FrameDescription.hpp>
#include <vine/graphics/Geometry.hpp>
#include <vine/graphics/Material.hpp>
#include <vine/graphics/Primitive.hpp>
#include <vine/graphics/RenderBackendRegistry.hpp>
#include <vine/graphics/DrawItem.hpp>
#include <vine/graphics/RenderPass.hpp>
#include <vine/graphics/RenderTarget.hpp>
#include <vine/graphics/ShaderProgram.hpp>
#include <vine/math/Matrix4x4.hpp>

#include <vine/backend_vk/internal/Device.hpp>

#include "PlanTestSupport.hpp"

using vn::Colorf;
using vn::String;
using vn::graphics::CameraSnapshot;
using vn::graphics::ClearPolicy;
using vn::graphics::DeviceDesc;
using vn::graphics::DevicePresentation;
using vn::graphics::FrameDescription;
using vn::graphics::RenderBackendRegistry;
using vn::graphics::DrawItem;
using vn::graphics::RenderTarget;
using vn::intrusive_ptr;

namespace
{

constexpr int kTargetSize = 64;

[[nodiscard]] intrusive_ptr<RenderTarget> makeTarget(const char8_t* name)
{
    auto target = intrusive_ptr<RenderTarget>(new RenderTarget());
    target->setName(String(name));
    target->attachColor(RenderTarget::ColorFormat::RGBA8);
    target->attachDepth(RenderTarget::DepthFormat::D24);
    target->setSize(kTargetSize, kTargetSize);
    return target;
}

/// @brief One frame with one pass clearing @p target and drawing nothing (a clear is a frame a target must exist for).
[[nodiscard]] FrameDescription clearingFrame(RenderTarget* target)
{
    FrameDescription frame;
    FrameDescription::Pass entry;
    entry.pass = vn::test::framePassForTest();

    vn::graphics::PassDraw pass;
    pass.target       = target;
    pass.clear        = ClearPolicy{};
    pass.clear->color = Colorf(0.0F, 0.0F, 0.0F, 1.0F);
    entry.draw        = std::move(pass);
    frame.passes.push_back(std::move(entry));
    return frame;
}

/// @brief One frame with one pass clearing @p target and drawing one textured triangle.
[[nodiscard]] FrameDescription drawingFrame(RenderTarget* target, std::shared_ptr<const std::vector<DrawItem>> commands)
{
    FrameDescription frame;
    FrameDescription::Pass entry;
    entry.pass = vn::test::framePassForTest();

    vn::graphics::PassDraw pass;
    pass.target       = target;
    pass.clear        = ClearPolicy{};
    pass.clear->color = Colorf(0.0F, 0.0F, 0.0F, 1.0F);

    vn::graphics::DrawPacket draw;
    draw.kind     = vn::graphics::DrawPacket::Kind::Scene;
    draw.view.camera   = CameraSnapshot{};
    draw.commands = std::move(commands);
    pass.packets.push_back(std::move(draw));

    entry.draw = std::move(pass);
    frame.passes.push_back(std::move(entry));
    return frame;
}

/// @brief The content ABI's vertex stage (the camera pair is the push, the draw block is set 1 binding 0).
constexpr const char8_t* kVertexStage = u8R"(
#version 450
layout(location = 0) in vec3 in_position;
layout(push_constant) uniform PushConstants
{
    mat4 projection;
    mat4 modelView;
} vn_pc;
layout(set = 1, binding = 0, std140) uniform VineDrawBlock
{
    mat4 model;
    vec4 params;
} vn_draw;
void main()
{
    gl_Position = vn_pc.projection * vn_pc.modelView * vec4(in_position, 1.0);
    gl_PointSize = vn_draw.params.y;
}
)";

/// @brief The content ABI's fragment stage: set 0 binding 0 is the material block.
constexpr const char8_t* kFragmentStage = u8R"(
#version 450
layout(set = 0, binding = 0, std140) uniform VineMaterialBlock
{
    vec4 base_color;
    vec4 emissive;
    vec4 specular;
    vec4 params;
    vec4 extra;
} vn_material;
layout(location = 0) out vec4 out_color;
void main()
{
    out_color = vn_material.base_color;
}
)";

[[nodiscard]] intrusive_ptr<vn::graphics::Geometry> makeTriangle()
{
    std::vector<float> positions{
        -0.75F, -0.75F, 0.5F,
        0.75F,  -0.75F, 0.5F,
        0.0F,   0.75F,  0.5F,
    };
    std::vector<std::uint32_t> indices{ 0U, 1U, 2U };

    auto geometry = intrusive_ptr<vn::graphics::Geometry>(new vn::graphics::Geometry());
    geometry->setPositions(intrusive_ptr<const vn::Buffer<float>>(new vn::Buffer<float>(std::move(positions))));
    vn::graphics::Primitive primitive;
    primitive.topology = vn::graphics::Topology::Triangles;
    primitive.indices  = vn::graphics::Primitive::IndexSpan::whole(
        intrusive_ptr<const vn::Buffer<std::uint32_t>>(new vn::Buffer<std::uint32_t>(std::move(indices))));
    geometry->clearPrimitives();
    geometry->addPrimitive(primitive);
    geometry->bumpRevision();
    return geometry;
}

[[nodiscard]] intrusive_ptr<vn::graphics::ShaderProgram> makeProgram()
{
    auto program = intrusive_ptr<vn::graphics::ShaderProgram>(new vn::graphics::ShaderProgram());
    program->setName(u8"vk_memory_program");
    program->addStage(vn::graphics::ShaderStage{ vn::graphics::ShaderStageType::Vertex, String(kVertexStage), String(u8"main") });
    program->addStage(vn::graphics::ShaderStage{ vn::graphics::ShaderStageType::Fragment, String(kFragmentStage), String(u8"main") });
    return program;
}

/// @brief A target too large for the blocks the pool already has open: serving it needs a NEW block.
///
/// WHY THE SIZE MATTERS. `vkAllocateMemory` runs when the pool creates a BLOCK, not when it carves a slice - so a
/// failure case has to ask for something the open blocks cannot hold. 4096x4096 RGBA8 is 64 MiB, which is the pool's
/// largest first-fill block, and the frame's uniform ring has already taken part of it: the request cannot fit, so the
/// next allocation is a block creation, which is the only moment the injected failure can be produced. (On an
/// integrated device both usages resolve to ONE memory type, so size is the only lever there is.)
[[nodiscard]] intrusive_ptr<RenderTarget> makeTargetNeedingANewBlock(const char8_t* name)
{
    auto target = intrusive_ptr<RenderTarget>(new RenderTarget());
    target->setName(String(name));
    target->attachColor(RenderTarget::ColorFormat::RGBA8);
    target->setSize(4096, 4096);
    return target;
}

} // namespace

TEST(VkMemoryPoolTest, ManyTargetsShareThePoolBlocks)
{
    // TWELVE ATTACHMENTS (six targets, a colour and a depth each) ARE TWELVE SLICES, NOT TWELVE ALLOCATIONS: the pool
    // creates a block when it has none, and every later slice is carved out of what already exists. This is the
    // regression the review's F7 named - one `vkAllocateMemory` per image is what makes `maxMemoryAllocationCount`
    // reachable from a scene that is allowed to grow.
    auto backend = RenderBackendRegistry::instance().create(u8"vk", DeviceDesc{ DevicePresentation::None, nullptr });
    ASSERT_NE(backend, nullptr);
    if (!backend->initialize()) {
        GTEST_SKIP() << "this machine has no Vulkan device the backend can come up on";
    }
    auto* const device = dynamic_cast<vn::backend_vk::Device*>(backend.get());
    ASSERT_NE(device, nullptr) << "the registry hands out the vk backend this case is about";

    const vn::backend_vk::MemoryStats before = device->memoryStats();

    std::vector<intrusive_ptr<RenderTarget>> targets;
    for (int index = 0; index < 6; ++index) {
        targets.push_back(makeTarget(u8"vk_memory_target"));
        (void)backend->executeFrame(clearingFrame(targets.back().get()), nullptr);
    }

    const vn::backend_vk::MemoryStats stats = device->memoryStats();
    EXPECT_GE(stats.allocations - before.allocations, 12U) << "six colour + six depth attachments take twelve slices";
    EXPECT_LE(stats.blocks_live, 2U) << "a scene of six targets created a block per target";
    EXPECT_LT(stats.blocks, stats.allocations) << "blocks did not outnumber slices: the pool is not suballocating";
    EXPECT_GT(stats.used_bytes, 0U);

    // AND NOTHING IS LEFT BEHIND: shutdown releases every slice and every block, which is the other half of "the pool
    // owns the memory" - a leak here would be a block the driver keeps for the process' lifetime.
    backend->shutdown();
    const vn::backend_vk::MemoryStats closed = device->memoryStats();
    EXPECT_EQ(closed.used_bytes, 0U) << "shutdown left slices live";
    EXPECT_EQ(closed.blocks_live, 0U) << "shutdown left blocks allocated";
    EXPECT_EQ(closed.reserved_bytes, 0U);
}

TEST(VkMemoryPoolTest, ASteadyFrameAllocatesNothing)
{
    // THE COST MODEL'S MEMORY HALF: a frame drawn again with the same scene asks the pool for nothing - the ring, the
    // streams, the sets and the attachments are all resident - and the staging buffers the first frame used are back
    // in the pool by the time it answers. An allocation delta across a second frame is the regression "a draw uploads
    // its content again", which the texture/stream caches exist to prevent.
    auto backend = RenderBackendRegistry::instance().create(u8"vk", DeviceDesc{ DevicePresentation::None, nullptr });
    ASSERT_NE(backend, nullptr);
    if (!backend->initialize()) {
        GTEST_SKIP() << "this machine has no Vulkan device the backend can come up on";
    }
    auto* const device = dynamic_cast<vn::backend_vk::Device*>(backend.get());
    ASSERT_NE(device, nullptr);

    const intrusive_ptr<RenderTarget>  target   = makeTarget(u8"vk_memory_steady");
    const intrusive_ptr<vn::graphics::ShaderProgram> program = makeProgram();
    const intrusive_ptr<vn::graphics::Geometry>      geometry = makeTriangle();
    const intrusive_ptr<vn::graphics::Material>      material = intrusive_ptr<vn::graphics::Material>(new vn::graphics::Material());
    material->setBaseColor(Colorf(1.0F, 0.0F, 0.0F, 1.0F));

    DrawItem command(geometry, nullptr, vn::math::Mat4d{});
    command.program              = program;
    command.program_description  = program->description();
    command.material             = material;
    command.material_description = material->description();
    const auto commands = std::make_shared<const std::vector<DrawItem>>(std::vector<DrawItem>{ std::move(command) });
    const FrameDescription frame = drawingFrame(target.get(), commands);

    (void)backend->executeFrame(frame, nullptr);
    const vn::backend_vk::MemoryStats first = device->memoryStats();

    (void)backend->executeFrame(frame, nullptr);
    const vn::backend_vk::MemoryStats steady = device->memoryStats();

    EXPECT_EQ(steady.allocations, first.allocations) << "a steady frame asked the pool for memory";
    EXPECT_EQ(steady.used_bytes, first.used_bytes) << "a steady frame left the pool holding different bytes";
    EXPECT_EQ(steady.blocks_live, first.blocks_live) << "a steady frame grew the pool";

    backend->shutdown();
}

TEST(VkMemoryPoolTest, AReadbackGivesItsStagingBytesBack)
{
    // A READBACK IS A SLICE THAT LIVES FOR ONE COPY: the staging buffer is created, mapped, read and released by the
    // time `readColorBuffer` answers. Five reads must therefore leave the pool exactly where the first one found it -
    // the bytes and the block count say so.
    auto backend = RenderBackendRegistry::instance().create(u8"vk", DeviceDesc{ DevicePresentation::None, nullptr });
    ASSERT_NE(backend, nullptr);
    if (!backend->initialize()) {
        GTEST_SKIP() << "this machine has no Vulkan device the backend can come up on";
    }
    auto* const device = dynamic_cast<vn::backend_vk::Device*>(backend.get());
    ASSERT_NE(device, nullptr);

    const intrusive_ptr<RenderTarget> target = makeTarget(u8"vk_memory_readback");
    (void)backend->executeFrame(clearingFrame(target.get()), nullptr);

    std::vector<std::uint8_t> pixels;
    ASSERT_TRUE(backend->readColorBuffer(target.get(), 0, pixels));
    const vn::backend_vk::MemoryStats first = device->memoryStats();
    EXPECT_GT(first.releases, 0U) << "a readback's staging slice was never released";

    for (int read = 0; read < 4; ++read) {
        pixels.clear();
        ASSERT_TRUE(backend->readColorBuffer(target.get(), 0, pixels));
        const vn::backend_vk::MemoryStats stats = device->memoryStats();
        EXPECT_EQ(stats.used_bytes, first.used_bytes) << "a readback left bytes in the pool";
        EXPECT_EQ(stats.blocks_live, first.blocks_live) << "a readback grew the pool";
        EXPECT_EQ(stats.reserved_bytes, first.reserved_bytes) << "a readback reserved more memory";
    }

    backend->shutdown();
}

namespace
{

/// @brief A device to open an allocator against: the case skips (never fails) on a machine with no Vulkan device.
struct AllocationDevice {
    vn::intrusive_ptr<vn::graphics::RenderBackend> backend;
    vn::backend_vk::MemoryAllocator               allocator;
    bool                                          ready{ false };

    AllocationDevice()
    {
        backend = RenderBackendRegistry::instance().create(u8"vk", DeviceDesc{ DevicePresentation::None, nullptr });
        if (backend == nullptr || !backend->initialize()) {
            return;
        }
        auto* const device = dynamic_cast<vn::backend_vk::Device*>(backend.get());
        if (device == nullptr) {
            return;
        }
        handles = device->handles();
        if (handles.device == VK_NULL_HANDLE || handles.physical == VK_NULL_HANDLE) {
            return;
        }
        volkLoadDeviceTable(&table, handles.device);
        vkGetPhysicalDeviceMemoryProperties(handles.physical, &memory);
        ready = allocator.open(table, handles.device, memory, kGranularity, vn::backend_vk::MemoryBlockSizes{ kBlockBytes, kBlockBytes });
    }

    ~AllocationDevice()
    {
        allocator.close();
        if (backend != nullptr) {
            backend->shutdown();
        }
    }

    /// @brief A requirement set every memory type of this device can serve (the caller states only size and alignment).
    [[nodiscard]] static VkMemoryRequirements requirement(const VkDeviceSize size, const VkDeviceSize alignment) noexcept
    {
        VkMemoryRequirements requirements{};
        requirements.size           = size;
        requirements.alignment      = alignment;
        requirements.memoryTypeBits = ~0U;
        return requirements;
    }

    static constexpr VkDeviceSize kGranularity = 64U * 1024U;
    static constexpr VkDeviceSize kBlockBytes  = 1U * 1024U * 1024U;

    VolkDeviceTable                  table{};
    VkPhysicalDeviceMemoryProperties memory{};
    vn::backend_vk::Handles          handles{};
};

} // namespace

TEST(VkMemoryAllocatorTest, FreeNeighboursMergeSoThePoolIsReused)
{
    // COALESCING IS WHAT KEEPS A LONG-LIVED SCENE'S BLOCKS USABLE: four quarter-blocks are released two at a time, and
    // the half-block that follows fits exactly the range they gave back. Without the merge the two free quarters stay
    // separate, the request does not fit, and the pool creates a SECOND block - which is what this case measures.
    AllocationDevice fixture;
    if (!fixture.ready) {
        GTEST_SKIP() << "this machine has no Vulkan device the allocator can be opened on";
    }

    constexpr VkDeviceSize kQuarter = AllocationDevice::kBlockBytes / 4U;
    std::array<vn::backend_vk::MemorySlice, 4> quarters{};
    for (vn::backend_vk::MemorySlice& slice : quarters) {
        slice = fixture.allocator.allocate(AllocationDevice::requirement(kQuarter, 256U),
                                           vn::backend_vk::MemoryUsage::DeviceLocal,
                                           vn::backend_vk::MemoryKind::Linear);
        ASSERT_TRUE(slice.valid()) << "a quarter of the block did not fit an empty block";
    }
    EXPECT_EQ(fixture.allocator.stats().blocks, 1U) << "four quarters of a block needed more than one block";

    fixture.allocator.release(quarters[0]);
    fixture.allocator.release(quarters[1]);
    EXPECT_FALSE(quarters[0].valid()) << "release did not clear the slice it took";

    vn::backend_vk::MemorySlice half = fixture.allocator.allocate(AllocationDevice::requirement(kQuarter * 2U, 256U),
                                                                 vn::backend_vk::MemoryUsage::DeviceLocal,
                                                                 vn::backend_vk::MemoryKind::Linear);
    ASSERT_TRUE(half.valid()) << "the two released quarters did not merge back into one range";
    EXPECT_EQ(fixture.allocator.stats().blocks, 1U) << "a merged range still needed a new block";
    EXPECT_EQ(half.offset, 0U) << "the merged range was not the one the first two quarters gave back";

    // Everything the case took goes back: the pool's used bytes are its own statement that nothing leaked.
    fixture.allocator.release(half);
    fixture.allocator.release(quarters[2]);
    fixture.allocator.release(quarters[3]);
    EXPECT_EQ(fixture.allocator.stats().used_bytes, 0U) << "released slices still count as used";
}

TEST(VkMemoryAllocatorTest, LinearAndOptimalDoNotShareAGranularityPage)
{
    // THE DEVICE'S OWN RULE (`bufferImageGranularity`): a buffer and an optimally tiled image may not share one
    // granularity-sized page of the same `VkDeviceMemory`. The first allocation is deliberately SMALLER than one page,
    // so without the rule the second would start right after it (1 KiB) instead of at the next page boundary - and
    // every real driver answers that placement with a validation error at bind time.
    AllocationDevice fixture;
    if (!fixture.ready) {
        GTEST_SKIP() << "this machine has no Vulkan device the allocator can be opened on";
    }

    vn::backend_vk::MemorySlice linear = fixture.allocator.allocate(AllocationDevice::requirement(1000U, 256U),
                                                                          vn::backend_vk::MemoryUsage::DeviceLocal,
                                                                          vn::backend_vk::MemoryKind::Linear);
    ASSERT_TRUE(linear.valid());
    EXPECT_EQ(linear.offset, 0U) << "the first allocation did not start at the block's beginning";

    vn::backend_vk::MemorySlice optimal = fixture.allocator.allocate(AllocationDevice::requirement(1000U, 256U),
                                                                           vn::backend_vk::MemoryUsage::DeviceLocal,
                                                                           vn::backend_vk::MemoryKind::Optimal);
    ASSERT_TRUE(optimal.valid());
    EXPECT_GE(optimal.offset, AllocationDevice::kGranularity)
        << "an optimally tiled image was placed in the page a buffer occupies";

    // ... AND A BUFFER AFTER IT MAY REUSE THE GAP the rule left BELOW the image: the rule constrains pages, not
    // ordering, so what must hold is that no page holds one of each kind - not that the buffer sits after the image.
    vn::backend_vk::MemorySlice buffer = fixture.allocator.allocate(AllocationDevice::requirement(1000U, 256U),
                                                                    vn::backend_vk::MemoryUsage::DeviceLocal,
                                                                    vn::backend_vk::MemoryKind::Linear);
    ASSERT_TRUE(buffer.valid());
    const VkDeviceSize granularity = AllocationDevice::kGranularity;
    const auto         round_up    = [granularity](const VkDeviceSize value) { return ((value + granularity - 1U) / granularity) * granularity; };
    const bool before_the_image = round_up(buffer.offset + 1000U) <= optimal.offset;
    const bool after_the_image  = buffer.offset >= round_up(optimal.offset + 1000U);
    EXPECT_TRUE(before_the_image || after_the_image) << "a buffer shared a granularity page with an optimally tiled image";

    fixture.allocator.release(linear);
    fixture.allocator.release(optimal);
    fixture.allocator.release(buffer);
    EXPECT_EQ(fixture.allocator.stats().used_bytes, 0U);
}

TEST(VkMemoryAllocatorTest, ARefusedBlockSaysWhichKindOfFailureItIs)
{
    // THE DEFECT THIS PINS (audit A1): `vkAllocateMemory`'s answer used to be collapsed into one "allocation failed", so
    // a LOST DEVICE was reported as an exhausted heap and the session kept running on a dead device. The four classes
    // below are four different repairs - rebuild the session, release content, escalate, fix the wiring - so they are
    // four answers, and the driver's own result travels with the report instead of being discarded.
    AllocationDevice fixture;
    if (!fixture.ready) {
        GTEST_SKIP() << "this machine has no Vulkan device the allocator can be opened on";
    }

    std::vector<vn::backend_vk::MemoryRefusalReport> reports;
    fixture.allocator.setFailureSink([&reports](const vn::backend_vk::MemoryRefusalReport& report) { reports.push_back(report); });

    const auto refuse_with = [&fixture](const VkResult result) {
        fixture.allocator.setBlockCreator([result](VkDevice, const VkMemoryAllocateInfo&, VkDeviceMemory*) { return result; });
    };
    const auto ask = [&fixture](vn::backend_vk::MemoryRefusal& refusal) {
        return fixture.allocator.allocate(AllocationDevice::requirement(4096U, 256U),
                                          vn::backend_vk::MemoryUsage::DeviceLocal,
                                          vn::backend_vk::MemoryKind::Linear,
                                          &refusal);
    };

    vn::backend_vk::MemoryRefusal refusal = vn::backend_vk::MemoryRefusal::Ok;

    refuse_with(VK_ERROR_DEVICE_LOST);
    EXPECT_FALSE(ask(refusal).valid());
    EXPECT_EQ(refusal, vn::backend_vk::MemoryRefusal::DeviceLost) << "a lost device is not an exhausted heap";
    ASSERT_FALSE(reports.empty());
    EXPECT_EQ(reports.back().driver_result, VK_ERROR_DEVICE_LOST) << "the driver's own answer was not carried";
    EXPECT_EQ(reports.back().requested_bytes, 4096U) << "the report did not say what was asked for";

    refuse_with(VK_ERROR_OUT_OF_DEVICE_MEMORY);
    EXPECT_FALSE(ask(refusal).valid());
    EXPECT_EQ(refusal, vn::backend_vk::MemoryRefusal::OutOfMemory);
    EXPECT_EQ(reports.back().driver_result, VK_ERROR_OUT_OF_DEVICE_MEMORY);

    refuse_with(VK_ERROR_TOO_MANY_OBJECTS);
    EXPECT_FALSE(ask(refusal).valid());
    EXPECT_EQ(refusal, vn::backend_vk::MemoryRefusal::DriverRefused)
        << "a device limit is not memory pressure: nothing a host releases would help";

    // A WORKING CREATOR IS REPORTED TOO, AND THAT IS THE POINT: the `Ok` report is how the reporting layer learns that
    // the condition ended, so its episodes re-arm (the repository's ReportOnce rule).
    fixture.allocator.setBlockCreator({});
    vn::backend_vk::MemorySlice slice = ask(refusal);
    ASSERT_TRUE(slice.valid()) << "the pool did not recover after the injection was cleared";
    EXPECT_EQ(refusal, vn::backend_vk::MemoryRefusal::Ok);
    EXPECT_EQ(reports.back().refusal, vn::backend_vk::MemoryRefusal::Ok) << "a success was not reported to the sink";
    fixture.allocator.release(slice);
}

TEST(VkMemoryAllocatorTest, ARefusalSaysHowMuchRoomThePoolStillHas)
{
    // THE ANSWER AN "OUT OF MEMORY" REPORT NEEDS (audit A2): a heap that refuses a NEW block while the pool holds free
    // bytes has room too SHREDDED to use, and "256 KiB free, largest run 128 KiB" tells a host that releasing one big
    // resource helps while releasing small ones may not. Without those numbers the report says only "out of memory",
    // which is the answer a host cannot act on.
    AllocationDevice fixture;
    if (!fixture.ready) {
        GTEST_SKIP() << "this machine has no Vulkan device the allocator can be opened on";
    }

    std::vector<vn::backend_vk::MemoryRefusalReport> reports;
    fixture.allocator.setFailureSink([&reports](const vn::backend_vk::MemoryRefusalReport& report) { reports.push_back(report); });

    // FRAGMENT THE FIRST BLOCK: eight slices fill it exactly, and the two given back have LIVE neighbours - so the two
    // free runs cannot merge, and there is no tail for a request to fall into (which would make this case prove
    // nothing).
    constexpr VkDeviceSize kSlice = 128U * 1024U;
    std::array<vn::backend_vk::MemorySlice, 8> slices{};
    for (vn::backend_vk::MemorySlice& slice : slices) {
        slice = fixture.allocator.allocate(AllocationDevice::requirement(kSlice, 256U),
                                           vn::backend_vk::MemoryUsage::DeviceLocal,
                                           vn::backend_vk::MemoryKind::Linear);
        ASSERT_TRUE(slice.valid()) << "the fixture's block did not hold eight slices";
    }
    fixture.allocator.release(slices[1]);
    fixture.allocator.release(slices[3]);

    const vn::backend_vk::MemoryStats fragmented = fixture.allocator.stats();
    EXPECT_GE(fragmented.free_bytes, 2U * kSlice) << "two slices were given back and the pool does not count them";
    EXPECT_LT(fragmented.largest_free_run, 2U * kSlice) << "the free runs merged: the case did not fragment anything";

    // A BLOCK THAT CANNOT BE CREATED, for a request no single free run holds: the refusal must carry the room.
    fixture.allocator.setBlockCreator([](VkDevice, const VkMemoryAllocateInfo&, VkDeviceMemory*) {
        return VK_ERROR_OUT_OF_DEVICE_MEMORY;
    });
    vn::backend_vk::MemoryRefusal refusal = vn::backend_vk::MemoryRefusal::Ok;
    const vn::backend_vk::MemorySlice refused =
        fixture.allocator.allocate(AllocationDevice::requirement(200U * 1024U, 256U),
                                   vn::backend_vk::MemoryUsage::DeviceLocal,
                                   vn::backend_vk::MemoryKind::Linear,
                                   &refusal);
    EXPECT_FALSE(refused.valid());
    EXPECT_EQ(refusal, vn::backend_vk::MemoryRefusal::OutOfMemory);
    ASSERT_FALSE(reports.empty());
    EXPECT_GE(reports.back().stats.free_bytes, 2U * kSlice) << "the refusal did not say how much room the pool has";
    EXPECT_LT(reports.back().stats.largest_free_run, 200U * 1024U)
        << "the refusal did not say that no single run holds the request";

    fixture.allocator.setBlockCreator({});
    for (const std::size_t index : { 0U, 2U, 4U, 5U, 6U, 7U }) {
        fixture.allocator.release(slices[index]);
    }
    EXPECT_EQ(fixture.allocator.stats().used_bytes, 0U);
}

TEST(VkMemoryAllocatorTest, AllocationBeforeOpenIsAWiringDefectNotOutOfMemory)
{
    // A CLOSED POOL IS A BUG IN THIS BACKEND, NOT A FULL DEVICE. Saying "out of memory" here sends a host looking for
    // content to release when what it has is a session that was never opened - which is why the refusal is its own
    // class and needs no Vulkan device to be checked.
    vn::backend_vk::MemoryAllocator allocator;

    VkMemoryRequirements requirements{};
    requirements.size           = 4096U;
    requirements.alignment      = 256U;
    requirements.memoryTypeBits = ~0U;

    vn::backend_vk::MemoryRefusal refusal = vn::backend_vk::MemoryRefusal::Ok;
    const vn::backend_vk::MemorySlice slice = allocator.allocate(requirements,
                                                                 vn::backend_vk::MemoryUsage::DeviceLocal,
                                                                 vn::backend_vk::MemoryKind::Linear,
                                                                 &refusal);
    EXPECT_FALSE(slice.valid());
    EXPECT_EQ(refusal, vn::backend_vk::MemoryRefusal::NotOpen);
}

TEST(VkMemoryFailureTest, AnExhaustedHeapIsOneReportWithItsNumbersAndTheSessionSurvives)
{
    // THE HALF OF A1 A HOST ACTS ON: an exhausted heap must NOT look like a lost device (the session survives), and the
    // sentence must carry what a host needs to decide - what was asked, which memory type, the driver's own result, and
    // what the pool holds. "Device memory could not be allocated" is the report this case exists to make impossible.
    auto backend = RenderBackendRegistry::instance().create(u8"vk", DeviceDesc{ DevicePresentation::None, nullptr });
    ASSERT_NE(backend, nullptr);
    if (!backend->initialize()) {
        GTEST_SKIP() << "this machine has no Vulkan device the backend can come up on";
    }
    auto* const device = dynamic_cast<vn::backend_vk::Device*>(backend.get());
    ASSERT_NE(device, nullptr);

    std::vector<vn::graphics::RenderDiagnostic> reported;
    backend->setDiagnosticSink([&reported](const vn::graphics::RenderDiagnostic& diagnostic) { reported.push_back(diagnostic); });

    int refusals = 0;
    device->setMemoryBlockCreator([&refusals](VkDevice, const VkMemoryAllocateInfo&, VkDeviceMemory*) {
        ++refusals;
        return VK_ERROR_OUT_OF_DEVICE_MEMORY;
    });

    const intrusive_ptr<RenderTarget> target = makeTargetNeedingANewBlock(u8"vk_memory_exhausted");
    const vn::backend_vk::MemoryStats before = device->memoryStats();
    (void)backend->executeFrame(clearingFrame(target.get()), nullptr);

    EXPECT_GT(refusals, 0) << "the injection was never reached: the case proved nothing";
    EXPECT_TRUE(backend->initialized()) << "an exhausted heap is not a lost device: the session must survive";
    EXPECT_EQ(device->memoryStats().blocks_live, before.blocks_live) << "a refused block was counted as a live one";
    EXPECT_EQ(device->memoryStats().blocks, before.blocks) << "the refused block was counted at all";

    bool saw_reason = false;
    for (const vn::graphics::RenderDiagnostic& diagnostic : reported) {
        if (diagnostic.category != vn::graphics::DiagnosticCategory::ResourcePressure) {
            continue;
        }
        saw_reason = true;
        const std::u8string_view text = diagnostic.message.std_u8str_view();
        EXPECT_TRUE(diagnostic.code.std_u8str_view() == u8"vk.memory.out_of_memory")
            << "the report did not name its cause by code (a host matches on this, not on the sentence)";
        EXPECT_NE(text.find(u8" MiB"), std::u8string_view::npos) << "the sentence did not name the request";
        EXPECT_NE(text.find(u8"VK_ERROR_OUT_OF_DEVICE_MEMORY"), std::u8string_view::npos)
            << "the sentence did not name the driver's own answer";
        EXPECT_NE(text.find(u8"reserved"), std::u8string_view::npos) << "the sentence did not say what the pool holds";
    }
    EXPECT_TRUE(saw_reason) << "the allocation failure was not reported as memory pressure";

    // AND THE POOL RECOVERS: with the injection gone the next frame builds the target, which is the difference between
    // "this frame lost a resource" and "the session is over".
    device->setMemoryBlockCreator({});
    (void)backend->executeFrame(clearingFrame(target.get()), nullptr);
    EXPECT_GT(device->memoryStats().blocks, before.blocks) << "the pool did not recover after the injection was cleared";

    // A SECOND EPISODE SPEAKS AGAIN: the successful allocation re-armed the episode, so the same failure later is a new
    // fact rather than a silent one (the ReportOnce rule - an episode ends where its condition ends).
    reported.clear();
    device->setMemoryBlockCreator([](VkDevice, const VkMemoryAllocateInfo&, VkDeviceMemory*) {
        return VK_ERROR_OUT_OF_DEVICE_MEMORY;
    });
    const intrusive_ptr<RenderTarget> second = makeTargetNeedingANewBlock(u8"vk_memory_exhausted_again");
    (void)backend->executeFrame(clearingFrame(second.get()), nullptr);
    bool saw_second = false;
    for (const vn::graphics::RenderDiagnostic& diagnostic : reported) {
        saw_second = saw_second || diagnostic.category == vn::graphics::DiagnosticCategory::ResourcePressure;
    }
    EXPECT_TRUE(saw_second) << "the episode was not re-armed by the successful allocation";

    backend->shutdown();
}

TEST(VkMemoryFailureTest, ATargetRefusalCarriesItsCodeItsSubjectAndTheLimit)
{
    // A HOST ACTS ON THE VALUE, NOT ON THE SENTENCE (audit A3): the CODE says which refusal it is, the SUBJECT says
    // which target, and the LIMIT says what to ask for instead. A target asking for a sample count that is not one the
    // device offers is the deterministic case - every device refuses 3, whatever its limit is.
    auto backend = RenderBackendRegistry::instance().create(u8"vk", DeviceDesc{ DevicePresentation::None, nullptr });
    ASSERT_NE(backend, nullptr);
    if (!backend->initialize()) {
        GTEST_SKIP() << "this machine has no Vulkan device the backend can come up on";
    }

    std::vector<vn::graphics::RenderDiagnostic> reported;
    backend->setDiagnosticSink([&reported](const vn::graphics::RenderDiagnostic& diagnostic) { reported.push_back(diagnostic); });

    const intrusive_ptr<RenderTarget> target = makeTarget(u8"vk_coded_target");
    target->setSamples(3U);
    (void)backend->executeFrame(clearingFrame(target.get()), nullptr);

    bool saw_refusal = false;
    for (const vn::graphics::RenderDiagnostic& diagnostic : reported) {
        if (diagnostic.code.std_u8str_view() != u8"vk.target.samples_not_a_count") {
            continue;
        }
        saw_refusal = true;
        EXPECT_TRUE(diagnostic.subject.std_u8str_view() == u8"vk_coded_target")
            << "the report did not name the target it is about";
        ASSERT_TRUE(diagnostic.limit.has_value()) << "a limit refusal without the pair a host acts on";
        EXPECT_EQ(diagnostic.limit->requested, 3U);
        // `supported` is the LARGEST count the device offers (the counts are powers of two), which is what the
        // sentence says - so the pair reads "you asked for 3; this device offers powers of two up to 8".
        EXPECT_GE(diagnostic.limit->supported, 1U) << "the pair must name what the device does offer";
        EXPECT_EQ(diagnostic.limit->supported & (diagnostic.limit->supported - 1U), 0U)
            << "the offered count is a power of two";
    }
    EXPECT_TRUE(saw_refusal) << "the sample-count refusal did not arrive with its stable code";

    backend->shutdown();
}

TEST(VkMemoryFailureTest, ALostDeviceDuringAnAllocationEndsTheSession)
{
    // THE OTHER HALF OF A1: the same call answering VK_ERROR_DEVICE_LOST is the session's end, and it must take the
    // SAME path a lost submit does - `initialized()` stops answering true, later frames are skipped without touching
    // the device, and the loss is said ONCE (the session is the episode, not each allocation that sees it).
    auto backend = RenderBackendRegistry::instance().create(u8"vk", DeviceDesc{ DevicePresentation::None, nullptr });
    ASSERT_NE(backend, nullptr);
    if (!backend->initialize()) {
        GTEST_SKIP() << "this machine has no Vulkan device the backend can come up on";
    }
    auto* const device = dynamic_cast<vn::backend_vk::Device*>(backend.get());
    ASSERT_NE(device, nullptr);

    int refusals = 0;
    device->setMemoryBlockCreator([&refusals](VkDevice, const VkMemoryAllocateInfo&, VkDeviceMemory*) {
        ++refusals;
        return VK_ERROR_DEVICE_LOST;
    });

    const intrusive_ptr<RenderTarget> target = makeTargetNeedingANewBlock(u8"vk_memory_lost");
    (void)backend->executeFrame(clearingFrame(target.get()), nullptr);

    EXPECT_GT(refusals, 0) << "the injection was never reached: the case proved nothing";
    EXPECT_FALSE(backend->initialized()) << "a lost device must end the session wherever it is seen";
    EXPECT_EQ(backend->diagnosticCount(vn::graphics::DiagnosticCategory::SubmissionFailed), 1U)
        << "the loss was reported once per allocation instead of once per session";

    // THE SESSION IS OVER, SO A LATER FRAME MUST NOT TOUCH THE DEVICE - and shutdown still returns it to a fresh state.
    (void)backend->executeFrame(clearingFrame(target.get()), nullptr);
    backend->shutdown();
    EXPECT_EQ(device->memoryStats().blocks_live, 0U) << "shutdown after a loss left blocks behind";
}
