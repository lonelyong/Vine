#include <vine/vk/internal/Device.hpp>

#include <cstdint>
#include <cstring>
#include <vector>

// volk is built with VOLK_NO_DEVICE_PROTOTYPES (see third_party/volk/CMakeLists.txt): device-level commands are not
// declared as free functions, and `VolkDeviceTable` is the supported way to reach them - one table filled per device.
#include <volk.h>

#include <vine/graphics/Capabilities.hpp>
#include <vine/graphics/FrameDescription.hpp>
#include <vine/graphics/PresentTarget.hpp>
#include <vine/graphics/RenderTarget.hpp>

VN_VK_NS_BEGIN

using vn::graphics::Capabilities;
using vn::graphics::DiagnosticCategory;
using vn::graphics::DiagnosticSeverity;
using vn::graphics::DeviceDesc;
using vn::graphics::FrameDescription;
using vn::graphics::PresentOutcome;
using vn::graphics::PresentTarget;
using vn::graphics::RenderTarget;

namespace
{

/// @brief The layer this backend asks for whenever the loader has it (see Device::initialize).
constexpr const char* kValidationLayerName = "VK_LAYER_KHRONOS_validation";

/**
 * @brief Builds a diagnostic message from a driver string.
 *
 * @param text The driver's `const char*`, or null.
 * @return The message, empty when @p text is null.
 */
[[nodiscard]] vn::String messageOf(const char* text)
{
    return vn::String(reinterpret_cast<const char8_t*>(text != nullptr ? text : ""));
}

/**
 * @brief The debug-utils callback: forwards every layer message to the device that installed it.
 *
 * @param severity   The layer's severity flags.
 * @param types      The layer's message-type flags (unused: M0 keeps one channel).
 * @param data       The message.
 * @param user_data  The `Device*` the messenger was created with.
 * @return `VK_FALSE`, as the validation layer requires.
 */
VKAPI_ATTR VkBool32 VKAPI_CALL debugCallback(VkDebugUtilsMessageSeverityFlagBitsEXT      severity,
                                             VkDebugUtilsMessageTypeFlagsEXT             types,
                                             const VkDebugUtilsMessengerCallbackDataEXT* data,
                                             void*                                       user_data)
{
    (void)types;
    auto* device = static_cast<Device*>(user_data);
    if (device != nullptr) {
        device->reportDriverMessage(static_cast<int>(severity), data != nullptr ? data->pMessage : nullptr);
    }
    return VK_FALSE;
}

} // namespace

struct Device::Impl {
    DeviceDesc      desc{};
    Capabilities    caps{};
    RenderTarget*   target{ nullptr };

    VkInstance               instance{ VK_NULL_HANDLE };
    VkDebugUtilsMessengerEXT messenger{ VK_NULL_HANDLE };
    VkPhysicalDevice         physical{ VK_NULL_HANDLE };
    VkDevice                 device{ VK_NULL_HANDLE };
    VkQueue                  queue{ VK_NULL_HANDLE };
    std::uint32_t            queue_family{ 0U };

    bool validation{ false };
    /// The device-level entry points (volk declares no free functions for them, see the include note).
    VolkDeviceTable device_table{};
    /// One report each for the two answers M0 cannot serve, so a host is told once rather than per frame.
    bool reported_no_draw{ false };
    bool reported_no_targets{ false };
};

Device::Device(const DeviceDesc& desc)
  : impl(std::make_unique<Impl>())
{
    impl->desc = desc;
}

Device::~Device()
{
    shutdown();
}

