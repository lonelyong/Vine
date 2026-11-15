#include <vine/backend_vk/internal/Device.hpp>

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

// volk is built with VOLK_NO_DEVICE_PROTOTYPES (see third_party/volk/CMakeLists.txt): device-level commands are not
// declared as free functions, and `VolkDeviceTable` is the supported way to reach them - one table filled per device.
#include <volk.h>

#include <vine/backend_vk/internal/BarrierPlan.hpp>
#include <vine/backend_vk/internal/ContentDraw.hpp>
#include <vine/backend_vk/internal/ContentImages.hpp>
#include <vine/backend_vk/internal/PipelineCacheFile.hpp>
#include <vine/backend_vk/internal/ReadbackStaging.hpp>
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
#include <vine/graphics/ReportOnce.hpp>
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

/**
 * @brief How often the device clock is sampled: every Nth frame, starting with the first (see Impl::timestamps).
 *
 * @details
 * - MEASURED, NOT GUESSED: writing the timestamp pair and reading it back costs ~0.8 ms per frame on this machine's
 *   software rasteriser (median 8.30 ms with the clock off, 9.01-9.20 ms with it on, three runs each). A production
 *   backend samples the clock for exactly that reason - a HUD needs a value per second, not per frame.
 * - THE COST IS THE SAMPLED FRAMES' ALONE: with 30, a steady frame pays the clock once in thirty, and the frames in
 *   between answer `FrameReport::timing` DISENGAGED rather than repeating the last value (a report describes the
 *   frame that just ended - see FrameTiming).
 */
constexpr std::uint64_t kTimestampSampleFrames = 30U;

/// @brief The size a Probe device's own window comes up at (the reference backend's own default: 640x360).
constexpr int kProbeWindowWidth  = 640;
constexpr int kProbeWindowHeight = 360;

/**
 * @brief How strongly a physical device is preferred, higher being better (see the device choice in initialize).
 *
 * @details
 * - THE FIRST DEVICE THAT CAN SERVE IS NOT THE BEST ONE: a machine lists its integrated GPU before the discrete
 *   one often enough that "take the first" quietly renders on the slower device, and nothing in a picture or a log
 *   says so.
 * - A virtual GPU outranks a CPU device (a virtualised desktop still has a GPU behind it); anything the loader
 *   reports as OTHER ranks last, and ties keep the earlier device, so the choice is deterministic.
 *
 * @param type The physical device's reported type.
 * @return The preference rank.
 */
[[nodiscard]] int deviceTypeRank(const VkPhysicalDeviceType type) noexcept
{
    switch (type) {
        case VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU: return 4;
        case VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU: return 3;
        case VK_PHYSICAL_DEVICE_TYPE_VIRTUAL_GPU: return 2;
        case VK_PHYSICAL_DEVICE_TYPE_CPU: return 1;
        default: break; // OTHER, and the MAX_ENUM the loader never reports
    }
    return 0;
}

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
 * @brief Spells a byte count the way a host reads it: "3.2 MiB", "512 KiB", "900 B".
 *
 * @details
 * - ONE DECIMAL FOR MiB is enough to tell 3.2 from 3.9, and the arithmetic is integer-only (`tenths`), so the sentence
 *   is exact and does not depend on a formatting library's rounding.
 *
 * @param bytes The count to spell.
 * @return The count as text.
 */
[[nodiscard]] vn::String bytesText(const VkDeviceSize bytes)
{
    constexpr VkDeviceSize kKiB = 1024U;
    constexpr VkDeviceSize kMiB = 1024U * 1024U;
    if (bytes >= kMiB) {
        const std::uint64_t tenths = (static_cast<std::uint64_t>(bytes) * 10U + kMiB / 2U) / kMiB;
        return numberText(static_cast<std::uint32_t>(tenths / 10U)) + vn::String(u8".") +
               numberText(static_cast<std::uint32_t>(tenths % 10U)) + vn::String(u8" MiB");
    }
    if (bytes >= kKiB) {
        return numberText(static_cast<std::uint32_t>((bytes + kKiB / 2U) / kKiB)) + vn::String(u8" KiB");
    }
    return numberText(static_cast<std::uint32_t>(bytes)) + vn::String(u8" B");
}

/**
 * @brief Spells a `VkResult` so a sentence can name the driver's own answer instead of guessing.
 *
 * @details
 * - A HOST ACTS ON THE DIFFERENCE between `VK_ERROR_OUT_OF_DEVICE_MEMORY` (release content) and
 *   `VK_ERROR_TOO_MANY_OBJECTS` (a device limit, nothing to release) - so the number the driver returned is part of
 *   the report, not a detail for a debugger. The list covers what this backend can meet; anything else is said as
 *   unlisted rather than silently numbered.
 *
 * @param result The result to spell.
 * @return The result's Vulkan spelling, or a statement that it is not in this list.
 */
[[nodiscard]] const char8_t* resultText(const VkResult result) noexcept
{
    switch (result) {
    case VK_SUCCESS: return u8"VK_SUCCESS";
    case VK_ERROR_DEVICE_LOST: return u8"VK_ERROR_DEVICE_LOST";
    case VK_ERROR_OUT_OF_DEVICE_MEMORY: return u8"VK_ERROR_OUT_OF_DEVICE_MEMORY";
    case VK_ERROR_OUT_OF_HOST_MEMORY: return u8"VK_ERROR_OUT_OF_HOST_MEMORY";
    case VK_ERROR_TOO_MANY_OBJECTS: return u8"VK_ERROR_TOO_MANY_OBJECTS";
    case VK_ERROR_FRAGMENTED_POOL: return u8"VK_ERROR_FRAGMENTED_POOL";
    case VK_ERROR_INITIALIZATION_FAILED: return u8"VK_ERROR_INITIALIZATION_FAILED";
    case VK_ERROR_MEMORY_MAP_FAILED: return u8"VK_ERROR_MEMORY_MAP_FAILED";
    case VK_ERROR_UNKNOWN: return u8"VK_ERROR_UNKNOWN";
    default: return u8"a VkResult this backend does not name";
    }
}

/**
 * @brief Builds the sentence one memory refusal becomes (see Device::noteMemoryRefusal).
 *
 * @details
 * - THE NUMBERS A HOST CAN ACT ON: what was asked, which memory type served it, the driver's own result, and what the
 *   pool holds right now. "Device memory could not be allocated" sends a host looking for a debugger; "3.2 MiB of
 *   device-local memory (type 2) could not be allocated: VK_ERROR_OUT_OF_DEVICE_MEMORY; the pool holds 64.0 MiB
 *   reserved, 61.2 MiB in use" sends it looking for content to release.
 *
 * @param what   What the pool was doing, as a phrase ("the device is out of memory").
 * @param report The pool's account of the refusal.
 * @return The sentence.
 */
[[nodiscard]] vn::String memoryRefusalSentence(const char8_t* what, const MemoryRefusalReport& report)
{
    vn::String sentence = vn::String(u8"vk: ") + vn::String(what);
    if (report.requested_bytes > 0U) {
        sentence += vn::String(u8": ") + bytesText(report.requested_bytes) +
                    (report.usage == MemoryUsage::DeviceLocal ? u8" of device-local memory"
                                                              : u8" of host-visible memory");
        if (report.type_index != kNoMemoryType) {
            sentence += vn::String(u8" (type ") + numberText(report.type_index) + u8")";
        }
        if (report.driver_result != VK_SUCCESS) {
            sentence += vn::String(u8" was refused by ");
            sentence += resultText(report.driver_result);
        } else {
            sentence += u8" could not be allocated";
        }
    } else if (report.driver_result != VK_SUCCESS) {
        sentence += vn::String(u8": ");
        sentence += resultText(report.driver_result);
    }
    sentence += vn::String(u8"; the pool holds ") + bytesText(report.stats.reserved_bytes) + u8" reserved, " +
                bytesText(report.stats.used_bytes) + u8" in use";
    return sentence;
}

/**
 * @brief Builds a diagnostic about ONE target: severity, category, stable code, the target's name and (when it
 *        applies) the limit it exceeded.
 *
 * @details
 * - WHY A HELPER: a target refusal is the same five facts at eight sites, and the `subject` (the target's name) is
 *   what a host badges on - it must not be forgotten at one of them (see `RenderDiagnostic`).
 * - THE CODE IS THE STABLE HALF: `message` may be reworded, `code` may not (`scripts/diagnostic_codes.txt` is the
 *   registry both directions are checked against).
 *
 * @param target   The target the report is about (its name becomes the subject).
 * @param severity How bad it is.
 * @param category The coarse class.
 * @param code     The stable identifier of the cause.
 * @param message  The human sentence (with the numbers in it).
 * @param limit    The requested/supported pair, when the report is about a device limit.
 * @return The diagnostic, ready to hand to reportDiagnostic.
 */
[[nodiscard]] vn::graphics::RenderDiagnostic targetDiagnostic(const RenderTarget&                          target,
                                                             const DiagnosticSeverity                     severity,
                                                             const DiagnosticCategory                     category,
                                                             const char8_t*                               code,
                                                             vn::String                                   message,
                                                             std::optional<vn::graphics::DiagnosticLimit> limit = std::nullopt)
{
    vn::graphics::RenderDiagnostic diagnostic;
    diagnostic.severity = severity;
    diagnostic.category = category;
    diagnostic.code     = vn::String(code);
    diagnostic.message  = std::move(message);
    diagnostic.subject  = target.name();
    diagnostic.limit    = limit;
    return diagnostic;
}

