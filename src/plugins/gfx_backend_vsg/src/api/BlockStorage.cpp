#include <vine/vsg/api/BlockStorage.hpp>

#include <algorithm>
#include <cstring>
#include <utility>

#include <vsg/core/Array.h>
#include <vsg/state/Buffer.h>
#include <vsg/vk/DeviceMemory.h>

V_VSG_NS_BEGIN

namespace
{

/// @brief Fallback when the device reports no uniform-buffer alignment (a null physical device).
constexpr std::uint64_t kFallbackAlignment = 256U;

/// @brief Rounds @p value up to a multiple of @p alignment (which must not be zero).
std::uint64_t alignUp(std::uint64_t value, std::uint64_t alignment) noexcept
{
    const std::uint64_t remainder = value % alignment;
    return remainder == 0 ? value : value + (alignment - remainder);
}

/// @brief Gets the alignment a dynamic offset must honour on @p device.
std::uint64_t uniformAlignment(const ::vsg::ref_ptr<::vsg::Device>& device) noexcept
{
    if (auto physical = device != nullptr ? device->getPhysicalDevice() : nullptr) {
        const std::uint64_t reported = physical->getProperties().limits.minUniformBufferOffsetAlignment;
        if (reported != 0) {
            return reported;
        }
    }
    return kFallbackAlignment;
}

}  // namespace