bool Device::initialize()
{
    if (impl->device != VK_NULL_HANDLE) {
        // A session that is already up is the "re-announced, unchanged" case: nothing is torn down, nothing rebuilt.
        return true;
    }

    if (volkInitialize() != VK_SUCCESS) {
        reportDiagnostic(DiagnosticSeverity::Error,
                         DiagnosticCategory::InitFailed,
                         u8"vk: no Vulkan loader (libvulkan / vulkan-1 could not be opened)");
        return false;
    }

    // WHAT THE MACHINE HAS. Layers and instance extensions are asked BEFORE the instance exists, and the answers
    // decide what is asked for - the validation layer is a preference, not a requirement, so a machine without it
    // still comes up (this backend is a learner: running it unvalidated beats not running it).
    bool          has_validation  = false;
    bool          has_debug_utils = false;
    std::uint32_t layer_count     = 0;
    if (vkEnumerateInstanceLayerProperties(&layer_count, nullptr) == VK_SUCCESS && layer_count > 0U) {
        std::vector<VkLayerProperties> layers(layer_count);
        if (vkEnumerateInstanceLayerProperties(&layer_count, layers.data()) == VK_SUCCESS) {
            for (const VkLayerProperties& layer : layers) {
                if (std::strcmp(layer.layerName, kValidationLayerName) == 0) {
                    has_validation = true;
                    break;
                }
            }
        }
    }
    std::uint32_t extension_count = 0;
    if (vkEnumerateInstanceExtensionProperties(nullptr, &extension_count, nullptr) == VK_SUCCESS && extension_count > 0U) {
        std::vector<VkExtensionProperties> extensions(extension_count);
        if (vkEnumerateInstanceExtensionProperties(nullptr, &extension_count, extensions.data()) == VK_SUCCESS) {
            for (const VkExtensionProperties& extension : extensions) {
                if (std::strcmp(extension.extensionName, VK_EXT_DEBUG_UTILS_EXTENSION_NAME) == 0) {
                    has_debug_utils = true;
                    break;
                }
            }
        }
    }

    const bool want_validation = has_validation;
    const bool want_debug      = want_validation && has_debug_utils;

    VkApplicationInfo application{};
    application.sType              = VK_STRUCTURE_TYPE_APPLICATION_INFO;
    application.pApplicationName   = "vk";
    application.applicationVersion = VK_MAKE_VERSION(0, 1, 0);
    application.pEngineName        = "Vine";
    application.engineVersion      = VK_MAKE_VERSION(0, 1, 0);
    application.apiVersion         = VK_API_VERSION_1_2;

    const char*   layer_names[1]{};
    std::uint32_t layer_name_count = 0;
    const char*   extension_names[1]{};
    std::uint32_t extension_name_count = 0;
    if (want_validation) {
        layer_names[layer_name_count++] = kValidationLayerName;
    }
    if (want_debug) {
        extension_names[extension_name_count++] = VK_EXT_DEBUG_UTILS_EXTENSION_NAME;
    }

    VkInstanceCreateInfo instance_info{};
    instance_info.sType                   = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
    instance_info.pApplicationInfo        = &application;
    instance_info.enabledLayerCount       = layer_name_count;
    instance_info.ppEnabledLayerNames     = layer_name_count > 0U ? layer_names : nullptr;
    instance_info.enabledExtensionCount   = extension_name_count;
    instance_info.ppEnabledExtensionNames = extension_name_count > 0U ? extension_names : nullptr;

    if (vkCreateInstance(&instance_info, nullptr, &impl->instance) != VK_SUCCESS) {
        reportDiagnostic(DiagnosticSeverity::Error, DiagnosticCategory::InitFailed, u8"vk: vkCreateInstance failed");
        shutdown();
        return false;
    }
    // Instance-level commands exist only after this (volk resolves them per instance).
    volkLoadInstance(impl->instance);
    impl->validation = want_validation;

    if (want_debug) {
        VkDebugUtilsMessengerCreateInfoEXT messenger_info{};
        messenger_info.sType           = VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT;
        messenger_info.messageSeverity = VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT |
                                         VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT;
        messenger_info.messageType = VK_DEBUG_UTILS_MESSAGE_TYPE_GENERAL_BIT_EXT |
                                     VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT |
                                     VK_DEBUG_UTILS_MESSAGE_TYPE_PERFORMANCE_BIT_EXT;
        messenger_info.pfnUserCallback = &debugCallback;
        messenger_info.pUserData       = this;
        if (vkCreateDebugUtilsMessengerEXT(impl->instance, &messenger_info, nullptr, &impl->messenger) != VK_SUCCESS) {
            // Not fatal: the layers still report to the loader's own output.
            impl->messenger = VK_NULL_HANDLE;
        }
    }

    // THE DEVICE: the first one with a graphics queue. M0 creates no surface, so no present support is asked for.
    std::uint32_t physical_count = 0;
    if (vkEnumeratePhysicalDevices(impl->instance, &physical_count, nullptr) != VK_SUCCESS || physical_count == 0U) {
        reportDiagnostic(DiagnosticSeverity::Error, DiagnosticCategory::InitFailed, u8"vk: no Vulkan physical device");
        shutdown();
        return false;
    }
    std::vector<VkPhysicalDevice> physical_devices(physical_count);
    if (vkEnumeratePhysicalDevices(impl->instance, &physical_count, physical_devices.data()) != VK_SUCCESS) {
        reportDiagnostic(DiagnosticSeverity::Error,
                         DiagnosticCategory::InitFailed,
                         u8"vk: vkEnumeratePhysicalDevices failed on its second call");
        shutdown();
        return false;
    }

    for (const VkPhysicalDevice candidate : physical_devices) {
        std::uint32_t family_count = 0;
        vkGetPhysicalDeviceQueueFamilyProperties(candidate, &family_count, nullptr);
        if (family_count == 0U) {
            continue;
        }
        std::vector<VkQueueFamilyProperties> families(family_count);
        vkGetPhysicalDeviceQueueFamilyProperties(candidate, &family_count, families.data());
        for (std::uint32_t index = 0; index < family_count; ++index) {
            if ((families[index].queueFlags & VK_QUEUE_GRAPHICS_BIT) != 0U) {
                impl->physical     = candidate;
                impl->queue_family = index;
                break;
            }
        }
        if (impl->physical != VK_NULL_HANDLE) {
            break;
        }
    }
    if (impl->physical == VK_NULL_HANDLE) {
        reportDiagnostic(DiagnosticSeverity::Error,
                         DiagnosticCategory::InitFailed,
                         u8"vk: no physical device has a graphics queue family");
        shutdown();
        return false;
    }

    constexpr float         kQueuePriority = 1.0F;
    VkDeviceQueueCreateInfo queue_info{};
    queue_info.sType            = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
    queue_info.queueFamilyIndex = impl->queue_family;
    queue_info.queueCount       = 1U;
    queue_info.pQueuePriorities = &kQueuePriority;

    VkDeviceCreateInfo device_info{};
    device_info.sType                = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
    device_info.queueCreateInfoCount = 1U;
    device_info.pQueueCreateInfos    = &queue_info;

    if (vkCreateDevice(impl->physical, &device_info, nullptr, &impl->device) != VK_SUCCESS) {
        reportDiagnostic(DiagnosticSeverity::Error, DiagnosticCategory::InitFailed, u8"vk: vkCreateDevice failed");
        shutdown();
        return false;
    }
    volkLoadDevice(impl->device);
    volkLoadDeviceTable(&impl->device_table, impl->device);
    impl->device_table.vkGetDeviceQueue(impl->device, impl->queue_family, 0U, &impl->queue);

    // CAPABILITIES ARE WHAT THIS BACKEND CAN SERVE, not what the device can do: M0 builds no target and no window, so
    // both are false and a host plans nothing against abilities that are not implemented yet (M1 / M5).
    VkPhysicalDeviceProperties properties{};
    vkGetPhysicalDeviceProperties(impl->physical, &properties);

    impl->caps                         = Capabilities{};
    impl->caps.render_targets          = false;
    impl->caps.presentation            = false;
    impl->caps.features                = 0U;
    impl->caps.max_samples             = 1U;
    impl->caps.max_anisotropy          = 1.0F;
    impl->caps.max_line_width          = properties.limits.lineWidthRange[1] > 0.0F ? properties.limits.lineWidthRange[1] : 1.0F;
    impl->caps.max_color_attachments   = 1U;

    return true;
}

