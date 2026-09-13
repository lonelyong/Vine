#pragma once
#include "vsg_global.hpp"

#include <cstdint>
#include <memory>
#include <vector>

#include <vsg/core/ref_ptr.h>
#include <vsg/state/DescriptorSet.h>
#include <vsg/state/DescriptorSetLayout.h>
#include <vsg/vk/Device.h>

#include <vine/raw_ptr.hpp>

V_VSG_NS_BEGIN

/**
 * @brief Persistent per-draw uniform slots, selected per drawable by a DYNAMIC OFFSET.
 *
 * WHY. Every drawable needs its own `VineDrawBlock` (the model matrix and the per-draw
 * scalars the shading reads), and the values change per frame. Giving each drawable its
 * own uniform BUFFER costs one buffer, one allocation and — because a uniform binding
 * bakes in the range it reads — one DESCRIPTOR SET per drawable, which is what decides
 * how a scene scales: descriptor sets are a pool-scarce resource and re-creating them
 * per new drawable is the expensive part.
 *
 * WHAT THIS DOES INSTEAD. One buffer holds many slots, each slot one block, spaced by
 * `stride()` (the block rounded up to the device's `minUniformBufferOffsetAlignment`).
 * A descriptor binds the buffer ONCE, with `range` = one block, and every drawable
 * selects its own slot with a dynamic offset — so a scene shares ONE descriptor set per
 * (chunk, set layout) instead of one per drawable, and the per-drawable cost is the
 * offset it binds with.
 *
 * WHY IT IS MAPPED. The slots live in HOST_VISIBLE | HOST_COHERENT memory that is written
 * directly, so a per-draw value costs the bytes it occupies (four floats for the opacity)
 * with no transfer task, no modified-count bookkeeping and no staging copy — and the
 * value is in the GPU's buffer before the frame that records it, not one frame later.
 * The alternative (a `Data` marked DYNAMIC) transfers the WHOLE slot every time one
 * drawable changes, which is why this pool maps instead.
 *
 * CHUNKS, NOT ONE BIG BUFFER. Slots must keep their address for as long as a drawable
 * lives (the retained state wrapper binds the offset), so the pool cannot move slots when
 * it grows. It grows by adding a CHUNK of `slotsPerChunk` slots, which keeps growth
 * allocation-only: no buffer is ever re-created and no descriptor set is ever rewritten.
 *
 * LIFETIME. A slot belongs to whoever reserved it and is returned with release(). The
 * caller is responsible for not reusing a slot whose frames may still be in flight (see
 * SceneBridge, which defers the release past its retire ring); the pool itself only
 * guarantees that a released slot is handed to the NEXT reserve(), never to two callers
 * at once.
 *
 * NOT thread-safe: it is used from the frame's own thread, like the rest of the bridge.
 */
class V_VSG_API VsgDrawBlockPool
{
  public:
    /** @brief Slots per chunk (one 16 KB chunk at the usual 256-byte stride). */
    static constexpr std::uint32_t kDefaultSlotsPerChunk = 64;

    /** @brief A chunk index that means "no slot". */
    static constexpr std::uint32_t kNoChunk = ~std::uint32_t{0};

    /** @brief One reserved slot: the chunk that owns it and its index within that chunk. */
    struct Slot
    {
        std::uint32_t chunk = kNoChunk; ///< Owning chunk, or kNoChunk when unset.
        std::uint32_t index = 0;        ///< Index within the chunk.

        /**
         * @brief Whether this handle refers to a slot.
         *
         * @return true when a slot was reserved into this handle.
         */
        bool valid() const noexcept { return chunk != kNoChunk; }
    };

    /**
     * @brief Creates an empty pool bound to @p device.
     *
     * No device memory is allocated until the first reserve(): a bridge that never draws
     * on our forward set never pays for a buffer.
     *
     * @param device          Device the slots' buffers belong to (may be null, in which
     *                        case the pool refuses to hand out slots).
     * @param slots_per_chunk Slots per chunk.
     */
    VsgDrawBlockPool(::vsg::ref_ptr<::vsg::Device> device, std::uint32_t slots_per_chunk = kDefaultSlotsPerChunk);
    ~VsgDrawBlockPool();

    VsgDrawBlockPool(const VsgDrawBlockPool&)            = delete;
    VsgDrawBlockPool& operator=(const VsgDrawBlockPool&) = delete;
    VsgDrawBlockPool(VsgDrawBlockPool&&)                 = delete;
    VsgDrawBlockPool& operator=(VsgDrawBlockPool&&)      = delete;

