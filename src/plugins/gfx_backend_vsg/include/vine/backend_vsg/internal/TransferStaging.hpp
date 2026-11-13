#pragma once

#include <cstdint>
#include <optional>

#include <vsg/app/TransferTask.h>
#include <vsg/core/Inherit.h>
#include <vsg/core/ref_ptr.h>

#include <vine/backend_vsg/backend_vsg_global.hpp>

namespace vsg
{

class Device;
class Viewer;

} // namespace vsg

VN_BACKEND_VSG_NS_BEGIN

namespace detail
{

/**
 * @brief The upload task this backend installs: a vsg transfer task whose staging blocks it can give back.
 *
 * @details
 * - WHY IT IS DERIVED. The block that has to be given back is `TransferTask`'s own scratch: `transferData()` sizes a
 *   HOST_VISIBLE|HOST_COHERENT buffer to the whole transfer and keeps it in the `TransferBlock` it will reuse, and no
 *   public member names that block - `TransferBlock` and the two `DataToCopy` members are `protected`. A subclass is
 *   therefore the only way to reach it without patching vsg, which is this adapter layer's job (it already hardcodes
 *   the task count `RecordAndSubmitTask` builds, hands the `transferQueue` over by hand and publishes the task on
 *   `device->transferTask`).
 * - WHAT IT DOES ABOUT IT. A block's staging buffer is the SOURCE of one `vkCmdCopyBuffer` and has no second reader, so
 *   once that copy's fence signals the bytes are dead weight - vsg simply never lets them go (measured: 1.2 GB resident
 *   after loading a model, and it grows to one such block per frame slot, because uploads rotate through them).
 *   `releaseStaging()` frees the buffer the moment the block's own fence proves the copy is done, which is the same
 *   state vsg's growth path already handles (`if (!staging || staging->size < totalSize) staging =
 *   createBufferAndMemory(...)`): the next upload through that block sizes a fresh one.
 * - WHAT IT KEEPS (T26): a block whose staging is AT OR UNDER `minimumStagingBufferSize` stays. That is the size vsg
 *   allocates for a small transfer anyway, so a host that uploads every frame (dynamic geometry, a streaming point cloud)
 *   would otherwise pay one allocation per frame forever; keeping the reusable block makes the steady state zero
 *   allocations. Larger blocks are one-off weight and are still given back - the 1.2 GB case the sweep exists for.
 * - WHY THAT IS BETTER THAN REPLACING THE TASK. A new task would also throw away every block's command pool, command
 *   buffer and fence, and would have to inherit three handovers correctly or uploads stop landing silently (see
 *   `installStagingTask`); clearing the buffer keeps all of that and costs one allocation on the next upload.
 * - HOW ROBUST THE DEPENDENCY IS. It touches three protected facts (`_earlyDataToCopy` / `_lateDataToCopy`,
 *   `TransferBlock::staging`, and `buffer_data` which points into it). Two of them are read against one stated rule -
 *   "`staging != nullptr` means this task owns a host block, and its size is what it holds" - and the write is the state
 *   vsg itself returns to, which is why a rename stops the build and a semantic change is what the byte-level tests
 *   watch (see `OffscreenSessionTest`). It adds no virtual function and no data.
 */
class StagingTask : public ::vsg::Inherit<::vsg::TransferTask, StagingTask> {
  public:
    using ::vsg::Inherit<::vsg::TransferTask, StagingTask>::Inherit;

    /** @brief Gets the bytes of host staging this task is holding right now.
     *
     * @details
     * - Sums every reusable block's staging buffer (there is one block per frame in flight, and each is grown to the
     *   transfer it served), under the task's own mutex - the transfer path may be sizing them from another call.
     * - Zero means "nothing to give back" - which is also the steady state once `releaseStaging()` has run.
     *
     * @return Staged bytes; zero when this task owns no staging buffer.
     */
    [[nodiscard]] ::VkDeviceSize stagedBytes() const noexcept;

