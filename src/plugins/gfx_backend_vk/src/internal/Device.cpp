#include <vine/backend_vk/internal/Device.hpp>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <unordered_map>
#include <vector>

// volk is built with VOLK_NO_DEVICE_PROTOTYPES (see third_party/volk/CMakeLists.txt): device-level commands are not
// declared as free functions, and `VolkDeviceTable` is the supported way to reach them - one table filled per device.
#include <volk.h>

#include <vine/backend_vk/internal/BarrierPlan.hpp>
#include <vine/backend_vk/internal/ContentDraw.hpp>
#include <vine/backend_vk/internal/ContentImages.hpp>
#include <vine/backend_vk/internal/WindowSystem.hpp>
#include <vine/backend_vk/internal/StateMapping.hpp>
#include <vine/backend_vk/internal/TargetImages.hpp>
#include <vine/backend_vk/internal/WindowTarget.hpp>
#include <vine/graphics/Capabilities.hpp>
#include <vine/graphics/FrameDescription.hpp>
#include <vine/graphics/FrameStats.hpp>
#include <vine/graphics/Geometry.hpp>
#include <vine/graphics/MaterialBase.hpp>
#include <vine/graphics/PrepareReport.hpp>
#include <vine/graphics/PresentTarget.hpp>
#include <vine/graphics/RenderTarget.hpp>
#include <vine/graphics/ShaderProgram.hpp>
#include <vine/graphics/Texture.hpp>
#include <vine/graphics/backend/Readback.hpp>

VN_BACKEND_VK_NS_BEGIN

using vn::graphics::Capabilities;
using vn::graphics::DiagnosticCategory;
using vn::graphics::DiagnosticSeverity;
using vn::graphics::DeviceDesc;
using vn::graphics::DevicePresentation;
using vn::graphics::DrawDescription;
using vn::graphics::FrameDescription;
using vn::graphics::ImageSource;
using vn::graphics::PassDraw;
using vn::graphics::PresentOutcome;
using vn::graphics::PresentTarget;
using vn::graphics::ReadbackResult;
using vn::graphics::RenderTarget;
using vn::graphics::SurfaceTargetResult;
using vn::graphics::backend::decodeDepth;
using vn::graphics::backend::depthReadbackOf;
using vn::graphics::backend::ImageLayout;
using vn::graphics::backend::planTarget;
using vn::graphics::backend::RepairReason;
using vn::graphics::backend::TargetAction;
using vn::graphics::backend::TargetDecision;
using vn::graphics::backend::TargetDesc;
using vn::graphics::backend::TargetInstance;

