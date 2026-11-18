#pragma once

#include <vine/backend_vk/backend_vk_global.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>

#include <volk.h>

#include <vine/backend_vk/internal/ContentAbi.hpp>
#include <vine/backend_vk/internal/MemoryAllocator.hpp>

VN_BACKEND_VK_NS_BEGIN

/**
 * @brief The per-frame scratch ONE FRAME DRIVER owns: the uniform ring its draws write their constants into, and the
 *        descriptor pool (with the set cache) those draws bind.
 *
 * @details
 * - WHY THIS IS NOT THE DEVICE'S. A ring is a frame's scratch: `beginFrame()` hands out slices from the beginning
 *   again, and a frame that does not fit records nothing more and grows the ring BEFORE the next frame (growing inside
 *   a frame would destroy a buffer the command buffer being recorded already names). Two drivers with one ring would
 *   interleave slices and each rewrite what the other's submitted frame may still read - so the ring, the pool and the
 *   set cache move with the frame, which is what makes a second `createFrameDriver()` "one more driver" rather than "one
 *   more way to corrupt the first".
 * - WHAT IT BORROWS, AND FROM WHOM. The device's entry points, its memory pool and the three descriptor-set layouts
 *   belong to the device (`ContentCaches::create` builds them, because a pipeline's layout is shared by every frame).
 *   They are BORROWED: the owner must destroy this scratch before it destroys the layouts and the device.
 * - THE SET CACHE IS THE FRAME'S, and it is safe because a set names resources that are stable across frames: a
 *   content set depends on the five texture views and the shadow map, a screen set on the source's views and the map.
 *   The per-draw variation arrives as a DYNAMIC OFFSET, so one set serves every draw that names the same resources -
 *   and eviction is a frame count, not a "did anybody use it during this frame" question.
 * - THE RING IS A SLICE OF THE DEVICE'S MEMORY POOL. `slicePointer()` answers the mapped address, and the recorder
 *   writes the ABI's blocks there; this type states the layout's OFFSETS (material, lights, draw, shadow) because they
 *   follow the device's `minUniformBufferOffsetAlignment`, not because it knows what the blocks mean.
 *
 * @warning One frame at a time per scratch, on the thread that drives it (the SDK's per-driver contract). Two drivers
 *          are concurrent because they own one each - not because this type is thread-safe.
 *
 * @see `.ai/design/graphics-sessions.md` §3.1.3 (F3), §9.3 step S3.
 */
class VN_BACKEND_VK_API FrameScratch final {
  public:
    /** @brief Builds an empty scratch; nothing exists until @ref create. */
    FrameScratch();

    /** @brief Releases every Vulkan object this scratch owns (see @ref destroy). */
    ~FrameScratch();

    FrameScratch(const FrameScratch&)            = delete;
    FrameScratch& operator=(const FrameScratch&) = delete;

  public:
    /**
     * @brief Creates the descriptor pool and the uniform ring over the device's borrowed state.
     *
     * @details
     * - THE LAYOUTS ARE THE DEVICE'S (`ContentCaches::contentSetLayout()` and friends): a set is allocated against a
     *   layout, and the layout is what the pipelines state, so borrowing is the only shape that cannot drift.
     * - On failure nothing is left behind, so a device that cannot hold 256 KiB of constants refuses to come up
     *   rather than answer frames with nothing.
     *
     * @param table              The device's entry points (volk table).
     * @param device             The logical device.
     * @param allocator          The device's memory pool (the ring is a slice of it).
     * @param uniform_alignment  `minUniformBufferOffsetAlignment` (0 selects 256, the Vulkan minimum).
     * @param content_set_layout Set 0 of a content program (borrowed).
     * @param draw_set_layout    Set 1 of a content program (borrowed).
     * @param screen_set_layout  Set 0 of a full-screen program (borrowed).
     * @return true when the pool and the ring exist.
     */
    [[nodiscard]] bool create(const VolkDeviceTable& table,
                              VkDevice               device,
                              MemoryAllocator&       allocator,
                              std::uint32_t          uniform_alignment,
                              VkDescriptorSetLayout  content_set_layout,
                              VkDescriptorSetLayout  draw_set_layout,
                              VkDescriptorSetLayout  screen_set_layout);