    /** @brief Frees every staging buffer whose copy is done, and says how much was given back.
     *
     * @details
     * - A block may be freed when its own fence signals: that fence covers exactly the `vkCmdCopyBuffer` that read the
     *   buffer, so a signalled fence IS the proof, and no frame window, park queue or device idle is needed. That is the
     *   ONLY condition - asking whether the DataToCopy still has data looks like a cheaper skip, but such a map can stay
     *   non-empty with nothing pending (the transfer path then returns early without clearing it) and would freeze the
     *   sweep for the rest of the session.
     * - A copy that has not finished yet is LEFT FOR THE NEXT CALL (the backend asks once per frame), never waited for:
     *   the frame boundary is the one place a device may not block, and a copy is one frame's worth of work.
     * - The buffer is dropped the way vsg sizes it (`staging` + the mapped `buffer_data` pointer into it), which is the
     *   state its growth path starts from.
     *
     * @return Bytes given back by this call; zero when nothing was ready.
     */
    ::VkDeviceSize releaseStaging();
};

/** @brief Gets how much host staging @p task holds, or nothing when that cannot be asked.
 *
 * @details
 * - Three answers, because the backend needs all three: a number for a task this backend installed, ZERO for "there
 *   is no transfer task at all" (nothing can be held), and empty for a task vsg built itself - its block is
 *   unreachable, so the caller must assume it may hold one rather than answer "nothing to give back" on a guess.
 *
 * @param task Transfer task to ask, or null.
 * @return The staged bytes, or `std::nullopt` when @p task is not a `StagingTask`.
 */
[[nodiscard]] std::optional<::VkDeviceSize> stagedBytesOf(const ::vsg::TransferTask* task) noexcept;

/** @brief Gets the transfer task the viewer's single record task runs (null when there is not exactly one).
 *
 * @param viewer Viewer whose tasks to read.
 * @return The task uploads are queued into, or null.
 */
[[nodiscard]] ::vsg::ref_ptr<::vsg::TransferTask> transferTaskOf(const ::vsg::Viewer& viewer) noexcept;

/**
 * @brief Installs the upload task whose staging blocks this backend can free, replacing the one vsg built.
 *
 * @details
 * - WHY IT IS NEEDED AT ALL. `RecordAndSubmitTask` builds a plain `vsg::TransferTask`, whose blocks are unreachable from
 *   outside; the backend needs one of its own kind to give them back (see @ref StagingTask).
 * - WHEN IT IS CALLED: once, right after the session's viewer has been given its record task - before any frame, so no
 *   submission names the task being replaced and dropping it costs nothing (its blocks were never sized).
 * - THE TWO HANDOVERS THAT ARE NOT OPTIONAL, both established by experiment and neither reporting itself: the new task
 *   must inherit `transferQueue` (a task without one has nothing to submit on - `transferData()` then faults) and the
 *   DEVICE's `transferTask` observer must be re-pointed (uploads are queued through `device->transferTask` while the
 *   frame runs the task LIST's one; left stale, every later upload goes into a task nobody records and the frame still
 *   looks correct).
 *
 * @warning
 * - ONE TRANSFER TASK PER DEVICE is the shape this backend builds. vsg gives each record task its OWN `TransferTask`
 *   while the queueing path reads the DEVICE's observer, so with several tasks the two ends already name different
 *   objects; this therefore refuses when the viewer has more than one task.
 * - A transfer that is queued but not submitted yet is a refusal: replacing the task now would lose it.
 *
 * @param viewer The viewer that owns the record tasks.
 * @param device The device whose `transferTask` observer must follow the replacement.
 * @param previous Receives the task that was replaced, or stays null (the caller drops it when it provably has nothing
 *        in flight, which at session start is always).
 * @return true when a `StagingTask` is now the task uploads are queued through; false when the viewer's shape or a
 *         queued copy made that impossible.
 */
[[nodiscard]] bool installStagingTask(::vsg::Viewer& viewer, ::vsg::Device& device, ::vsg::ref_ptr<::vsg::TransferTask>& previous);

/**
 * @brief Gives one session's upload staging back, all of it that is provably free.
 *
 * @details
 * - ONE HOME FOR BOTH SESSION KINDS: a windowed session and a windowless one own the same thing here (a viewer with one
 *   record task), so they run this same body rather than two copies kept in step by hand.
 * - NOTHING IS WAITED FOR: the proof is each block's own fence (see @ref StagingTask), so this never idles the device,
 *   never parks anything and never needs the frame timeline. It runs at every frame boundary, which is the whole
 *   trigger: a host does not ask, and a copy that has not finished is simply picked up by a later frame.
 *
 * @param viewer Viewer whose record task uploads are queued through.
 * @return Bytes given back by this call; zero when nothing was held or nothing had finished copying.
 */
[[nodiscard]] ::VkDeviceSize releaseStaging(::vsg::Viewer& viewer);

} // namespace detail

VN_BACKEND_VSG_NS_END
