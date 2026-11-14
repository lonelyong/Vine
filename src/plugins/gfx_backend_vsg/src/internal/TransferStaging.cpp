#include <vine/backend_vsg/internal/TransferStaging.hpp>

#include <mutex>

#include <vsg/app/RecordAndSubmitTask.h>
#include <vsg/app/Viewer.h>
#include <vsg/state/Buffer.h>
#include <vsg/vk/Device.h>
#include <vsg/vk/Fence.h>
#include <vsg/vk/Queue.h>
#include <vsg/vk/Semaphore.h>

VN_BACKEND_VSG_NS_BEGIN

namespace detail
{

::VkDeviceSize StagingTask::stagedBytes() const noexcept
{
    // The transfer path sizes these buffers under the same mutex, so the read is taken under it as well: a caller that
    // asks outside the frame thread must not see a half-grown block.
    const std::lock_guard<std::mutex> lock(_mutex);

    ::VkDeviceSize total = 0;
    for (const auto* to_copy : { &_earlyDataToCopy, &_lateDataToCopy }) {
        for (const auto& block : to_copy->frames) {
            if (block != nullptr && block->staging != nullptr) {
                // Grow-only and never shrunk by vsg: as long as the pointer is here, so are the bytes.
                total += block->staging->size;
            }
        }
    }
    return total;
}

::VkDeviceSize StagingTask::releaseStaging()
{
    const std::scoped_lock lock(_mutex);

    ::VkDeviceSize freed = 0;
    for (auto* to_copy : { &_earlyDataToCopy, &_lateDataToCopy }) {
        for (auto& block : to_copy->frames) {
            if (block == nullptr || block->staging == nullptr) {
                continue;
            }
            // THE FENCE IS THE PROOF, and it is the ONLY condition: it covers exactly the `vkCmdCopyBuffer` that read
            // this buffer, so a signalled one means the copy is done and the bytes are dead weight. No frame window, no
            // park, no device idle - and a copy that has not finished is picked up by a later frame rather than waited
            // for here.
            // NOTHING ELSE IS ASKED, deliberately: "this DataToCopy has data to copy" reads like a cheaper skip, but a
            // map that holds an entry with no pending change stays true forever (`_transferData()` returns early
            // without clearing it), which would freeze the sweep and keep the whole load's staging resident. The cost of
            // not asking is one allocation on the next upload through this block - vsg's own growth path.
            if (block->fence != nullptr && block->fence->status() != VK_SUCCESS) {
                continue;
            }
            // THE REUSABLE SIZE STAYS (T26). vsg sizes a block's staging to max(transfer, minimumStagingBufferSize), so a
            // block at or under the minimum is the buffer a PER-FRAME uploader would otherwise rebuild every frame;
            // keeping it turns that allocation into zero. A LARGER one is one-off weight (a model load) and is given
            // back here - which is the case the original measurement was about.
            if (block->staging->size <= minimumStagingBufferSize) {
                continue;
            }
            freed += block->staging->size;
            // EXACTLY THE STATE vsg's own growth path starts from (`!staging` -> create + map), so the next upload through
            // this block sizes a fresh buffer; `buffer_data` points into the memory being freed.
            block->staging     = nullptr;
            block->buffer_data = nullptr;
        }
    }
    return freed;
}

std::optional<::VkDeviceSize> stagedBytesOf(const ::vsg::TransferTask* task) noexcept
{
    if (task == nullptr) {
        return ::VkDeviceSize{ 0 }; // no upload path at all: nothing can be held
    }
    const auto* asking = dynamic_cast<const StagingTask*>(task);
    if (asking == nullptr) {
        return std::nullopt; // vsg built it, so its block is unreachable: assume it may hold one
    }
    return asking->stagedBytes();
}

::vsg::ref_ptr<::vsg::TransferTask> transferTaskOf(const ::vsg::Viewer& viewer) noexcept
{
    const auto& tasks = viewer.recordAndSubmitTasks;
    if (tasks.size() != 1u || tasks.front() == nullptr) {
        return {};
    }
    return tasks.front()->transferTask;
}

bool flushUploads(::vsg::TransferTask& task, std::uint64_t timeout_ns)
{
    if (task.transferQueue == nullptr || task.device == nullptr) {
        return false; // nowhere to submit: the bytes stay staged for the frame's own transfer step
    }

    // THE FRAME'S OWN COPY PATH, run now (see the header): whatever a warm-up handed over is recorded into the task's
    // own transfer command buffer. A task with nothing pending returns with no semaphore and has nothing to wait on.
    const ::vsg::TransferTask::TransferResult transfer = task.transferData(::vsg::TransferTask::TRANSFER_ALL);
    if (transfer.result != VK_SUCCESS) {
        return false;
    }
    if (transfer.dataTransferredSemaphore == nullptr) {
        return true; // zero work, zero submit
    }

    // THE WAIT THAT CONSUMES THE SIGNAL: a submission with no command buffers of its own that waits the transfer's
    // completion semaphore, fenced so this call can answer only once the copies have run. Without the wait the
    // semaphore would stay signalled and the next transfer would signal it a second time - an invalid one.
    ::vsg::ref_ptr<::vsg::Fence> fence = ::vsg::Fence::create(task.device.get());
    if (fence == nullptr) {
        return false;
    }
    VkSemaphore          semaphore = transfer.dataTransferredSemaphore->vk();
    VkPipelineStageFlags stage     = transfer.dataTransferredSemaphore->pipelineStageFlags();
    VkSubmitInfo         wait{};
    wait.sType                = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    wait.waitSemaphoreCount   = 1U;
    wait.pWaitSemaphores      = &semaphore;
    wait.pWaitDstStageMask    = &stage;
    if (task.transferQueue->submit(wait, fence.get()) != VK_SUCCESS) {
        return false;
    }
    return fence->wait(timeout_ns) == VK_SUCCESS;
}

bool installStagingTask(::vsg::Viewer& viewer, ::vsg::Device& device, ::vsg::ref_ptr<::vsg::TransferTask>& previous)
{
    // ONE TASK PER DEVICE, and anything else is a refusal rather than a guess. vsg gives each record task its OWN
    // TransferTask, while the queueing path (`BufferInfo::copyDataToBuffer`) reads the DEVICE's observer: with two
    // tasks the two ends already name different objects, and replacing one of them would split the uploads between a
    // task the frame runs and one nobody does.
    if (viewer.recordAndSubmitTasks.size() != 1u) {
        return false;
    }

    ::vsg::ref_ptr<::vsg::RecordAndSubmitTask>& task    = viewer.recordAndSubmitTasks.front();
    ::vsg::ref_ptr<::vsg::TransferTask>&        current = task->transferTask;
    if (current == nullptr) {
        return false;
    }
    // A copy that is queued but not submitted yet names no block we could drop, and swapping now would lose it.
    if (current->containsDataToTransfer(::vsg::TransferTask::TRANSFER_ALL)) {
        return false;
    }

    // NOTHING IS WAITED FOR HERE, and nothing has to be: this runs before the session's first frame, so no submission
    // names either task and the replaced one owns nothing but empty blocks. It is handed back anyway, so the caller
    // decides where it dies instead of a swap deciding for it.
    previous = current;

    // The block count the viewer's record task was built with (`RecordAndSubmitTask(device, numBuffers = 3)`): the
    // frames in flight, so the fresh task has the same number of reusable blocks.
    constexpr std::uint32_t kTransferBuffers = 3u;
    ::vsg::ref_ptr<::vsg::TransferTask> fresh = StagingTask::create(&device, kTransferBuffers);

    // THE TWO HANDOVERS THAT ARE NOT OPTIONAL (both established by experiment, and neither reports itself):
    // - the queue: the viewer chose it when it built the task, and a task without one has nothing to submit on
    //   (transferData() then faults);
    // - the DEVICE's observer: uploads are queued through `device->transferTask` while the frame runs the task LIST's
    //   one. Left pointing at the object being replaced, every later upload is queued into a task nobody records -
    //   silently, and the frame still looks correct.
    fresh->transferQueue   = current->transferQueue;
    fresh->instrumentation = current->instrumentation;
    fresh->level           = current->level;
    task->transferTask     = fresh;
    device.transferTask    = fresh.get();
    return true;
}

::VkDeviceSize releaseStaging(::vsg::Viewer& viewer)
{
    // ONE ANSWER PER SESSION, from the session's own task: a task this backend did not install keeps its blocks to
    // itself, so nothing can be given back here (and `installStagingTask` is what makes sure that does not happen).
    const ::vsg::ref_ptr<::vsg::TransferTask> task    = transferTaskOf(viewer);
    auto*                                     staging = dynamic_cast<StagingTask*>(task.get());
    if (staging == nullptr) {
        return 0;
    }
    return staging->releaseStaging();
}

} // namespace detail

VN_BACKEND_VSG_NS_END
