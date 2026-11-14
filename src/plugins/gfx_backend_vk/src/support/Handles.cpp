#include <vine/backend_vk/support/Handles.hpp>

#include <vine/backend_vk/internal/Device.hpp>

VN_BACKEND_VK_NS_BEGIN

Handles vkHandles(const vn::graphics::RenderDevice& device) noexcept
{
    // THE ANSWER IS A TYPE TEST, NOT A PROTOCOL. A `vk` device is the only thing that has these handles, so the whole
    // implementation is one `dynamic_cast`: every other backend - and this one before `initialize()` - answers the
    // empty struct instead of a fabricated handle.
    const auto* vk_device = dynamic_cast<const Device*>(&device);
    if (vk_device == nullptr) {
        return {};
    }
    return vk_device->handles();
}

VN_BACKEND_VK_NS_END
