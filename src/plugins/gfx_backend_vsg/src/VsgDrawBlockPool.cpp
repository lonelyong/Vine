#include <vine/vsg/VsgDrawBlockPool.hpp>

#include <cstddef>
#include <cstring>
#include <utility>

#include <vsg/state/Buffer.h>
#include <vsg/state/BufferInfo.h>
#include <vsg/state/DescriptorBuffer.h>
#include <vsg/vk/DeviceMemory.h>

#include <vine/graphics/ShaderAbi.hpp>

V_VSG_NS_BEGIN

namespace
{

/** @brief Rounds @p value up to the next multiple of @p alignment (a power of two). */
constexpr std::uint32_t alignUp(std::uint32_t value, std::uint32_t alignment) noexcept
{
    return alignment <= 1u ? value : ((value + alignment - 1u) / alignment) * alignment;
}

} // namespace

/** @brief One chunk: the buffer its slots live in, the mapping onto it and its free list. */
struct VsgDrawBlockPool::Chunk
{
    ::vsg::ref_ptr<::vsg::Buffer>                       buffer;
    ::vsg::ref_ptr<::vsg::DeviceMemory>                 memory;
    ::vsg::ref_ptr<::vsg::MappedData<::vsg::ubyteArray>> mapped;
    std::vector<std::uint32_t>                          free_indices; ///< LIFO of unhanded-out slots.
    // One descriptor set per set layout that has bound this chunk. A set binds the buffer
    // once with `range` = one block; the slot is chosen by the dynamic offset, so every
    // drawable in the chunk shares these sets.
    std::vector<std::pair<::vsg::ref_ptr<::vsg::DescriptorSetLayout>, ::vsg::ref_ptr<::vsg::DescriptorSet>>> sets;
};

VsgDrawBlockPool::VsgDrawBlockPool(::vsg::ref_ptr<::vsg::Device> device, std::uint32_t slots_per_chunk) :
    device_(std::move(device)),
    slots_per_chunk_(slots_per_chunk > 0u ? slots_per_chunk : 1u),
    block_size_(static_cast<std::uint32_t>(sizeof(vine::graphics::VineDrawBlock))),
    params_offset_(static_cast<std::uint32_t>(offsetof(vine::graphics::VineDrawBlock, params)))
{
    // The pool writes `params` at the offset the ABI declares, so the two cannot drift.
    static_assert(sizeof(vine::graphics::VineDrawBlock) == 80u, "the draw block is 1 mat4 + 1 vec4");
    static_assert(offsetof(vine::graphics::VineDrawBlock, params) == 64u, "the draw block's params follow its model");

    std::uint32_t alignment = 1u;
    if (device_ != nullptr) {
        if (auto physical = device_->getPhysicalDevice()) {
            alignment = static_cast<std::uint32_t>(physical->getProperties().limits.minUniformBufferOffsetAlignment);
        }
    }
    // A slot must start ON the device's alignment: the dynamic offset a bind passes is
    // checked against it (VUID-vkCmdBindDescriptorSets-pDynamicOffsets-01971), so the slots
    // are spaced by the block rounded UP to that alignment.
    stride_ = alignUp(block_size_, alignment);
}

std::shared_ptr<VsgDrawBlockPool> VsgDrawBlockPool::create(::vsg::ref_ptr<::vsg::Device> device,
                                                           std::uint32_t                 slots_per_chunk)
{
    // shared_ptr(new ...), not make_shared: the constructor is private (see create's contract), and this
    // allocation is the one written inside the class that may name it.
    return std::shared_ptr<VsgDrawBlockPool>(new VsgDrawBlockPool(std::move(device), slots_per_chunk));
}

VsgDrawBlockPool::~VsgDrawBlockPool() = default;