struct BlockStorage::Data
{
    Data(::vsg::ref_ptr<::vsg::Device> device_in, const Layout& layout, std::uint64_t alignment)
      : device(std::move(device_in)),
        view_ring(withAlignment(layout.views, alignment)),
        draw_ring(withAlignment(layout.draws, alignment)),
        light_ring(withAlignment(layout.lights, alignment)),
        shadow_ring(withAlignment(layout.shadows, alignment)),
        arena(withAlignedStride(layout.materials, alignment))
    {
        const std::uint64_t views_bytes   = view_ring.capacityBytes();
        const std::uint64_t draws_base    = alignUp(views_bytes, alignment);
        const std::uint64_t draws_bytes   = draw_ring.capacityBytes();
        const std::uint64_t lights_base   = alignUp(draws_base + draws_bytes, alignment);
        const std::uint64_t lights_bytes  = light_ring.capacityBytes();
        const std::uint64_t shadows_base  = alignUp(lights_base + lights_bytes, alignment);
        const std::uint64_t shadows_bytes = shadow_ring.capacityBytes();
        const std::uint64_t materials_base = alignUp(shadows_base + shadows_bytes, alignment);
        const std::uint64_t materials_bytes = arena.capacityBytes();

        regions.views_base     = 0;
        regions.views_bytes    = views_bytes;
        regions.draws_base     = draws_base;
        regions.draws_bytes    = draws_bytes;
        regions.lights_base    = lights_base;
        regions.lights_bytes   = lights_bytes;
        regions.shadows_base   = shadows_base;
        regions.shadows_bytes  = shadows_bytes;
        regions.materials_base = materials_base;
        regions.materials_bytes = materials_bytes;

        // One buffer for all four regions: they are all "the bytes this frame writes", and a single
        // allocation is what keeps the steady state to zero allocations as well as zero transfers.
        buffer = ::vsg::Buffer::create(materials_base + materials_bytes, VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT,
                                       VK_SHARING_MODE_EXCLUSIVE);
        if (buffer == nullptr) {
            return;
        }
        buffer->compile(device.get());

        // HOST_VISIBLE + HOST_COHERENT: written by the frame's own thread, read by the GPU, visible without
        // a flush. That is what makes the in-place write the whole update mechanism (see the file note).
        memory = ::vsg::DeviceMemory::create(device.get(), buffer->getMemoryRequirements(device->deviceID),
                                             VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                                                 VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
        if (memory == nullptr) {
            return;
        }
        buffer->bind(memory, 0);

        mapped = ::vsg::MappedData<::vsg::ubyteArray>::create(memory.get(), 0, 0u,
                                                              materials_base + materials_bytes);
    }

    /** @brief Buys the ring's layout with the device's alignment (a caller cannot know it). */
    static core::FrameRing::Layout withAlignment(const core::FrameRing::Layout& layout, std::uint64_t alignment)
    {
        core::FrameRing::Layout adjusted = layout;
        adjusted.alignment               = alignment;
        return adjusted;
    }

    /** @brief Buys the arena's block stride with the device's alignment (see @ref withAlignment). */
    static core::MaterialArena::Layout withAlignedStride(const core::MaterialArena::Layout& layout,
                                                         std::uint64_t                    alignment)
    {
        core::MaterialArena::Layout adjusted = layout;
        const std::uint64_t stride = alignUp(layout.block_bytes, alignment);
        adjusted.block_bytes = static_cast<std::uint32_t>(std::min<std::uint64_t>(stride, 0xFFFFFFFFU));
        return adjusted;
    }

    /** @brief Whether the buffer and its mapping exist. */
    [[nodiscard]] bool ready() const noexcept
    {
        return buffer != nullptr && memory != nullptr && mapped != nullptr && mapped->data() != nullptr;
    }

    /** @brief Copies @p block into the mapping at @p offset, if it fits the region's stride. */
    bool writeInto(std::uint64_t offset, std::size_t stride, std::span<const std::byte> block,
                   std::uint64_t& oversized) noexcept
    {
        if (block.size() > stride) {
            ++oversized;
            return false;
        }
        std::memcpy(mapped->data() + offset, block.data(), block.size());
        ++writes;
        bytes_written += block.size();
        return true;
    }

    ::vsg::ref_ptr<::vsg::Device>                 device;
    core::FrameRing                               view_ring;
    core::FrameRing                               draw_ring;
    core::FrameRing                               light_ring;
    core::FrameRing                               shadow_ring;
    core::MaterialArena                           arena;
    ::vsg::ref_ptr<::vsg::Buffer>                 buffer;
    ::vsg::ref_ptr<::vsg::DeviceMemory>           memory;
    ::vsg::ref_ptr<::vsg::MappedData<::vsg::ubyteArray>> mapped;
    Regions                                       regions;
    std::uint64_t                                 writes{0};
    std::uint64_t                                 bytes_written{0};
    std::uint64_t                                 oversized_count{0};
};

BlockStorage::BlockStorage(::vsg::ref_ptr<::vsg::Device> device, const Layout& layout)
{
    // Read the alignment BEFORE the device is moved into the storage: a function-argument evaluation order
    // is unspecified, so an inline uniformAlignment(std::move(device)) could see an empty pointer and lay
    // every region out with the fallback.
    const std::uint64_t alignment = uniformAlignment(device);
    d = std::make_unique<Data>(std::move(device), layout, alignment);
}

std::unique_ptr<BlockStorage> BlockStorage::create(::vsg::ref_ptr<::vsg::Device> device, const Layout& layout)
{
    if (device == nullptr) {
        return nullptr;
    }
    // make_unique cannot see the private constructor; this allocation is the one written inside the class
    // that may name it (the same spelling VsgDrawBlockPool::create uses).
    auto storage = std::unique_ptr<BlockStorage>(new BlockStorage(std::move(device), layout));
    return storage->d->ready() ? std::move(storage) : nullptr;
}

BlockStorage::~BlockStorage() = default;

void BlockStorage::beginFrame() noexcept
{
    d->view_ring.beginFrame();
    d->draw_ring.beginFrame();
    d->light_ring.beginFrame();
    d->shadow_ring.beginFrame();
    d->arena.beginFrame();
}

BlockStorage::Block BlockStorage::writeView(std::span<const std::byte> block) noexcept
{
    // The size is checked before a reservation is taken: an oversized block must not consume a frame's
    // block budget on its way to being refused.
    if (block.size() > d->view_ring.stride()) {
        ++d->oversized_count;
        return {};
    }
    const core::FrameRing::Reservation reservation = d->view_ring.reserve();
    if (!reservation.valid) {
        return {};
    }
    const std::uint64_t offset = d->regions.views_base + reservation.offset;
    if (!d->writeInto(offset, d->view_ring.stride(), block, d->oversized_count)) {
        return {};
    }
    return {true, offset};
}

BlockStorage::Block BlockStorage::writeDraw(std::span<const std::byte> block) noexcept
{
    if (block.size() > d->draw_ring.stride()) {
        ++d->oversized_count;
        return {};
    }
    const core::FrameRing::Reservation reservation = d->draw_ring.reserve();
    if (!reservation.valid) {
        return {};
    }
    const std::uint64_t offset = d->regions.draws_base + reservation.offset;
    if (!d->writeInto(offset, d->draw_ring.stride(), block, d->oversized_count)) {
        return {};
    }
    return {true, offset};
}

BlockStorage::Block BlockStorage::writeLights(std::span<const std::byte> block) noexcept
{
    // The size is checked before a reservation is taken, exactly like the view and draw writes: an oversized block
    // must not consume a frame's budget on its way to being refused.
    if (block.size() > d->light_ring.stride()) {
        ++d->oversized_count;
        return {};
    }
    const core::FrameRing::Reservation reservation = d->light_ring.reserve();
    if (!reservation.valid) {
        return {};
    }
    const std::uint64_t offset = d->regions.lights_base + reservation.offset;
    if (!d->writeInto(offset, d->light_ring.stride(), block, d->oversized_count)) {
        return {};
    }
    return {true, offset};
}

BlockStorage::Block BlockStorage::writeShadows(std::span<const std::byte> block) noexcept
{
    if (block.size() > d->shadow_ring.stride()) {
        ++d->oversized_count;
        return {};
    }
    const core::FrameRing::Reservation reservation = d->shadow_ring.reserve();
    if (!reservation.valid) {
        return {};
    }
    const std::uint64_t offset = d->regions.shadows_base + reservation.offset;
    if (!d->writeInto(offset, d->shadow_ring.stride(), block, d->oversized_count)) {
        return {};
    }
    return {true, offset};
}

BlockStorage::MaterialWrite BlockStorage::writeMaterial(const void* material, std::uint64_t revision,
                                                        std::span<const std::byte> block)
{
    // Same rule as the rings: an oversized block is refused BEFORE the arena records the revision, or the
    // material would be marked as written and its real bytes never reach the buffer.
    if (block.size() > d->arena.blockBytes()) {
        ++d->oversized_count;
        return {};
    }

    const core::MaterialArena::Write write = d->arena.note(material, revision);
    // WHERE THE BLOCK IS, for EVERY answer. On a hit nothing is written, but the bytes the LAST write put
    // there are still the material's (the arena keeps a slot's copies readable for as long as a frame may),
    // so the offset a draw binds is the same one - and answering 0 instead would bind the first block of the
    // storage's buffer, i.e. another region's data: measured as a steady frame shading with a zero material
    // (the frame's second draw of one material read the view region - see BlockStorageTest's steady-frame
    // case, which pins the bytes at that offset).
    const std::uint64_t offset = d->regions.materials_base + d->arena.offsetOf(write.slot, write.copy);
    if (write.kind == core::MaterialArena::WriteKind::Unchanged) {
        return {write.kind, offset, 0};
    }

    if (!d->writeInto(offset, d->arena.blockBytes(), block, d->oversized_count)) {
        return {write.kind, 0, 0};
    }
    return {write.kind, offset, block.size()};
}

BlockStorage::Regions BlockStorage::regions() const noexcept
{
    return d->regions;
}

BlockStorage::Strides BlockStorage::strides() const noexcept
{
    return {d->view_ring.stride(), d->draw_ring.stride(), d->light_ring.stride(), d->shadow_ring.stride(),
            d->arena.blockBytes()};
}

std::span<const std::byte> BlockStorage::bytes() const noexcept
{
    const std::uint8_t* data = d->mapped != nullptr ? d->mapped->data() : nullptr;
    if (data == nullptr) {
        return {};
    }
    return {reinterpret_cast<const std::byte*>(data), d->mapped->size()};
}

::vsg::ref_ptr<::vsg::Buffer> BlockStorage::buffer() const noexcept
{
    return d->buffer;
}

std::uint64_t BlockStorage::capacityBytes() const noexcept
{
    return d->regions.materials_base + d->regions.materials_bytes;
}

std::uint64_t BlockStorage::frames() const noexcept
{
    return d->view_ring.frames();
}

std::uint64_t BlockStorage::writes() const noexcept
{
    return d->writes;
}

std::uint64_t BlockStorage::bytesWritten() const noexcept
{
    return d->bytes_written;
}

std::uint64_t BlockStorage::oversized() const noexcept
{
    return d->oversized_count;
}

std::uint64_t BlockStorage::overflows() const noexcept
{
    return d->view_ring.overflows() + d->draw_ring.overflows() + d->light_ring.overflows() +
           d->shadow_ring.overflows();
}

std::size_t BlockStorage::liveMaterials() const noexcept
{
    return d->arena.live();
}

std::uint64_t BlockStorage::materialWrites() const noexcept
{
    return d->arena.writes();
}

std::uint64_t BlockStorage::materialHits() const noexcept
{
    return d->arena.hits();
}

std::uint64_t BlockStorage::materialEvictions() const noexcept
{
    return d->arena.evictions();
}

V_VSG_NS_END
