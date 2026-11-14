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
#include <vine/graphics/RenderCommand.hpp>
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
using vn::graphics::RenderCommand;
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
[[nodiscard]] FrameDescription drawingFrame(RenderTarget* target, std::shared_ptr<const std::vector<RenderCommand>> commands)
{
    FrameDescription frame;
    FrameDescription::Pass entry;
    entry.pass = vn::test::framePassForTest();

    vn::graphics::PassDraw pass;
    pass.target       = target;
    pass.clear        = ClearPolicy{};
    pass.clear->color = Colorf(0.0F, 0.0F, 0.0F, 1.0F);

    vn::graphics::DrawDescription draw;
    draw.kind     = vn::graphics::DrawDescription::Kind::Scene;
    draw.camera   = CameraSnapshot{};
    draw.commands = std::move(commands);
    pass.draws.push_back(std::move(draw));

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

    RenderCommand command(geometry, nullptr, vn::math::Mat4d{});
    command.program              = program;
    command.program_description  = program->description();
    command.material             = material;
    command.material_description = material->description();
    const auto commands = std::make_shared<const std::vector<RenderCommand>>(std::vector<RenderCommand>{ std::move(command) });
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