void Device::shutdown()
{
    if (impl->device != VK_NULL_HANDLE) {
        // A counted stop: every queue is idle before the objects it recorded against go.
        (void)impl->device_table.vkDeviceWaitIdle(impl->device);
        impl->device_table.vkDestroyDevice(impl->device, nullptr);
        impl->device       = VK_NULL_HANDLE;
        impl->queue        = VK_NULL_HANDLE;
        impl->device_table = VolkDeviceTable{};
    }
    if (impl->messenger != VK_NULL_HANDLE) {
        vkDestroyDebugUtilsMessengerEXT(impl->instance, impl->messenger, nullptr);
        impl->messenger = VK_NULL_HANDLE;
    }
    if (impl->instance != VK_NULL_HANDLE) {
        vkDestroyInstance(impl->instance, nullptr);
        impl->instance = VK_NULL_HANDLE;
    }
    impl->physical            = VK_NULL_HANDLE;
    impl->queue_family        = 0U;
    impl->validation          = false;
    impl->caps                = Capabilities{};
    impl->reported_no_draw    = false;
    impl->reported_no_targets = false;
}

bool Device::initialized() const noexcept
{
    return impl->device != VK_NULL_HANDLE;
}

vn::graphics::RenderFrameExecutor& Device::executor()
{
    return *this;
}