VsgDrawBlockPool::Slot VsgDrawBlockPool::reserve()
{
    if (device_ == nullptr) {
        return {};
    }
    for (std::uint32_t index = 0u; index < chunks_.size(); ++index) {
        auto& chunk = *chunks_[index];
        if (chunk.free_indices.empty()) {
            continue;
        }
        const std::uint32_t slot_index = chunk.free_indices.back();
        chunk.free_indices.pop_back();
        ++reserved_;
        return Slot{ index, slot_index };
    }

    auto chunk = makeChunk();
    if (chunk == nullptr) {
        return {}; // out of device memory / device gone: the caller must not bind a block
    }
    const std::uint32_t chunk_index = static_cast<std::uint32_t>(chunks_.size());
    chunks_.push_back(std::move(chunk));

    auto& created = *chunks_.back();
    const std::uint32_t slot_index = created.free_indices.back();
    created.free_indices.pop_back();
    ++reserved_;
    return Slot{ chunk_index, slot_index };
}

VsgDrawBlockPool::Lease VsgDrawBlockPool::acquire()
{
    const Slot slot = reserve();
    if (!slot.valid()) {
        return Lease{};
    }
    // The lease holds the pool through the shared_ptr it was created with, so a caller that drops the lease
    // later than the session drops the pool still returns its slot into live memory (see Lease::LIFETIME).
    return Lease{ shared_from_this(), slot };
}

VsgDrawBlockPool::Lease::Lease(Lease&& other) noexcept :
    pool_(std::move(other.pool_)), slot_(other.slot_)
{
    other.slot_ = {};
}

VsgDrawBlockPool::Lease& VsgDrawBlockPool::Lease::operator=(Lease&& other) noexcept
{
    if (this != &other) {
        // Release what THIS lease holds before taking the other's: the pool must not end up with a
        // slot that is busy in two leases at once.
        retireNow();
        pool_       = std::move(other.pool_);
        slot_       = other.slot_;
        other.slot_ = {};
    }
    return *this;
}

VsgDrawBlockPool::Lease::~Lease()
{
    retireNow();
}

void VsgDrawBlockPool::Lease::retireNow() noexcept
{
    if (pool_ != nullptr && slot_.valid()) {
        // The pool's counting queue, never the free list: the frames in flight may still bind this
        // slot's offset, which is the whole reason a reservation is given back through retire(). The pool is
        // held by this lease, so it is alive right here whatever order the session tore down in.
        pool_->retire(slot_);
    }
    pool_.reset();
    slot_ = {};
}

void VsgDrawBlockPool::Lease::writeOpacity(float opacity) noexcept
{
    if (pool_ != nullptr) {
        pool_->writeOpacity(slot_, opacity);
    }
}

std::uint32_t VsgDrawBlockPool::Lease::offset() const noexcept
{
    return pool_ != nullptr ? pool_->offset(slot_) : 0u;
}

::vsg::ref_ptr<::vsg::DescriptorSet> VsgDrawBlockPool::Lease::descriptorSet(
    ::vsg::ref_ptr<::vsg::DescriptorSetLayout> layout) const
{
    return pool_ != nullptr ? pool_->descriptorSet(slot_, std::move(layout)) : ::vsg::ref_ptr<::vsg::DescriptorSet>();
}

void VsgDrawBlockPool::release(Slot slot) noexcept
{
    auto* chunk = chunkOf(slot);
    if (chunk == nullptr || reserved_ == 0u) {
        return;
    }
    // A stale frame can still read this slot (the caller defers the release past the frames
    // in flight), so its parameters are zeroed rather than left holding the previous
    // drawable's opacity: the next owner writes its own values before it draws.
    std::uint8_t* bytes = static_cast<std::uint8_t*>(chunk->mapped->data());
    std::memset(bytes + static_cast<std::size_t>(slot.index) * stride_ + params_offset_, 0, 16u);
    chunk->free_indices.push_back(slot.index);
    --reserved_;
}

void VsgDrawBlockPool::retire(Slot slot)
{
    if (!slot.valid()) {
        return;
    }
    // The slot stays reserved while it waits: `reserved_` is decremented by the release() that ends
    // the countdown, so a retired slot is neither free nor usable in between.
    //
    // The queue is the POOL's, not the caller's: a caller (a SceneBridge) is destroyed together with
    // its content slots during a teardown, and the slots it had retired -- one per retained drawable
    // -- would be destroyed with it, never coming back to the session pool. The pool outlives the
    // callers, so the countdown keeps running and the capacity comes back (see the declaration).
    retired.park(slot);
}

