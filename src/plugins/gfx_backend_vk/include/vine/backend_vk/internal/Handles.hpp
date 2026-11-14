#pragma once

#include <vine/backend_vk/backend_vk_global.hpp>

#include <cstdint>

#include <volk.h>

VN_BACKEND_VK_NS_BEGIN

/**
 * @brief The Vulkan objects one `vk` device created, as a value.
 *
 * @details
 * - WHAT IT IS. The four handles (`VkInstance`, `VkPhysicalDevice`, `VkDevice`, the graphics `VkQueue`), the queue
 *   family they were taken from, and whether this device's messages are validated. It is the FACT both halves of the
 *   escape door read: `Device::handles()` answers it inside the plugin, and `support/Handles.hpp`'s `vkHandles()`
 *   hands it to a test or a tool.
 * - WHY IT LIVES HERE AND NOT IN `support/`. `support/` is the outermost layer - the one a tool includes - while this
 *   header is part of the plugin's internals, and the dependency must point one way (`G2`): `support/Handles.hpp`
 *   includes this, and nothing under `internal/` includes `support/`. A struct declared only in the door header would
 *   have forced that edge backwards, which is why the door's landing criterion is "the frame path does not include
 *   `support/Handles.hpp`" rather than "the door has a type of its own".
 * - A VALUE, NOT AN OWNER. The handles are borrowed from the device that answered them and die with its
 *   `shutdown()`; holding this struct keeps nothing alive.
 * - THE MEMBERS, and what "empty" means for each:
 *   - `instance` - the `VkInstance`; `VK_NULL_HANDLE` before `initialize()` and after `shutdown()`.
 *   - `physical` - the `VkPhysicalDevice` the instance was judged on; same nullness.
 *   - `device` - the logical `VkDevice`; same nullness.
 *   - `graphics` - the graphics `VkQueue` taken from that device; same nullness.
 *   - `graphics_family` - the index of that queue's family; 0 when there is no queue, which is why 0 is "none" here
 *     and not "the first family".
 *   - `validation` - whether the validation layer is enabled for this device. The one non-handle fact here, kept
 *     because this backend exists to be observed: a capture or an experiment has to know whether what it sees was
 *     validated.
 *
 * @warning
 * - Read it between frames, from the thread that drives the device: nothing here is synchronised.
 * - A handle is not a lifetime. Using one after `shutdown()` is a use-after-free on the driver's side, and this value
 *   cannot tell you it happened.
 */
struct Handles {
    VkInstance       instance{ VK_NULL_HANDLE }; // The `VkInstance`.
    VkPhysicalDevice physical{ VK_NULL_HANDLE }; // The `VkPhysicalDevice`.
    VkDevice         device{ VK_NULL_HANDLE };   // The logical `VkDevice`.
    VkQueue          graphics{ VK_NULL_HANDLE }; // The graphics `VkQueue`.
    std::uint32_t    graphics_family{ 0U };      // Index of that queue's family (0 when there is none).
    bool             validation{ false };        // Whether messages are being validated on this device.
};

VN_BACKEND_VK_NS_END
