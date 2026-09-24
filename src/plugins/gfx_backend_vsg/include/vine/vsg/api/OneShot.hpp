#pragma once

#include <cstdint>

#include <vsg/commands/Commands.h>
#include <vsg/core/ref_ptr.h>
#include <vsg/vk/CommandPool.h>
#include <vsg/vk/Device.h>
#include <vsg/vk/Fence.h>
#include <vsg/vk/PhysicalDevice.h>
#include <vsg/vk/SubmitCommands.h>

#include <vine/vsg/vsg_global.hpp>

VN_VSG_NS_BEGIN

/**
 * @brief Submits @p commands once, on a fresh command buffer, and waits for them.
 *
 * ONE SPELLING FOR ONE THING: the readback's copy and a freshly built target's bootstrap barrier both need
 * "run these commands now, on this device's queue, and answer only when they are done" - the record step's
 * buffers belong to the frame's own slots, so neither can borrow one. The wait is what makes the call
 * synchronous, and the timeout is generous on purpose: a timeout means the work never finished, not that it
 * was slow.
 *
 * WHERE IT IS RIGHT. Between frames, or while a frame is being recorded: the queue serialises the submission
 * against whatever is already there, which is exactly the guarantee both callers need (a barrier that must be
 * in place before the next frame's passes run, a copy whose result the host is about to read).
 *
 * @param device    The device whose graphics queue runs the commands.
 * @param commands  The commands to record (a fresh command buffer is allocated for them).
 * @param timeout_ns How long to wait for the fence before giving up.
 * @return true when the submission was handed to the queue (submitCommandsToQueue waits for the fence).
 */
inline bool submitCommandsOnce(::vsg::Device& device, const ::vsg::ref_ptr<::vsg::Commands>& commands,
                               std::uint64_t timeout_ns = 100'000'000'000ull)
{
    ::vsg::PhysicalDevice* physical = device.getPhysicalDevice();
    if (physical == nullptr || commands == nullptr)
    {
        return false;
    }
    const auto queue_family = physical->getQueueFamily(VK_QUEUE_GRAPHICS_BIT);
    auto       command_pool = ::vsg::CommandPool::create(&device, queue_family);
    auto       fence        = ::vsg::Fence::create(&device);
    auto       queue        = device.getQueue(queue_family);
    if (command_pool == nullptr || fence == nullptr || queue == nullptr)
    {
        return false;
    }
    ::vsg::submitCommandsToQueue(command_pool, fence, timeout_ns, queue,
                                 [&commands](::vsg::CommandBuffer& command_buffer) {
                                     commands->record(command_buffer);
                                 });
    // submitCommandsToQueue() waits on the fence unless the submission failed: a failed submit is the only
    // way out of it with the commands unperformed, and the caller answers for that either way.
    return true;
}

VN_VSG_NS_END