void Device::setRenderTarget(RenderTarget* target)
{
    impl->target = target;
    if (target != nullptr && !impl->reported_no_targets) {
        impl->reported_no_targets = true;
        reportDiagnostic(DiagnosticSeverity::Warning,
                         DiagnosticCategory::UnsupportedRequest,
                         u8"vk: render targets are not built yet (M1); this device draws nothing into the target named");
    }
}

Capabilities Device::capabilities() const noexcept
{
    return impl->caps;
}

PresentOutcome Device::executeFrame(const FrameDescription& frame, PresentTarget* present_to)
{
    (void)frame;
    (void)present_to;
    if (!impl->reported_no_draw) {
        impl->reported_no_draw = true;
        reportDiagnostic(DiagnosticSeverity::Warning,
                         DiagnosticCategory::UnsupportedRequest,
                         u8"vk: pass recording is not implemented yet (M1/M2); the frame ran and drew nothing");
    }
    return PresentOutcome::Skipped;
}

Device::Handles Device::handles() const noexcept
{
    Handles out;
    out.instance        = reinterpret_cast<std::uint64_t>(impl->instance);
    out.physical_device = reinterpret_cast<std::uint64_t>(impl->physical);
    out.device          = reinterpret_cast<std::uint64_t>(impl->device);
    out.queue           = reinterpret_cast<std::uint64_t>(impl->queue);
    out.queue_family    = impl->queue_family;
    out.validation      = impl->validation;
    return out;
}

void Device::reportDriverMessage(int severity_flags, const char* text)
{
    DiagnosticSeverity severity = DiagnosticSeverity::Info;
    if ((severity_flags & static_cast<int>(VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT)) != 0) {
        severity = DiagnosticSeverity::Error;
    }
    else if ((severity_flags & static_cast<int>(VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT)) != 0) {
        severity = DiagnosticSeverity::Warning;
    }
    // M0 has no frame, so every driver message it can produce is about coming up - which is also why InitFailed is
    // the right category rather than a per-draw one (see the header).
    reportDiagnostic(severity, DiagnosticCategory::InitFailed, messageOf(text));
}

VN_VK_NS_END
