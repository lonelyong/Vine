#include <vine/backend_vk/internal/Device.hpp>

#include <cstdint>
#include <cstring>
#include <limits>
#include <unordered_map>
#include <vector>

// volk is built with VOLK_NO_DEVICE_PROTOTYPES (see third_party/volk/CMakeLists.txt): device-level commands are not
// declared as free functions, and `VolkDeviceTable` is the supported way to reach them - one table filled per device.
#include <volk.h>

#include <vine/backend_vk/internal/BarrierPlan.hpp>
#include <vine/backend_vk/internal/ContentDraw.hpp>
#include <vine/backend_vk/internal/TargetImages.hpp>
#include <vine/graphics/Capabilities.hpp>
#include <vine/graphics/FrameDescription.hpp>
#include <vine/graphics/PresentTarget.hpp>
#include <vine/graphics/RenderTarget.hpp>
#include <vine/graphics/ShaderProgram.hpp>

VN_BACKEND_VK_NS_BEGIN

using vn::graphics::Capabilities;
using vn::graphics::DiagnosticCategory;
using vn::graphics::DiagnosticSeverity;
using vn::graphics::DeviceDesc;
using vn::graphics::DrawDescription;
using vn::graphics::FrameDescription;
using vn::graphics::PassDraw;
using vn::graphics::PresentOutcome;
using vn::graphics::PresentTarget;
using vn::graphics::ReadbackResult;
using vn::graphics::RenderTarget;
using vn::graphics::backend::ImageLayout;

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
    VkPhysicalDeviceMemoryProperties memory{};

    /// The command machinery every frame and every readback borrows: one pool, one fence, one buffer at a time.
    VkCommandPool command_pool{ VK_NULL_HANDLE };
    VkFence       completion{ VK_NULL_HANDLE };

    /// One entry per off-screen target this device built, keyed by the host's object (the identity the frame names).
    std::unordered_map<const RenderTarget*, TargetImages> targets{};

    /// M2's content path: compiled programs, uploaded streams, pipelines and the per-frame uniform buffers.
    ContentDraw content{};
    /// The program content without one of its own is drawn with, taken as a VALUE when it was announced.
    vn::graphics::ShaderProgramDescriptionPtr default_program{};

    /// One report each for the answers M1 cannot serve, so a host is told once rather than per frame.
    bool reported_no_targets{ false };
    bool reported_no_clear{ false };
    bool reported_window{ false };
    bool reported_content{ false };
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

    // THE COMMAND MACHINERY: one pool (the backend records one frame at a time) and one fence it waits on.
    VkCommandPoolCreateInfo pool_info{};
    pool_info.sType            = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
    pool_info.flags            = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    pool_info.queueFamilyIndex = impl->queue_family;
    if (impl->device_table.vkCreateCommandPool(impl->device, &pool_info, nullptr, &impl->command_pool) != VK_SUCCESS) {
        reportDiagnostic(DiagnosticSeverity::Error, DiagnosticCategory::InitFailed, u8"vk: vkCreateCommandPool failed");
        shutdown();
        return false;
    }

    VkFenceCreateInfo fence_info{};
    fence_info.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
    if (impl->device_table.vkCreateFence(impl->device, &fence_info, nullptr, &impl->completion) != VK_SUCCESS) {
        reportDiagnostic(DiagnosticSeverity::Error, DiagnosticCategory::InitFailed, u8"vk: vkCreateFence failed");
        shutdown();
        return false;
    }

    vkGetPhysicalDeviceMemoryProperties(impl->physical, &impl->memory);

    // CAPABILITIES ARE WHAT THIS BACKEND CAN SERVE, not what the device can do: M1 builds off-screen colour targets and
    // reads them back, and still presents nothing (M5).
    VkPhysicalDeviceProperties properties{};
    vkGetPhysicalDeviceProperties(impl->physical, &properties);

    impl->caps                         = Capabilities{};
    impl->caps.render_targets          = true;
    impl->caps.presentation            = false;
    impl->caps.features                = 0U;
    impl->caps.max_samples             = 1U;
    impl->caps.max_anisotropy          = 1.0F;
    impl->caps.max_line_width          = properties.limits.lineWidthRange[1] > 0.0F ? properties.limits.lineWidthRange[1] : 1.0F;
    impl->caps.max_color_attachments   = 1U;

    // M2'S CONTENT PATH: the layouts, the descriptor pool and the caches every draw uses. A device that cannot build
    // them is not a device that can draw, so it refuses to come up rather than answering frames with nothing.
    if (!impl->content.create(impl->device_table, impl->device, impl->memory, properties.limits.minUniformBufferOffsetAlignment)) {
        reportDiagnostic(DiagnosticSeverity::Error,
                         DiagnosticCategory::InitFailed,
                         u8"vk: the content layout / descriptor pool could not be created");
        shutdown();
        return false;
    }

    return true;
}