    /** @brief Destroys every object this scratch owns (idempotent; safe before @ref create). */
    void destroy() noexcept;

    /** @brief Answers whether the pool and the ring are up. */
    [[nodiscard]] bool ready() const noexcept;

    /**
     * @brief Opens one frame's scratch: the slices start at the beginning, and the frame that did not fit is paid for.
     *
     * @details
     * - THE PREVIOUS FRAME IS DONE: this driver submits and waits inside its frame call, so when this runs nothing
     *   recorded against the ring is still reading it.
     * - A RING THAT RAN OUT LAST FRAME IS GROWN HERE, between frames, because growing destroys a buffer and doing
     *   that while recording would pull the buffer out from under the command buffer being built.
     */
    void beginFrame();

    /** @brief Byte offset of the light block inside one draw's slice (material starts at 0). */
    [[nodiscard]] VkDeviceSize lightsOffset() const noexcept;
    /** @brief Byte offset of the draw block inside one draw's slice. */
    [[nodiscard]] VkDeviceSize drawOffset() const noexcept;
    /** @brief Byte offset of the shadow block inside one draw's slice. */
    [[nodiscard]] VkDeviceSize shadowOffset() const noexcept;
    /** @brief Size of one draw's slice: material, lights, draw and the shadow block. */
    [[nodiscard]] VkDeviceSize uniformSize() const noexcept;

    /**
     * @brief Hands out the next slice of the frame's ring.
     *
     * @details
     * - A FULL RING DOES NOT GROW IN PLACE: the demand is remembered and this answers false, so the caller skips that
     *   draw; @ref beginFrame grows to the demand before the next frame records anything.
     *
     * @param slice Receives the slice's byte offset.
     * @return true when the slice was handed out.
     */
    [[nodiscard]] bool reserveSlice(VkDeviceSize& slice) noexcept;

    /**
     * @brief Gets the mapped address of @p slice, where the caller writes its blocks at the offsets this type states.
     *
     * @param slice The slice's byte offset (see @ref reserveSlice).
     * @return The host-visible address (null when nothing was reserved).
     */
    [[nodiscard]] std::byte* slicePointer(VkDeviceSize slice) const noexcept;

    /**
     * @brief Answers the content set cached under the resources @p slots and @p shadow_view name, building it once.
     *
     * @param slots          One image info per texture slot, in `TextureSlot` order.
     * @param shadow_view    The map this pass resolved, or the white stand-in.
     * @param shadow_sampler The map's sampler.
     * @return The set, or `VK_NULL_HANDLE` when the pool is exhausted.
     */
    [[nodiscard]] VkDescriptorSet contentSetFor(const std::array<VkDescriptorImageInfo, detail::kTextureSlots>& slots,
                                                VkImageView                                               shadow_view,
                                                VkSampler                                                 shadow_sampler);

    /** @brief Answers the full-screen set for one call's source and map (see @ref contentSetFor). */
    [[nodiscard]] VkDescriptorSet screenSetFor(const std::array<VkDescriptorImageInfo, detail::kTextureSlots>& slots,
                                               VkImageView                                               shadow_view,
                                               VkSampler                                                 shadow_sampler);

    /** @brief The per-draw set: one handle per ring generation (its only binding is the ring's draw block). */
    [[nodiscard]] VkDescriptorSet drawSetFor();

    /** @brief Rings this scratch created, cumulative (one in a steady session). */
    [[nodiscard]] std::uint64_t ringBuilds() const noexcept;
    /** @brief Descriptor sets this scratch BUILT, cumulative - a cache miss, not a draw. */
    [[nodiscard]] std::uint64_t setBuilds() const noexcept;
    /** @brief Descriptor sets cached NOW. */
    [[nodiscard]] std::uint64_t setsLive() const noexcept;

  private:
    struct Impl;

    std::unique_ptr<Impl> impl;
};

VN_BACKEND_VK_NS_END
