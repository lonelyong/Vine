#pragma once

#include <cstddef>
#include <cstdint>
#include <span>

#include <vine/backend_vk/backend_vk_global.hpp>

#include <volk.h>

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
 * - WHAT IT OWNS: one buffer, its memory, one primary command buffer (one-time submit) and the mapping. Nothing else.
 *   The IMAGE and its layouts stay with the caller, because only the caller knows what the image is - a colour
 *   attachment a pass cleared, a depth picture a shader sampled - and therefore which barriers surround the copy.
 * - WHY A TYPE, NOT A FUNCTION: a copy is a SEQUENCE - create the transfer target, record the transitions and the
 *   copy, submit, wait, map, read, release - and the sequence is what must not be re-spelled by every readback entry
 *   point. "How" is the same for colour and depth; "what" is the caller's. The order cannot be skipped: `commands()`
 *   records, `run()` is the ONE submit and wait, `bytes()` answers only after it ran, and the destructor releases the
 *   objects in the only order that is valid.
 * - IT DECIDES NOTHING: no diagnostic and no `ReadbackResult`; the device maps `StagingRefusal` / `StagingRun` onto
 *   its own answer (and reports a lost device once, see `Device::noteDeviceLost`).
 * - MEMORY: one allocation per copy, like the rest of this backend (see `TargetImages`) - the allocator is its own
 *   later step.
 *
 * @warning Not thread-safe and not reentrant: one copy at a time, which is this device's contract for its ONE frame
 *          fence.
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
     * @brief Creates the staging buffer, allocates its memory and begins the one-time command buffer.
     *
     * @param table  The device's entry points (borrowed; must outlive this copy).
     * @param device The logical device.
     * @param memory The device's memory properties (the host-visible coherent type search).
     * @param pool   The command pool the copy's command buffer is allocated from.
     * @param bytes  How many bytes the copy will write (width * height * texel size).
     * @return Ok when @ref commands may be recorded; Unsupported when no host-visible coherent memory type exists;
     *         Failed when a Vulkan object could not be created.
     */
    [[nodiscard]] StagingRefusal open(const VolkDeviceTable&                  table,
                                      VkDevice                                device,
                                      const VkPhysicalDeviceMemoryProperties& memory,
                                      VkCommandPool                           pool,
                                      VkDeviceSize                            bytes);

    /** @brief Gets the command buffer to record the transitions and the copy into (allocated and begun by @ref open). */
    [[nodiscard]] VkCommandBuffer commands() const noexcept;

    /**
     * @brief Records a tightly packed 2D image-to-buffer copy.
     *
     * @details
     * - The region is mip 0 of layer 0 of the whole extent, tightly packed (`bufferRowLength` / `bufferImageHeight`
     *   0), READ from `TRANSFER_SRC_OPTIMAL` - the layout the caller transitioned the image into.
     *
     * @param image  The image to copy (must be in `TRANSFER_SRC_OPTIMAL`).
     * @param aspect The ONE aspect to copy (a depth copy names `DEPTH`: the stencil half is not a depth, and nothing
     *               here decodes it).
     * @param extent The extent copied (the target's width/height, depth 1).
     */
    void recordCopy(VkImage image, VkImageAspectFlags aspect, VkExtent3D extent) noexcept;

    /**
     * @brief Ends the command buffer, submits it on @p queue, waits for @p fence and maps the bytes.
     *
     * @details
     * - The fence is RESET before this returns, so the device's next submit starts from a signalled state whatever
     *   this copy did - the same rule the frame path follows.
     *
     * @param queue The queue the copy runs on (the device's one).
     * @param fence The device's frame fence (the one every other submit in this backend uses).
     * @return What the end/submit/wait answered; @ref bytes is readable exactly when @ref StagingRun::ran is true and
     *         the mapping succeeded.
     */
    [[nodiscard]] StagingRun run(VkQueue queue, VkFence fence) noexcept;

    /** @brief Gets the copied bytes (empty unless @ref run answered `ran` and the mapping succeeded). */
    [[nodiscard]] std::span<const std::byte> bytes() const noexcept;

  private:
    /// Unmaps, then frees the command buffer, the memory and the buffer (each step null-checked; idempotent).
    void release() noexcept;

    const VolkDeviceTable* table_{ nullptr };
    VkDevice               device_{ VK_NULL_HANDLE };
    VkCommandPool          pool_{ VK_NULL_HANDLE };
    VkBuffer               buffer_{ VK_NULL_HANDLE };
    VkDeviceMemory         memory_{ VK_NULL_HANDLE };
    VkCommandBuffer        commands_{ VK_NULL_HANDLE };
    void*                  mapped_{ nullptr };
    VkDeviceSize           bytes_{ 0 };
};

VN_BACKEND_VK_NS_END
