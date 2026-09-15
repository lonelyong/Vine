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
#include <vine/vsg/VsgDeferredRelease.hpp>

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
class V_VSG_API VsgDrawBlockPool : public std::enable_shared_from_this<VsgDrawBlockPool>
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
     * @brief A reserved slot that gives itself back when it goes out of scope.
     *
     * WHY. A slot's offset is baked into the state wrapper of the drawable that bound it, so it has to
     * come back through retire() — a countdown, not a free-list push. Every path that drops a drawable
     * therefore had to remember to retire its slot, and the path that forgot it leaked the slot for the
     * rest of the session (a chunk is never given back, so the pool bought a buffer for every chunk the
     * leak needed, and the leak ran per frame). A Slot is a plain handle and cannot express that
     * obligation; a Lease is the same handle with the return trip built into its destructor, so the
     * reservation cannot be dropped without being returned.
     *
     * Move-only on purpose: two leases holding one slot would retire it twice — the pool would hand the
     * same slot to two drawables.
     *
     * LIFETIME. A lease KEEPS ITS POOL ALIVE (it holds a shared_ptr to it), so there is no destruction order
     * to get right between the two: a bridge destroyed after its session's pool, a pool released before the
     * bridges that hold leases — both are safe, and the slot still goes back through the counting queue. That
     * is why the pool can only be created through create(): a pool owned any other way could not outlive its
     * leases, and a lease that outlived its pool would write into freed memory.
     */
    class Lease
    {
      public:
        /** @brief Creates an empty lease (no slot, no pool). */
        Lease() = default;
        Lease(const Lease&)            = delete;
        Lease& operator=(const Lease&) = delete;
        Lease(Lease&& other) noexcept;
        Lease& operator=(Lease&& other) noexcept;
        /** @brief Returns the slot to the pool's retired queue (an empty lease does nothing). */
        ~Lease();

        /** @brief Gets whether this lease holds a slot.
         *
         * @return true when a slot was reserved into this lease and it is still held.
         */
        [[nodiscard]] bool valid() const noexcept { return pool_ != nullptr && slot_.valid(); }

        /** @brief Writes this drawable's opacity into its slot (see VsgDrawBlockPool::writeOpacity).
         *
         * @param opacity The drawable's effective opacity (an empty lease does nothing).
         */
        void writeOpacity(float opacity) noexcept;

        /** @brief Gets the dynamic offset a bind of this slot passes (see VsgDrawBlockPool::offset).
         *
         * @return Byte offset of the slot within its chunk's buffer (0 for an empty lease).
         */
        [[nodiscard]] std::uint32_t offset() const noexcept;

        /** @brief Gets the descriptor set that binds this slot's chunk (see VsgDrawBlockPool::descriptorSet).
         *
         * @param layout Set layout the bind command will use (non-null).
         * @return The descriptor set, or null when it could not be created (or the lease is empty).
         */
        [[nodiscard]] ::vsg::ref_ptr<::vsg::DescriptorSet> descriptorSet(
            ::vsg::ref_ptr<::vsg::DescriptorSetLayout> layout) const;

      private:
        friend class VsgDrawBlockPool;

        /** @brief Takes @p slot from @p pool, to be returned by the destructor.
         *
         * @param pool Pool the slot belongs to (kept alive by the lease).
         * @param slot Slot to hold.
         */
        Lease(std::shared_ptr<VsgDrawBlockPool> pool, Slot slot) noexcept :
            pool_(std::move(pool)), slot_(slot)
        {
        }

        /** @brief Hands the held slot to the pool's retired queue and empties the lease. */
        void retireNow() noexcept;

        std::shared_ptr<VsgDrawBlockPool> pool_; ///< Pool the slot goes back to (kept alive here).
        Slot                              slot_; ///< The held slot, or an invalid one.
    };

    /**
     * @brief Creates a pool bound to @p device.
     *
     * The only way to get a pool, and through a shared_ptr on purpose: the leases a pool hands out keep it
     * alive (see @ref Lease), so a pool that were owned by anything else could be destroyed under them. No
     * device memory is allocated until the first acquire(): a bridge that never draws on our forward set
     * never pays for a buffer.
     *
     * @param device          Device the slots' buffers belong to (may be null, in which case the pool
     *                        refuses to hand out slots).
     * @param slots_per_chunk Slots per chunk.
     * @return The pool.
     */
    [[nodiscard]] static std::shared_ptr<VsgDrawBlockPool> create(::vsg::ref_ptr<::vsg::Device> device,
                                                                 std::uint32_t slots_per_chunk = kDefaultSlotsPerChunk);

    ~VsgDrawBlockPool();

    VsgDrawBlockPool(const VsgDrawBlockPool&)            = delete;
    VsgDrawBlockPool& operator=(const VsgDrawBlockPool&) = delete;
    VsgDrawBlockPool(VsgDrawBlockPool&&)                 = delete;
    VsgDrawBlockPool& operator=(VsgDrawBlockPool&&)      = delete;

    /**
     * @brief Reserves one slot and hands it over as a lease — the ONLY way to hold a slot.
     *
     * There is no plain-handle entry point, on purpose: a Slot is a value with no obligation attached, and
     * every way to get the return trip wrong (a drop path that forgets to retire, a retire twice, a slot
     * handed out while frames in flight still bind its offset) is silent. The lease's destructor is the one
     * place a slot goes back, so those paths do not exist to be forgotten.
     *
     * @return The lease — invalid when the device could not provide a chunk (device gone / out of memory),
     *         which is the one case the caller must handle by not binding a block.
     */
    [[nodiscard]] Lease acquire();

    /**
     * @brief Hands @p slot to the pool's RETIRED queue instead of the free list.
     *
     * Called by the lease when it goes (that is the normal way in); public because the queue's timing is a
     * rule of its own, and the test that pins it drives this directly with a hand-made slot.
     *
     * A slot's offset is baked into the state wrapper of the drawable that bound it, so the frames
     * still in flight may read it: it must not be handed to another drawable until they are done.
     * The queue that records that is the POOL's, deliberately, and not the caller's: a caller (a
     * SceneBridge) is dropped together with its content slots during a teardown — an offscreen
     * target rebuilt at a new size, a pass retargeted, a target released — and a queue that lived in
     * the caller would be destroyed with it, never returning those slots. Here the slots come back
     * kDeferredReleaseFrames submitted frames later, whoever dropped them, so teardown cannot leak the
     * pool's capacity (which costs a new chunk, i.e. a new buffer, each time it runs out).
     *
     * Measured on this build (self-test, 376 frames, at most 9 live content slots): with the deferred
     * queue the pool peaked at ONE chunk and 13 of its 64 slots reserved, while the same run with the
     * slots handed back immediately (what the per-bridge queue effectively did once its bridge was
     * destroyed) peaked at THREE chunks with 147 of 192 slots reserved — capacity for a scene that
     * never used more than 13 at once, and growing with every teardown.
     *
     * @param slot Slot to retire (an invalid slot is a no-op).
     */
    void retire(Slot slot);

    /**
     * @brief Advances the retired queue by one SUBMITTED frame.
     *
     * Called once per frame that reaches the GPU queue (see VsgRenderer::settleSubmittedFrame), for
     * the same reason the retire rings are: the count of frames in flight is what makes a retired
     * slot safe to hand out again.
     */
    void advanceRetired();
    /**
     * @brief Gets the byte stride between two slots.
     *
     * @return The block size rounded up to the device's uniform-buffer alignment.
     */
    std::uint32_t stride() const noexcept { return stride_; }

    /**
     * @brief What the pool is holding, in one value.
     *
     * A pool's health is a picture, not four numbers a caller has to remember to ask for together:
     * `chunks` × `capacity` is what has been allocated, `reserved` is what drawables hold, and
     * `retired` is the part waiting out the frames in flight (it is reserved, but not usable).
     * Chunks that ARE allocated are never given back — the pool grows by adding them — so a
     * `capacity` that keeps climbing while `reserved` does not is the shape of a leak.
     */
    struct Stats
    {
        std::uint32_t chunks   = 0; ///< Chunks allocated (each one a buffer + descriptor sets).
        std::uint32_t capacity = 0; ///< Slots the allocated chunks can hold.
        std::uint32_t reserved = 0; ///< Slots a drawable holds right now.
        std::uint32_t retired  = 0; ///< Of those, the ones waiting out the frames in flight.
    };

    /**
     * @brief Gets what the pool is holding right now.
     *
     * @return The pool's picture (see @ref Stats).
     */
    [[nodiscard]] Stats stats() const noexcept
    {
        return Stats{ chunkCount(), capacity(), reserved_, static_cast<std::uint32_t>(retired.parkedCount()) };
    }

  private:
    /** @brief Binds a pool to @p device (see create(), the only way in).
     *
     * @param device          Device the slots' buffers belong to (may be null).
     * @param slots_per_chunk Slots per chunk (at least 1).
     */
    VsgDrawBlockPool(::vsg::ref_ptr<::vsg::Device> device, std::uint32_t slots_per_chunk);

    /** @brief Chunks allocated so far. */
    [[nodiscard]] std::uint32_t chunkCount() const noexcept { return static_cast<std::uint32_t>(chunks_.size()); }

    /** @brief Slots the allocated chunks can hold. */
    [[nodiscard]] std::uint32_t capacity() const noexcept { return chunkCount() * slots_per_chunk_; }

    struct Chunk;

    /**
     * @brief Reserves one slot, adding a chunk when every existing slot is busy.
     *
     * The pool's own half of @ref acquire: private because a Slot on its own is the handle the lease
     * exists to replace (see acquire).
     *
     * @return The reserved slot, or an invalid Slot when the device could not provide a chunk (device
     *         gone / out of memory).
     */
    [[nodiscard]] Slot reserve();

    /**
     * @brief Writes the drawable's opacity into its slot's `params.x` (the lease forwards here).
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
     * @brief Gets the descriptor set that binds @p slot's chunk for @p layout (the lease forwards here).
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
     * @brief Gets the dynamic offset a bind of @p slot must pass (the lease forwards here).
     *
     * @param slot Slot to describe.
     * @return Byte offset of the slot within its chunk's buffer.
     */
    std::uint32_t offset(Slot slot) const noexcept;

    /**
     * @brief Returns @p slot to the pool for reuse.
     *
     * The retired queue's last step, and deliberately not part of the public surface: handing a slot
     * back the moment its drawable goes is the mistake the countdown exists to prevent (the frames in
     * flight may still bind its offset), so the only way in is @ref retire().
     *
     * @param slot Slot to release (an invalid slot is a no-op).
     */
    void release(Slot slot) noexcept;

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
    // Slots whose frames may still be in flight, on the shared deferral clock (see
    // VsgDeferredRelease): the queue belongs to the pool, so a teardown that destroys the caller
    // cannot lose it (see retire()).
    VsgDeferredRelease<Slot> retired;
    // The chunks, indexed by the Slot::chunk handle. Held by pointer because Chunk is
    // incomplete here and because a chunk's address must stay stable for the offsets
    // already bound to it.
    std::vector<std::unique_ptr<Chunk>> chunks_;
};

V_VSG_NS_END