    /**
     * @brief Reserves one slot, adding a chunk when every existing slot is busy.
     *
     * @return The reserved slot, or an invalid Slot when the device could not provide a
     *         chunk (device gone / out of memory) — the caller must then not bind a block.
     */
    [[nodiscard]] Slot reserve();

    /**
     * @brief Returns @p slot to the pool for reuse.
     *
     * @param slot Slot to release (an invalid slot is a no-op).
     */
    void release(Slot slot) noexcept;

    /**
     * @brief Writes the drawable's opacity into its slot's `params.x`.
     *
     * Only the block's parameter slot is written, not the whole block: the model matrix in
     * `VineDrawBlock` is not read by the vsg forward stage (its matrices arrive in the push
     * range), so filling it would be 64 bytes per moved drawable spent on nothing. A
     * backend whose shader reads the model writes it here as well.
     *
     * @param slot    Slot to write (an invalid slot is a no-op).
     * @param opacity The drawable's effective opacity.
     */
    void writeOpacity(Slot slot, float opacity) noexcept;

    /**
     * @brief Gets the descriptor set that binds @p slot's chunk for @p layout.
     *
     * One set per (chunk, layout): the set binds the chunk's buffer once, and every
     * drawable in it selects a slot with the dynamic offset `offset(slot)`. The set is
     * created on first use for a layout, because the layout belongs to the pipeline
     * configurator that declared it (see the set-1 custom binding).
     *
     * @param slot   Slot whose chunk to bind (must be valid).
     * @param layout Set layout the bind command will use (non-null).
     * @return The descriptor set, or null when it could not be created.
     */
    ::vsg::ref_ptr<::vsg::DescriptorSet> descriptorSet(Slot slot, ::vsg::ref_ptr<::vsg::DescriptorSetLayout> layout);

    /**
     * @brief Gets the dynamic offset a bind of @p slot must pass.
     *
     * @param slot Slot to describe.
     * @return Byte offset of the slot within its chunk's buffer.
     */
    std::uint32_t offset(Slot slot) const noexcept;

    /**
     * @brief Gets the byte stride between two slots.
     *
     * @return The block size rounded up to the device's uniform-buffer alignment.
     */
    std::uint32_t stride() const noexcept { return stride_; }

    /**
     * @brief Gets how many chunks the pool has allocated.
     *
     * @return Chunk count (diagnostics and tests).
     */
    std::uint32_t chunkCount() const noexcept { return static_cast<std::uint32_t>(chunks_.size()); }

    /**
     * @brief Gets how many slots the pool can hand out without another chunk.
     *
     * @return Total slot capacity of the allocated chunks.
     */
    std::uint32_t capacity() const noexcept { return chunkCount() * slots_per_chunk_; }

    /**
     * @brief Gets how many slots are reserved right now.
     *
     * @return Number of live reservations (diagnostics and tests).
     */
    std::uint32_t reservedCount() const noexcept { return reserved_; }

  private:
    struct Chunk;

    /**
     * @brief Allocates one chunk (buffer + mapped host-visible memory + its free list).
     *
     * @return The new chunk, or null when the device could not provide one.
     */
    std::unique_ptr<Chunk> makeChunk();

    /**
     * @brief Gets the chunk @p slot lives in.
     *
     * @param slot Slot to look up.
     * @return The chunk, or null when @p slot is invalid.
     */
    Chunk* chunkOf(Slot slot) noexcept;

    ::vsg::ref_ptr<::vsg::Device> device_;
    std::uint32_t                 slots_per_chunk_ = kDefaultSlotsPerChunk;
    std::uint32_t                 stride_ = 0;        ///< Bytes between two slots.
    std::uint32_t                 block_size_ = 0;    ///< VineDrawBlock size (the bound range).
    std::uint32_t                 params_offset_ = 0; ///< Where `params` sits inside a slot.
    std::uint32_t                 reserved_ = 0;      ///< Live reservations (diagnostics).
    // The chunks, indexed by the Slot::chunk handle. Held by pointer because Chunk is
    // incomplete here and because a chunk's address must stay stable for the offsets
    // already bound to it.
    std::vector<std::unique_ptr<Chunk>> chunks_;
};

V_VSG_NS_END
