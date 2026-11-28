#pragma once

#include <cstddef>
#include <cstdint>
#include <mutex>
#include <span>

#include <vine/backend_vk/backend_vk_global.hpp>

#include <volk.h>

#include <vine/backend_vk/internal/MemoryAllocator.hpp>

VN_BACKEND_VK_NS_BEGIN

/**
 * @brief Why a staging copy could not be opened (see ReadbackStaging::open).
 */
enum class StagingRefusal : std::uint8_t
{
    Ok,          // The copy is ready: record into commands() and run() it.
    Unsupported, // No host-visible coherent memory type serves the buffer: this device cannot copy to the host.
    Failed,      // A Vulkan object could not be created (out of memory, a lost device).
};

/**
 * @brief What one staging copy answered (see ReadbackStaging::run).
 *
 * @details
 * - THREE RESULTS, kept apart because the device classifies them: `VK_ERROR_DEVICE_LOST` on the submit or the wait
 *   is the session-level fact `Device::noteDeviceLost` reports, while `end` failing never reached the queue at all.
 *   Handing the values back keeps that classification where every other queue call is classified - this unit decides
 *   nothing and reports nothing.
 */
struct StagingRun {
    VkResult end{ VK_SUCCESS };    // The one-time command buffer closed.
    VkResult submit{ VK_SUCCESS }; // The queue took it.
    VkResult wait{ VK_SUCCESS };   // The fence signalled: the copy ran.

    /** @brief Answers whether the copy was recorded, submitted and completed. */
    [[nodiscard]] bool ran() const noexcept
    {
        return end == VK_SUCCESS && submit == VK_SUCCESS && wait == VK_SUCCESS;
    }
};

/**
 * @brief One image-to-host copy: the host-visible staging buffer, the command buffer that carries it, the submit/wait
 *        and the mapped bytes.
 *
 * @details
 * - WHAT IT OWNS: one command pool, one fence, one buffer, its memory, one primary command buffer (one-time submit)
 *   and the mapping. Nothing is borrowed from a FRAME DRIVER: a copy brings the command machinery it runs on, so a
 *   readback works whatever sessions exist and can never record into, or wait on, a frame's own command buffer and
 *   fence (`C7` of `.ai/design/graphics-sessions.md`, landed as S7). The IMAGE and its layouts stay with the caller,
 *   because only the caller knows what the image is - a colour attachment a pass cleared, a depth picture a shader
 *   sampled - and therefore which barriers surround the copy.
 * - WHY A TYPE, NOT A FUNCTION: a copy is a SEQUENCE - create the transfer target, record the transitions and the
 *   copy, submit, wait, map, read, release - and the sequence is what must not be re-spelled by every readback entry
 *   point. "How" is the same for colour and depth; "what" is the caller's. The order cannot be skipped: `commands()`
 *   records, `run()` is the ONE submit and wait, `bytes()` answers only after it ran, and the destructor releases the
 *   objects in the only order that is valid.
 * - IT DECIDES NOTHING: no diagnostic and no `ReadbackStatus`; the device maps `StagingRefusal` / `StagingRun` onto
 *   its own answer (and reports a lost device once, see `Device::noteDeviceLost`).
 * - MEMORY: the buffer is a SLICE of the device pool (see MemoryAllocator), bound at that offset and reached through
 *   the pool's persistent mapping - the same arrangement every other buffer here uses, so a copy does not allocate a
 *   whole `VkDeviceMemory` of its own.
 * - THE POOL AND THE FENCE ARE PER COPY, not per device: a readback is the slowest host verb (`P7`), so two more
 *   create/destroy calls beside a GPU round trip are noise - and a copy that owns both cannot race or outlive one
 *   that does not.
 *
 * @warning Not thread-safe as an object: one copy is used from one thread, and copies do not share machinery. Every
 *          submit in this backend still passes through the device's queue-submit lock (Vulkan requires host access to
 *          a `VkQueue` to be externally synchronized), which @ref run takes for the submit call only.
 */
class VN_BACKEND_VK_API ReadbackStaging final
{
  public:
    ReadbackStaging() = default;

    /** @brief Releases the mapping and the Vulkan objects (idempotent, and safe after a failed @ref open). */
    ~ReadbackStaging();