void VsgDrawBlockPool::advanceRetired()
{
    // One advance per submitted frame, on the same clock the retire rings run on: a slot retired this
    // frame is handed back kDeferredReleaseFrames submits later, by which time the command buffers
    // that could have bound its offset have been re-recorded.
    retired.advance([this](Slot& slot) { release(slot); });
}

void VsgDrawBlockPool::writeOpacity(Slot slot, float opacity) noexcept
{
    auto* chunk = chunkOf(slot);
    if (chunk == nullptr) {
        return;
    }
    std::uint8_t* bytes = static_cast<std::uint8_t*>(chunk->mapped->data());
    // params.x is the first float of the block's parameter slot (see ShaderAbi.hpp). The
    // whole slot is written through the SAME mapping the GPU reads: HOST_COHERENT memory
    // needs no flush, and no transfer task is involved, so the value is visible to the
    // frame that records this draw rather than the next one.
    float* params = reinterpret_cast<float*>(bytes + static_cast<std::size_t>(slot.index) * stride_ + params_offset_);
    params[0] = opacity;
}

::vsg::ref_ptr<::vsg::DescriptorSet> VsgDrawBlockPool::descriptorSet(Slot slot,
                                                                    ::vsg::ref_ptr<::vsg::DescriptorSetLayout> layout)
{
    auto* chunk = chunkOf(slot);
    if (chunk == nullptr || layout == nullptr) {
        return {};
    }
    for (auto& [cached_layout, cached_set] : chunk->sets) {
        if (cached_layout.get() == layout.get()) {
            return cached_set;
        }
    }

    // `range` is ONE block, and the descriptor's own offset stays 0: the slot is chosen by
    // the dynamic offset at bind time, which is what lets every drawable sharing this set
    // read its own block from the one buffer.
    auto buffer_info = ::vsg::BufferInfo::create(chunk->buffer, 0, block_size_);
    auto descriptor  = ::vsg::DescriptorBuffer::create(::vsg::BufferInfoList{ buffer_info }, 0u, 0u,
                                                       VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC);
    auto set         = ::vsg::DescriptorSet::create(layout, ::vsg::Descriptors{ descriptor });
    if (set == nullptr) {
        return {};
    }
    chunk->sets.emplace_back(layout, set);
    return set;
}

std::uint32_t VsgDrawBlockPool::offset(Slot slot) const noexcept
{
    return slot.index * stride_;
}

std::unique_ptr<VsgDrawBlockPool::Chunk> VsgDrawBlockPool::makeChunk()
{
    const std::uint32_t byte_count = slots_per_chunk_ * stride_;
    auto                buffer     = ::vsg::Buffer::create(byte_count, VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT,
                                                           VK_SHARING_MODE_EXCLUSIVE);
    buffer->compile(device_.get());

    // HOST_VISIBLE + HOST_COHERENT: the slots are written by the frame's own thread and read
    // by the GPU, so the write must be visible without a flush and without a staging copy
    // (the same memory the readback path uses — see VsgReadback).
    auto memory = ::vsg::DeviceMemory::create(device_.get(), buffer->getMemoryRequirements(device_->deviceID),
                                              VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    if (memory == nullptr) {
        return {};
    }
    buffer->bind(memory, 0);

    auto mapped = ::vsg::MappedData<::vsg::ubyteArray>::create(memory.get(), 0, 0u, byte_count);
    if (mapped == nullptr || mapped->data() == nullptr) {
        return {};
    }

    auto chunk       = std::make_unique<Chunk>();
    chunk->buffer    = std::move(buffer);
    chunk->memory    = std::move(memory);
    chunk->mapped    = std::move(mapped);
    chunk->free_indices.reserve(slots_per_chunk_);
    // Handed out BACK-TO-FRONT so the first reservation takes index 0: a scene that only
    // ever needs a handful of drawables then touches the first bytes of the chunk.
    for (std::uint32_t index = slots_per_chunk_; index-- > 0u;) {
        chunk->free_indices.push_back(index);
    }
    return chunk;
}

VsgDrawBlockPool::Chunk* VsgDrawBlockPool::chunkOf(Slot slot) noexcept
{
    if (!slot.valid() || slot.chunk >= chunks_.size()) {
        return nullptr;
    }
    return chunks_[slot.chunk].get();
}

V_VSG_NS_END