void Device::shutdown()
{
    if (impl->device != VK_NULL_HANDLE) {
        // A counted stop: every queue is idle before the objects it recorded against go.
        (void)impl->device_table.vkDeviceWaitIdle(impl->device);

        // Pipelines are built against a target's render pass, so the content caches go BEFORE the targets do.
        impl->content.destroy();

        for (auto& [target, images] : impl->targets) {
            (void)target;
            destroyTarget(impl->device_table, impl->device, images);
        }
        impl->targets.clear();

        if (impl->completion != VK_NULL_HANDLE) {
            impl->device_table.vkDestroyFence(impl->device, impl->completion, nullptr);
            impl->completion = VK_NULL_HANDLE;
        }
        if (impl->command_pool != VK_NULL_HANDLE) {
            impl->device_table.vkDestroyCommandPool(impl->device, impl->command_pool, nullptr);
            impl->command_pool = VK_NULL_HANDLE;
        }

        impl->device_table.vkDestroyDevice(impl->device, nullptr);
        impl->device       = VK_NULL_HANDLE;
        impl->queue        = VK_NULL_HANDLE;
        impl->device_table = VolkDeviceTable{};
        impl->memory       = VkPhysicalDeviceMemoryProperties{};
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
    impl->default_program     = nullptr;
    impl->reported_no_targets = false;
    impl->reported_no_clear   = false;
    impl->reported_window     = false;
    impl->reported_content    = false;
}

bool Device::initialized() const noexcept
{
    return impl->device != VK_NULL_HANDLE;
}

void Device::setDefaultContentProgram(vn::intrusive_ptr<const vn::graphics::ShaderProgram> program)
{
    // THE VALUE, NOT THE OBJECT: what a frame records is the text that was current when the host announced it.
    impl->default_program = program != nullptr ? program->description() : nullptr;
}

vn::graphics::RenderFrameExecutor& Device::executor()
{
    return *this;
}

void Device::setRenderTarget(RenderTarget* target)
{
    impl->target = target;
}

bool Device::ensureTarget(const RenderTarget& target)
{
    // WHAT THE HOST'S OBJECT ASKS FOR, as Vulkan spelling. The extent is the target's own (a target is a DESCRIPTION,
    // and its attachments exist only after a backend builds them - see RenderTarget's note).
    const int      width  = target.width();
    const int      height = target.height();
    const VkFormat color  = target.colorCount() > 0 ? colorFormatOf(target.colorFormat(0)) : VK_FORMAT_UNDEFINED;
    const VkFormat depth  = depthFormatOf(target.depthFormat());

    TargetImages& images = impl->targets[&target];
    if (images.built() && images.matches(width, height, color, depth)) {
        return true;
    }

    // A RESIZE IS A REBUILD in M1: `planTarget`'s ResizeInPlace (M3) is the step that keeps the render pass and re-points
    // the views; until then the honest answer is to build the objects that fit and drop the ones that do not.
    // Pipelines are built against THIS render pass, so they go with it.
    impl->content.releaseRenderPass(images.render_pass);
    destroyTarget(impl->device_table, impl->device, images);
    images.width        = width;
    images.height       = height;
    images.color_format = color;
    images.depth_format = depth;
    if (!buildTarget(impl->device_table, impl->device, impl->memory, images)) {
        reportDiagnostic(DiagnosticSeverity::Error,
                         DiagnosticCategory::TargetBuildFailed,
                         u8"vk: the off-screen target's attachments could not be built (see the loader's report)");
        return false;
    }
    return true;
}

Capabilities Device::capabilities() const noexcept
{
    return impl->caps;
}

PresentOutcome Device::executeFrame(const FrameDescription& frame, PresentTarget* present_to)
{
    (void)present_to;
    if (!initialized()) {
        reportDiagnostic(DiagnosticSeverity::Error,
                         DiagnosticCategory::SubmissionFailed,
                         u8"vk: a frame was handed to a device that is not up");
        return PresentOutcome::Skipped;
    }

    // WHICH PASSES THIS BACKEND CAN RUN TODAY: the ones that name an off-screen target and state a clear. Everything
    // else is reported once (a window pass is M5, a LOAD pass needs the M3 variant) instead of being drawn wrong.
    std::vector<const PassDraw*> clear_passes;
    clear_passes.reserve(frame.passes.size());
    for (const FrameDescription::Pass& entry : frame.passes) {
        if (!entry.draw.has_value()) {
            continue; // a pass that issued nothing: the scope opens and closes, nothing to record
        }
        const PassDraw& draw = *entry.draw;
        if (draw.target == nullptr) {
            if (!impl->reported_window) {
                impl->reported_window = true;
                reportDiagnostic(DiagnosticSeverity::Warning,
                                 DiagnosticCategory::UnsupportedRequest,
                                 u8"vk: a pass drawing into the window is not served yet (M5); it was left out");
            }
            continue;
        }        if (!draw.clear.has_value()) {
            if (!impl->reported_no_clear) {
                impl->reported_no_clear = true;
                reportDiagnostic(DiagnosticSeverity::Warning,
                                 DiagnosticCategory::UnsupportedRequest,
                                 u8"vk: a pass that loads its target instead of clearing it needs the LOAD variant (M3); "
                                 u8"it was left out");
            }
            continue;
        }
        if (!ensureTarget(*draw.target)) {
            continue;
        }
        clear_passes.push_back(&draw);
    }

    if (clear_passes.empty()) {
        if (!impl->reported_no_targets) {
            impl->reported_no_targets = true;
            reportDiagnostic(DiagnosticSeverity::Info,
                             DiagnosticCategory::UnsupportedRequest,
                             u8"vk: the frame had no pass this backend can run yet (M1 serves off-screen targets with a "
                             u8"clear policy); nothing was submitted");
        }
        return PresentOutcome::Skipped;
    }

    // ONE COMMAND BUFFER, ONE FENCE, ONE WAIT. M1 is a learning step: the frame is submitted and finished before the call
    // returns, which is what makes "read the pixels right after" mean what it says.
    VkCommandBufferAllocateInfo allocate{};
    allocate.sType              = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    allocate.commandPool        = impl->command_pool;
    allocate.level              = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    allocate.commandBufferCount = 1U;

    VkCommandBuffer commands{ VK_NULL_HANDLE };
    if (impl->device_table.vkAllocateCommandBuffers(impl->device, &allocate, &commands) != VK_SUCCESS) {
        reportDiagnostic(DiagnosticSeverity::Error, DiagnosticCategory::SubmissionFailed, u8"vk: no command buffer");
        return PresentOutcome::Skipped;
    }

    VkCommandBufferBeginInfo begin{};
    begin.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    if (impl->device_table.vkBeginCommandBuffer(commands, &begin) != VK_SUCCESS) {
        reportDiagnostic(DiagnosticSeverity::Error, DiagnosticCategory::SubmissionFailed, u8"vk: vkBeginCommandBuffer failed");
        impl->device_table.vkFreeCommandBuffers(impl->device, impl->command_pool, 1U, &commands);
        return PresentOutcome::Skipped;
    }

    // ONE DESCRIPTOR POOL RESET PER FRAME: the pool's sets are the per-draw uniform bindings, and the previous frame
    // is complete by now (the device submits synchronously), so nothing still names them.
    impl->content.beginFrame();

    for (const PassDraw* draw : clear_passes) {
        const TargetImages& images = impl->targets.at(draw->target);

        VkClearValue clear_values[2]{};
        const Colorf& colour    = draw->clear->color;
        clear_values[0].color   = { { colour.r, colour.g, colour.b, colour.a } };
        // Reverse-Z: every backend clears depth to its own far plane, and this camera convention's far plane is 0
        // (see ClearPolicy: the policy states WHETHER, never to what).
        clear_values[1].depthStencil = { 0.0F, 0U };

        VkRenderPassBeginInfo pass_begin{};
        pass_begin.sType             = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
        pass_begin.renderPass        = images.render_pass;
        pass_begin.framebuffer       = images.framebuffer;
        pass_begin.renderArea.offset = VkOffset2D{ 0, 0 };
        pass_begin.renderArea.extent = VkExtent2D{ static_cast<std::uint32_t>(images.width), static_cast<std::uint32_t>(images.height) };
        pass_begin.clearValueCount   = images.depth_format != VK_FORMAT_UNDEFINED ? 2U : 1U;
        pass_begin.pClearValues      = clear_values;

        impl->device_table.vkCmdBeginRenderPass(commands, &pass_begin, VK_SUBPASS_CONTENTS_INLINE);
        // WHAT THIS PASS DRAWS. Each draw description is borrowed for this call; every command inside it becomes one
        // drawing call (M2: one `RenderCommand` = one pipeline bind + one indexed draw).
        for (const DrawDescription& description : draw->draws) {
            vn::String                      why;
            vn::graphics::DiagnosticCategory category = DiagnosticCategory::ContentSkipped;
            (void)impl->content.record(commands, images.render_pass, images.width, images.height, description, *draw, impl->default_program.get(), &why, &category);
            if (!why.empty() && !impl->reported_content) {
                impl->reported_content = true;
                reportDiagnostic(DiagnosticSeverity::Warning, category, why);
            }
        }
        impl->device_table.vkCmdEndRenderPass(commands);
    }

    if (impl->device_table.vkEndCommandBuffer(commands) != VK_SUCCESS) {
        reportDiagnostic(DiagnosticSeverity::Error, DiagnosticCategory::SubmissionFailed, u8"vk: vkEndCommandBuffer failed");
        impl->content.endFrame();
        impl->device_table.vkFreeCommandBuffers(impl->device, impl->command_pool, 1U, &commands);
        return PresentOutcome::Skipped;
    }

    VkSubmitInfo submit{};
    submit.sType              = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    submit.commandBufferCount = 1U;
    submit.pCommandBuffers    = &commands;

    const bool submitted = impl->device_table.vkQueueSubmit(impl->queue, 1U, &submit, impl->completion) == VK_SUCCESS &&
                           impl->device_table.vkWaitForFences(impl->device, 1U, &impl->completion, VK_TRUE, UINT64_MAX) == VK_SUCCESS;
    (void)impl->device_table.vkResetFences(impl->device, 1U, &impl->completion);
    impl->device_table.vkFreeCommandBuffers(impl->device, impl->command_pool, 1U, &commands);
    // The per-draw uniform buffers were named by the command buffer that just finished (or failed to): the queue is
    // idle either way, so the bytes can go.
    impl->content.endFrame();

    if (!submitted) {
        reportDiagnostic(DiagnosticSeverity::Error,
                         DiagnosticCategory::SubmissionFailed,
                         u8"vk: the frame's command buffer did not run to completion");
    }
    return PresentOutcome::Skipped;
}

bool Device::readColorBuffer(const RenderTarget* target, int attachment, std::vector<std::uint8_t>& outPixels, ReadbackResult* why)
{
    const auto fail = [why](const ReadbackResult reason) {
        if (why != nullptr) {
            *why = reason;
        }
        return false;
    };

    if (why != nullptr) {
        *why = ReadbackResult::Ok;
    }
    if (!initialized()) {
        return fail(ReadbackResult::Failed);
    }
    if (target == nullptr) {
        return fail(ReadbackResult::Invalid);
    }
    if (attachment != 0) {
        return fail(ReadbackResult::Invalid); // M1 builds one colour attachment (see Capabilities::max_color_attachments)
    }

    const auto found = impl->targets.find(target);
    if (found == impl->targets.end() || !found->second.built()) {
        return fail(ReadbackResult::NotReady);
    }
    const TargetImages& images = found->second;
    if (images.color_format != VK_FORMAT_R8G8B8A8_UNORM) {
        // The readback contract is packed RGBA8; another colour format would need a conversion this step does not write.
        return fail(ReadbackResult::Unsupported);
    }

    const VkDeviceSize bytes = static_cast<VkDeviceSize>(images.width) * static_cast<VkDeviceSize>(images.height) * 4U;

    VkBufferCreateInfo buffer_info{};
    buffer_info.sType       = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    buffer_info.size        = bytes;
    buffer_info.usage       = VK_BUFFER_USAGE_TRANSFER_DST_BIT;
    buffer_info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;

    VkBuffer staging{ VK_NULL_HANDLE };
    if (impl->device_table.vkCreateBuffer(impl->device, &buffer_info, nullptr, &staging) != VK_SUCCESS) {
        return fail(ReadbackResult::Failed);
    }

    VkMemoryRequirements requirements{};
    impl->device_table.vkGetBufferMemoryRequirements(impl->device, staging, &requirements);

    std::uint32_t memory_type = std::numeric_limits<std::uint32_t>::max();
    for (std::uint32_t index = 0; index < impl->memory.memoryTypeCount; ++index) {
        const VkMemoryPropertyFlags flags = impl->memory.memoryTypes[index].propertyFlags;
        const bool allowed                = (requirements.memoryTypeBits & (1U << index)) != 0U;
        if (allowed && (flags & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT) != 0U && (flags & VK_MEMORY_PROPERTY_HOST_COHERENT_BIT) != 0U) {
            memory_type = index;
            break;
        }
    }

    VkDeviceMemory staging_memory{ VK_NULL_HANDLE };
    if (memory_type == std::numeric_limits<std::uint32_t>::max()) {
        impl->device_table.vkDestroyBuffer(impl->device, staging, nullptr);
        return fail(ReadbackResult::Unsupported);
    }

    VkMemoryAllocateInfo allocate{};
    allocate.sType           = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    allocate.allocationSize  = requirements.size;
    allocate.memoryTypeIndex = memory_type;
    if (impl->device_table.vkAllocateMemory(impl->device, &allocate, nullptr, &staging_memory) != VK_SUCCESS ||
        impl->device_table.vkBindBufferMemory(impl->device, staging, staging_memory, 0U) != VK_SUCCESS) {
        if (staging_memory != VK_NULL_HANDLE) {
            impl->device_table.vkFreeMemory(impl->device, staging_memory, nullptr);
        }
        impl->device_table.vkDestroyBuffer(impl->device, staging, nullptr);
        return fail(ReadbackResult::Failed);
    }

    // RECORD: colour attachment -> transfer source -> copy -> back, so the next frame's render pass finds the layout its
    // render pass was built with (COLOR_ATTACHMENT_OPTIMAL).
    VkCommandBufferAllocateInfo command_allocate{};
    command_allocate.sType              = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    command_allocate.commandPool        = impl->command_pool;
    command_allocate.level              = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    command_allocate.commandBufferCount = 1U;

    VkCommandBuffer commands{ VK_NULL_HANDLE };
    const auto      release = [&] {
        if (commands != VK_NULL_HANDLE) {
            impl->device_table.vkFreeCommandBuffers(impl->device, impl->command_pool, 1U, &commands);
        }
        impl->device_table.vkFreeMemory(impl->device, staging_memory, nullptr);
        impl->device_table.vkDestroyBuffer(impl->device, staging, nullptr);
    };

    if (impl->device_table.vkAllocateCommandBuffers(impl->device, &command_allocate, &commands) != VK_SUCCESS) {
        release();
        return fail(ReadbackResult::Failed);
    }

    VkCommandBufferBeginInfo begin{};
    begin.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    if (impl->device_table.vkBeginCommandBuffer(commands, &begin) != VK_SUCCESS) {
        release();
        return fail(ReadbackResult::Failed);
    }

    const VkImageMemoryBarrier to_transfer =
        barrierBetween(images.colors[0], stateOf(ImageLayout::ColorAttachment), transferReadState(), VK_IMAGE_ASPECT_COLOR_BIT);
    impl->device_table.vkCmdPipelineBarrier(commands,
                                            VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
                                            VK_PIPELINE_STAGE_TRANSFER_BIT,
                                            0U,
                                            0U,
                                            nullptr,
                                            0U,
                                            nullptr,
                                            1U,
                                            &to_transfer);

    VkBufferImageCopy region{};
    region.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    region.imageSubresource.layerCount = 1U;
    region.imageExtent                 = VkExtent3D{ static_cast<std::uint32_t>(images.width),
                                                      static_cast<std::uint32_t>(images.height), 1U };
    impl->device_table.vkCmdCopyImageToBuffer(commands, images.colors[0], VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, staging, 1U, &region);

    const VkImageMemoryBarrier to_attachment =
        barrierBetween(images.colors[0], transferReadState(), stateOf(ImageLayout::ColorAttachment), VK_IMAGE_ASPECT_COLOR_BIT);
    impl->device_table.vkCmdPipelineBarrier(commands,
                                            VK_PIPELINE_STAGE_TRANSFER_BIT,
                                            VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
                                            0U,
                                            0U,
                                            nullptr,
                                            0U,
                                            nullptr,
                                            1U,
                                            &to_attachment);

    if (impl->device_table.vkEndCommandBuffer(commands) != VK_SUCCESS) {
        release();
        return fail(ReadbackResult::Failed);
    }

    VkSubmitInfo submit{};
    submit.sType              = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    submit.commandBufferCount = 1U;
    submit.pCommandBuffers    = &commands;

    const bool ran = impl->device_table.vkQueueSubmit(impl->queue, 1U, &submit, impl->completion) == VK_SUCCESS &&
                     impl->device_table.vkWaitForFences(impl->device, 1U, &impl->completion, VK_TRUE, UINT64_MAX) == VK_SUCCESS;
    (void)impl->device_table.vkResetFences(impl->device, 1U, &impl->completion);

    if (!ran) {
        release();
        return fail(ReadbackResult::Failed);
    }

    void* mapped = nullptr;
    if (impl->device_table.vkMapMemory(impl->device, staging_memory, 0U, bytes, 0U, &mapped) != VK_SUCCESS || mapped == nullptr) {
        release();
        return fail(ReadbackResult::Failed);
    }

    outPixels.resize(static_cast<std::size_t>(bytes));
    std::memcpy(outPixels.data(), mapped, static_cast<std::size_t>(bytes));
    impl->device_table.vkUnmapMemory(impl->device, staging_memory);

    release();
    return true;
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

VN_BACKEND_VK_NS_END
