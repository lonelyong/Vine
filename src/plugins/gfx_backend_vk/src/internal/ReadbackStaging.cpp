#include <vine/backend_vk/internal/ReadbackStaging.hpp>

#include <limits>

VN_BACKEND_VK_NS_BEGIN

ReadbackStaging::~ReadbackStaging()
{
    release();
}

StagingRefusal ReadbackStaging::open(const VolkDeviceTable& table,
                                     const VkDevice         device,
                                     MemoryAllocator&       allocator,
                                     const std::uint32_t    queue_family,
                                     const VkDeviceSize     bytes)
{
    // THE BORROWED FACTS FIRST, so every failure path below can release through them.
    table_     = &table;
    device_    = device;
    allocator_ = &allocator;
    bytes_     = bytes;

    // THE COPY'S OWN COMMAND MACHINERY, first: it was the reason this type exists next to a frame driver (see the
    // include note). A transient pool holds one one-shot command buffer, which the pool's destruction frees again.
    VkCommandPoolCreateInfo pool_info{};
    pool_info.sType            = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
    pool_info.flags            = VK_COMMAND_POOL_CREATE_TRANSIENT_BIT;
    pool_info.queueFamilyIndex = queue_family;
    if (table.vkCreateCommandPool(device, &pool_info, nullptr, &pool_) != VK_SUCCESS) {
        pool_ = VK_NULL_HANDLE;
        return StagingRefusal::Failed;
    }

    VkFenceCreateInfo fence_info{};
    fence_info.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
    if (table.vkCreateFence(device, &fence_info, nullptr, &fence_) != VK_SUCCESS) {
        fence_ = VK_NULL_HANDLE;
        release();
        return StagingRefusal::Failed;
    }

    VkBufferCreateInfo buffer_info{};
    buffer_info.sType       = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    buffer_info.size        = bytes;
    buffer_info.usage       = VK_BUFFER_USAGE_TRANSFER_DST_BIT;
    buffer_info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    if (table.vkCreateBuffer(device, &buffer_info, nullptr, &buffer_) != VK_SUCCESS) {
        buffer_ = VK_NULL_HANDLE;
        release();
        return StagingRefusal::Failed;
    }

    VkMemoryRequirements requirements{};
    table.vkGetBufferMemoryRequirements(device, buffer_, &requirements);

    // NO HOST-VISIBLE COHERENT TYPE IS `Unsupported`, NOT `Failed`: the device cannot copy to the host at all, and a
    // caller acts on that difference (see StagingRefusal).
    MemoryRefusal refusal = MemoryRefusal::Ok;
    memory_               = allocator.allocate(requirements, MemoryUsage::HostVisible, MemoryKind::Linear, &refusal);
    if (!memory_.valid()) {
        release();
        return refusal == MemoryRefusal::NoType ? StagingRefusal::Unsupported : StagingRefusal::Failed;
    }
    if (table.vkBindBufferMemory(device, buffer_, memory_.memory, memory_.offset) != VK_SUCCESS) {
        release();
        return StagingRefusal::Failed;
    }

    VkCommandBufferAllocateInfo command_allocate{};
    command_allocate.sType              = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    command_allocate.commandPool        = pool_;
    command_allocate.level              = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    command_allocate.commandBufferCount = 1U;
    if (table.vkAllocateCommandBuffers(device, &command_allocate, &commands_) != VK_SUCCESS) {
        commands_ = VK_NULL_HANDLE;
        release();
        return StagingRefusal::Failed;
    }

    VkCommandBufferBeginInfo begin{};
    begin.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    if (table.vkBeginCommandBuffer(commands_, &begin) != VK_SUCCESS) {
        release();
        return StagingRefusal::Failed;
    }
    return StagingRefusal::Ok;
}

VkCommandBuffer ReadbackStaging::commands() const noexcept
{
    return commands_;
}

void ReadbackStaging::recordCopy(const VkImage image, const VkImageAspectFlags aspect, const VkExtent3D extent) noexcept
{
    VkBufferImageCopy region{};
    region.imageSubresource.aspectMask = aspect;
    region.imageSubresource.layerCount = 1U;
    region.imageExtent                 = extent;
    table_->vkCmdCopyImageToBuffer(commands_, image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, buffer_, 1U, &region);
}

StagingRun ReadbackStaging::run(const VkQueue queue, std::mutex& submit_lock) noexcept
{
    StagingRun answered{};
    if (commands_ == VK_NULL_HANDLE) {
        // NOT OPENED (or already failed): answer a result `ran` refuses instead of pretending success. `end` is the
        // field the caller cannot mistake for a copy that ran.
        answered.end = VK_ERROR_UNKNOWN;
        return answered;
    }

    answered.end = table_->vkEndCommandBuffer(commands_);
    if (answered.end != VK_SUCCESS) {
        return answered; // the recording never closed; nothing was submitted
    }

    VkSubmitInfo submit{};
    submit.sType              = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    submit.commandBufferCount = 1U;
    submit.pCommandBuffers    = &commands_;
    {
        // THE QUEUE IS EXTERNALLY SYNCHRONIZED (see the include note): the submit is the one call that needs the
        // device's lock, and it is the only call taken under it - the wait below is where the time goes.
        const std::lock_guard<std::mutex> submit_held(submit_lock);
        answered.submit = table_->vkQueueSubmit(queue, 1U, &submit, fence_);
    }
    answered.wait = answered.submit == VK_SUCCESS ? table_->vkWaitForFences(device_, 1U, &fence_, VK_TRUE, UINT64_MAX)
                                                  : answered.submit;
    (void)table_->vkResetFences(device_, 1U, &fence_);

    if (answered.wait == VK_SUCCESS) {
        // A failed mapping is not a failed copy: bytes() answers empty and the caller's "is this a whole picture"
        // check refuses the read (the mapping is a second fact, not a third way to lose the copy).
        mapped_ = allocator_->map(memory_);
    }
    return answered;
}

std::span<const std::byte> ReadbackStaging::bytes() const noexcept
{
    if (mapped_ == nullptr) {
        return {};
    }
    return std::span<const std::byte>(static_cast<const std::byte*>(mapped_), static_cast<std::size_t>(bytes_));
}

void ReadbackStaging::release() noexcept
{
    if (table_ == nullptr) {
        return;
    }
    mapped_ = nullptr;
    // THE POOL OWNS THE COMMAND BUFFER: destroying it frees the buffer with it (the copy waited for its fence, or never
    // submitted, so nothing is still recording against it).
    commands_ = VK_NULL_HANDLE;
    if (pool_ != VK_NULL_HANDLE) {
        table_->vkDestroyCommandPool(device_, pool_, nullptr);
        pool_ = VK_NULL_HANDLE;
    }
    if (fence_ != VK_NULL_HANDLE) {
        table_->vkDestroyFence(device_, fence_, nullptr);
        fence_ = VK_NULL_HANDLE;
    }
    if (allocator_ != nullptr) {
        allocator_->release(memory_);
    }
    if (buffer_ != VK_NULL_HANDLE) {
        table_->vkDestroyBuffer(device_, buffer_, nullptr);
        buffer_ = VK_NULL_HANDLE;
    }
}

VN_BACKEND_VK_NS_END