/**
 * @brief Builds a diagnostic about the DEVICE itself: severity, category, stable code and the sentence.
 *
 * @details
 * - NO SUBJECT AND NO LIMIT: these reports are about the session or the pool, not about one object, and their numbers
 *   are in the sentence when there are any.
 * - The shape is the same as `targetDiagnostic` on purpose: a coded report is built by a helper and handed to
 *   `reportDiagnostic`, so the code is visible in the report call - which is what the gate scans for.
 *
 * @param severity How bad it is.
 * @param category The coarse class.
 * @param code     The stable identifier of the cause.
 * @param message  The human sentence (with the numbers in it).
 * @return The diagnostic, ready to hand to reportDiagnostic.
 */
[[nodiscard]] vn::graphics::RenderDiagnostic deviceDiagnostic(const DiagnosticSeverity severity,
                                                             const DiagnosticCategory category,
                                                             const char8_t*           code,
                                                             vn::String               message)
{
    vn::graphics::RenderDiagnostic diagnostic;
    diagnostic.severity = severity;
    diagnostic.category = category;
    diagnostic.code     = vn::String(code);
    diagnostic.message  = std::move(message);
    return diagnostic;
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

    /**
     * @brief The session's device memory: EVERY resource's slices come from here (see MemoryAllocator).
     *
     * @details
     * - ONE POOL PER SESSION, not per subsystem: the attachments, textures, streams, the ring, staging and readbacks
     *   would otherwise each search the memory types themselves and each create a `VkDeviceMemory` per resource - the
     *   count `maxMemoryAllocationCount` limits.
     * - OPENED RIGHT BEFORE THE CONTENT PATH and closed right after the last resource was released: it borrows the
     *   device and the entry points, so it must not outlive `vkDestroyDevice`.
     */
    MemoryAllocator allocator{};

    /**
     * @brief The persisted pipeline cache (see PipelineCacheFile): where the HOST said files may live, and what the
     *        session's file did.
     *
     * @details
     * - THE DIRECTORY COMES FROM `DeviceDesc::cache_directory` and is empty unless a host stated one (or the plugin's
     *   application data directory was defaulted by its factory). An empty directory is "keep nothing", not a fault.
     * - THE KEY IS READ FROM THE DEVICE, ONCE, at initialize(): vendor / device / driver / API / `pipelineCacheUUID`,
     *   which is the identity a pipeline cache is only valid for.
     * - `cache_books` is the session's answer (see Device::PipelineCacheReport); it is reset when a session starts and
     *   KEPT when it ends, because the save is part of the ending.
     */
    std::filesystem::path cache_directory{};
    PipelineCacheKey      cache_key{};
    struct CacheBooks {
        bool          persisted{ false };
        bool          discarded{ false };
        std::uint64_t loaded_bytes{ 0 };
        std::uint64_t saved_bytes{ 0 };
        bool          hits_measured{ false };
        std::uint64_t cache_hits{ 0 };
        bool          preload_handed{ false };
        bool          preload_refused{ false };
    };
    CacheBooks cache_books{};

    /// The sample counts the device's COLOUR and DEPTH attachments both offer, as Vulkan spells them
    /// (`VK_SAMPLE_COUNT_*_BIT`): one target has one sample count, so a count either attachment cannot do is a count
    /// this device cannot build (see `ensureTarget`).
    std::uint32_t supported_sample_counts{ VK_SAMPLE_COUNT_1_BIT };

    /// Whether this device can resolve a multisampled DEPTH (`VkSubpassDescriptionDepthStencilResolve`, and with it
    /// `vkCreateRenderPass2`): true from Vulkan 1.2, which is the version that made both core. A device below it still
    /// serves every other shape - only "multisampled AND depth-attached" is refused (see `ensureTarget`).
    bool depth_resolve_supported{ false };

    /// Whether this device answers PIPELINE CREATION FEEDBACK (Vulkan 1.3 or `VK_EXT_pipeline_creation_feedback`): the
    /// one observable that says a pipeline came out of the persisted cache rather than being compiled again.
    bool creation_feedback{ false };

    /**
     * @brief The device clock: one query pair written around each frame's command buffer (see FrameTiming).
     *
     * @details
     * - WHY ONE PAIR IS ENOUGH: this device submits and waits inside executeFrame, so the pair of the frame that just
     *   ran is readable as soon as its fence signals - no ring of queries, no waiting for a frame in flight.
     * - A DEVICE THAT DOES NOT REPORT `timestampComputeAndGraphics` keeps no clock at all, and the report answers
     *   disengaged rather than a fabricated zero (see FrameTiming).
     */
    VkQueryPool timestamp_pool{ VK_NULL_HANDLE };
    float       timestamp_period{ 0.0F }; // nanoseconds per tick, from the device's limits
    bool        timestamps{ false };
    std::uint64_t clock_frames{ 0 }; // frames executeFrame has begun (the sampling counter)

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

    /**
     * @brief The answers this backend cannot serve, ONE EPISODE EACH (see ReportOnce).
     *
     * @details
     * - A HOST IS TOLD ONCE PER EPISODE, and told again when the condition comes back after a clean run; the type owns
     *   both halves of that rule (`shouldReport` / `rearm`), so the episode's boundary is a line a reader can point at
     *   rather than a bare bool reset somewhere else.
     * - ONE EPISODE PER CONDITION: two refusals sharing a flag means the first one silences the second for the rest of
     *   the session - the host fixes that wiring and hears nothing about what is actually wrong.
     */
    vn::graphics::ReportOnce no_targets_report{};
    vn::graphics::ReportOnce content_report{};
    vn::graphics::ReportOnce size_unknown_report{};
    vn::graphics::ReportOnce samples_report{};
    vn::graphics::ReportOnce samples_depth_report{};
    vn::graphics::ReportOnce prepare_target_report{};
    /// A multisampled target asked to borrow its depth (one image cannot be a multi-sample attachment and a
    /// single-sample picture at once).
    vn::graphics::ReportOnce borrow_samples_report{};
    /// A target borrows a depth from one this device has not built (or one that has none).
    vn::graphics::ReportOnce borrow_source_report{};
    /// A target and the target it borrows its depth from are not equally sized.
    vn::graphics::ReportOnce borrow_extent_report{};

    /**
     * @brief The window half's refusals, one episode each (see ReportOnce).
     *
     * @details
     * - SIX CONDITIONS, NOT ONE FLAG: "this device has no window of its own", "this window's surface could not be
     *   created" and "the frame's image could not be acquired" are different facts with different repairs.
     * - THE EPISODE ENDS WHEN THE HALF WORKS: `rearmCreation` once a window target is handed out, `rearmFrames` once a
     *   window pass has acquired its image.
     */
    struct WindowReports {
        /// A Handle device was asked for a target and has no native window handle.
        vn::graphics::ReportOnce handle_missing;
        /// A Probe device's own window could not be brought up.
        vn::graphics::ReportOnce own_window;
        /// No display connection, so no window surface can exist.
        vn::graphics::ReportOnce window_system;
        /// The window's surface / swapchain could not be created.
        vn::graphics::ReportOnce attach;
        /// A window pass named no presenter this device handed out.
        vn::graphics::ReportOnce presenter;
        /// The window's next image could not be acquired.
        vn::graphics::ReportOnce acquire;

        /** @brief Ends the four creation episodes: a window target is being handed out (see createSurfaceTarget). */
        void rearmCreation() noexcept
        {
            handle_missing.rearm();
            own_window.rearm();
            window_system.rearm();
            attach.rearm();
        }

        /**
         * @brief Ends every window episode: the next session starts with nothing reported (see shutdown).
         *
         * @note
         * - The two FRAME episodes are re-armed at their own sites (a pass that found a presenter, an image that
         *   arrived), not here: those are the moments each condition is observed to have ended.
         */
        void rearmAll() noexcept
        {
            rearmCreation();
            presenter.rearm();
            acquire.rearm();
        }
    };
    WindowReports window_reports{};

    /**
     * @brief The memory pool's refusals, one episode per class (see Device::noteMemoryRefusal).
     *
     * @details
     * - ONE EPISODE PER CLASS, not one for "memory failed": an exhausted heap and a driver that refused for its own
     *   reason are different facts with different repairs, and an episode that covered both would swallow the second.
     *   `DeviceLost` is not here - it goes through `device_lost_report`, the session's own episode.
     * - THEY END WHEN AN ALLOCATION SUCCEEDS (`Ok` reports re-arm them), which is where the condition ends rather than
     *   at shutdown.
     */
    struct MemoryReports {
        /// The device heap is exhausted: releasing content may help (a warning a host can act on).
        vn::graphics::ReportOnce out_of_memory;
        /// `vkAllocateMemory` refused for a reason that is neither a lost device nor an exhausted heap.
        vn::graphics::ReportOnce driver_refused;
        /// This device has no memory type that serves the resource's usage.
        vn::graphics::ReportOnce no_type;
        /// A resource asked for zero bytes: a content defect.
        vn::graphics::ReportOnce invalid_request;
        /// `allocate` before `open`: a wiring defect.
        vn::graphics::ReportOnce not_open;
        /// The pool contradicted its own invariant: a defect in the allocator.
        vn::graphics::ReportOnce internal;

        /** @brief Ends every memory episode: an allocation worked, so the pool can allocate again. */
        void rearmAll() noexcept
        {
            out_of_memory.rearm();
            driver_refused.rearm();
            no_type.rearm();
            invalid_request.rearm();
            not_open.rearm();
            internal.rearm();
        }
    };
    MemoryReports memory_reports{};

    /**
     * @brief The device's ONE-FRAME-AT-A-TIME gate (see FrameGate): today's contract, checked instead of assumed.
     *
     * @details
     * - A second THREAD waits here; the thread that already drives a frame is refused, because waiting would be it
     *   waiting for itself. Both verbs that touch the frame machinery go through it (`executeFrame` and
     *   `prepareFrame`, which submits a transfer batch of its own).
     * - Step 2 of `ADR-2026-11-15-device-is-a-pool-sessions-own-frames.md` replaces it with per-session state; until
     *   then it is what makes "a device drives one frame at a time" true rather than documented.
     */
    vn::graphics::backend::FrameGate frames{};
    /// The episode of "a frame was started from the thread that already drives one" (see executeFrame).
    vn::graphics::ReportOnce frame_reentry_report{};

    /// True once a queue/acquire/present call answered `VK_ERROR_DEVICE_LOST`: the session is over (see
    /// Device::noteDeviceLost), so `initialized()` stops answering true and the host rebuilds it.
    bool device_lost{ false };
    /// One report for the loss itself, whatever number of calls saw it afterwards (the session is the episode).
    vn::graphics::ReportOnce device_lost_report{};

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
        /// What the last frame cost on the device (see FrameTiming); disengaged when the device keeps no clock.
        std::optional<vn::graphics::FrameTiming> timing;
        std::uint64_t                            released_nodes{ 0 };
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
     * @param lent     The depths THIS FRAME lends to another target (see RenderTarget::shareDepth); such an image
     *                 cannot be a sampled picture and a pass' depth attachment at once, so it is skipped.
     * @param out      Receives the binding; empty (`view == VK_NULL_HANDLE`) when the pass samples no readable map.
     * @return true when a map was resolved (and transitioned).
     */
    [[nodiscard]] bool resolveShadow(VkCommandBuffer                            commands,
                                     std::span<const ImageSource>                inputs,
                                     const std::unordered_set<const RenderTarget*>& lent,
                                     ContentDraw::ShadowSource&                  out)
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
            if (lent.count(&map) != 0) {
                // LENDING REVOKES SAMPLING: another target in this frame draws against this map's depth, so the image
                // has to stay a depth attachment - a shader cannot sample it and a pass cannot attach it in the same
                // frame (see RenderTarget::shareDepth). Skipped rather than transitioned, because the transition the
                // borrower's pass depends on is the one already in effect.
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
    // THE HOST'S DIRECTORY, converted ONCE: vn::String carries UTF-8 bytes, and std::filesystem::path is what the file
    // work speaks, so the two meet here instead of at every use.
    if (!desc.cache_directory.empty()) {
        const std::u8string text = std::u8string(desc.cache_directory.c_str());
        impl->cache_directory    = std::filesystem::path(text);
    }
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
    if (impl->device_lost) {
        // A LOST SESSION CANNOT BE REUSED: the rebuild a host is asking for starts by letting the dead one go.
        // Vulkan allows a lost device's objects to be destroyed, which is what shutdown() does (and why the process
        // does not have to leak them to recover).
        shutdown();
    }
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

    // THE DEVICE: the BEST one that can serve this session (a graphics queue, plus present support when a surface is
    // named) - see deviceTypeRank for why "first" is not the answer. M0 creates no surface, so no present support is
    // asked for there.
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

    int best_rank = -1; // no candidate yet: every rank (>= 0) is better than "none"
    for (const VkPhysicalDevice candidate : physical_devices) {
        std::uint32_t family_count = 0;
        vkGetPhysicalDeviceQueueFamilyProperties(candidate, &family_count, nullptr);
        if (family_count == 0U) {
            continue;
        }
        std::vector<VkQueueFamilyProperties> families(family_count);
        vkGetPhysicalDeviceQueueFamilyProperties(candidate, &family_count, families.data());
        std::uint32_t serving_family = std::numeric_limits<std::uint32_t>::max();
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
            serving_family = index;
            break;
        }
        if (serving_family == std::numeric_limits<std::uint32_t>::max()) {
            continue;
        }

        // THE BEST CANDIDATE SO FAR, not the first one (see deviceTypeRank): a machine that lists its integrated GPU
        // first must not be the reason the slower device draws every frame.
        VkPhysicalDeviceProperties candidate_properties{};
        vkGetPhysicalDeviceProperties(candidate, &candidate_properties);
        const int rank = deviceTypeRank(candidate_properties.deviceType);
        if (rank > best_rank) {
            best_rank          = rank;
            impl->physical     = candidate;
            impl->queue_family = serving_family;
        }
    }
    if (impl->physical == VK_NULL_HANDLE) {
        reportDiagnostic(DiagnosticSeverity::Error,
                         DiagnosticCategory::InitFailed,
                         u8"vk: no physical device has a graphics queue family");
        shutdown();
        return false;
    }

    // THE CHOSEN DEVICE'S PROPERTIES, ONCE: the extension choice below, the pipeline cache's key and the capabilities
    // all describe the SAME device, so they read one value (see PipelineCacheKey).
    VkPhysicalDeviceProperties properties{};
    vkGetPhysicalDeviceProperties(impl->physical, &properties);

    constexpr float         kQueuePriority = 1.0F;
    VkDeviceQueueCreateInfo queue_info{};
    queue_info.sType            = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
    queue_info.queueFamilyIndex = impl->queue_family;
    queue_info.queueCount       = 1U;
    queue_info.pQueuePriorities = &kQueuePriority;

    // THE FEATURES THIS BACKEND ASKS FOR ARE THE ONES THE SDK CAN EXPRESS (see RenderState and PassRenderState): a
    // WIREFRAME view (`fillModeNonSolid`), a POINT SIZE (`largePoints`), a THICK LINE (`wideLines`), ANISOTROPIC
    // filtering (`samplerAnisotropy`) and MRT with per-attachment blends (`independentBlend`). Every one of those is a
    // switch a host may throw, and a switch that fails at pipeline creation is a feature this backend does not have -
    // so what the device offers is asked for, and what was asked for is what the capabilities below report.
    VkPhysicalDeviceFeatures available_features{};
    vkGetPhysicalDeviceFeatures(impl->physical, &available_features);
    VkPhysicalDeviceFeatures wanted_features{};
    wanted_features.fillModeNonSolid  = available_features.fillModeNonSolid;
    wanted_features.largePoints       = available_features.largePoints;
    wanted_features.wideLines         = available_features.wideLines;
    wanted_features.samplerAnisotropy = available_features.samplerAnisotropy;
    wanted_features.independentBlend  = available_features.independentBlend;

    VkDeviceCreateInfo device_info{};
    device_info.sType                = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
    device_info.queueCreateInfoCount = 1U;
    device_info.pQueueCreateInfos    = &queue_info;
    device_info.pEnabledFeatures     = &wanted_features;

    // THE DEVICE EXTENSIONS THIS BACKEND USES, EACH ASKED FOR ONLY IF THE DEVICE OFFERS IT:
    //  - VK_KHR_swapchain, only when a window is to be served. Without it this device presents nothing: a warning, and
    //    a device that still runs off-screen frames.
    //  - VK_EXT_pipeline_creation_feedback, which is WHAT MAKES "the persisted cache helped" ANSWERABLE: the driver says
    //    per creation whether it was served from the cache (see ContentDraw's hit counter). Vulkan 1.3 made it core,
    //    so a 1.3 device needs no extension for it. A device with neither reports NO answer, never a zero.
    bool has_swapchain = false;
    bool has_creation_feedback = properties.apiVersion >= VK_API_VERSION_1_3;
    {
        std::uint32_t                      available = 0;
        std::vector<VkExtensionProperties> extensions;
        if (vkEnumerateDeviceExtensionProperties(impl->physical, nullptr, &available, nullptr) == VK_SUCCESS && available > 0U) {
            extensions.resize(available);
            if (vkEnumerateDeviceExtensionProperties(impl->physical, nullptr, &available, extensions.data()) != VK_SUCCESS) {
                extensions.clear();
            }
        }
        for (const VkExtensionProperties& extension : extensions) {
            if (std::strcmp(extension.extensionName, "VK_KHR_swapchain") == 0) {
                has_swapchain = true;
            }
            if (std::strcmp(extension.extensionName, "VK_EXT_pipeline_creation_feedback") == 0) {
                has_creation_feedback = true;
            }
        }
    }
    std::vector<const char*> device_extension_names;
    if (impl->presentation_wanted) {
        if (has_swapchain) {
            device_extension_names.push_back("VK_KHR_swapchain");
        }
        else {
            reportDiagnostic(DiagnosticSeverity::Warning,
                             DiagnosticCategory::UnsupportedRequest,
                             u8"vk: the device does not offer VK_KHR_swapchain; this device presents nothing");
            impl->presentation_wanted = false;
        }
    }
    if (has_creation_feedback && properties.apiVersion < VK_API_VERSION_1_3) {
        device_extension_names.push_back("VK_EXT_pipeline_creation_feedback");
    }
    impl->creation_feedback = has_creation_feedback;
    if (!device_extension_names.empty()) {
        device_info.enabledExtensionCount   = static_cast<std::uint32_t>(device_extension_names.size());
        device_info.ppEnabledExtensionNames = device_extension_names.data();
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

    // THE MEMORY POOL COMES FIRST OF EVERYTHING THAT OWNS MEMORY: every resource below - the window's depth image, the
    // attachments, the textures, the streams and the ring - asks it for a slice, and it is the one place that answers
    // which memory type serves a purpose (see MemoryAllocator). `bufferImageGranularity` is the device's rule about
    // which slices may share a page, so it is a device fact, read here and nowhere else.
    if (!impl->allocator.open(impl->device_table, impl->device, impl->memory, properties.limits.bufferImageGranularity)) {
        reportDiagnostic(DiagnosticSeverity::Error,
                         DiagnosticCategory::InitFailed,
                         u8"vk: the device's memory pool could not be opened");
        shutdown();
        return false;
    }

    // THE ONE PLACE AN ALLOCATION'S OUTCOME BECOMES AN ANSWER (see noteMemoryRefusal): the pool reports every
    // allocation here, so no allocation site can drop the reason. That is the defect this wiring removes - five of the
    // six sites used to throw the reason away, so a lost device read as "out of memory" all the way to the host.
    impl->allocator.setFailureSink([this](const MemoryRefusalReport& report) { noteMemoryRefusal(report); });

    // THE TIMESLICE WHERE THE SWAPCHAINS ARE MADE: the device (and therefore its tables) exist, so each window's
    // surface can now be paired with a swapchain. A target that cannot be attached stays detached: the host still
    // holds the object it was given, and every later call answers the truth instead of touching a dead surface.
    if (impl->presentation_wanted) {
        const WindowDevice window_device{ &impl->device_table, impl->instance, impl->physical, impl->device, impl->queue, impl->queue_family, &impl->allocator };
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

    // DEPTH RESOLVE IS THE DEVICE'S API VERSION (see Impl::depth_resolve_supported): the render pass that carries a
    // `pDepthStencilResolveAttachment` is created with `vkCreateRenderPass2` under a
    // `VkSubpassDescriptionDepthStencilResolve`, and both are Vulkan 1.2.
    impl->depth_resolve_supported = properties.apiVersion >= VK_API_VERSION_1_2;

    // THE DEVICE CLOCK (see FrameTiming): two queries per frame, written around the frame's own command buffer.
    impl->timestamp_period = properties.limits.timestampPeriod;
    if (properties.limits.timestampComputeAndGraphics != VK_FALSE && impl->timestamp_period > 0.0F) {
        VkQueryPoolCreateInfo query_info{};
        query_info.sType      = VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO;
        query_info.queryType  = VK_QUERY_TYPE_TIMESTAMP;
        query_info.queryCount = 2U;
        if (impl->device_table.vkCreateQueryPool(impl->device, &query_info, nullptr, &impl->timestamp_pool) == VK_SUCCESS) {
            impl->timestamps = true;
        }
        else {
            impl->timestamp_pool = VK_NULL_HANDLE;
        }
    }

    impl->caps                         = Capabilities{};
    impl->caps.render_targets          = true;
    impl->caps.presentation            = impl->presentation_wanted;
    impl->caps.features                = 0U;
    // THE FEATURE BITS ARE WHAT WAS ENABLED, not what the device might do: each one is set from the SAME value
    // `pEnabledFeatures` carried, so a switch a host sees advertised is a switch the driver will accept.
    // Anisotropy additionally requires a limit above 1: a bit saying "anisotropy is available" beside a factor of 1
    // would advertise a quality setting that cannot do anything.
    if (wanted_features.fillModeNonSolid == VK_TRUE) {
        impl->caps.features |= Capabilities::bit(Capabilities::Feature::Wireframe);
    }
    if (wanted_features.samplerAnisotropy == VK_TRUE && properties.limits.maxSamplerAnisotropy > 1.0F) {
        impl->caps.features |= Capabilities::bit(Capabilities::Feature::AnisotropicFiltering);
    }
    // GPU TIMINGS IS A DEVICE FACT, not a promise this backend measures anything yet (see Feature::GpuTimings): the
    // device can write timestamps for graphics and compute work.
    if (properties.limits.timestampComputeAndGraphics != VK_FALSE) {
        impl->caps.features |= Capabilities::bit(Capabilities::Feature::GpuTimings);
    }
    // ONE target has ONE sample count, and its colour and depth attachments must both support it, so the set this
    // device offers is the INTERSECTION of the two (a device whose depth tops out lower is the binding side). That
    // intersection is a `VkSampleCountFlags` BIT SET, while `Capabilities::max_samples` states the largest COUNT a
    // target may ask for - the number a host hands to `RenderTarget::setSamples` - so the limit is read OUT of the
    // set rather than being the set. The same set is what `ensureTarget` clamps a request against.
    impl->supported_sample_counts      = static_cast<std::uint32_t>(properties.limits.framebufferColorSampleCounts & properties.limits.framebufferDepthSampleCounts);
    impl->caps.max_samples             = largestSampleCount(impl->supported_sample_counts);
    // THE LIMITS TRAVEL BESIDE THE BITS, and each is the device's own answer: "anisotropy exists" and "how much of
    // it" are different facts, and so are "wide lines are allowed" and "how wide".
    impl->caps.max_anisotropy          = impl->caps.supports(Capabilities::Feature::AnisotropicFiltering) ? properties.limits.maxSamplerAnisotropy : 1.0F;
    impl->caps.max_line_width          = properties.limits.lineWidthRange[1] > 0.0F ? properties.limits.lineWidthRange[1] : 1.0F;
    // THE DEVICE'S OWN COUNT: an off-screen pass here can reference as many colour attachments as the render pass
    // takes, and capping that below what the device offers would be an amputation nothing in this backend needs.
    impl->caps.max_color_attachments   = properties.limits.maxColorAttachments;

    // THE PIPELINE CACHE FILE (see PipelineCacheFile): a cache is valid for ONE device and driver identity, so the key
    // is read here and the file is refused unless it states the same one. A refused file is a slower start, never a
    // failed session - the next shutdown overwrites it with this session's.
    impl->cache_books            = Impl::CacheBooks{};
    impl->cache_key              = PipelineCacheKey::of(properties);
    std::vector<std::byte>       initial_pipeline_cache;
    if (!impl->cache_directory.empty()) {
        PipelineCacheRefusal refusal = PipelineCacheRefusal::Missing;
        initial_pipeline_cache       = PipelineCacheFile::read(impl->cache_directory, impl->cache_key, refusal);
        impl->cache_books.persisted    = true;
        impl->cache_books.loaded_bytes = initial_pipeline_cache.size();
        impl->cache_books.discarded    = refusal != PipelineCacheRefusal::Ok && refusal != PipelineCacheRefusal::Missing;
    }

    // M2'S CONTENT PATH: the layouts, the descriptor pool and the caches every draw uses. A device that cannot build
    // them is not a device that can draw, so it refuses to come up rather than answering frames with nothing.
    if (!impl->content.create(impl->device_table,
                              impl->device,
                              impl->allocator,
                              properties.limits.minUniformBufferOffsetAlignment,
                              initial_pipeline_cache)) {
        reportDiagnostic(DiagnosticSeverity::Error,
                         DiagnosticCategory::InitFailed,
                         u8"vk: the content layout / descriptor pool could not be created");
        shutdown();
        return false;
    }
    // THE TWO RASTERISATION LIMITS THE RECORDERS HAVE TO KNOW: a line wider than the device's range is an invalid
    // command, and an anisotropy factor past the device's maximum is an invalid sampler - both are clamped to the
    // numbers the capabilities just reported, so a host asking for more gets the device's best answer instead of a
    // validation error.
    impl->content.setLineWidthLimit(impl->caps.max_line_width);
    impl->content.images().setMaxAnisotropy(impl->caps.max_anisotropy);
    impl->content.setCreationFeedback(impl->creation_feedback);

    // WHAT THE RECORDER DID WITH THE FILE, recorded where it will outlive the session (see pipelineCacheReport): the
    // hand-over and any refusal happen ONCE, at creation, and `content.destroy()` is what would erase them.
    impl->cache_books.hits_measured   = impl->creation_feedback;
    impl->cache_books.preload_handed  = impl->content.pipelineCachePreloadHanded();
    impl->cache_books.preload_refused = impl->content.pipelineCachePreloadRefused();

    return true;
}

void Device::shutdown()
{
    if (impl->device != VK_NULL_HANDLE) {
        // A counted stop: every queue is idle before the objects it recorded against go.
        (void)impl->device_table.vkDeviceWaitIdle(impl->device);

        // THE SESSION'S PIPELINE CACHE GOES TO THE HOST'S DIRECTORY FIRST: `vkGetPipelineCacheData` needs the cache
        // object, which `content.destroy()` below is about to destroy. A write that fails is a cold start next time,
        // never a failed shutdown (the file is an optimisation).
        impl->cache_books.cache_hits = impl->content.pipelineCacheHits();
        if (!impl->cache_directory.empty()) {
            const std::vector<std::byte> pipeline_cache = impl->content.pipelineCacheBytes();
            if (PipelineCacheFile::write(impl->cache_directory, impl->cache_key, pipeline_cache)) {
                impl->cache_books.saved_bytes = pipeline_cache.size();
            }
        }

        // Pipelines are keyed by render-pass COMPATIBILITY, so they hold no render-pass handle; destroying the content
        // caches first is still the order that keeps teardown simple (the queue is idle, nothing names them).
        impl->content.destroy();

        for (auto& [target, images] : impl->targets) {
            (void)target;
            destroyTarget(impl->device_table, impl->device, impl->allocator, images);
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
        if (impl->timestamp_pool != VK_NULL_HANDLE) {
            impl->device_table.vkDestroyQueryPool(impl->device, impl->timestamp_pool, nullptr);
            impl->timestamp_pool = VK_NULL_HANDLE;
        }
        impl->timestamps = false;
        impl->books.timing.reset();

        // THE POOL GOES AFTER THE RESOURCES IT SERVED, BEFORE THE DEVICE IT BORROWED: every slice above has been
        // released by now, so this unmaps and frees the blocks while the entry points still exist.
        impl->allocator.close();

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
    impl->default_program        = nullptr;
    // THE NEXT SESSION STARTS WITH NOTHING REPORTED: these episodes are about one session's wiring and one session's
    // answers, so the rebuild a lost device needs must be able to tell its own story (see ReportOnce).
    impl->no_targets_report.rearm();
    impl->content_report.rearm();
    impl->size_unknown_report.rearm();
    impl->samples_report.rearm();
    impl->samples_depth_report.rearm();
    impl->prepare_target_report.rearm();
    impl->borrow_samples_report.rearm();
    impl->borrow_source_report.rearm();
    impl->borrow_extent_report.rearm();
    impl->window_reports.rearmAll();
    impl->depth_resolve_supported = false;
    impl->device_lost             = false;
    impl->device_lost_report.rearm();
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
    // A LOST DEVICE IS NOT UP, even though its handle is still there: that is the answer a host rebuilds on (see
    // noteDeviceLost). The handle survives until shutdown()/initialize() so the objects can be destroyed in order.
    return impl->device != VK_NULL_HANDLE && !impl->device_lost;
}

bool Device::noteDeviceLost(const VkResult result, const char* what)
{
    if (result != VK_ERROR_DEVICE_LOST) {
        return false;
    }
    impl->device_lost = true;
    if (impl->device_lost_report.shouldReport()) {
        // THE SENTENCE SAYS WHAT TO DO, not only what happened: this is the one failure a retry cannot fix.
        const std::string sentence = std::string("vk: ") + (what != nullptr ? what : "a device call") +
                                     " answered VK_ERROR_DEVICE_LOST; this session cannot continue - rebuild it with "
                                     "shutdown() and initialize()";
        vn::graphics::RenderDiagnostic diagnostic;
        diagnostic.severity = DiagnosticSeverity::Error;
        diagnostic.category = DiagnosticCategory::SubmissionFailed;
        diagnostic.code     = vn::String(u8"vk.session.device_lost");
        diagnostic.message  = vn::String::fromUtf8(sentence);
        reportDiagnostic(diagnostic);
    }
    return true;
}

void Device::noteMemoryRefusal(const MemoryRefusalReport& report)
{
    if (report.refusal == MemoryRefusal::Ok) {
        // THE EPISODES END WHERE THE CONDITION ENDS: an allocation that worked says the pool can allocate again, which
        // is the moment each of these conditions is observed to be over (the repository's ReportOnce rule).
        impl->memory_reports.rearmAll();
        return;
    }

    // ONE CASE PER CLASS, because each has a different repair - and because an episode that covered two classes would
    // swallow the second. Each carries its own STABLE CODE (`vk.memory.*`), which is what a host or a test matches on;
    // the category is the coarse class beside it.
    switch (report.refusal) {
    case MemoryRefusal::Ok:
        return; // handled above; kept so the switch stays exhaustive without a default
    case MemoryRefusal::DeviceLost:
        // THE SAME FACT AS A LOST SUBMIT, so it takes the same path: initialized() stops answering true, later frames
        // are skipped without touching the device, and the session's ONE sentence names the call that saw it.
        (void)noteDeviceLost(report.driver_result, "vkAllocateMemory");
        return;
    case MemoryRefusal::OutOfMemory:
        if (impl->memory_reports.out_of_memory.shouldReport()) {
            reportDiagnostic(deviceDiagnostic(DiagnosticSeverity::Warning,
                                              DiagnosticCategory::ResourcePressure,
                                              u8"vk.memory.out_of_memory",
                                              memoryRefusalSentence(u8"the device is out of memory", report)));
        }
        return;
    case MemoryRefusal::DriverRefused:
        if (impl->memory_reports.driver_refused.shouldReport()) {
            // AN ERROR, NOT A PRESSURE: nothing a host releases changes a device limit or a driver bug, so this is a
            // fact to escalate rather than a capacity to relieve.
            reportDiagnostic(deviceDiagnostic(DiagnosticSeverity::Error,
                                              DiagnosticCategory::ResourcePressure,
                                              u8"vk.memory.driver_refused",
                                              memoryRefusalSentence(u8"the driver refused an allocation", report)));
        }
        return;
    case MemoryRefusal::NoType:
        if (impl->memory_reports.no_type.shouldReport()) {
            reportDiagnostic(deviceDiagnostic(DiagnosticSeverity::Warning,
                                              DiagnosticCategory::UnsupportedRequest,
                                              u8"vk.memory.no_type",
                                              memoryRefusalSentence(u8"this device has no memory type that serves the request", report)));
        }
        return;
    case MemoryRefusal::InvalidRequest:
        if (impl->memory_reports.invalid_request.shouldReport()) {
            reportDiagnostic(deviceDiagnostic(DiagnosticSeverity::Error,
                                              DiagnosticCategory::ContentSkipped,
                                              u8"vk.memory.invalid_request",
                                              memoryRefusalSentence(u8"a resource asked for zero bytes of memory", report)));
        }
        return;
    case MemoryRefusal::NotOpen:
        if (impl->memory_reports.not_open.shouldReport()) {
            reportDiagnostic(deviceDiagnostic(DiagnosticSeverity::Error,
                                              DiagnosticCategory::InitFailed,
                                              u8"vk.memory.not_open",
                                              memoryRefusalSentence(u8"device memory was asked for before the pool was opened", report)));
        }
        return;
    case MemoryRefusal::Internal:
        if (impl->memory_reports.internal.shouldReport()) {
            reportDiagnostic(deviceDiagnostic(DiagnosticSeverity::Error,
                                              DiagnosticCategory::InitFailed,
                                              u8"vk.memory.internal",
                                              memoryRefusalSentence(u8"the pool could not place a resource in a block it had just made", report)));
        }
        return;
    }
}

void Device::setMemoryBlockCreator(MemoryBlockCreator creator)
{
    // A TEST SEAM, NOT A POLICY (see the header): blocks are created lazily, so this is read by the first target or
    // texture that needs one, and empty means the device table's own `vkAllocateMemory`.
    impl->allocator.setBlockCreator(std::move(creator));
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
        destroyTarget(impl->device_table, impl->device, impl->allocator, found->second);
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
            if (impl->window_reports.handle_missing.shouldReport()) {
                reportDiagnostic(DiagnosticSeverity::Warning,
                                 DiagnosticCategory::UnsupportedRequest,
                                 u8"vk: DevicePresentation::Handle needs a host window handle; this device brings up none");
            }
            return nullptr;
        }
        // THE CONNECTION AND THE WINDOW ARE TWO FACTS: a display that cannot be reached and a window that would not
        // come up have different repairs, so each is its own episode (see WindowReports).
        vn::String why;
        if (!ensureWindowSystem(&why)) {
            answer(SurfaceTargetResult::UnsupportedSurface);
            if (impl->window_reports.window_system.shouldReport()) {
                reportDiagnostic(DiagnosticSeverity::Warning,
                                 DiagnosticCategory::UnsupportedRequest,
                                 why.empty() ? vn::String(u8"vk: no window system is available, so no window surface can be created") : std::move(why));
            }
            return nullptr;
        }
        if (!impl->window_system->openOwnWindow(kProbeWindowWidth, kProbeWindowHeight, &why)) {
            answer(SurfaceTargetResult::UnsupportedSurface);
            if (impl->window_reports.own_window.shouldReport()) {
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
            if (impl->window_reports.window_system.shouldReport()) {
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
            &impl->device_table, impl->instance, impl->physical, impl->device, impl->queue, impl->queue_family, &impl->allocator
        };
        vn::String reason;
        if (!target->attach(window_device, &reason)) {
            answer(SurfaceTargetResult::UnsupportedSurface);
            if (impl->window_reports.attach.shouldReport()) {
                reportDiagnostic(DiagnosticSeverity::Warning,
                                 DiagnosticCategory::UnsupportedRequest,
                                 reason.empty() ? vn::String(u8"vk: the window target could not be created") : std::move(reason));
            }
            return nullptr;
        }
        impl->window_reports.attach.rearm(); // the surface and swapchain exist: this episode is over
        // A target accepted after initialize() still means this session presents.
        impl->presentation_wanted = true;
        impl->caps.presentation    = true;
    }
    // Before initialize(): the target only remembers the handle; the surface and swapchain are created when the
    // instance and the device exist (see initialize).
    WindowTarget* raw = target.get();
    impl->surfaces.push_back(std::move(target));
    // A TARGET IS BEING HANDED OUT: every creation episode is over, so the next refusal is a new one (see
    // WindowReports).
    impl->window_reports.rearmCreation();
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
    if (wanted.width > 0 && wanted.height > 0) {
        // A USABLE EXTENT EXISTS: the "unusable extent" episode is over (see ReportOnce). It is a different fact from
        // the sample-count refusals below, so it has an episode of its own.
        impl->size_unknown_report.rearm();
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
        if (impl->samples_report.shouldReport()) {
            // The two reasons a count is unbuildable are said as the difference they are: a request past the limit,
            // and a number that is not a sample count at all. Both carry the pair a host acts on (requested vs
            // offered) and their own code, so the two are distinguishable without reading the sentence.
            const vn::String request = vn::String(u8"vk: a target asks for ") + numberText(requested_samples) + u8" samples per pixel";
            const bool       above   = requested_samples > largest_samples;
            const vn::String why     = above
                                           ? request + u8", above what this device offers (up to " + numberText(largest_samples) +
                                                 u8"; see Capabilities::max_samples)"
                                           : request + u8", which is not a sample count this device offers (the counts are powers of two, up to " +
                                                 numberText(largest_samples) + u8"; see Capabilities::max_samples)";
            reportDiagnostic(targetDiagnostic(target,
                                              DiagnosticSeverity::Warning,
                                              DiagnosticCategory::UnsupportedRequest,
                                              above ? u8"vk.target.samples_above_limit" : u8"vk.target.samples_not_a_count",
                                              why,
                                              vn::graphics::DiagnosticLimit{ requested_samples, largest_samples }));
        }
        return false;
    }
    impl->samples_report.rearm(); // a target with a count this device builds: the episode is over
    wanted.shape.samples = requested_samples;
    wanted.shape.subpass = 0U;
    // A MULTISAMPLED DEPTH IS RESOLVED, and a device that cannot do that refuses the combination (ADR-2026-11-14).
    // `TargetImages` builds the single-sample picture the render pass resolves into and every reader uses
    // (`depthPicture`: the shadow path samples it), with `sample0` as the mode - so multisampling cannot change the
    // depth a later pass tests against. The resolve is Vulkan 1.2 (see Impl::depth_resolve_supported), and a device
    // below that genuinely cannot serve this shape: nothing is created, nothing is drawn, and `readColorBuffer`
    // answers NotReady.
    if (requested_samples > 1U && target.hasDepth() && !impl->depth_resolve_supported) {
        if (impl->samples_depth_report.shouldReport()) {
            reportDiagnostic(targetDiagnostic(target,
                                              DiagnosticSeverity::Warning,
                                              DiagnosticCategory::UnsupportedRequest,
                                              u8"vk.target.multisampled_depth_unsupported",
                                              vn::String(u8"vk: a target asks for ") + numberText(requested_samples) +
                                                  u8" samples per pixel and a depth attachment; this device cannot resolve a multisampled depth "
                                                  u8"(it needs Vulkan 1.2's depth/stencil resolve), so the target is refused (see Capabilities::max_samples)",
                                              vn::graphics::DiagnosticLimit{ requested_samples, 1U }));
        }
        return false;
    }
    impl->samples_depth_report.rearm(); // a depth-attached target this device can resolve: the episode is over

    // A BORROWED DEPTH (RenderTarget::shareDepth): this target draws against ANOTHER target's depth image instead of
    // owning one, which is what makes "the composite tests against the G-buffer's depth" a composition rather than
    // two copies of one picture. Three facts are the host's to satisfy, and each refusal is its own episode because
    // each has a different repair:
    //  - a MULTISAMPLED target cannot borrow: one image cannot be a multi-sample attachment for one pass and a
    //    single-sample picture for another (the SDK says every backend refuses that combination);
    //  - the LENDER is a target THIS device has built and that has a depth - the SDK's ordering rule (the lender
    //    renders earlier in the frame) is what makes that true, and a frame that breaks it is wiring, not a capability;
    //  - both targets are EQUALLY SIZED: the lender's picture is bound into a framebuffer of the borrower's extent.
    const vn::graphics::RenderTarget* lender      = target.depthSource();
    VkImageView                       lender_view = VK_NULL_HANDLE;
    if (lender != nullptr) {
        if (requested_samples > 1U) {
            if (impl->borrow_samples_report.shouldReport()) {
                reportDiagnostic(targetDiagnostic(target,
                                                  DiagnosticSeverity::Warning,
                                                  DiagnosticCategory::UnsupportedRequest,
                                                  u8"vk.target.borrow_multisampled",
                                                  vn::String(u8"vk: a target asks for ") + numberText(requested_samples) +
                                                      u8" samples per pixel and borrows its depth (RenderTarget::shareDepth); one depth image "
                                                      u8"cannot be a multi-sample attachment for one pass and a single-sample picture for another",
                                                  vn::graphics::DiagnosticLimit{ requested_samples, 1U }));
            }
            return false;
        }
        impl->borrow_samples_report.rearm(); // a single-sample borrower: the episode is over
        const auto source = impl->targets.find(lender);
        if (source == impl->targets.end() || !source->second.built() || !source->second.hasDepth()) {
            if (impl->borrow_source_report.shouldReport()) {
                reportDiagnostic(targetDiagnostic(target,
                                                  DiagnosticSeverity::Warning,
                                                  DiagnosticCategory::UnsupportedRequest,
                                                  u8"vk.target.borrow_source_missing",
                                                  u8"vk: a target borrows its depth from a target this device has not built (or one with no "
                                                  u8"depth attachment); the lender must render earlier in the same frame (see RenderTarget::shareDepth)"));
            }
            return false;
        }
        impl->borrow_source_report.rearm(); // the lender is built here: the episode is over
        if (lender->width() != target.width() || lender->height() != target.height()) {
            if (impl->borrow_extent_report.shouldReport()) {
                reportDiagnostic(targetDiagnostic(target,
                                                  DiagnosticSeverity::Warning,
                                                  DiagnosticCategory::UnsupportedRequest,
                                                  u8"vk.target.borrow_extent_mismatch",
                                                  vn::String(u8"vk: a target borrows the depth of a differently sized target (") + numberText(static_cast<std::uint32_t>(lender->width())) +
                                                      u8"x" + numberText(static_cast<std::uint32_t>(lender->height())) + u8" against " +
                                                      numberText(static_cast<std::uint32_t>(target.width())) + u8"x" +
                                                      numberText(static_cast<std::uint32_t>(target.height())) + u8"); a shared depth has one extent"));
            }
            return false;
        }
        impl->borrow_extent_report.rearm(); // equally sized: the episode is over
        lender_view = source->second.depthPictureView();
        // THE LENDER'S DEPTH FORMAT IS WHAT THIS TARGET'S PASS IS BUILT WITH: the borrower's own declaration is not a
        // picture anyone has, and the shape states what the attachment IS - so a lender rebuilt at another format
        // rebuilds the borrower through `planTarget`'s own comparison.
        wanted.shape.depth_format        = source->second.desc.shape.depth_format;
        wanted.shape.device_depth_format = source->second.desc.shape.device_depth_format;
    }

    TargetImages& images = impl->targets[&target];
    TargetInstance current;
    current.desc                   = images.desc;
    current.generation             = 0U;
    current.built                  = images.built();
    current.attachments_invalidated = false;

    TargetDecision decision = planTarget(current, wanted);
    // THE BORROW EDGE IS PART OF WHAT WAS BUILT even though `planTarget` does not compare it: it is not a property of
    // the shape but a relationship to another record, so a target that started or stopped borrowing - or that borrows
    // a lender whose depth picture was rebuilt - has a different framebuffer to bind. A target with no usable extent
    // still decides first, as always.
    const bool borrow_moved = images.built() && (images.borrowsDepth() != (lender != nullptr) ||
                                                 (lender != nullptr && images.borrowed_depth_view != lender_view));
    if (borrow_moved && !(decision.action == TargetAction::Repair && decision.reason == RepairReason::SizeUnknown)) {
        decision.action = TargetAction::Rebuild;
        decision.reason = RepairReason::None;
    }
    switch (decision.action) {
    case TargetAction::None:
        return true;
    case TargetAction::Repair:
        if (decision.reason == RepairReason::SizeUnknown) {
            if (impl->size_unknown_report.shouldReport()) {
                reportDiagnostic(targetDiagnostic(target,
                                                  DiagnosticSeverity::Warning,
                                                  DiagnosticCategory::TargetBuildFailed,
                                                  u8"vk.target.size_unknown",
                                                  u8"vk: a target with an unusable extent (0 in a dimension) was left unbuilt"));
            }
            return false;
        }
        // Bootstrap: the attachments exist and have the right shape; the colour attachments always CLEAR, so the first
        // pass in does not need a second variant.
        return true;
    case TargetAction::ResizeInPlace:
        images.desc = wanted;
        if (!resizeTarget(impl->device_table, impl->device, impl->allocator, images)) {
            reportDiagnostic(targetDiagnostic(target,
                                              DiagnosticSeverity::Error,
                                              DiagnosticCategory::TargetBuildFailed,
                                              u8"vk.target.resize_failed",
                                              u8"vk: the target's attachments could not be rebuilt at the new extent (see the memory report)"));
            return false;
        }
        return true;
    case TargetAction::Rebuild:
        // The shape changed: the render pass, the framebuffer and the attachments are rebuilt; pipelines are keyed by
        // compatibility, so the ones built for the old shape are simply never asked for again (and a resize, which
        // keeps the shape, reuses them - see ContentDraw).
        destroyTarget(impl->device_table, impl->device, impl->allocator, images);
        images.desc                = wanted;
        images.depth_borrowed      = lender != nullptr;
        images.borrowed_depth_view = lender_view;
        if (!buildTarget(impl->device_table, impl->device, impl->allocator, images)) {
            reportDiagnostic(targetDiagnostic(target,
                                              DiagnosticSeverity::Error,
                                              DiagnosticCategory::TargetBuildFailed,
                                              u8"vk.target.build_failed",
                                              u8"vk: the off-screen target's attachments could not be built (see the memory report)"));
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
    // ONE FRAME AT A TIME, AS A CHECKED FACT (see FrameGate): another thread waits here, and a frame started from the
    // thread that is ALREADY driving one is refused - the shared machinery (command pool, fence, ring, descriptor
    // pool) is device-wide, so a re-entrant frame would corrupt it silently. The lease releases the gate on EVERY
    // return below, which is why it is a lease and not a pair of calls.
    const vn::graphics::backend::FrameGate::Lease frame_lease = impl->frames.enter();
    if (!frame_lease) {
        if (impl->frame_reentry_report.shouldReport()) {
            reportDiagnostic(deviceDiagnostic(DiagnosticSeverity::Error,
                                              DiagnosticCategory::PassProtocolViolation,
                                              u8"vk.session.frame_reentry",
                                              u8"vk: a frame was started from the thread that is already driving one (a hook "
                                              u8"or a sink drove another engine); this frame was refused - a device drives "
                                              u8"one frame at a time, so drive the second engine from another thread or after "
                                              u8"waitForFrame"));
        }
        return PresentOutcome::Refused;
    }

    if (!initialized()) {
        if (impl->device_lost) {
            // The one loss report already said why and what to do (see noteDeviceLost): a frame asked after it is
            // skipped without touching the device, not re-explained per frame. A frame that expected a window still
            // says its picture did not arrive - Lost is the outcome a host acts on.
            return present_to != nullptr ? PresentOutcome::Lost : PresentOutcome::Skipped;
        }
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
            calls += vn::graphics::isDrawingCall(description) ? 1U : 0U;
        }
        // COUNTED ONLY FOR A PASS THIS DEVICE CAN RUN: a drawing call is recorded, and a pass whose target could not
        // be built recorded nothing (see the sample-count refusals in ensureTarget) - reporting its calls would say a
        // frame drew when nothing reached the queue.
        const bool has_effect = vn::graphics::passHasEffect(draw);
        const auto count_pass = [&counters, calls, has_effect]() {
            if (has_effect) {
                ++counters.passes;
                counters.draws += calls;
            }
        };
        if (draw.target == nullptr) {
            if (window == nullptr || !window->ready()) {
                if (impl->window_reports.presenter.shouldReport()) {
                    reportDiagnostic(DiagnosticSeverity::Warning,
                                     DiagnosticCategory::UnsupportedRequest,
                                     u8"vk: a window pass names no presenter this device handed out; it was left out");
                }
                refused_pass = true;
                continue;
            }
            impl->window_reports.presenter.rearm(); // the pass named a presenter this device handed out
            if (!window_acquired) {
                vn::String why;
                if (!window->acquire(&why)) {
                    if (window->deviceLost()) {
                        // The frame is over: nothing can run on a lost device, so the walk stops here instead of
                        // letting every later pass build against it. The sentence is reported once (see
                        // noteDeviceLost), and Lost is the outcome the host acts on.
                        (void)noteDeviceLost(VK_ERROR_DEVICE_LOST, "vkAcquireNextImageKHR");
                        return PresentOutcome::Lost;
                    }
                    if (impl->window_reports.acquire.shouldReport()) {
                        reportDiagnostic(DiagnosticSeverity::Warning,
                                         DiagnosticCategory::UnsupportedRequest,
                                         why.empty() ? vn::String(u8"vk: the window's next image could not be acquired") : std::move(why));
                    }
                    refused_pass = true;
                    continue;
                }
                impl->window_reports.acquire.rearm(); // the image arrived: this episode is over
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
        if (!refused_pass && impl->no_targets_report.shouldReport()) {
            reportDiagnostic(DiagnosticSeverity::Info,
                             DiagnosticCategory::UnsupportedRequest,
                             u8"vk: the frame had no pass this backend can run (off-screen targets and host windows are "
                             u8"served); nothing was submitted");
        }
        return PresentOutcome::Skipped;
    }
    impl->no_targets_report.rearm(); // a pass ran: the "nothing I can run" episode is over

    // THE DEPTHS THIS FRAME LENDS: every target another pass' target borrows its depth from (RenderTarget::shareDepth
    // resolves to one pointer per borrower, and the borrower was built above). `resolveShadow` skips them - an image
    // cannot be a shader's picture and a pass' depth attachment in the same frame.
    std::unordered_set<const RenderTarget*> lent_depths;
    for (const Runnable& item : runnable_passes) {
        if (item.draw->target != nullptr) {
            if (const vn::raw_ptr<RenderTarget> lender = item.draw->target->depthSource(); lender != nullptr) {
                lent_depths.insert(lender);
            }
        }
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

    // THE DEVICE CLOCK OPENS HERE (see FrameTiming): the reset is legal outside a render pass, which is where this is,
    // and the pair is closed before the buffer ends below. SAMPLED, not every frame (see kTimestampSampleFrames).
    ++impl->clock_frames;
    const bool sample_clock = impl->timestamps && (impl->clock_frames % kTimestampSampleFrames) == 1U;
    if (sample_clock) {
        impl->device_table.vkCmdResetQueryPool(commands, impl->timestamp_pool, 0U, 2U);
        impl->device_table.vkCmdWriteTimestamp(commands, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, impl->timestamp_pool, 0U);
    }

    // ONE DESCRIPTOR POOL RESET PER FRAME: the pool's sets are the per-draw uniform bindings, and the previous frame
    // is complete by now (the device submits synchronously), so nothing still names them.
    impl->content.beginFrame();
    // The pass count was taken from the frame's shape above (a pass whose draws were all skipped is still a pass
    // that ran); what the loop below adds is the drawing calls this backend actually recorded.

    bool first_window_pass = true;
    // Whether a draw this frame named could not be recorded. The refused-content episode ends when a frame records
    // everything it was handed - a frame that still names one bad draw keeps the episode open (see ReportOnce).
    bool content_refused = false;
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
        (void)impl->resolveShadow(commands, item.entry->inputs, lent_depths, shadow);

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
                                                 shadow,
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
            if (!why.empty()) {
                content_refused = true;
                if (impl->content_report.shouldReport()) {
                    reportDiagnostic(DiagnosticSeverity::Warning, category, why);
                }
            }
        }
        impl->device_table.vkCmdEndRenderPass(commands);
        if (images != nullptr) {
            images->fresh = false;
        }
    }
    if (!content_refused) {
        impl->content_report.rearm(); // every draw the frame named was recorded: the episode is over
    }

    // THE PRESENT TRANSITION, after the window's last pass and before the command buffer ends: every window pass
    // stores into COLOR_ATTACHMENT_OPTIMAL, and vkQueuePresentKHR reads PRESENT_SRC_KHR.
    if (window_acquired) {
        window->recordPresentTransition(commands);
    }

    if (sample_clock) {
        impl->device_table.vkCmdWriteTimestamp(commands, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, impl->timestamp_pool, 1U);
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

    const VkResult submit_result = impl->device_table.vkQueueSubmit(impl->queue, 1U, &submit, impl->completion);
    const bool     submitted     = submit_result == VK_SUCCESS;
    if (submitted) {
        ++impl->books.progress.submitted;
    }
    const VkResult fence_result = submitted ? impl->device_table.vkWaitForFences(impl->device, 1U, &impl->completion, VK_TRUE, UINT64_MAX)
                                            : submit_result;
    const bool     completed    = fence_result == VK_SUCCESS;
    if (completed) {
        ++impl->books.progress.completed;
        // THE PAIR IS READABLE NOW (the fence says the frame ran): one read, one value, and the report carries it as
        // the frame's device time (see FrameTiming). A frame the clock did not sample answers DISENGAGED - never the
        // previous frame's value.
        if (sample_clock) {
            std::uint64_t stamps[2]{ 0U, 0U };
            if (impl->device_table.vkGetQueryPoolResults(impl->device,
                                                         impl->timestamp_pool,
                                                         0U,
                                                         2U,
                                                         sizeof(stamps),
                                                         stamps,
                                                         sizeof(std::uint64_t),
                                                         VK_QUERY_RESULT_64_BIT) == VK_SUCCESS &&
                stamps[1] > stamps[0]) {
                const double micros = static_cast<double>(stamps[1] - stamps[0]) * static_cast<double>(impl->timestamp_period) / 1000.0;
                impl->books.timing  = vn::graphics::FrameTiming{ static_cast<std::uint64_t>(micros) };
            }
            else {
                impl->books.timing.reset();
            }
        }
        else {
            impl->books.timing.reset();
        }
    }
    (void)impl->device_table.vkResetFences(impl->device, 1U, &impl->completion);
    impl->device_table.vkFreeCommandBuffers(impl->device, impl->command_pool, 1U, &commands);
    // The per-draw uniform buffers were named by the command buffer that just finished (or failed to): the queue is
    // idle either way, so the bytes can go.
    impl->content.endFrame();

    if (!completed) {
        // A LOST DEVICE GETS ITS OWN SENTENCE, ONCE (see noteDeviceLost); the generic one is for the failures a retry
        // can still fix, which is the difference that matters to a host.
        if (!noteDeviceLost(submit_result, "vkQueueSubmit") && !noteDeviceLost(fence_result, "vkWaitForFences")) {
            reportDiagnostic(DiagnosticSeverity::Error,
                             DiagnosticCategory::SubmissionFailed,
                             u8"vk: the frame's command buffer did not run to completion");
        }
    }

    // THE FRAME'S TAIL: the executor is what presents the frame it ran (see RenderFrameExecutor::executeFrame).
    PresentOutcome outcome = PresentOutcome::Skipped;
    if (window_acquired) {
        outcome = completed ? window->present() : PresentOutcome::Lost;
        if (window->deviceLost()) {
            // A present that found the device gone is the same session-level fact as a lost submit: one report, and
            // the frame's Lost outcome is what the host acts on (see PresentOutcome::Lost).
            (void)noteDeviceLost(VK_ERROR_DEVICE_LOST, "vkQueuePresentKHR");
        }
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

    // THE SAME GATE AS executeFrame, because a warm-up records and may submit a transfer batch of its own (see
    // ADR-2026-11-14): a prepare racing a frame would share the command pool. `Refused` carries no reason - the
    // report says which refusal it was (see executeFrame).
    const vn::graphics::backend::FrameGate::Lease frame_lease = impl->frames.enter();
    if (!frame_lease) {
        if (impl->frame_reentry_report.shouldReport()) {
            reportDiagnostic(deviceDiagnostic(DiagnosticSeverity::Error,
                                              DiagnosticCategory::PassProtocolViolation,
                                              u8"vk.session.frame_reentry",
                                              u8"vk: a warm-up was started from the thread that is already driving a frame "
                                              u8"(a hook or a sink drove another engine); it was refused - a device drives one "
                                              u8"frame at a time"));
        }
        return PrepareReport{ PrepareReport::Outcome::Refused };
    }

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
            if (impl->prepare_target_report.shouldReport()) {
                reportDiagnostic(DiagnosticSeverity::Warning,
                                 DiagnosticCategory::UnsupportedRequest,
                                 u8"vk: a frame named a surface target this device did not hand out; nothing was prepared");
            }
            return PrepareReport{ PrepareReport::Outcome::Refused };
        }
        impl->prepare_target_report.rearm(); // the frame named a target this device handed out
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
    // Whether a draw this call could not prepare. The refused-content episode ends when a call prepares everything it
    // was handed, exactly as the frame path ends it (see ReportOnce).
    bool content_refused{ false };

    // THE SHAPE OF THE FRAME ASKED FOR, computed by the SDK (see FrameShape): one definition, so this backend and the
    // reference one cannot report different shapes for the same frame - and the rule is the one a recorded frame's
    // `FrameCounters` follows (a pass that draws something or announces a clear).
    const vn::graphics::FrameShape shape = vn::graphics::frameShapeOf(frame);
    report.passes = shape.passes;
    report.draws  = shape.draws;

    for (const FrameDescription::Pass& entry : frame.passes) {
        if (entry.pass == nullptr) {
            continue; // the SDK's rule: an entry that names no pass is nothing to run (see executeFrame)
        }
        if (!entry.draw.has_value()) {
            continue;
        }
        const PassDraw& draw = *entry.draw;
        if (!vn::graphics::passHasEffect(draw)) {
            continue; // nothing drawn, nothing cleared: there is nothing to prepare
        }

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
            if (!why.empty()) {
                content_refused = true;
                if (impl->content_report.shouldReport()) {
                    reportDiagnostic(DiagnosticSeverity::Warning, category, why);
                }
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
            const VkResult submit_result = impl->device_table.vkQueueSubmit(impl->queue, 1U, &submit, impl->completion);
            const VkResult fence_result  = submit_result == VK_SUCCESS
                                               ? impl->device_table.vkWaitForFences(impl->device, 1U, &impl->completion, VK_TRUE, UINT64_MAX)
                                               : submit_result;
            const bool     completed     = fence_result == VK_SUCCESS;
            (void)impl->device_table.vkResetFences(impl->device, 1U, &impl->completion);
            impl->device_table.vkFreeCommandBuffers(impl->device, impl->command_pool, 1U, &batch);
            if (completed) {
                impl->content.images().releaseUploadStaging();
            }
            else if (!noteDeviceLost(submit_result, "vkQueueSubmit") && !noteDeviceLost(fence_result, "vkWaitForFences")) {
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
    if (!content_refused) {
        impl->content_report.rearm(); // every draw the call named was prepared: the episode is over
    }
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
    report.timing   = impl->books.timing;  // the device's own clock; absent when the device keeps none (see FrameTiming)

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

    // THE COPY MECHANISM IS ReadbackStaging's (buffer, memory, command buffer, submit, wait, mapping); this function
    // says WHAT to copy and what the bytes mean. The target's own layout state (color_sampled) is still this backend's
    // knowledge, recorded below through the staging command buffer.
    const VkDeviceSize bytes = static_cast<VkDeviceSize>(images.width()) * static_cast<VkDeviceSize>(images.height()) * 4U;

    ReadbackStaging staging;
    const StagingRefusal opened = staging.open(impl->device_table, impl->device, impl->allocator, impl->command_pool, bytes);
    if (opened == StagingRefusal::Unsupported) {
        return fail(ReadbackResult::Unsupported); // no host-visible coherent memory: this device cannot copy to the host
    }
    if (opened != StagingRefusal::Ok) {
        return fail(ReadbackResult::Failed);
    }

    // RECORD: colour attachment -> transfer source -> copy -> back, so the next frame's render pass finds the layout its
    // render pass was built with (COLOR_ATTACHMENT_OPTIMAL).

    // A target a SCREEN DRAW sampled is in SHADER_READ_ONLY: put every colour attachment back before this readback's
    // own transition names COLOR_ATTACHMENT_OPTIMAL as the old layout.
    if (images.color_sampled) {
        for (const VkImage image : images.colors) {
            const VkImageMemoryBarrier back =
                barrierBetween(image, stateOf(ImageLayout::ShaderReadOnly), stateOf(ImageLayout::ColorAttachment), VK_IMAGE_ASPECT_COLOR_BIT);
            impl->device_table.vkCmdPipelineBarrier(staging.commands(),
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
    impl->device_table.vkCmdPipelineBarrier(staging.commands(),
                                            VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
                                            VK_PIPELINE_STAGE_TRANSFER_BIT,
                                            0U,
                                            0U,
                                            nullptr,
                                            0U,
                                            nullptr,
                                            1U,
                                            &to_transfer);

    staging.recordCopy(color_image,
                       VK_IMAGE_ASPECT_COLOR_BIT,
                       VkExtent3D{ static_cast<std::uint32_t>(images.width()), static_cast<std::uint32_t>(images.height()), 1U });

    const VkImageMemoryBarrier to_attachment =
        barrierBetween(color_image, transferReadState(), stateOf(ImageLayout::ColorAttachment), VK_IMAGE_ASPECT_COLOR_BIT);
    impl->device_table.vkCmdPipelineBarrier(staging.commands(),
                                            VK_PIPELINE_STAGE_TRANSFER_BIT,
                                            VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
                                            0U,
                                            0U,
                                            nullptr,
                                            0U,
                                            nullptr,
                                            1U,
                                            &to_attachment);

    const StagingRun run = staging.run(impl->queue, impl->completion);
    if (!run.ran()) {
        // The mechanism hands back the VkResults so the session-level fact is classified where every other queue call
        // is (see noteDeviceLost); the read itself answers Failed like any other failed transfer.
        (void)noteDeviceLost(run.submit, "vkQueueSubmit");
        (void)noteDeviceLost(run.wait, "vkWaitForFences");
        return fail(ReadbackResult::Failed);
    }
    const std::span<const std::byte> copied = staging.bytes();
    if (copied.size() != static_cast<std::size_t>(bytes)) {
        return fail(ReadbackResult::Failed); // a copy that is not a whole picture is not a picture of anything
    }

    // THE HOST'S VECTOR IS ONLY WRITTEN ONCE THERE IS A WHOLE PICTURE, which is what keeps a refused read from handing
    // out stale pixel counts (the SDK's rule, see RenderBackend::readColorBuffer).
    outPixels.resize(static_cast<std::size_t>(bytes));
    std::memcpy(outPixels.data(), copied.data(), static_cast<std::size_t>(bytes));
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

    ReadbackStaging staging;
    const StagingRefusal opened = staging.open(impl->device_table, impl->device, impl->allocator, impl->command_pool, bytes);
    if (opened == StagingRefusal::Unsupported) {
        return fail(ReadbackResult::Unsupported); // no host-visible coherent memory: this device cannot copy to the host
    }
    if (opened != StagingRefusal::Ok) {
        return fail(ReadbackResult::Failed);
    }

    // RECORD: depth picture -> transfer source -> copy -> back to the one layout a render pass expects, so a later
    // pass into this target finds its depth where its render pass was built to write it (DEPTH_ATTACHMENT).

    // A PASS THAT SAMPLED THIS DEPTH AS A SHADOW MAP left the picture in SHADER_READ_ONLY: put it back before this
    // readback's own transition names DEPTH_ATTACHMENT as the old layout (the mirror of readColorBuffer's
    // colour_sampled note, and the same stage masks executeFrame's restore records).
    if (images.depth_sampled) {
        const VkImageMemoryBarrier back = barrierBetween(depth_image,
                                                         stateOf(ImageLayout::ShaderReadOnly),
                                                         stateOf(ImageLayout::DepthAttachment),
                                                         depthAspectMask(images.depthFormat()));
        impl->device_table.vkCmdPipelineBarrier(staging.commands(),
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
    impl->device_table.vkCmdPipelineBarrier(staging.commands(),
                                            VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT,
                                            VK_PIPELINE_STAGE_TRANSFER_BIT,
                                            0U,
                                            0U,
                                            nullptr,
                                            0U,
                                            nullptr,
                                            1U,
                                            &to_transfer);

    staging.recordCopy(depth_image, VK_IMAGE_ASPECT_DEPTH_BIT, extent);

    const VkImageMemoryBarrier to_attachment =
        barrierBetween(depth_image, transferReadState(), stateOf(ImageLayout::DepthAttachment), depthAspectMask(images.depthFormat()));
    impl->device_table.vkCmdPipelineBarrier(staging.commands(),
                                            VK_PIPELINE_STAGE_TRANSFER_BIT,
                                            VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT,
                                            0U,
                                            0U,
                                            nullptr,
                                            0U,
                                            nullptr,
                                            1U,
                                            &to_attachment);

    const StagingRun run = staging.run(impl->queue, impl->completion);
    if (!run.ran()) {
        // The mechanism hands back the VkResults so the session-level fact is classified where every other queue call
        // is (see noteDeviceLost); the read itself answers Failed like any other failed transfer.
        (void)noteDeviceLost(run.submit, "vkQueueSubmit");
        (void)noteDeviceLost(run.wait, "vkWaitForFences");
        return fail(ReadbackResult::Failed);
    }

    // THE DECODER IS THE SDK'S TOO: one table says how many bytes a texel is, one function turns them into the
    // normalised values a probe answers with (D32/D32F arrive as their float, D16 divided by its full scale). A copy
    // that is not a whole image is not a picture of anything, so a short decode is a failure - and the HOST's vector
    // is only written once there IS a picture, which is what keeps a refused read from handing out stale values.
    std::vector<float> values = decodeDepth(target->depthFormat(), staging.bytes());
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

Device::ContentCacheCounters Device::contentCacheCounters() const noexcept
{
    const ContentDraw::Progress numbers = impl->content.progress();
    return ContentCacheCounters{ numbers.ring_builds, numbers.set_builds, numbers.sets_live };
}

MemoryStats Device::memoryStats() const noexcept
{
    return impl->allocator.stats();
}

Device::PipelineCacheReport Device::pipelineCacheReport() const noexcept
{
    PipelineCacheReport report{
        .persisted       = impl->cache_books.persisted,
        .discarded       = impl->cache_books.discarded,
        .loaded_bytes    = impl->cache_books.loaded_bytes,
        .saved_bytes     = impl->cache_books.saved_bytes,
        .hits_measured   = impl->cache_books.hits_measured,
        .cache_hits      = impl->cache_books.cache_hits,
        .preload_handed  = impl->cache_books.preload_handed,
        .preload_refused = impl->cache_books.preload_refused,
    };
    // A HIT MOVES WHILE THE SESSION RUNS, so it is read from the recorder while there is one (the rest of the report is
    // fixed when it happens and kept in the books, which is what makes it readable after shutdown).
    if (impl->device != VK_NULL_HANDLE) {
        report.cache_hits = impl->content.pipelineCacheHits();
    }
    return report;
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