    ReadbackStaging(const ReadbackStaging&)            = delete;
    ReadbackStaging& operator=(const ReadbackStaging&) = delete;
    ReadbackStaging(ReadbackStaging&&)                 = delete;
    ReadbackStaging& operator=(ReadbackStaging&&)      = delete;

  public:
    /**
     * @brief Creates the copy's command pool, fence, staging buffer and memory, and begins the one-time command buffer.
     *
     * @param table        The device's entry points (borrowed; must outlive this copy).
     * @param device       The logical device.
     * @param allocator    The device's memory pool (the buffer's slice comes from it, borrowed).
     * @param queue_family The queue family the copy's OWN command pool belongs to (the device's graphics family).
     * @param bytes        How many bytes the copy will write (width * height * texel size).
     * @return Ok when @ref commands may be recorded; Unsupported when no host-visible coherent memory type exists;
     *         Failed when a Vulkan object could not be created.
     */
    [[nodiscard]] StagingRefusal open(const VolkDeviceTable& table,
                                      VkDevice               device,
                                      MemoryAllocator&       allocator,
                                      std::uint32_t          queue_family,
                                      VkDeviceSize           bytes);

    /** @brief Gets the command buffer to record the transitions and the copy into (allocated and begun by @ref open). */
    [[nodiscard]] VkCommandBuffer commands() const noexcept;

    /**
     * @brief Records a tightly packed 2D image-to-buffer copy.
     *
     * @details
     * - The region is mip 0 of layer 0 of the whole extent, tightly packed (`bufferRowLength` / `bufferImageHeight`
     *   0), READ from @p layout - the layout the caller transitioned the image into, passed in because the layout
     *   vocabulary belongs to the caller's side (this unit names no render-seam type; see C10).
     *
     * @param image  The image to copy.
     * @param aspect The ONE aspect to copy (a depth copy names `DEPTH`: the stencil half is not a depth, and nothing
     *               here decodes it).
     * @param extent The extent copied (the target's width/height, depth 1).
     * @param layout The layout the image is in (the caller's `transferReadState().layout`; Vulkan requires the copy to
     *               name the image's actual layout).
     */
    void recordCopy(VkImage image, VkImageAspectFlags aspect, VkExtent3D extent, VkImageLayout layout) noexcept;

    /**
     * @brief Ends the command buffer, submits it on @p queue under @p submit_lock, waits for the copy's OWN fence and
     *        maps the bytes.
     *
     * @details
     * - The submit is the one call Vulkan requires to be externally synchronized on the queue, so it is taken under the
     *   device's lock; the fence wait is where the time goes and it stays OUTSIDE the lock.
     * - The fence is RESET before this returns, so the next submit on this copy starts from a signalled state whatever
     *   this copy did - the same rule the frame path follows.
     *
     * @param queue       The queue the copy runs on (the device's one).
     * @param submit_lock The device's queue-submit lock (see the class warning; never held during the wait).
     * @return What the end/submit/wait answered; @ref bytes is readable exactly when @ref StagingRun::ran is true and
     *         the mapping succeeded.
     */
    [[nodiscard]] StagingRun run(VkQueue queue, std::mutex& submit_lock) noexcept;

    /** @brief Gets the copied bytes (empty unless @ref run answered `ran` and the mapping succeeded). */
    [[nodiscard]] std::span<const std::byte> bytes() const noexcept;

  private:
    /// Releases the pool (with its command buffer), the fence, the buffer's slice and the buffer (each null-checked;
    /// idempotent).
    void release() noexcept;

    const VolkDeviceTable* table_{ nullptr };
    /// The pool the buffer's slice belongs to (borrowed; outlives this copy).
    MemoryAllocator*       allocator_{ nullptr };
    VkDevice               device_{ VK_NULL_HANDLE };
    /// This copy's OWN command pool and fence (never a frame driver's - see the class note).
    VkCommandPool          pool_{ VK_NULL_HANDLE };
    VkFence                fence_{ VK_NULL_HANDLE };
    VkBuffer               buffer_{ VK_NULL_HANDLE };
    MemorySlice            memory_{};
    VkCommandBuffer        commands_{ VK_NULL_HANDLE };
    const void*            mapped_{ nullptr };
    VkDeviceSize           bytes_{ 0 };
};

VN_BACKEND_VK_NS_END