namespace
{

/// @brief The layer this backend asks for whenever the loader has it (see Device::initialize).
constexpr const char* kValidationLayerName = "VK_LAYER_KHRONOS_validation";

/// @brief The size a Probe device's own window comes up at (the reference backend's own default: 640x360).
constexpr int kProbeWindowWidth  = 640;
constexpr int kProbeWindowHeight = 360;

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

/// @brief The sample counts Vulkan offers, largest first (see largestSampleCount / ensureBuildableSamples).
constexpr std::uint32_t kSampleCounts[] = { 64U, 32U, 16U, 8U, 4U, 2U, 1U };

/**
 * @brief Whether @p samples is a count this device can build a target with.
 *
 * @details
 * - A COUNT IS A COUNT (see ADR-2026-11-14-sample-count-refused-not-lowered): the legal counts are the powers of two,
 *   so a number between two of them (three, say) is a count the device does not offer at all - mapping it through
 *   `StateMapping::sampleCount` would silently answer one sample, which is the same lie as lowering an over-large
 *   request. `Capabilities::max_samples` plus "the counts are powers of two" names the whole set, and this is where
 *   that set is checked, once, before any object is made.
 *
 * @param samples Samples per pixel a target asks for (already normalised: 0 is not a count).
 * @param supported The device's `framebufferColorSampleCounts & framebufferDepthSampleCounts` flags.
 * @return true when @p samples is a power of two the @p supported flags contain.
 */
[[nodiscard]] bool ensureBuildableSamples(const std::uint32_t samples, const std::uint32_t supported) noexcept
{
    if (samples == 0U || (samples & (samples - 1U)) != 0U) {
        return false;
    }
    return (supported & static_cast<std::uint32_t>(StateMapping::sampleCount(samples))) != 0U;
}

/**
 * @brief Gets the largest sample count @p supported flags offer.
 *
 * @param supported The device's `framebufferColorSampleCounts & framebufferDepthSampleCounts` flags.
 * @return The largest supported count; 1 when the flags offer nothing usable.
 */
[[nodiscard]] std::uint32_t largestSampleCount(const std::uint32_t supported) noexcept
{
    for (const std::uint32_t count : kSampleCounts) {
        if ((supported & static_cast<std::uint32_t>(StateMapping::sampleCount(count))) != 0U) {
            return count;
        }
    }
    return 1U;
}

/**
 * @brief Builds a message fragment from a count.
 *
 * @details
 * - A diagnostic that names the numbers a host can act on ("asks for 8, this device offers 4") is worth more than one
 *   that only says "too many", and `vn::String` holds UTF-8 while `std::to_string` answers UTF-8 bytes, so the
 *   conversion is a copy.
 *
 * @param value The number to spell.
 * @return The number as text.
 */
[[nodiscard]] vn::String numberText(const std::uint32_t value)
{
    return vn::String::fromUtf8(std::to_string(value));
}

/**
 * @brief Whether @p description is a DRAWING CALL at all (see FrameCounters::draws).
 *
 * @details
 * - A drawing call is one `DrawDescription` a backend can issue, NOT the GPU commands inside it: one drawable is one
 *   call whatever its primitives (the reference recorder counts it the same way, which is what makes the two frame
 *   reports comparable - see the parity suite).
 * - The conditions are the SDK's own: a call with no view is no call at all, a screen call without a program draws
 *   nothing, and a content call with an empty list has nothing to draw.
 *
 * @param description The drawing call as the frame describes it.
 * @return true when it is a call the record counts.
 */
[[nodiscard]] bool isDrawingCall(const vn::graphics::DrawDescription& description) noexcept
{
    if (!description.camera.present) {
        return false;
    }
    if (description.kind == vn::graphics::DrawDescription::Kind::Screen) {
        return description.program != nullptr || description.program_description != nullptr;
    }
    return description.commands != nullptr && !description.commands->empty();
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

    /// The sample counts the device's COLOUR and DEPTH attachments both offer, as Vulkan spells them
    /// (`VK_SAMPLE_COUNT_*_BIT`): one target has one sample count, so a count either attachment cannot do is a count
    /// this device cannot build (see `ensureTarget`).
    std::uint32_t supported_sample_counts{ VK_SAMPLE_COUNT_1_BIT };

    /// Whether this device can resolve a multisampled DEPTH (`VkSubpassDescriptionDepthStencilResolve`, and with it
    /// `vkCreateRenderPass2`): true from Vulkan 1.2, which is the version that made both core. A device below it still
    /// serves every other shape - only "multisampled AND depth-attached" is refused (see `ensureTarget`).
    bool depth_resolve_supported{ false };

    /// The command machinery every frame and every readback borrows: one pool, one fence, one buffer at a time.
    VkCommandPool command_pool{ VK_NULL_HANDLE };
    VkFence       completion{ VK_NULL_HANDLE };

    /// One entry per off-screen target this device built, keyed by the host's object (the identity the frame names).
    std::unordered_map<const RenderTarget*, TargetImages> targets{};

    /// M2's content path: compiled programs, uploaded streams, pipelines and the per-frame uniform buffers.
    ContentDraw content{};
    /// The program content without one of its own is drawn with, taken as a VALUE when it was announced.
    vn::graphics::ShaderProgramDescriptionPtr default_program{};

    /// The windows this device presents through (one per host handle; owned here, borrowed by the host).
    std::vector<std::unique_ptr<WindowTarget>> surfaces;
    /// The window system this device's surfaces are created through (one per session, owned here; the connection
    /// outlives every window target made through it).
    std::unique_ptr<WindowSystem> window_system;
    /// Whether this session was opened with a presentation intent (a window half exists).
    bool presentation_wanted{ false };

    /// One report each for the answers M1 cannot serve, so a host is told once rather than per frame.
    bool reported_no_targets{ false };
    bool reported_surface{ false };
    bool reported_content{ false };
    bool reported_size_unknown{ false };
    bool reported_samples{ false };
    bool reported_samples_depth{ false };
    bool reported_prepare_target{ false };

    /**
     * @brief What the last frame did and what this device has done so far (see FrameReport).
     *
     * @details
     * - `counters` / `uploads` describe the frame that just ended, `progress` / `released_nodes` are cumulative.
     * - `uploads` stays disengaged until a frame has run: an unmeasured frame must not read like an empty one.
     * - `device_waits` is 0 by construction: a release needs no idle here, because every frame ends having waited
     *   on its own fence, so between frames nothing is in flight.
     */
    struct Books {
        vn::graphics::FrameCounters               counters{};
        vn::graphics::FrameProgress               progress{};
        std::optional<vn::graphics::FrameUploads> uploads;
        std::uint64_t                             released_nodes{ 0 };
        std::uint64_t                             device_waits{ 0 };
    };
    Books books{};

    /** @brief One snapshot of the content path's cumulative numbers (a frame's own delta is the difference of two). */
    struct ContentNumbers {
        std::uint64_t stream_builds{ 0 };
        std::uint64_t stream_bytes{ 0 };
        std::uint64_t program_builds{ 0 };
        std::uint64_t pipeline_builds{ 0 };
        std::uint64_t texture_builds{ 0 };
        std::uint64_t texture_bytes{ 0 };
    };

    /** @brief Reads the content path's cumulative numbers (see ContentNumbers). */
    [[nodiscard]] ContentNumbers contentNumbers() const noexcept
    {
        const ContentDraw::Progress   content_numbers = content.progress();
        const ContentImages::Progress image_numbers   = content.images().progress();
        ContentNumbers                numbers;
        numbers.stream_builds    = content_numbers.stream_builds;
        numbers.stream_bytes     = content_numbers.stream_bytes;
        numbers.program_builds   = content_numbers.program_builds;
        numbers.pipeline_builds  = content_numbers.pipeline_builds;
        numbers.texture_builds   = image_numbers.builds;
        numbers.texture_bytes    = image_numbers.bytes;
        return numbers;
    }

    /**
     * @brief Closes one frame's books: what the frame built is the difference from @p before.
     *
     * @param before   The numbers when the frame opened.
     * @param counters The frame's own recording counters (passes, draws).
     */
    void closeFrameBooks(const ContentNumbers& before, const vn::graphics::FrameCounters& counters) noexcept
    {
        const ContentNumbers after = contentNumbers();
        books.counters             = counters;
        // `stream_releases` / `texture_releases` stay 0 for a frame: this device only lets go when a host says so,
        // which happens BETWEEN frames - and an explicit release is an announcement, not a frame's own number (see
        // FrameUploads). A texture a new revision replaced is a BUILD (it never stopped being resident).
        vn::graphics::FrameUploads frame_uploads;
        frame_uploads.stream_uploads  = after.stream_builds - before.stream_builds;
        frame_uploads.stream_bytes    = after.stream_bytes - before.stream_bytes;
        frame_uploads.texture_uploads = after.texture_builds - before.texture_builds;
        frame_uploads.texture_bytes   = after.texture_bytes - before.texture_bytes;
        books.uploads = frame_uploads;
    }

    /**
     * @brief Resolves the SHADOW MAP a pass samples out of what it declared, and makes the map readable.
     *
     * @details
     * - WHICH INPUT: the first one whose TARGET says it is a shadow map (`RenderTarget::shadowOf`), whose depth this
     *   device actually built and whose host left promotable, and whose producer published how to read it
     *   (`hasProducerViewProjection`) - the SDK's rule (see `ShadowFacts`), restated here because this backend consumes
     *   the frame value directly rather than going through the shared compiler. A G-buffer's depth is not a shadow: a
     *   target nobody stated a light for is skipped, which is the defect the rule exists to prevent.
     * - WHAT IT DOES BESIDES ANSWER: moves the map's depth image to `SHADER_READ_ONLY_OPTIMAL` before the consuming
     *   pass begins. A barrier is not legal inside a render pass instance, and the map was left in the attachment
     *   layout by the pass that wrote it - so this is the one moment the transition can be recorded.
     *
     * @param commands The command buffer being recorded (OUTSIDE any render pass).
     * @param inputs   The pass' resolved inputs, in declaration order.
     * @param out      Receives the binding; empty (`view == VK_NULL_HANDLE`) when the pass samples no readable map.
     * @return true when a map was resolved (and transitioned).
     */
    [[nodiscard]] bool resolveShadow(VkCommandBuffer commands, std::span<const ImageSource> inputs, ContentDraw::ShadowSource& out)
    {
        for (const ImageSource& input : inputs) {
            if (input.target == nullptr || !input.isProduced()) {
                continue;
            }
            const RenderTarget& map = *input.target;
            const auto          images = targets.find(&map);
            if (images == targets.end() || !images->second.built() || !images->second.hasDepth()) {
                continue;
            }
            const vn::raw_ptr<const vn::graphics::Light> light = map.shadowOf();
            if (light == nullptr || !map.hasProducerViewProjection()) {
                continue; // not a map, or a map nobody said how to read (shading nothing is the honest answer)
            }
            if (!map.isDepthPromoted()) {
                // The host said this target's depth is NOT to be read as a texture (RenderTarget::setDepthPromotion):
                // binding it anyway would serve a picture the host declared it does not want, and the reference backend
                // refuses the same map for the same reason (`DepthFacts` / `depthPlan().sampleable`).
                continue;
            }

            // THE MAP IS ABOUT TO BE READ: one transition, from the attachment layout the producer left it in (or
            // UNDEFINED when nobody wrote it yet) to the sampled one. IT IS THE PICTURE that moves - on a
            // multisampled map that is the resolved depth image, which is also the only one a shader may sample.
            TargetImages& sampled = images->second;
            if (!sampled.depth_sampled) {
                const ImageState from = stateOf(sampled.fresh ? ImageLayout::Undefined : ImageLayout::DepthAttachment);
                const VkImageMemoryBarrier barrier =
                    barrierBetween(sampled.depthPicture(), from, stateOf(ImageLayout::ShaderReadOnly), depthAspectMask(sampled.depthFormat()));
                device_table.vkCmdPipelineBarrier(commands,
                                                  VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT,
                                                  VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
                                                  0U,
                                                  0U,
                                                  nullptr,
                                                  0U,
                                                  nullptr,
                                                  1U,
                                                  &barrier);
                sampled.depth_sampled = true;
            }

            out.view                  = sampled.depthPictureView();
            out.has_view_projection   = true;
            out.view_projection       = map.producerViewProjection();
            out.light                 = light;
            return true;
        }
        return false;
    }

    /** @brief Notes this backend's three residency kinds into @p ledger (see frameReport). */
    void noteResidency(vn::graphics::ResidencyLedger& ledger) const
    {
        const ContentDraw::Progress   content_numbers = content.progress();
        const ContentImages::Progress image_numbers   = content.images().progress();
        ledger.note(vn::graphics::ResidencyRow{ .kind    = vn::graphics::ResidencyKind::Streams,
                                                .live    = content_numbers.streams_live,
                                                .bytes   = content_numbers.stream_bytes_live,
                                                .built   = content_numbers.stream_builds,
                                                .retired = content_numbers.stream_releases });
        ledger.note(vn::graphics::ResidencyRow{ .kind    = vn::graphics::ResidencyKind::Images,
                                                .live    = image_numbers.live,
                                                .bytes   = image_numbers.live_bytes,
                                                .built   = image_numbers.builds,
                                                .retired = image_numbers.releases });
        ledger.note(vn::graphics::ResidencyRow{ .kind    = vn::graphics::ResidencyKind::Halves,
                                                .live    = content_numbers.pipelines_live,
                                                .built   = content_numbers.pipeline_builds,
                                                .retired = content_numbers.pipeline_releases });
    }
};

Device::Device(const DeviceDesc& desc)
  : impl(std::make_unique<Impl>())
{
    impl->desc = desc;
    // THE CREATION INTENT IS ANSWERED BEFORE ANY SESSION EXISTS: `capabilities().presentation` is "does this device
    // present" (see RenderDevice), and a host reads it before it drives a frame. A session that turns out to serve
    // nothing (no surface extension, a window that would not come up) says so when initialize() answers.
    impl->caps.presentation = desc.presentation != DevicePresentation::None;
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
    // still comes up (running unvalidated beats not running: a missing layer is a warning, not a failure).
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
    bool          has_surface_extension  = false;
    bool          has_platform_extension = false;
    const char*   platform_extension     = WindowSystem::instanceExtension();
    if (vkEnumerateInstanceExtensionProperties(nullptr, &extension_count, nullptr) == VK_SUCCESS && extension_count > 0U) {
        std::vector<VkExtensionProperties> extensions(extension_count);
        if (vkEnumerateInstanceExtensionProperties(nullptr, &extension_count, extensions.data()) == VK_SUCCESS) {
            for (const VkExtensionProperties& extension : extensions) {
                if (std::strcmp(extension.extensionName, VK_EXT_DEBUG_UTILS_EXTENSION_NAME) == 0) {
                    has_debug_utils = true;
                }
                if (std::strcmp(extension.extensionName, VK_KHR_SURFACE_EXTENSION_NAME) == 0) {
                    has_surface_extension = true;
                }
                if (platform_extension[0] != '\0' && std::strcmp(extension.extensionName, platform_extension) == 0) {
                    has_platform_extension = true;
                }
            }
        }
    }

    // THE WINDOW HALF'S CREATION INTENT. Handle names the host's window (a wiring error otherwise); Probe's own
    // window is brought up HERE, before the instance exists, because the surface is created on it and the physical
    // device is judged against that surface (see DevicePresentation). A missing surface extension is the same answer:
    // this machine cannot present, and the off-screen path still runs whole frames.
    impl->presentation_wanted = impl->desc.presentation != DevicePresentation::None;
    if (impl->presentation_wanted) {
        vn::String why;
        if (!ensureWindowSystem(&why)) {
            // A Probe device PROMISED a window (it is what the creation input says), so not having one is a failed
            // session rather than a quiet degradation - a host that wants the off-screen form asks for None. A
            // Handle device keeps its session: the off-screen path still runs whole frames.
            const bool probe = impl->desc.presentation == DevicePresentation::Probe;
            reportDiagnostic(probe ? DiagnosticSeverity::Error : DiagnosticSeverity::Warning,
                             probe ? DiagnosticCategory::InitFailed : DiagnosticCategory::UnsupportedRequest,
                             why.empty() ? vn::String(u8"vk: no window system is available, so this device presents nothing") : std::move(why));
            if (probe) {
                shutdown();
                return false;
            }
            impl->presentation_wanted = false;
        }
    }
    if (impl->presentation_wanted && impl->desc.presentation == DevicePresentation::Probe && impl->surfaces.empty()) {
        vn::String why;
        if (!impl->window_system->openOwnWindow(kProbeWindowWidth, kProbeWindowHeight, &why)) {
            reportDiagnostic(DiagnosticSeverity::Error,
                             DiagnosticCategory::InitFailed,
                             why.empty() ? vn::String(u8"vk: the device-owned (Probe) window could not be brought up") : std::move(why));
            shutdown();
            return false;
        }
        impl->surfaces.push_back(std::make_unique<WindowTarget>(*impl->window_system, impl->window_system->ownWindow()));
    }
    if (impl->presentation_wanted && impl->desc.presentation == DevicePresentation::Handle && impl->desc.native_handle == nullptr &&
        impl->surfaces.empty()) {
        reportDiagnostic(DiagnosticSeverity::Error,
                         DiagnosticCategory::UnsupportedRequest,
                         u8"vk: DevicePresentation::Handle needs a native window handle; this device presents nothing");
        impl->presentation_wanted = false;
    }
    if (impl->presentation_wanted && (!has_surface_extension || !has_platform_extension)) {
        reportDiagnostic(DiagnosticSeverity::Warning,
                         DiagnosticCategory::UnsupportedRequest,
                         u8"vk: the instance does not offer the surface extensions a window needs; this device presents nothing");
        impl->presentation_wanted = false;
    }
    if (impl->presentation_wanted && impl->desc.presentation == DevicePresentation::Handle && impl->surfaces.empty()) {
        // The SDK's Handle form names the window as a CREATION input; the target object exists before the instance so
        // the surface can be created as soon as it does.
        impl->surfaces.push_back(std::make_unique<WindowTarget>(*impl->window_system, impl->desc.native_handle));
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
    const char*   extension_names[3]{};
    std::uint32_t extension_name_count = 0;
    if (want_validation) {
        layer_names[layer_name_count++] = kValidationLayerName;
    }
    if (want_debug) {
        extension_names[extension_name_count++] = VK_EXT_DEBUG_UTILS_EXTENSION_NAME;
    }
    if (impl->presentation_wanted) {
        extension_names[extension_name_count++] = VK_KHR_SURFACE_EXTENSION_NAME;
        extension_names[extension_name_count++] = platform_extension;
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

    // THE SURFACES, while the instance is the newest object: a Vulkan surface is created on the instance and is what
    // the physical device is judged against, which is why it exists before the device is picked.
    if (impl->presentation_wanted) {
        for (const auto& surface : impl->surfaces) {
            vn::String why;
            if (!surface->createSurface(impl->instance, &why)) {
                reportDiagnostic(DiagnosticSeverity::Warning,
                                 DiagnosticCategory::UnsupportedRequest,
                                 why.empty() ? vn::String(u8"vk: the host window's surface could not be created") : std::move(why));
                impl->presentation_wanted = false;
                break;
            }
        }
    }

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

    const VkSurfaceKHR presentation_surface =
        impl->presentation_wanted && !impl->surfaces.empty() ? impl->surfaces.front()->surfaceHandle() : VK_NULL_HANDLE;

    for (const VkPhysicalDevice candidate : physical_devices) {
        std::uint32_t family_count = 0;
        vkGetPhysicalDeviceQueueFamilyProperties(candidate, &family_count, nullptr);
        if (family_count == 0U) {
            continue;
        }
        std::vector<VkQueueFamilyProperties> families(family_count);
        vkGetPhysicalDeviceQueueFamilyProperties(candidate, &family_count, families.data());
        for (std::uint32_t index = 0; index < family_count; ++index) {
            if ((families[index].queueFlags & VK_QUEUE_GRAPHICS_BIT) == 0U) {
                continue;
            }
            if (presentation_surface != VK_NULL_HANDLE) {
                VkBool32 present_supported = VK_FALSE;
                if (vkGetPhysicalDeviceSurfaceSupportKHR(candidate, index, presentation_surface, &present_supported) != VK_SUCCESS ||
                    present_supported == VK_FALSE) {
                    continue;
                }
            }
            impl->physical     = candidate;
            impl->queue_family = index;
            break;
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

    // THE SWAPCHAIN EXTENSION: only when a window is to be served, and only if the device offers it.
    const char* device_extension_names[1]{ "VK_KHR_swapchain" };
    if (impl->presentation_wanted) {
        std::uint32_t available = 0;
        bool          found     = false;
        if (vkEnumerateDeviceExtensionProperties(impl->physical, nullptr, &available, nullptr) == VK_SUCCESS && available > 0U) {
            std::vector<VkExtensionProperties> extensions(available);
            if (vkEnumerateDeviceExtensionProperties(impl->physical, nullptr, &available, extensions.data()) == VK_SUCCESS) {
                for (const VkExtensionProperties& extension : extensions) {
                    if (std::strcmp(extension.extensionName, "VK_KHR_swapchain") == 0) {
                        found = true;
                        break;
                    }
                }
            }
        }
        if (!found) {
            reportDiagnostic(DiagnosticSeverity::Warning,
                             DiagnosticCategory::UnsupportedRequest,
                             u8"vk: the device does not offer VK_KHR_swapchain; this device presents nothing");
            impl->presentation_wanted = false;
        }
    }
    if (impl->presentation_wanted) {
        device_info.enabledExtensionCount   = 1U;
        device_info.ppEnabledExtensionNames = device_extension_names;
    }

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

    // THE TIMESLICE WHERE THE SWAPCHAINS ARE MADE: the device (and therefore its tables) exist, so each window's
    // surface can now be paired with a swapchain. A target that cannot be attached stays detached: the host still
    // holds the object it was given, and every later call answers the truth instead of touching a dead surface.
    if (impl->presentation_wanted) {
        const WindowDevice window_device{ &impl->device_table, impl->instance, impl->physical, impl->device, impl->queue, impl->queue_family, impl->memory };
        for (const auto& surface : impl->surfaces) {
            vn::String why;
            if (!surface->attach(window_device, &why)) {
                reportDiagnostic(DiagnosticSeverity::Warning,
                                 DiagnosticCategory::UnsupportedRequest,
                                 why.empty() ? vn::String(u8"vk: the host window's swapchain could not be created") : std::move(why));
                impl->presentation_wanted = false;
                break;
            }
        }
    }

    // CAPABILITIES ARE WHAT THIS BACKEND CAN SERVE, not what the device can do: M1 builds off-screen colour targets and
    // reads them back, and still presents nothing (M5).
    VkPhysicalDeviceProperties properties{};
    vkGetPhysicalDeviceProperties(impl->physical, &properties);

    // DEPTH RESOLVE IS THE DEVICE'S API VERSION (see Impl::depth_resolve_supported): the render pass that carries a
    // `pDepthStencilResolveAttachment` is created with `vkCreateRenderPass2` under a
    // `VkSubpassDescriptionDepthStencilResolve`, and both are Vulkan 1.2.
    impl->depth_resolve_supported = properties.apiVersion >= VK_API_VERSION_1_2;

    impl->caps                         = Capabilities{};
    impl->caps.render_targets          = true;
    impl->caps.presentation            = impl->presentation_wanted;
    impl->caps.features                = 0U;
    // ONE target has ONE sample count, and its colour and depth attachments must both support it, so the set this
    // device offers is the INTERSECTION of the two (a device whose depth tops out lower is the binding side). That
    // intersection is a `VkSampleCountFlags` BIT SET, while `Capabilities::max_samples` states the largest COUNT a
    // target may ask for - the number a host hands to `RenderTarget::setSamples` - so the limit is read OUT of the
    // set rather than being the set. The same set is what `ensureTarget` clamps a request against.
    impl->supported_sample_counts      = static_cast<std::uint32_t>(properties.limits.framebufferColorSampleCounts & properties.limits.framebufferDepthSampleCounts);
    impl->caps.max_samples             = largestSampleCount(impl->supported_sample_counts);
    impl->caps.max_anisotropy          = 1.0F;
    impl->caps.max_line_width          = properties.limits.lineWidthRange[1] > 0.0F ? properties.limits.lineWidthRange[1] : 1.0F;
    impl->caps.max_color_attachments   = std::min<std::uint32_t>(properties.limits.maxColorAttachments, 4U);

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

        // Pipelines are keyed by render-pass COMPATIBILITY, so they hold no render-pass handle; destroying the content
        // caches first is still the order that keeps teardown simple (the queue is idle, nothing names them).
        impl->content.destroy();

        for (auto& [target, images] : impl->targets) {
            (void)target;
            destroyTarget(impl->device_table, impl->device, images);
        }
        impl->targets.clear();

        // The window targets first: their destructors use the device table/instance this block is about to destroy.
        impl->surfaces.clear();
        impl->presentation_wanted = false;

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
    // The creation intent survives the session (see the constructor): a shut-down Probe device still intends to
    // present when it is brought up again.
    impl->caps.presentation   = impl->desc.presentation != DevicePresentation::None;
    impl->default_program     = nullptr;
    impl->reported_no_targets = false;
    impl->reported_surface    = false;
    impl->reported_content    = false;
    impl->reported_size_unknown = false;
    impl->reported_samples    = false;
    impl->reported_prepare_target = false;
    impl->depth_resolve_supported = false;
    // THE WINDOW SYSTEM GOES LAST, and whether or not a Vulkan session came up: its connection is older than the
    // session and every surface created through it was destroyed above (see WindowSystem's lifetime warning).
    impl->surfaces.clear();
    if (impl->window_system != nullptr) {
        impl->window_system->closeOwnWindow();
        impl->window_system->close();
        impl->window_system.reset();
    }
    // The frame's books go with the session: the next initialize() starts a device that measured nothing yet.
    impl->books = Impl::Books{};
}

bool Device::ensureWindowSystem(vn::String* why)
{
    if (impl->window_system != nullptr) {
        return impl->window_system->open(why); // idempotent: the connection is opened once
    }
    auto window_system = std::make_unique<WindowSystem>();
    if (!window_system->open(why)) {
        return false;
    }
    impl->window_system = std::move(window_system);
    return true;
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

void Device::releaseRenderTarget(RenderTarget* target)
{
    if (target == nullptr) {
        return;
    }
    const auto found = impl->targets.find(target);
    if (found == impl->targets.end()) {
        // Never built, or already released: the host's statement is already true.
        return;
    }
    if (impl->device != VK_NULL_HANDLE) {
        // Between frames by contract (see the header): this device waits on its own fence before it answers, so
        // nothing in flight names these image/view/memory handles and the destroy needs no idle.
        destroyTarget(impl->device_table, impl->device, found->second);
    }
    impl->targets.erase(found);
}

vn::raw_ptr<vn::graphics::WindowPresentTarget> Device::createSurfaceTarget(void* native_handle, SurfaceTargetResult* why)
{
    const auto answer = [why](const SurfaceTargetResult result) {
        if (why != nullptr) {
            *why = result;
        }
    };

    // A DISPLAY-LESS DEVICE ANSWERS SILENTLY: that is a first-class state, not a failure (see SurfaceTargetResult).
    if (impl->desc.presentation == DevicePresentation::None) {
        answer(SurfaceTargetResult::NoPresentation);
        return nullptr;
    }

    // A NULL HANDLE IS THE DEVICE'S OWN WINDOW on a Probe device (see DevicePresentation): brought up here when the
    // host asks before initialize() - the SDK's documented order - or when the device came up. A Handle device has
    // no window of its own, so a null handle there is wiring this device cannot repair.
    void* window_handle = native_handle;
    if (window_handle == nullptr) {
        if (impl->desc.presentation != DevicePresentation::Probe) {
            answer(SurfaceTargetResult::UnsupportedSurface);
            if (!impl->reported_surface) {
                impl->reported_surface = true;
                reportDiagnostic(DiagnosticSeverity::Warning,
                                 DiagnosticCategory::UnsupportedRequest,
                                 u8"vk: DevicePresentation::Handle needs a host window handle; this device brings up none");
            }
            return nullptr;
        }
        vn::String why;
        if (!ensureWindowSystem(&why) || !impl->window_system->openOwnWindow(kProbeWindowWidth, kProbeWindowHeight, &why)) {
            answer(SurfaceTargetResult::UnsupportedSurface);
            if (!impl->reported_surface) {
                impl->reported_surface = true;
                reportDiagnostic(DiagnosticSeverity::Warning,
                                 DiagnosticCategory::UnsupportedRequest,
                                 why.empty() ? vn::String(u8"vk: the device-owned window could not be brought up") : std::move(why));
            }
            return nullptr;
        }
        window_handle = impl->window_system->ownWindow();
    }
    // A HOST WINDOW NEEDS THE CONNECTION TOO, and this is the call that opens it: a target asked for before
    // initialize() still creates its surface later, through the same object.
    {
        vn::String why;
        if (!ensureWindowSystem(&why)) {
            answer(SurfaceTargetResult::UnsupportedSurface);
            if (!impl->reported_surface) {
                impl->reported_surface = true;
                reportDiagnostic(DiagnosticSeverity::Warning,
                                 DiagnosticCategory::UnsupportedRequest,
                                 why.empty() ? vn::String(u8"vk: no window system is available, so no window surface can be created") : std::move(why));
            }
            return nullptr;
        }
    }
    for (const auto& surface : impl->surfaces) {
        if (surface->nativeHandle() == window_handle) {
            // THE ASK IS REFUSED, and the host already holds the target the first ask answered (see RenderDevice):
            // one window carries one live swapchain, so a second target would be a second owner of the same surface.
            answer(SurfaceTargetResult::WindowOccupied);
            return nullptr;
        }
    }

    auto target = std::make_unique<WindowTarget>(*impl->window_system, window_handle);
    if (initialized()) {
        const WindowDevice window_device{
            &impl->device_table, impl->instance, impl->physical, impl->device, impl->queue, impl->queue_family, impl->memory
        };
        vn::String reason;
        if (!target->attach(window_device, &reason)) {
            answer(SurfaceTargetResult::UnsupportedSurface);
            if (!impl->reported_surface) {
                impl->reported_surface = true;
                reportDiagnostic(DiagnosticSeverity::Warning,
                                 DiagnosticCategory::UnsupportedRequest,
                                 reason.empty() ? vn::String(u8"vk: the window target could not be created") : std::move(reason));
            }
            return nullptr;
        }
        // A target accepted after initialize() still means this session presents.
        impl->presentation_wanted = true;
        impl->caps.presentation    = true;
    }
    // Before initialize(): the target only remembers the handle; the surface and swapchain are created when the
    // instance and the device exist (see initialize).
    WindowTarget* raw = target.get();
    impl->surfaces.push_back(std::move(target));
    answer(SurfaceTargetResult::Ok);
    return raw;
}

bool Device::ensureTarget(const RenderTarget& target)
{
    // WHAT THE HOST'S OBJECT ASKS FOR, in the value `planTarget` compares: an extent plus the shape - the engine's
    // formats AND the device spelling only this backend can fill (see TargetShape).
    TargetDesc wanted;
    wanted.width  = target.width();
    wanted.height = target.height();
    for (int index = 0; index < target.colorCount(); ++index) {
        wanted.shape.color_formats.push_back(target.colorFormat(index));
        wanted.shape.device_color_formats.push_back(static_cast<std::uint32_t>(colorFormatOf(target.colorFormat(index))));
    }
    if (target.hasDepth()) {
        wanted.shape.depth_format        = target.depthFormat();
        wanted.shape.device_depth_format = static_cast<std::uint32_t>(depthFormatOf(target.depthFormat()));
    }
    // A COUNT IS A COUNT (see ADR-2026-11-14-sample-count-refused-not-lowered): the target states the number of
    // samples it is drawn with, and `wanted.shape.samples` is that number - never another one. A count this device
    // cannot build (above its limit, or not a power of two) REFUSES the target: one warning naming the request and
    // the limit, nothing created, nothing drawn into it, and `readColorBuffer` answering NotReady. Lowering it would
    // leave `RenderTarget::samples()` saying a number the images are not drawn with, which no value in this shape can
    // express.
    const std::uint32_t requested_samples = target.samples() == 0U ? 1U : target.samples();
    const std::uint32_t largest_samples   = largestSampleCount(impl->supported_sample_counts);
    if (!ensureBuildableSamples(requested_samples, impl->supported_sample_counts)) {
        if (!impl->reported_samples) {
            impl->reported_samples = true;
            // The two reasons a count is unbuildable are said as the difference they are: a request past the limit,
            // and a number that is not a sample count at all. Both name the limit the host should read.
            const vn::String request = vn::String(u8"vk: a target asks for ") + numberText(requested_samples) + u8" samples per pixel";
            const vn::String why     = requested_samples > largest_samples
                                           ? request + u8", above what this device offers (up to " + numberText(largest_samples) +
                                                 u8"; see Capabilities::max_samples)"
                                           : request + u8", which is not a sample count this device offers (the counts are powers of two, up to " +
                                                 numberText(largest_samples) + u8"; see Capabilities::max_samples)";
            reportDiagnostic(DiagnosticSeverity::Warning, DiagnosticCategory::UnsupportedRequest, why);
        }
        return false;
    }
    wanted.shape.samples = requested_samples;
    wanted.shape.subpass = 0U;
    // A MULTISAMPLED DEPTH IS RESOLVED, and a device that cannot do that refuses the combination (ADR-2026-11-14).
    // `TargetImages` builds the single-sample picture the render pass resolves into and every reader uses
    // (`depthPicture`: the shadow path samples it), with `sample0` as the mode - so multisampling cannot change the
    // depth a later pass tests against. The resolve is Vulkan 1.2 (see Impl::depth_resolve_supported), and a device
    // below that genuinely cannot serve this shape: nothing is created, nothing is drawn, and `readColorBuffer`
    // answers NotReady.
    if (requested_samples > 1U && target.hasDepth() && !impl->depth_resolve_supported) {
        if (!impl->reported_samples_depth) {
            impl->reported_samples_depth = true;
            reportDiagnostic(DiagnosticSeverity::Warning,
                             DiagnosticCategory::UnsupportedRequest,
                             vn::String(u8"vk: a target asks for ") + numberText(requested_samples) +
                                 u8" samples per pixel and a depth attachment; this device cannot resolve a multisampled depth "
                                 u8"(it needs Vulkan 1.2's depth/stencil resolve), so the target is refused (see Capabilities::max_samples)");
        }
        return false;
    }

    TargetImages& images = impl->targets[&target];
    TargetInstance current;
    current.desc                   = images.desc;
    current.generation             = 0U;
    current.built                  = images.built();
    current.attachments_invalidated = false;

    const TargetDecision decision = planTarget(current, wanted);
    switch (decision.action) {
    case TargetAction::None:
        return true;
    case TargetAction::Repair:
        if (decision.reason == RepairReason::SizeUnknown) {
            if (!impl->reported_size_unknown) {
                impl->reported_size_unknown = true;
                reportDiagnostic(DiagnosticSeverity::Warning,
                                 DiagnosticCategory::TargetBuildFailed,
                                 u8"vk: a target with an unusable extent (0 in a dimension) was left unbuilt");
            }
            return false;
        }
        // Bootstrap: the attachments exist and have the right shape; the colour attachments always CLEAR, so the first
        // pass in does not need a second variant.
        return true;
    case TargetAction::ResizeInPlace:
        images.desc = wanted;
        if (!resizeTarget(impl->device_table, impl->device, impl->memory, images)) {
            reportDiagnostic(DiagnosticSeverity::Error,
                             DiagnosticCategory::TargetBuildFailed,
                             u8"vk: the target's attachments could not be rebuilt at the new extent (see the loader's report)");
            return false;
        }
        return true;
    case TargetAction::Rebuild:
        // The shape changed: the render pass, the framebuffer and the attachments are rebuilt; pipelines are keyed by
        // compatibility, so the ones built for the old shape are simply never asked for again (and a resize, which
        // keeps the shape, reuses them - see ContentDraw).
        destroyTarget(impl->device_table, impl->device, images);
        images.desc = wanted;
        if (!buildTarget(impl->device_table, impl->device, impl->memory, images)) {
            reportDiagnostic(DiagnosticSeverity::Error,
                             DiagnosticCategory::TargetBuildFailed,
                             u8"vk: the off-screen target's attachments could not be built (see the loader's report)");
            return false;
        }
        return true;
    }
    return false;
}

Capabilities Device::capabilities() const noexcept
{
    return impl->caps;
}

PresentOutcome Device::executeFrame(const FrameDescription& frame, PresentTarget* present_to)
{
    if (!initialized()) {
        reportDiagnostic(DiagnosticSeverity::Error,
                         DiagnosticCategory::SubmissionFailed,
                         u8"vk: a frame was handed to a device that is not up");
        return PresentOutcome::Skipped;
    }

    // THE FRAME'S BOOKS OPEN HERE AND CLOSE ON EVERY EXIT (see closeFrameBooks): the report always describes the
    // frame that just ended - an empty one included - and the upload numbers are this frame's own delta.
    const Impl::ContentNumbers    before = impl->contentNumbers();
    vn::graphics::FrameCounters   counters{};
    struct BooksCloser {
        Impl*                        impl;
        const Impl::ContentNumbers&  before;
        vn::graphics::FrameCounters& counters;
        ~BooksCloser()
        {
            impl->closeFrameBooks(before, counters);
        }
    } books_closer{ impl.get(), before, counters };

    // WHICH PASSES THIS BACKEND CAN RUN: off-screen passes, and WINDOW passes when the frame names one of this
    // device's own targets as its presenter. A frame that names no presenter presents nowhere (a first-class state:
    // an offline bake), and a pass that names no target while a presenter IS named draws into that window.
    WindowTarget* window = nullptr;
    if (present_to != nullptr) {
        auto* candidate = dynamic_cast<WindowTarget*>(present_to);
        for (const auto& surface : impl->surfaces) {
            if (surface.get() == candidate) {
                window = candidate;
                break;
            }
        }
    }

    struct Runnable {
        const FrameDescription::Pass* entry{ nullptr };
        const PassDraw*               draw{ nullptr };
        bool                          window{ false };
    };

    std::vector<Runnable> runnable_passes;
    runnable_passes.reserve(frame.passes.size());
    bool window_acquired = false;
    // Whether a pass was dropped because a description this device cannot build was named (a sample count, say). The
    // refusal has already said WHY, so the generic "nothing I can run" sentence below would be the same fact twice.
    bool refused_pass = false;
    for (const FrameDescription::Pass& entry : frame.passes) {
        if (entry.pass == nullptr) {
            continue; // the SDK's rule: an entry that names no pass is nothing to run (see RenderFrameExecutor)
        }
        if (!entry.draw.has_value()) {
            continue; // a pass that issued nothing: the scope opens and closes, nothing to record
        }
        const PassDraw& draw = *entry.draw;

        // WHAT THE FRAME'S SHAPE RECORDS (see FrameCounters): a scope enters the plan when it has an effect - it drew
        // something, or it announced a clear (a clear is the pass' own load-op, so a clear-only pass is a real pass
        // with no draws). The reference recorder states the same rule, which is what makes the two reports comparable.
        std::uint64_t calls = 0U;
        for (const DrawDescription& description : draw.draws) {
            calls += isDrawingCall(description) ? 1U : 0U;
        }
        // COUNTED ONLY FOR A PASS THIS DEVICE CAN RUN: a drawing call is recorded, and a pass whose target could not
        // be built recorded nothing (see the sample-count refusals in ensureTarget) - reporting its calls would say a
        // frame drew when nothing reached the queue.
        const bool has_effect = calls != 0U || draw.clear.has_value();
        const auto count_pass = [&counters, calls, has_effect]() {
            if (has_effect) {
                ++counters.passes;
                counters.draws += calls;
            }
        };
        if (draw.target == nullptr) {
            if (window == nullptr || !window->ready()) {
                if (!impl->reported_surface) {
                    impl->reported_surface = true;
                    reportDiagnostic(DiagnosticSeverity::Warning,
                                     DiagnosticCategory::UnsupportedRequest,
                                     u8"vk: a window pass names no presenter this device handed out; it was left out");
                }
                refused_pass = true;
                continue;
            }
            if (!window_acquired) {
                vn::String why;
                if (!window->acquire(&why)) {
                    if (!impl->reported_surface) {
                        impl->reported_surface = true;
                        reportDiagnostic(DiagnosticSeverity::Warning,
                                         DiagnosticCategory::UnsupportedRequest,
                                         why.empty() ? vn::String(u8"vk: the window's next image could not be acquired") : std::move(why));
                    }
                    refused_pass = true;
                    continue;
                }
                window_acquired = true;
            }
            count_pass();
            runnable_passes.push_back(Runnable{ &entry, &draw, true });
            continue;
        }
        if (!ensureTarget(*draw.target)) {
            refused_pass = true;
            continue;
        }
        count_pass();
        runnable_passes.push_back(Runnable{ &entry, &draw, false });
    }

    if (runnable_passes.empty()) {
        if (!refused_pass && !impl->reported_no_targets) {
            impl->reported_no_targets = true;
            reportDiagnostic(DiagnosticSeverity::Info,
                             DiagnosticCategory::UnsupportedRequest,
                             u8"vk: the frame had no pass this backend can run (off-screen targets and host windows are "
                             u8"served); nothing was submitted");
        }
        return PresentOutcome::Skipped;
    }

    // ONE COMMAND BUFFER, ONE FENCE, ONE WAIT. The frame is submitted and finished before the call returns, which is
    // what makes "read the pixels right after" mean what it says.
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
    // The pass count was taken from the frame's shape above (a pass whose draws were all skipped is still a pass
    // that ran); what the loop below adds is the drawing calls this backend actually recorded.

    bool first_window_pass = true;
    for (const Runnable& item : runnable_passes) {
        const PassDraw& draw = *item.draw;
        // THE PASS' TEXTURES ARE PREPARED BEFORE ITS RENDER PASS BEGINS: an upload records image barriers and
        // buffer-to-image copies, neither of which is legal inside a render pass instance.
        impl->content.preparePass(commands, draw.draws);

        // A SOURCE A SCREEN DRAW SAMPLES is about to be read: transition it to SHADER_READ_ONLY now, where an image
        // barrier is unambiguously allowed (before any render pass instance begins).
        for (const DrawDescription& description : draw.draws) {
            if (description.kind != DrawDescription::Kind::Screen || description.source == nullptr) {
                continue;
            }
            const auto source = impl->targets.find(description.source);
            if (source == impl->targets.end() || !source->second.built() || source->second.color_sampled) {
                continue;
            }
            TargetImages& sampled = source->second;
            for (const VkImage image : sampled.colors) {
                const ImageState from = sampled.fresh ? stateOf(ImageLayout::Undefined) : stateOf(ImageLayout::ColorAttachment);
                const ImageState to   = stateOf(ImageLayout::ShaderReadOnly);
                const VkImageMemoryBarrier barrier = barrierBetween(image, from, to, VK_IMAGE_ASPECT_COLOR_BIT);
                impl->device_table.vkCmdPipelineBarrier(commands,
                                                        VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
                                                        VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
                                                        0U,
                                                        0U,
                                                        nullptr,
                                                        0U,
                                                        nullptr,
                                                        1U,
                                                        &barrier);
            }
            sampled.color_sampled = true;
        }

        // THE SHADOW MAP THIS PASS SAMPLES, if it declared one: resolved from the target's own statement and moved to
        // SHADER_READ_ONLY here, before the render pass begins (see resolveShadow).
        ContentDraw::ShadowSource shadow{};
        (void)impl->resolveShadow(commands, item.entry->inputs, shadow);

        TargetImages*                                  images        = nullptr;
        VkRenderPass                                   render_pass   = VK_NULL_HANDLE;
        VkFramebuffer                                  framebuffer   = VK_NULL_HANDLE;
        VkExtent2D                                     extent{};
        std::uint32_t                                  color_count   = 0U;
        bool                                           has_depth     = false;
        vn::graphics::backend::RenderPassCompatibility compatibility{};

        if (item.window) {
            // A WINDOW PASS: the swapchain's image, the window's render pass. The first pass into a frame's image
            // clears both attachments; a later pass into the same image loads what the earlier one wrote.
            const bool load = !first_window_pass;
            render_pass     = window->renderPass(load, load);
            framebuffer     = window->framebuffer();
            extent          = VkExtent2D{ window->width(), window->height() };
            color_count     = 1U;
            has_depth       = true;
            compatibility   = window->compatibility();
            first_window_pass = false;
        }
        else {
            images = &impl->targets.at(draw.target);

            // A TARGET A SCREEN DRAW SAMPLED is in SHADER_READ_ONLY: put its colour images back before this pass
            // writes them.
            if (images->color_sampled) {
                for (const VkImage image : images->colors) {
                    const VkImageMemoryBarrier back = barrierBetween(image,
                                                                     stateOf(ImageLayout::ShaderReadOnly),
                                                                     stateOf(ImageLayout::ColorAttachment),
                                                                     VK_IMAGE_ASPECT_COLOR_BIT);
                    impl->device_table.vkCmdPipelineBarrier(commands,
                                                            VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
                                                            VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
                                                            0U,
                                                            0U,
                                                            nullptr,
                                                            0U,
                                                            nullptr,
                                                            1U,
                                                            &back);
                }
                images->color_sampled = false;
            }

            // A TARGET A PASS SAMPLED AS A SHADOW MAP has its depth PICTURE in SHADER_READ_ONLY: put it back before
            // this pass writes the depth (the mirror of the colour rule above, on the depth attachment).
            if (images->depth_sampled) {
                const VkImageMemoryBarrier back = barrierBetween(images->depthPicture(),
                                                                 stateOf(ImageLayout::ShaderReadOnly),
                                                                 stateOf(ImageLayout::DepthAttachment),
                                                                 depthAspectMask(images->depthFormat()));
                impl->device_table.vkCmdPipelineBarrier(commands,
                                                        VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
                                                        VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT,
                                                        0U,
                                                        0U,
                                                        nullptr,
                                                        0U,
                                                        nullptr,
                                                        1U,
                                                        &back);
                images->depth_sampled = false;
            }

            // A FRESH attachment set is UNDEFINED: a pass that asked to preserve something gets the CLEAR variant for
            // its first frame (there is nothing to load yet). A pass that announces NO clear loads both attachments;
            // a fresh target falls back to CLEAR, the honest reading of "load an image nobody wrote".
            const bool has_clear  = draw.clear.has_value();
            const bool load_color = !has_clear && !images->fresh;
            const bool load_depth = has_clear ? (!draw.clear->depth && !images->fresh) : !images->fresh;
            render_pass           = images->renderPassFor(load_depth, load_color);
            framebuffer           = images->framebuffer;
            extent                = VkExtent2D{ static_cast<std::uint32_t>(images->width()), static_cast<std::uint32_t>(images->height()) };
            color_count           = static_cast<std::uint32_t>(images->colorCount());
            has_depth             = images->hasDepth();
            compatibility         = images->compatibility();
        }

        // PER-ATTACHMENT CLEAR (a LOAD variant ignores these values): the clear values are indexed by ATTACHMENT, and
        // attachment 0 is the pass' colour while the rest of the COLOUR slots come from extra_colors (an absent entry
        // is transparent black - see ClearPolicy). A MULTISAMPLED target's array is [sample buffers, resolve images,
        // depth], so its depth slot is not `color_count`; the resolve slots in between are DONT_CARE and their values
        // are never read (see TargetImages' class note).
        const std::size_t         depth_slot  = images != nullptr ? images->depthAttachmentIndex() : static_cast<std::size_t>(color_count);
        std::vector<VkClearValue> clear_values(depth_slot + (has_depth ? 1U : 0U));
        const Colorf              colour      = draw.clear.has_value() ? draw.clear->color : Colorf(0.0F, 0.0F, 0.0F, 1.0F);
        if (color_count != 0U) {
            // A DEPTH-ONLY PASS (a shadow map) has no colour slot to clear: its clear values start at the depth.
            clear_values[0].color = { { colour.r, colour.g, colour.b, colour.a } };
        }
        for (std::size_t index = 1; index < color_count; ++index) {
            const Colorf extra = images != nullptr && draw.clear.has_value() && index - 1U < draw.clear->extra_colors.size()
                                     ? draw.clear->extra_colors[index - 1U]
                                     : Colorf(0.0F, 0.0F, 0.0F, 0.0F);
            clear_values[index].color = { { extra.r, extra.g, extra.b, extra.a } };
        }
        if (has_depth) {
            // Reverse-Z: every backend clears depth to its own far plane, and this camera convention's far plane is 0
            // (see ClearPolicy: the policy states WHETHER, never to what).
            clear_values[depth_slot].depthStencil = { 0.0F, 0U };
        }

        VkRenderPassBeginInfo pass_begin{};
        pass_begin.sType             = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
        pass_begin.renderPass        = render_pass;
        pass_begin.framebuffer       = framebuffer;
        pass_begin.renderArea.offset = VkOffset2D{ 0, 0 };
        pass_begin.renderArea.extent = extent;
        pass_begin.clearValueCount   = static_cast<std::uint32_t>(clear_values.size());
        pass_begin.pClearValues      = clear_values.data();

        impl->device_table.vkCmdBeginRenderPass(commands, &pass_begin, VK_SUBPASS_CONTENTS_INLINE);
        // WHAT THIS PASS DRAWS. Each draw description is borrowed for this call; every command inside it becomes one
        // GPU draw (M3: resolved state selects the pipeline, one `RenderCommand` = one indexed draw). The frame's
        // DRAWING-CALL count was taken from the frame's shape above - a call is the description, not its commands.
        for (const DrawDescription& description : draw.draws) {
            vn::String                       why;
            vn::graphics::DiagnosticCategory category = DiagnosticCategory::ContentSkipped;
            if (description.kind == DrawDescription::Kind::Screen) {
                // THE FULL-SCREEN ABI: the source's colour attachments, bound in order. A source this device never
                // built answers an empty span, which makes the recorder report (and skip) the draw.
                ContentDraw::ScreenSource source{};
                const auto                source_images = description.source != nullptr ? impl->targets.find(description.source) : impl->targets.end();
                if (source_images != impl->targets.end() && source_images->second.built()) {
                    source.images = source_images->second.colors;
                    source.views  = source_images->second.color_views;
                }
                (void)impl->content.recordScreen(commands,
                                                 render_pass,
                                                 compatibility,
                                                 color_count,
                                                 static_cast<int>(extent.width),
                                                 static_cast<int>(extent.height),
                                                 description,
                                                 draw,
                                                 source,
                                                 &why,
                                                 &category);
            }
            else {
                (void)impl->content.record(commands,
                                           render_pass,
                                           compatibility,
                                           color_count,
                                           static_cast<int>(extent.width),
                                           static_cast<int>(extent.height),
                                           description,
                                           draw,
                                           impl->default_program.get(),
                                           shadow,
                                           &why,
                                           &category);
            }
            if (!why.empty() && !impl->reported_content) {
                impl->reported_content = true;
                reportDiagnostic(DiagnosticSeverity::Warning, category, why);
            }
        }
        impl->device_table.vkCmdEndRenderPass(commands);
        if (images != nullptr) {
            images->fresh = false;
        }
    }

    // THE PRESENT TRANSITION, after the window's last pass and before the command buffer ends: every window pass
    // stores into COLOR_ATTACHMENT_OPTIMAL, and vkQueuePresentKHR reads PRESENT_SRC_KHR.
    if (window_acquired) {
        window->recordPresentTransition(commands);
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

    // THE WINDOW'S SEMAPHORES: the acquire signalled image_available, the submit waits on it and signals
    // render_finished, and the present waits on that. One frame at a time (the device waits on its fence below), so
    // one pair per window target is enough.
    const VkSemaphore          wait_semaphore   = window_acquired ? window->imageAvailableSemaphore() : VK_NULL_HANDLE;
    const VkSemaphore          signal_semaphore = window_acquired ? window->renderFinishedSemaphore() : VK_NULL_HANDLE;
    const VkPipelineStageFlags wait_stage       = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    if (window_acquired) {
        submit.waitSemaphoreCount   = 1U;
        submit.pWaitSemaphores      = &wait_semaphore;
        submit.pWaitDstStageMask    = &wait_stage;
        submit.signalSemaphoreCount = 1U;
        submit.pSignalSemaphores    = &signal_semaphore;
    }

    const bool submitted = impl->device_table.vkQueueSubmit(impl->queue, 1U, &submit, impl->completion) == VK_SUCCESS;
    if (submitted) {
        ++impl->books.progress.submitted;
    }
    const bool completed = submitted &&
                           impl->device_table.vkWaitForFences(impl->device, 1U, &impl->completion, VK_TRUE, UINT64_MAX) == VK_SUCCESS;
    if (completed) {
        ++impl->books.progress.completed;
    }
    (void)impl->device_table.vkResetFences(impl->device, 1U, &impl->completion);
    impl->device_table.vkFreeCommandBuffers(impl->device, impl->command_pool, 1U, &commands);
    // The per-draw uniform buffers were named by the command buffer that just finished (or failed to): the queue is
    // idle either way, so the bytes can go.
    impl->content.endFrame();

    if (!completed) {
        reportDiagnostic(DiagnosticSeverity::Error,
                         DiagnosticCategory::SubmissionFailed,
                         u8"vk: the frame's command buffer did not run to completion");
    }

    // THE FRAME'S TAIL: the executor is what presents the frame it ran (see RenderFrameExecutor::executeFrame).
    PresentOutcome outcome = PresentOutcome::Skipped;
    if (window_acquired) {
        outcome = completed ? window->present() : PresentOutcome::Lost;
        if (outcome == PresentOutcome::Presented) {
            ++impl->books.progress.presented;
        }
    }
    return outcome;
}

bool Device::releaseContent(vn::raw_ptr<const vn::Object> content)
{
    if (content == nullptr || !initialized()) {
        return false;
    }

    // ONE TYPE DISPATCH: the SDK names the entry with Object because the four content types have no common narrower
    // base (see graphics-content-lifecycle.md). No two of them are related, and a Texture2D / CubeMap answers through
    // Texture.
    if (const auto* geometry = vn::obj_cast<const vn::graphics::Geometry>(content)) {
        const vn::graphics::GeometryDescriptionPtr description = geometry->description();
        const std::size_t dropped = description != nullptr ? impl->content.releaseGeometry(description.get()) : 0U;
        if (dropped > 0U) {
            ++impl->books.released_nodes;
        }
        return true;
    }
    if (const auto* program = vn::obj_cast<const vn::graphics::ShaderProgram>(content)) {
        const vn::graphics::ShaderProgramDescriptionPtr description = program->description();
        const std::size_t dropped = description != nullptr ? impl->content.releaseProgram(description.get()) : 0U;
        if (dropped > 0U) {
            ++impl->books.released_nodes;
        }
        return true;
    }
    if (const auto* material = vn::obj_cast<const vn::graphics::MaterialBase>(content)) {
        // RECOGNIZED, AND NOTHING TO DROP: a material's values are packed into each draw's uniform buffer, and no
        // per-material GPU state is kept. Answering false here would say the object is not content - it is.
        (void)material;
        return true;
    }
    if (const auto* texture = vn::obj_cast<const vn::graphics::Texture>(content)) {
        const std::uint64_t before = impl->content.images().progress().releases;
        const bool          answered = impl->content.images().releaseTexture(*texture);
        if (answered && impl->content.images().progress().releases != before) {
            ++impl->books.released_nodes;
        }
        return answered;
    }
    return false;
}

bool Device::releaseContent(const vn::Buffer<float>& buffer)
{
    if (!initialized()) {
        return false;
    }
    // The element type is the caller's way of saying WHICH stream; the residency keys on the buffer's ADDRESS (see
    // ContentDraw::releaseStreams), so the two overloads share one rule and one body would.
    const std::size_t dropped = impl->content.releaseStreams(&buffer);
    if (dropped > 0U) {
        ++impl->books.released_nodes;
    }
    return true;
}

bool Device::releaseContent(const vn::Buffer<std::uint32_t>& buffer)
{
    if (!initialized()) {
        return false;
    }
    const std::size_t dropped = impl->content.releaseStreams(&buffer);
    if (dropped > 0U) {
        ++impl->books.released_nodes;
    }
    return true;
}

vn::graphics::PrepareReport Device::prepareFrame(const FrameDescription& frame, PresentTarget* present_to)
{
    using vn::graphics::EngineNotReady;
    using vn::graphics::PrepareReport;

    if (!initialized()) {
        // "There is nothing to prebuild" and "the device is not up" are different answers (see PrepareReport).
        return PrepareReport{ PrepareReport::Outcome::NotReady, EngineNotReady::NotInitialized };
    }

    // WHICH SURFACE THE FRAME NAMES - the same resolution executeFrame makes, minus everything that would touch it:
    // nothing is acquired, resized or presented, and the frame water level does not move (see
    // RenderFrameExecutor::prepareFrame, P11). The ONE command buffer this call records holds transfers only.
    WindowTarget* window = nullptr;
    if (present_to != nullptr) {
        auto* candidate = dynamic_cast<WindowTarget*>(present_to);
        for (const auto& surface : impl->surfaces) {
            if (surface.get() == candidate) {
                window = candidate;
                break;
            }
        }
        if (window == nullptr) {
            if (!impl->reported_prepare_target) {
                impl->reported_prepare_target = true;
                reportDiagnostic(DiagnosticSeverity::Warning,
                                 DiagnosticCategory::UnsupportedRequest,
                                 u8"vk: a frame named a surface target this device did not hand out; nothing was prepared");
            }
            return PrepareReport{ PrepareReport::Outcome::Refused };
        }
    }

    const Impl::ContentNumbers before = impl->contentNumbers();
    PrepareReport             report;
    report.outcome = PrepareReport::Outcome::Prepared;

    // THE ONE THING THIS CALL MAY SUBMIT IS A TRANSFER BATCH OF ITS OWN (see RenderFrameExecutor::prepareFrame and
    // ADR-2026-11-14): the builds that need a command buffer - the 1x1 stand-in and every material texture of a pass -
    // are recorded on one command buffer here, submitted on the device's own fence and waited for before this call
    // answers. It is NOT a frame: no swapchain image is acquired, no render pass opens, nothing is drawn or presented,
    // and `books.progress` does not move. The buffer is allocated LAZILY, on the first pass this device can serve: a
    // frame whose passes it cannot serve records nothing and therefore allocates nothing.
    VkCommandBuffer batch{ VK_NULL_HANDLE };
    bool            batch_attempted{ false };

    for (const FrameDescription::Pass& entry : frame.passes) {
        if (entry.pass == nullptr) {
            continue; // the SDK's rule: an entry that names no pass is nothing to run (see executeFrame)
        }
        if (!entry.draw.has_value()) {
            continue;
        }
        const PassDraw& draw = *entry.draw;
        if (draw.draws.empty()) {
            continue;
        }
        // THE SHAPE OF THE FRAME ASKED FOR (see PrepareReport): counted whether or not this device can build it.
        ++report.passes;
        std::uint32_t calls = 0U;
        for (const DrawDescription& description : draw.draws) {
            calls += isDrawingCall(description) ? 1U : 0U;
        }
        report.draws += calls;

        VkRenderPass                                  render_pass   = VK_NULL_HANDLE;
        std::uint32_t                                 color_count   = 0U;
        vn::graphics::backend::RenderPassCompatibility compatibility{};
        if (draw.target == nullptr) {
            // A WINDOW PASS: its shape is known only once a swapchain exists - and preparing must not create one.
            if (window == nullptr || !window->ready()) {
                continue;
            }
            render_pass   = window->renderPass(false, false);
            color_count   = 1U;
            compatibility = window->compatibility();
        }
        else {
            // THE TARGET'S ATTACHMENTS ARE WHAT THE FRAME WOULD BUILD ANYWAY, so preparing them is the point.
            if (!ensureTarget(*draw.target)) {
                continue;
            }
            TargetImages& images = impl->targets.at(draw.target);
            render_pass   = images.renderPassFor(false, false);
            color_count   = static_cast<std::uint32_t>(images.colorCount());
            compatibility = images.compatibility();
        }

        // THE BATCH IS OPENED ON THE FIRST PASS THIS DEVICE CAN SERVE - the same condition the branch above just
        // answered, so a frame nothing can be built for never allocates a command buffer.
        if (!batch_attempted) {
            batch_attempted = true;
            VkCommandBufferAllocateInfo allocate{};
            allocate.sType              = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
            allocate.commandPool        = impl->command_pool;
            allocate.level              = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
            allocate.commandBufferCount = 1U;
            batch                       = VK_NULL_HANDLE;
            VkCommandBufferBeginInfo begin{};
            begin.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
            begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
            if (impl->device_table.vkAllocateCommandBuffers(impl->device, &allocate, &batch) != VK_SUCCESS) {
                batch = VK_NULL_HANDLE;
                reportDiagnostic(DiagnosticSeverity::Error,
                                 DiagnosticCategory::SubmissionFailed,
                                 u8"vk: the preparation's transfer batch could not be recorded; the next frame uploads the "
                                 u8"content again");
            }
            else if (impl->device_table.vkBeginCommandBuffer(batch, &begin) != VK_SUCCESS) {
                impl->device_table.vkFreeCommandBuffers(impl->device, impl->command_pool, 1U, &batch);
                batch = VK_NULL_HANDLE;
                reportDiagnostic(DiagnosticSeverity::Error,
                                 DiagnosticCategory::SubmissionFailed,
                                 u8"vk: the preparation's transfer batch could not be recorded; the next frame uploads the "
                                 u8"content again");
            }
        }

        for (const DrawDescription& description : draw.draws) {
            vn::String                       why;
            vn::graphics::DiagnosticCategory category = DiagnosticCategory::ContentSkipped;
            if (description.kind == DrawDescription::Kind::Screen) {
                impl->content.prepareScreen(compatibility, render_pass, color_count, description, draw, &why, &category);
            }
            else {
                impl->content.prepare(compatibility,
                                      render_pass,
                                      color_count,
                                      description,
                                      draw,
                                      impl->default_program.get(),
                                      &why,
                                      &category);
            }
            if (!why.empty() && !impl->reported_content) {
                impl->reported_content = true;
                reportDiagnostic(DiagnosticSeverity::Warning, category, why);
            }
        }

        // THE PASS' OWN UPLOADS, recorded into the batch: exactly the copies the frame would record before this pass'
        // render pass begins (see ContentDraw::preparePass), so the frame then finds them resident and records
        // nothing. A pass whose batch could not be opened is still prepared; the next frame uploads it.
        if (batch != VK_NULL_HANDLE) {
            impl->content.preparePass(batch, draw.draws);
        }
    }

    // THE BATCH RUNS BEFORE THIS CALL ANSWERS: one submit on the device's frame fence, no semaphore (nothing was
    // acquired, so the queue has nothing to wait for), one wait, and the staging buffers go as soon as the copies
    // have run - the release the frame path reaches through ContentDraw::endFrame -> ContentImages::releaseFrame.
    if (batch != VK_NULL_HANDLE) {
        if (impl->device_table.vkEndCommandBuffer(batch) != VK_SUCCESS) {
            // THE BUFFER NEVER CLOSED, so whatever it recorded never ran - and it cannot be recorded again: the copies
            // were already recorded against entries this call made resident. Said plainly, because "the next frame
            // uploads it again" would be a promise the revision check does not keep.
            reportDiagnostic(DiagnosticSeverity::Error,
                             DiagnosticCategory::SubmissionFailed,
                             u8"vk: the preparation's transfer batch could not be closed; the copies it recorded never ran");
            impl->device_table.vkFreeCommandBuffers(impl->device, impl->command_pool, 1U, &batch);
        }
        else if (!impl->content.images().hasPendingUploads()) {
            // ZERO WORK, ZERO SUBMIT: an unchanged frame records no copy, so there is nothing to run and nothing is
            // submitted - the property a second prepare of the same frame is observed by (see ADR-2026-11-14).
            impl->device_table.vkFreeCommandBuffers(impl->device, impl->command_pool, 1U, &batch);
        }
        else {
            VkSubmitInfo submit{};
            submit.sType              = VK_STRUCTURE_TYPE_SUBMIT_INFO;
            submit.commandBufferCount = 1U;
            submit.pCommandBuffers    = &batch;
            const bool submitted = impl->device_table.vkQueueSubmit(impl->queue, 1U, &submit, impl->completion) == VK_SUCCESS;
            const bool completed = submitted &&
                                   impl->device_table.vkWaitForFences(impl->device, 1U, &impl->completion, VK_TRUE, UINT64_MAX) == VK_SUCCESS;
            (void)impl->device_table.vkResetFences(impl->device, 1U, &impl->completion);
            impl->device_table.vkFreeCommandBuffers(impl->device, impl->command_pool, 1U, &batch);
            if (completed) {
                impl->content.images().releaseUploadStaging();
            }
            else {
                // The staging buffers stay: the next frame's releaseFrame lets go of them once its own queue is idle.
                // They cannot buy a re-upload, though - the entries were already made resident with what the copies were
                // going to write - so this says what actually happened rather than promising a retry: a queue that
                // refuses a submit is a device-level failure (out of memory or lost device), not a content problem.
                reportDiagnostic(DiagnosticSeverity::Error,
                                 DiagnosticCategory::SubmissionFailed,
                                 u8"vk: the preparation's transfer batch did not run (the queue refused it); the images it "
                                 u8"was going to fill were never written");
            }
        }
    }

    // WHAT THIS CALL BUILT, not what is resident: a second prepare of an unchanged frame answers zeros (see
    // PrepareReport). `uploads` counts BOTH kinds FrameUploads reports - the streams this call uploaded and the
    // textures it made resident (see ADR-2026-11-14). `sets` stays 0 because no descriptor set outlives a frame here.
    const Impl::ContentNumbers after = impl->contentNumbers();
    report.halves    = after.program_builds - before.program_builds;
    report.pipelines = after.pipeline_builds - before.pipeline_builds;
    report.uploads   = (after.stream_builds - before.stream_builds) + (after.texture_builds - before.texture_builds);
    return report;
}

vn::graphics::FrameReport Device::frameReport() const noexcept
{
    vn::graphics::FrameReport report;
    report.counters = impl->books.counters;
    report.progress = impl->books.progress;
    report.uploads  = impl->books.uploads; // this frame's own delta; absent until a frame ran

    vn::graphics::RetentionStats retention;
    retention.released_nodes = impl->books.released_nodes;
    retention.device_waits   = impl->books.device_waits;
    if (initialized()) {
        // WHAT IS RESIDENT, PER KIND: the three kinds this backend retains, composed here where all three tables are
        // reachable. Material blocks and descriptor sets are per-frame objects, so no row claims them.
        vn::graphics::ResidencyLedger ledger;
        impl->noteResidency(ledger);
        retention.residency = std::move(ledger);
    }
    report.retention = std::move(retention);
    return report;
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

    const auto found = impl->targets.find(target);
    if (found == impl->targets.end() || !found->second.built()) {
        return fail(ReadbackResult::NotReady);
    }
    TargetImages& images = found->second;
    if (attachment < 0 || static_cast<std::size_t>(attachment) >= images.colorCount()) {
        return fail(ReadbackResult::Invalid);
    }
    const std::size_t   attachment_index = static_cast<std::size_t>(attachment);
    const VkImage       color_image      = images.colors[attachment_index];
    if (images.colorFormat(attachment_index) != VK_FORMAT_R8G8B8A8_UNORM) {
        // The readback contract is packed RGBA8; another colour format would need a conversion this step does not write.
        return fail(ReadbackResult::Unsupported);
    }

    const VkDeviceSize bytes = static_cast<VkDeviceSize>(images.width()) * static_cast<VkDeviceSize>(images.height()) * 4U;

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

    // A target a SCREEN DRAW sampled is in SHADER_READ_ONLY: put every colour attachment back before this readback's
    // own transition names COLOR_ATTACHMENT_OPTIMAL as the old layout.
    if (images.color_sampled) {
        for (const VkImage image : images.colors) {
            const VkImageMemoryBarrier back =
                barrierBetween(image, stateOf(ImageLayout::ShaderReadOnly), stateOf(ImageLayout::ColorAttachment), VK_IMAGE_ASPECT_COLOR_BIT);
            impl->device_table.vkCmdPipelineBarrier(commands,
                                                    VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
                                                    VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
                                                    0U,
                                                    0U,
                                                    nullptr,
                                                    0U,
                                                    nullptr,
                                                    1U,
                                                    &back);
        }
        images.color_sampled = false;
    }

    const VkImageMemoryBarrier to_transfer =
        barrierBetween(color_image, stateOf(ImageLayout::ColorAttachment), transferReadState(), VK_IMAGE_ASPECT_COLOR_BIT);
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
    region.imageExtent                 = VkExtent3D{ static_cast<std::uint32_t>(images.width()),
                                                      static_cast<std::uint32_t>(images.height()), 1U };
    impl->device_table.vkCmdCopyImageToBuffer(commands, color_image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, staging, 1U, &region);

    const VkImageMemoryBarrier to_attachment =
        barrierBetween(color_image, transferReadState(), stateOf(ImageLayout::ColorAttachment), VK_IMAGE_ASPECT_COLOR_BIT);
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

bool Device::readDepthBuffer(const RenderTarget* target, std::vector<float>& outDepths, ReadbackResult* why)
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

    const auto found = impl->targets.find(target);
    if (found == impl->targets.end() || !found->second.built()) {
        return fail(ReadbackResult::NotReady);
    }
    // A BORROWED DEPTH IS READ THROUGH THE SOURCE (the SDK's rule, see RenderBackend::readDepthBuffer): the image
    // belongs to the lender, and this target never promised it - so the refusal comes before any format question.
    if (target->depthSource() != nullptr) {
        return fail(ReadbackResult::Unsupported);
    }
    TargetImages& images = found->second;
    if (!images.hasDepth()) {
        return fail(ReadbackResult::Invalid); // no depth attachment at all: the request names nothing
    }

    // ONE TABLE SAYS WHAT A DEPTH READBACK IS - the SDK's, the same one `decodeDepth` reads. D24 is Vulkan's COMBINED
    // depth/stencil format with no plain depth copy, so it is refused here rather than decoded into numbers that
    // look like depths and are not (the reference backend answers the same way, and the rule is why `depthReadbackOf`
    // exists at all). The per-texel size below is the table's too, so the copy and the decoder cannot disagree.
    const vn::graphics::backend::ReadbackFormat packed = depthReadbackOf(target->depthFormat());
    if (!packed.readable) {
        return fail(ReadbackResult::Unsupported);
    }

    const VkImage    depth_image = images.depthPicture();
    const VkExtent3D extent{ static_cast<std::uint32_t>(images.width()), static_cast<std::uint32_t>(images.height()), 1U };
    const VkDeviceSize bytes = static_cast<VkDeviceSize>(extent.width) * static_cast<VkDeviceSize>(extent.height) *
                               static_cast<VkDeviceSize>(packed.bytes_per_texel);

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

    // RECORD: depth picture -> transfer source -> copy -> back to the one layout a render pass expects, so a later
    // pass into this target finds its depth where its render pass was built to write it (DEPTH_ATTACHMENT).
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

    // A PASS THAT SAMPLED THIS DEPTH AS A SHADOW MAP left the picture in SHADER_READ_ONLY: put it back before this
    // readback's own transition names DEPTH_ATTACHMENT as the old layout (the mirror of readColorBuffer's
    // colour_sampled note, and the same stage masks executeFrame's restore records).
    if (images.depth_sampled) {
        const VkImageMemoryBarrier back = barrierBetween(depth_image,
                                                         stateOf(ImageLayout::ShaderReadOnly),
                                                         stateOf(ImageLayout::DepthAttachment),
                                                         depthAspectMask(images.depthFormat()));
        impl->device_table.vkCmdPipelineBarrier(commands,
                                                VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
                                                VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT,
                                                0U,
                                                0U,
                                                nullptr,
                                                0U,
                                                nullptr,
                                                1U,
                                                &back);
        images.depth_sampled = false;
    }

    // A BARRIER NAMES BOTH HALVES of a combined depth/stencil image (see depthAspectMask); the COPY names only the
    // aspect it reads, because a depth readback is the depth half - the stencil half is not a depth and nothing here
    // decodes it (which is also why a combined format is refused above: this backend serves the formats the SDK's
    // table can decode, and that table has no combined entry).
    const VkImageMemoryBarrier to_transfer =
        barrierBetween(depth_image, stateOf(ImageLayout::DepthAttachment), transferReadState(), depthAspectMask(images.depthFormat()));
    impl->device_table.vkCmdPipelineBarrier(commands,
                                            VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT,
                                            VK_PIPELINE_STAGE_TRANSFER_BIT,
                                            0U,
                                            0U,
                                            nullptr,
                                            0U,
                                            nullptr,
                                            1U,
                                            &to_transfer);

    VkBufferImageCopy region{};
    region.imageSubresource.aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT;
    region.imageSubresource.layerCount = 1U;
    region.imageExtent                 = extent;
    impl->device_table.vkCmdCopyImageToBuffer(commands, depth_image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, staging, 1U, &region);

    const VkImageMemoryBarrier to_attachment =
        barrierBetween(depth_image, transferReadState(), stateOf(ImageLayout::DepthAttachment), depthAspectMask(images.depthFormat()));
    impl->device_table.vkCmdPipelineBarrier(commands,
                                            VK_PIPELINE_STAGE_TRANSFER_BIT,
                                            VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT,
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

    // THE DECODER IS THE SDK'S TOO: one table says how many bytes a texel is, one function turns them into the
    // normalised values a probe answers with (D32/D32F arrive as their float, D16 divided by its full scale). A copy
    // that is not a whole image is not a picture of anything, so a short decode is a failure - and the HOST's vector
    // is only written once there IS a picture, which is what keeps a refused read from handing out stale values.
    std::vector<float> values = decodeDepth(target->depthFormat(),
                                            std::span<const std::byte>(static_cast<const std::byte*>(mapped), static_cast<std::size_t>(bytes)));
    impl->device_table.vkUnmapMemory(impl->device, staging_memory);

    release();
    if (values.size() != static_cast<std::size_t>(extent.width) * static_cast<std::size_t>(extent.height)) {
        return fail(ReadbackResult::Failed);
    }
    outDepths = std::move(values);
    return true;
}

Handles Device::handles() const noexcept
{
    Handles out;
    out.instance        = impl->instance;
    out.physical        = impl->physical;
    out.device          = impl->device;
    out.graphics        = impl->queue;
    out.graphics_family = impl->queue_family;
    out.validation      = impl->validation;
    return out;
}

std::uint64_t Device::contentPipelines() const noexcept
{
    return impl->content.pipelinesCreated();
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
