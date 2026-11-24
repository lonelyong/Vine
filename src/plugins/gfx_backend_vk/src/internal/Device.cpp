#include <vine/backend_vk/internal/Device.hpp>

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <algorithm>
#include <array>
#include <atomic>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

// volk is built with VOLK_NO_DEVICE_PROTOTYPES (see third_party/volk/CMakeLists.txt): device-level commands are not
// declared as free functions, and `VolkDeviceTable` is the supported way to reach them - one table filled per device.
#include <volk.h>
#include <shared_mutex>

#include <vine/backend_vk/internal/BarrierPlan.hpp>
#include <vine/backend_vk/internal/ContentCaches.hpp>
#include <vine/backend_vk/internal/ContentDraw.hpp>
#include <vine/backend_vk/internal/ContentImages.hpp>
#include <vine/backend_vk/internal/DynamicRendering.hpp>
#include <vine/backend_vk/internal/FrameHead.hpp>
#include <vine/backend_vk/internal/FrameScratch.hpp>
#include <vine/backend_vk/internal/NumberText.hpp>
#include <vine/backend_vk/internal/PipelineCacheFile.hpp>
#include <vine/backend_vk/internal/ReadbackStaging.hpp>
#include <vine/backend_vk/internal/SampleCounts.hpp>
#include <vine/backend_vk/internal/TargetTable.hpp>
#include <vine/backend_vk/internal/WindowSystem.hpp>
#include <vine/backend_vk/internal/StateMapping.hpp>
#include <vine/backend_vk/internal/TargetObjects.hpp>
#include <vine/backend_vk/internal/WindowTarget.hpp>
#include <vine/graphics/Capabilities.hpp>
#include <vine/graphics/FrameDescription.hpp>
#include <vine/graphics/FrameReport.hpp>
#include <vine/graphics/Geometry.hpp>
#include <vine/graphics/MaterialValues.hpp>
#include <vine/graphics/PrepareReport.hpp>
#include <vine/graphics/PresentTarget.hpp>
#include <vine/graphics/RenderTarget.hpp>
#include <vine/graphics/ReportOnce.hpp>
#include <vine/graphics/DeviceClock.hpp>
#include <vine/graphics/RetirementQueue.hpp>
#include <vine/graphics/ShaderProgram.hpp>
#include <vine/graphics/Texture.hpp>
#include <vine/graphics/Readback.hpp>

VN_BACKEND_VK_NS_BEGIN

using vn::graphics::Capabilities;
using vn::graphics::DiagnosticCategory;
using vn::graphics::DiagnosticSeverity;
using vn::graphics::DeviceDesc;
using vn::graphics::DevicePresentation;
using vn::graphics::DrawPacket;
using vn::graphics::FrameDescription;
using vn::graphics::ImageSource;
using vn::graphics::PassDraw;
using vn::graphics::PresentOutcome;
using vn::graphics::PresentTarget;
using vn::graphics::ReadbackResult;
using vn::graphics::RenderTarget;
using vn::graphics::SurfaceTargetResult;
using vn::graphics::decodeDepth;
using vn::graphics::depthReadbackOf;
using vn::backend_vk::ImageLayout;
using vn::graphics::planTarget;
using vn::graphics::RepairReason;
using vn::graphics::TargetAction;
using vn::graphics::TargetDecision;
using vn::graphics::TargetDesc;
using vn::graphics::TargetInstance;

/**
 * @brief Closes this frame's clock token on EVERY exit, then lets the park queue run.
 *
 * @details
 * - A FRAME HAS MANY EARLY RETURNS AND EXACTLY ONE SUCCESS, so "which one happened" is a bool the success path sets and
 *   the destructor acts on. A frame that bailed out before the submit must still close its token: the timeline answers
 *   the SAME open token to the next `begin()` (see FrameTimeline::begin), so leaving one open would run two frames as
 *   one - and the parked releases would never see a watermark.
 * - SUBMITTING IS NOT COMPLETING. The success path reports completion separately, after the fence (see the callers);
 *   this guard only spends the frame.
 * - THE LAST STEP IS THE QUEUE'S: `advance()` runs after the token is closed, so a frame that just proved completion
 *   releases what that proof allows. Running it in the destructor is what puts it on every path.
 * - ONE HOLD OF THE DEVICE'S CLOCK LOCK (see Impl::clock_mutex): the clock and the queue are single-threaded value
 *   types, and two drivers end their frames on two threads (S6c-2).
 */
struct FrameTokenCloser {
    vn::graphics::DeviceClock&     clock;
    vn::graphics::RetirementQueue& retirement;
    std::mutex&                             clock_mutex;
    vn::graphics::FrameToken       token;
    bool                                    submitted{ false };

    ~FrameTokenCloser()
    {
        const std::lock_guard<std::mutex> held(clock_mutex);
        if (submitted) {
            clock.submitted(token);
        }
        else {
            clock.abandoned(token);
        }
        retirement.advance(clock.timeline());
    }
};

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
    if (report.stats.free_bytes > 0U) {
        // THE ROOM THAT COULD NOT BE USED (see MemoryStats): a refusal while the pool holds free bytes means the room
        // is too shredded, which is a different repair from "the heap really is full".
        sentence += vn::String(u8" and ") + bytesText(report.stats.free_bytes) + u8" free (largest run " +
                    bytesText(report.stats.largest_free_run) + u8")";
    }
    return sentence;
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

/**
 * @brief What ONE frame did: its counters, what it built and how long it took on the device.
 *
 * @details
 * - IT LIVES WITH THE FRAME DRIVER (see `Device::FrameDriver`), not with the device: `counters` / `uploads` describe the
 *   frame that just ended, and a second driver keeps its own. `progress` is a delta of the device's cumulative numbers,
 *   which is why closing the books reads both.
 * - `uploads` stays disengaged until a frame has run: an unmeasured frame must not read like an empty one.
 */
struct FrameBooks {
    vn::graphics::FrameCounters               counters{};
    vn::graphics::FrameProgress               progress{};
    std::optional<vn::graphics::FrameUploads> uploads;
    /// What the last frame cost on the device (see FrameTiming); disengaged when the device keeps no clock.
    std::optional<vn::graphics::FrameTiming> timing;
    /// What the last frame spent on the cache lock (see FrameLocking); engaged from the first frame that ends.
    std::optional<vn::graphics::FrameLocking> locking;
};

/** @brief One snapshot of the content path's cumulative numbers (a frame's own delta is the difference of two). */
struct ContentTotals {
    std::uint64_t stream_builds{ 0 };
    std::uint64_t stream_bytes{ 0 };
    std::uint64_t program_builds{ 0 };
    std::uint64_t pipeline_builds{ 0 };
    std::uint64_t texture_builds{ 0 };
    std::uint64_t texture_bytes{ 0 };
    /// The cache lock's cumulative numbers: the frame's own use of the lock is the difference of two (see FrameLocking).
    std::uint64_t cache_lock_acquisitions{ 0 };
    std::uint64_t cache_lock_waits{ 0 };
};

/**
 * @brief ONE layout record a frame moved on ANOTHER target, and the value it had before (see `rewindUnrunFrame`).
 *
 * @details
 * - WHY IT IS NOT "RESET TO UNDEFINED": a sampled source or a shadow map may hold a picture an EARLIER frame wrote and
 *   nothing in this frame would reproduce - so a frame the GPU never ran has to put back exactly what the record said,
 *   not the "nothing was ever drawn here" value this frame's own attachment sets get.
 * - The DEPTH picture is a flag rather than a record pointer because a borrowed or resolved depth record is reached
 *   through `TargetObjects`' accessors, not by address (see `setDepthPictureLayout`).
 */
struct MovedLayout {
    TargetObjects* images{ nullptr };                        ///< The target whose record moved.
    std::size_t    color_index{ 0U };                        ///< The colour picture that moved (when `depth_picture` is false).
    bool           depth_picture{ false };                   ///< True when the DEPTH picture moved.
    ImageLayout    previous{ ImageLayout::Undefined };       ///< What the record stated before the move.
};

/**
 * @brief ONE FRAME DRIVER: the command machinery a frame needs, its books, and the gate that makes them safe.
 *
 * @details
 * - WHAT IT OWNS, and why that is not the device's: a command pool, the fence a frame is waited on, the timestamp pool
 *   its GPU time is read from, its books and the `FrameGate` that turns "one frame at a time" from a sentence into a
 *   check. Every one of those is written WHILE A FRAME RUNS and means nothing between frames - the definition of
 *   per-session state (see `.ai/design/graphics-sessions.md`).
 * - WHAT IT DOES NOT OWN: the Vulkan device and queue, the memory pool, the content caches, the targets, the clock and
 *   the park queue. Those belong to the device and are shared by every session it will ever hand out; this type reaches
 *   them through the `Impl` it borrows, which is what step 2 of the ADR turns into an explicit `createFrameDriver()`.
 * - ONE FRAME DRIVER TODAY, and the device drives its own frames through it: `Device::executeFrame` / `prepareFrame` are
 *   still the bodies, and the machinery they use is this object's. When `createFrameDriver()` hands out more than one, the
 *   bodies move in here (they will change at step 3 anyway) and the gate becomes one per driver for free.
 * - THE DEVICE DESTROYS IT (`Device::shutdown`), never the destructor: the objects need the device's entry points and
 *   an idle queue, and `shutdown` is the one place that has both.
 *
 * @warning One frame at a time per driver, CHECKED (`FrameGate`): a second thread waits, a re-entrant call is refused.
 */
class Device::FrameDriver final : public vn::graphics::RenderFrameExecutor
{
  private:
    /// The device whose services this driver borrows (it owns this object, so it outlives it). Declared FIRST so the
    /// `recorder` member can borrow the device's caches while this object is being constructed.
    Impl& owner;

  public:
    /// @brief What `create()` refused, so the device can report which object never came up.
    enum class CreateRefusal : std::uint8_t
    {
        Ok,          ///< Both objects exist (or already existed).
        NoDevice,    ///< There is no Vulkan device to create them on.
        CommandPool, ///< `vkCreateCommandPool` refused.
        Fence,       ///< `vkCreateFence` refused.
    };

  public:
    explicit FrameDriver(Impl& owner_device) noexcept;

    FrameDriver(const FrameDriver&)            = delete;
    FrameDriver& operator=(const FrameDriver&) = delete;

  public:
    PresentOutcome              executeFrame(const FrameDescription& frame, PresentTarget* present_to) override;
    vn::graphics::PrepareReport prepareFrame(const FrameDescription& frame, PresentTarget* present_to) override;
    vn::graphics::FrameReport   frameReport() const noexcept override;
    /// The default content program is a FRAME setting (see RenderFrameExecutor): this driver keeps its own, so two
    /// drivers handed out by one device may shade program-less content differently (`C8`).
    void setDefaultContentProgram(vn::intrusive_ptr<const vn::graphics::ShaderProgram> program) override;

    /** @brief Creates the objects a frame needs (idempotent; safe again after `destroy()`). */
    [[nodiscard]] CreateRefusal create() noexcept;

    /**
     * @brief Creates this driver's frame scratch over the device's layouts (after the caches exist).
     *
     * @details
     * - A SEPARATE STEP, because the scratch borrows the three descriptor-set layouts and those belong to
     *   `Device::initialize`'s content step: `create()` runs first (command pool, fence, clock window), the caches are
     *   built next, and this is called immediately after them.
     *
     * @return true when the descriptor pool and the uniform ring exist.
     */
    [[nodiscard]] bool createScratch() noexcept;

    /** @brief Destroys them (idempotent): the device is idle and nothing can still name them. */
    void destroy() noexcept;

    /**
     * @brief Gets this driver's texture-upload batch: the id the image cache releases when THIS driver's fence passed.
     *
     * @details
     * - ONE BATCH PER DRIVER (`C2`): batch 0 is the warm-up's transfer buffer. A frame must never release another
     *   driver's staging - its command buffer may still be recording (see ContentImages::BatchId).
     *
     * @return `clock_window + 1`, so the first driver's batch is 1 and 0 stays the warm-up's.
     */
    [[nodiscard]] ContentImages::BatchId uploadBatch() const noexcept
    {
        return static_cast<ContentImages::BatchId>(clock_window) + 1U;
    }

    /**
     * @brief Makes the clock's query pool hold at least @p needed queries, recreating it when it does not.
     *
     * @details
     * - GROWING IS A BETWEEN-FRAMES ACT: this driver submits and waits inside `executeFrame`, so when this runs no
     *   command buffer names the old pool. The pool is therefore sized by the LARGEST frame seen so far, not by a
     *   guessed maximum - a frame with more passes pays one recreate, once.
     * - A device that cannot create the pool keeps `timestamps == false`, and the report answers timing DISENGAGED
     *   rather than a fabricated zero (see FrameTiming).
     *
     * @param needed Queries the next frame needs (2 + passes + 1).
     * @return true when the pool holds at least @p needed queries.
     */
    [[nodiscard]] bool ensureTimestampQueries(std::uint32_t needed) noexcept;

    /**
     * @brief Closes one frame's books: what the frame built is the difference from @p before.
     *
     * @param before   The content numbers when the frame opened.
     * @param counters The frame's own recording counters (passes, draws).
     */
    void closeFrameBooks(const ContentTotals& before, const vn::graphics::FrameCounters& counters) noexcept;

  public:
    VkCommandPool command_pool{ VK_NULL_HANDLE };
    VkFence       completion{ VK_NULL_HANDLE };
    /// The GPU clock's query pool for THIS driver's frames (see ensureTimestampQueries).
    VkQueryPool   timestamp_pool{ VK_NULL_HANDLE };
    bool          timestamps{ false };
    std::uint64_t clock_frames{ 0 }; ///< Frames this driver has begun (the sampling counter).
    std::uint32_t timestamp_queries{ 0 };
    FrameBooks    books{};
    /// The program content without one of its own is drawn with, taken as a VALUE when it was announced.
    vn::graphics::ShaderProgramDescriptionPtr default_program{};
    /// Guards `default_program`: a host may announce a program while this driver's frame reads it (S6c-2).
    mutable std::mutex program_mutex{};
    ///
    /// THIS DRIVER'S CONTENT EPISODES (see ReportOnce): a frame it ran that could not record or prepare something says
    /// so once per episode. They are per DRIVER because two drivers' frames may both be in flight - one shared flag
    /// would let one engine's refusal silence the other's, and would be a data race besides.
    vn::graphics::ReportOnce content_report{};
    /// This driver ran a frame with no pass it could serve.
    vn::graphics::ReportOnce no_targets_report{};
    /// The ONE-FRAME-AT-A-TIME gate: one per driver, which is what will make two of them concurrent (see FrameGate).
    vn::graphics::FrameGate frames{};
    /// The episode of "a frame was started from the thread that already drives one".
    vn::graphics::ReportOnce frame_reentry_report{};
    ///
    /// The targets whose claim conflict was already reported in this episode (see TargetTable): a pass that
    /// lost its target says so ONCE, and a frame/prepare that completes without a conflict ends the episode.
    std::unordered_set<const RenderTarget*> claim_conflicts{};
    /// This driver's clock window: the id its completion evidence is reported under (see DeviceClock).
    vn::graphics::FrameDriverId clock_window{ 0 };
    bool                                 clock_window_live{ false };

  public:
    /**
     * @brief THIS DRIVER'S frame scratch: the uniform ring its draws write constants into, and the descriptor pool
     *        (with the set cache) they bind.
     *
     * @details
     * - IT IS THE DRIVER'S, NOT THE DEVICE'S (`G1`, and `.ai/design/graphics-sessions.md` §3.1.3): two frames' slices
     *   must not interleave, while the CACHES (compiled programs, uploaded streams, pipelines) are exactly what a
     *   second driver wants to share. `createScratch()` builds it once the device's layouts exist.
     */
    FrameScratch scratch{};
    /// THIS DRIVER'S recorder: turns one draw description into commands, over the device's caches and `scratch`.
    /// It owns neither, and it is constructed after both are in place (see the constructor).
    ContentDraw recorder;
    ///
    /// THE FRAME HEAD'S ANSWERS (see FrameHead): one entry per runnable pass, reused across frames. The recording
    /// path takes no cache lock because everything it reads was resolved into this value (S6c).
    FrameHead frame_head{};
};

struct Device::Impl {
    /// The two device verbs a driver still calls. `RenderDevice::reportDiagnostic` is protected, so a nested class
    /// cannot reach it through the device object; classifying a lost device (`noteDeviceLost`) is a DECISION OF THE
    /// DEVICE - one per failure, never per draw.
    vn::graphics::DiagnosticSink               report{};
    std::function<bool(VkResult, const char*)> note_device_lost{};

    /// THE DEVICE'S OWN FRAME DRIVER: it owns the command machinery, the frame books and the gate.
    std::unique_ptr<FrameDriver> session{};
    /// Every OTHER driver a host borrowed (see Device::createFrameDriver); the device owns them too.
    std::vector<std::unique_ptr<FrameDriver>> extra_sessions{};
    ///
    /// THE DEVICE'S TWO LOCKS, ONE FOR EACH QUESTION.
    /// - `body_gate` answers "may any frame body run while this verb runs?" (see graphics::FrameGate). FRAME BODIES
    ///   TAKE IT SHARED, so two drivers record and submit at the same time (S6c-2); a frame that PRESENTS takes it
    ///   EXCLUSIVE (the swapchain is one device resource), and so do the host verbs that cannot overlap a frame - a
    ///   readback, a target release, a teardown, the diagnostic readers. A host verb called from a frame body's thread
    ///   gets an EMPTY lease and answers a refusal (see `report_host_reentry`) instead of deadlocking.
    ///   `releaseContent` deliberately does not take it: its objects are parked against the watermark (R6).
    /// - `clock_mutex` serialises the device's completion clock and park queue (P4). Both are single-threaded value
    ///   types (see DeviceClock / RetirementQueue); a device with two drivers is their lock, and every use of
    ///   `clock.timeline()` happens inside it - the reference never escapes.
    mutable vn::graphics::FrameGate body_gate{};
    mutable std::mutex                        clock_mutex{};
    ///
    /// THE QUEUE'S EXTERNAL SYNCHRONIZATION (Vulkan: host access to a `VkQueue` must be serialized). Two drivers submit
    /// on two threads (S6c-2), and a readback submits from the host's - so every `vkQueueSubmit` and `vkDeviceWaitIdle`
    /// in this file runs under this lock. It is held for the DRIVER CALL ONLY: the fence wait, where the time goes,
    /// stays outside, which is what keeps two frames' device time overlapping.
    ///
    /// `vkQueuePresentKHR` is the one queue call that does not take it: a window frame holds `body_gate` EXCLUSIVELY, so
    /// no other submitter can be inside while it presents (see executeFrame).
    mutable std::mutex        queue_submit{};
    /// One report at a time at the host's sink: frames on two threads both report, and the sink is the host's.
    /// RECURSIVE because the sink itself may call back into the device (a host verb from a diagnostic sink is exactly
    /// the re-entry case `host_reentry_report` answers): that call reports on the same thread, and a plain mutex would
    /// turn the case into a deadlock instead of a refusal.
    mutable std::recursive_mutex report_mutex{};
    ///
    /// THE HOST-REENTRY EPISODE (see `body_gate`): a host verb called from a frame body's thread is refused, and the
    /// reason is the same for every verb - so it is said ONCE per device, naming the verb that was refused. The flag is
    /// a plain bool, so ask-and-set happens under `reentry_mutex`; the report itself happens AFTER that mutex is
    /// dropped, because the host's sink may call back into the device and would otherwise meet a mutex it holds.
    vn::graphics::ReportOnce         host_reentry_report{};
    mutable std::mutex               reentry_mutex{};
    std::function<void(const char8_t*)> report_host_reentry{};
    /// One operation at a time on the surface list: a frame resolves its presenter while a host may add one.
    mutable std::mutex        surfaces_mutex{};
    DeviceDesc      desc{};
    Capabilities    caps{};

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
    /// this device cannot build (see `TargetTable::ensure`).
    std::uint32_t supported_sample_counts{ VK_SAMPLE_COUNT_1_BIT };

    /// Whether this device can resolve a multisampled DEPTH *the way this backend states it*. Dynamic rendering makes the
    /// resolve a field (`resolveMode` / `resolveImageView`) instead of a structure, but the MODE is the device's answer:
    /// `VK_RESOLVE_MODE_SAMPLE_ZERO_BIT` in `supportedDepthResolveModes`, the only one no multisampled draw can change.
    /// Asked once in `initialize`, refused per shape in `TargetTable::ensure`.
    bool depth_resolve_supported{ false };

    /// Whether this device answers PIPELINE CREATION FEEDBACK (Vulkan 1.3 or `VK_EXT_pipeline_creation_feedback`): the
    /// one observable that says a pipeline came out of the persisted cache rather than being compiled again.
    bool creation_feedback{ false };

    /**
     * @brief The device's clock TICK, in nanoseconds (`VkPhysicalDeviceLimits::timestampPeriod`).
     *
     * @details
     * - THE TICK IS THE DEVICE'S; THE QUERY POOL IS THE DRIVER'S (see `Device::FrameDriver`). A frame's GPU time is one
     *   pair of queries written around its command buffer, and which pool holds them is a per-driver fact - while what
     *   a tick is worth is a property of this GPU that never changes.
     * - A device that does not report `timestampComputeAndGraphics` leaves this 0, its drivers keep no pool, and the
     *   report answers timing DISENGAGED rather than a fabricated zero (see FrameTiming).
     */
    float timestamp_period{ 0.0F };

    /// M2's content path, DEVICE HALF: compiled programs, uploaded streams, pipelines, the shared layouts and the
    /// image cache. The frame's own half (the uniform ring and the descriptor pool) is each driver's `FrameScratch`.
    ContentCaches caches{};

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
    vn::graphics::ReportOnce prepare_target_report{};

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
        /// A window this device already hands a target out for was asked for again.
        vn::graphics::ReportOnce occupied;
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
         * @details
         * - `occupied` ends here and not in rearmCreation(): its episode is "the host still holds a target for a window", which is true until the session ends, so a window created afterwards does not end it.
         *
         * @note
         * - The two FRAME episodes are re-armed at their own sites (a pass that found a presenter, an image that
         *   arrived), not here: those are the moments each condition is observed to have ended.
         */
        void rearmAll() noexcept
        {
            rearmCreation();
            occupied.rearm();
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
     * @brief The device's completion clock and its park queue: WHEN an object a host released may really be destroyed.
     *
     * @details
     * - ONE CLOCK, ONE PARK QUEUE, like the reference backend: whether a buffer or an image may be destroyed is a
     *   question about the WHOLE device, because every frame reads the same objects - so the answer cannot live in a
     *   session.
     * - THIS DEVICE REGISTERS ONE CLOCK WINDOW, because a frame here is submitted and waited for inside the call
     *   (`slots` = 1). Step 2 of `ADR-2026-11-15-device-is-a-pool-sessions-own-frames.md` registers one window per
     *   session; the queue and the arithmetic do not change - only the number of witnesses whose minimum is the
     *   watermark.
     * - `releaseAll()` at shutdown is what makes the parked releases run at all: parking exists to wait for evidence,
     *   and a device being torn down has produced it by idling.
     */
    vn::graphics::DeviceClock       clock{};
    vn::graphics::RetirementQueue   retirement{ 0 };
    ///
    /// THE DEVICE'S OFF-SCREEN TARGETS, one object (see TargetTable): the map from a host target to the images it
    /// became, the per-frame leases two drivers contend for, and the release that parks what it removed. Device-level
    /// because a target outlives every session and is exactly what two of them can contend for.
    TargetTable target_table{};
    /// Frames in flight on this device today: one, because `executeFrame` submits and waits inside the call.
    static constexpr std::uint32_t               kFramesInFlight = 1U;

    /**
     * @brief Runs @p destroy now, or parks it until the device's watermark has passed every frame that could name it.
     *
     * @details
     * - THE ONE PLACE THE DECISION IS MADE. Every released object - a geometry's streams, a program's modules and
     *   pipelines, a texture's image, a target's attachments - comes through here, so "when may this be destroyed"
     *   has one answer instead of one per cache.
     * - WITHOUT EVIDENCE IT IDLES: a release with no armed queue (a device that never came up, or one being torn
     *   down) has no parking window it could trust, so the device is idled and the object destroyed - and the idle is
     *   COUNTED (see RetirementQueue::deviceWaits). A queue that is armed never takes this path.
     *
     * @param destroy What to run when the object may go (capture handles and slices by value).
     */
    void releaseLater(std::function<void()> destroy)
    {
        {
            const std::lock_guard<std::mutex> held(clock_mutex);
            // A LIVE WINDOW IS THE EVIDENCE'S PREMISE: a queue whose timeline nobody reports cannot date a park. It
            // is asked HERE rather than kept in a flag, because any number of drivers may come and go (S6c-2).
            if (clock.windows() != 0 && retirement.retire(clock.timeline(), destroy)) {
                return;
            }
        }
        // NO EVIDENCE, SO NO PARKING: the honest answer to "we cannot prove it" is to stop the device (and count it),
        // never to guess a window that would destroy an object a submitted command buffer still names. The idle is a
        // host call on every queue, so it takes the queue's external-synchronization lock (see queue_submit): a frame
        // on another thread may be inside its submit right now (a release runs beside frames, R6).
        if (device != VK_NULL_HANDLE) {
            const std::lock_guard<std::mutex> submit_held(queue_submit);
            (void)device_table.vkDeviceWaitIdle(device);
        }
        {
            const std::lock_guard<std::mutex> held(clock_mutex);
            retirement.noteDeviceWait();
        }
        destroy();
    }

    /// True once a queue/acquire/present call answered `VK_ERROR_DEVICE_LOST`: the session is over (see
    /// Device::noteDeviceLost), so `initialized()` stops answering true and the host rebuilds it. ATOMIC because a
    /// host reads it while another session's frame is the thread that writes it.
    std::atomic<bool> device_lost{ false };
    /// One report for the loss itself, whatever number of calls saw it afterwards (the session is the episode).
    vn::graphics::ReportOnce device_lost_report{};
    /// Guards the loss episode: two frames may both see the loss on two threads (S6c-2); the atomic bool says WHETHER,
    /// this says WHO reports it.
    mutable std::mutex loss_mutex{};

    /// How many nodes a release announcement let go, cumulative (a frame's own number is 0 - see FrameUploads).
    /// ATOMIC because two host threads may release at the same time now that releases do not take the body gate.
    std::atomic<std::uint64_t> released_nodes{ 0 };

    /** @brief Reads the content path's cumulative numbers (see ContentTotals). */
    [[nodiscard]] ContentTotals contentNumbers() const noexcept
    {
        const ContentCaches::Numbers numbers = caches.numbers();
        ContentTotals                 totals;
        totals.stream_builds    = numbers.content.stream_builds;
        totals.stream_bytes     = numbers.content.stream_bytes;
        totals.program_builds   = numbers.content.program_builds;
        totals.pipeline_builds  = numbers.content.pipeline_builds;
        totals.texture_builds   = numbers.images.builds;
        totals.texture_bytes    = numbers.images.bytes;
        // THE LOCK'S OWN NUMBERS, read WITHOUT it (both are atomic): a frame's `locked` delta must not depend on the
        // sample taking the lock it measures.
        totals.cache_lock_acquisitions = caches.lockAcquisitions();
        totals.cache_lock_waits        = caches.lockWaits();
        return totals;
    }

    /**
     * @brief Resolves the SHADOW MAP a pass samples out of what it declared, and makes the map readable.
     *
     * @details
     * - WHICH INPUT: the first one whose TARGET says it is a shadow map (`RenderTarget::shadowOf`), whose depth this
     *   device actually built and whose host left promotable, and whose producer published how to read it
     *   (`hasProducerViewProjection`) - that is the SDK's rule, restated here because this backend consumes
     *   the frame value directly. A G-buffer's depth is not a shadow: a
     *   target nobody stated a light for is skipped, which is the defect the rule exists to prevent.
     * - WHAT IT DOES BESIDES ANSWER: moves the map's depth image to `SHADER_READ_ONLY_OPTIMAL` before the consuming
     *   pass begins. A barrier is not legal inside a rendering instance, and the map was left in the attachment
     *   layout by the pass that wrote it - so this is the one moment the transition can be recorded.
     *
     * @param commands The command buffer being recorded (OUTSIDE any rendering instance).
     * @param inputs   The pass' resolved inputs, in declaration order.
     * @param lent     The depths THIS FRAME lends to another target (see RenderTarget::shareDepth); such an image
     *                 cannot be a sampled picture and a pass' depth attachment at once, so it is skipped.
     * @param out      Receives the binding; empty (`view == VK_NULL_HANDLE`) when the pass samples no readable map.
     * @param moved    Receives the record this call rewrote, so a frame that never reaches the queue can put it back
     *                 (see `rewindUnrunFrame`).
     * @return true when a map was resolved (and transitioned).
     */
    [[nodiscard]] bool resolveShadow(VkCommandBuffer                            commands,
                                     std::span<const ImageSource>                inputs,
                                     const std::unordered_set<const RenderTarget*>& lent,
                                     ContentDraw::ShadowSource&                  out,
                                     std::vector<MovedLayout>&                    moved)
    {
        for (const ImageSource& input : inputs) {
            if (input.target == nullptr || !input.isProduced()) {
                continue;
            }
            const RenderTarget& map = *input.target;
            const std::shared_ptr<TargetObjects> map_entry = target_table.lookup(&map);
            if (map_entry == nullptr || !map_entry->built() || !map_entry->hasDepth()) {
                continue;
            }
            TargetObjects* map_images = map_entry.get();
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

            // THE MAP IS ABOUT TO BE READ: one transition to the sampled layout, recorded together with the record it
            // moves (see TargetObjects::transitionDepthPicture - the record is the resolve image's or the attachment's,
            // which is exactly why the verb lives on the record). IT IS THE PICTURE that moves - on a multisampled map
            // that is the resolved depth image, which is also the only one a shader may sample.
            TargetObjects& sampled = *map_images;
            // A MAP THAT BORROWS ITS DEPTH OWNS NO PICTURE, so there is nothing for a shader to read: sampling it would
            // bind a null view with the switch on. The borrower's depth belongs to its lender, which is what the pass
            // that SAMPLES would have to name instead.
            if (sampled.depthPictureView() == VK_NULL_HANDLE) {
                continue;
            }
            const ImageLayout previous = sampled.depthPictureLayout();
            if (previous != ImageLayout::ShaderReadOnly) {
                moved.push_back(MovedLayout{ &sampled, 0U, true, previous });
                (void)sampled.transitionDepthPicture(device_table, commands, ImageLayout::ShaderReadOnly);
            }

            out.view                  = sampled.depthPictureView();
            out.has_view_projection   = true;
            out.view_projection       = map.producerViewProjection();
            out.light                 = light;
            return true;
        }
        return false;
    }

    /**
     * @brief Notes every residency kind into @p ledger (see frameReport).
     *
     * @details
     * - EVERY KIND, ALWAYS: a row's existence is how a host learns this backend accounts for that kind, so a kind
     *   this device retains nothing of is reported as `live = 0` rather than left out.
     * - `Materials` is that kind here, and it is MEASURED rather than missing: material values are written into each
     *   frame's own scratch (see FrameScratch), so nothing survives a frame - `live` and `bytes` are both a real 0.
     */
    void noteResidency(vn::graphics::ResidencyLedger& ledger) const
    {
        const ContentCaches::Numbers numbers = caches.numbers();
        const ContentCaches::Progress& content_numbers = numbers.content;
        const ContentImages::Progress& image_numbers   = numbers.images;
        ledger.note(vn::graphics::ResidencyRow{ .kind    = vn::graphics::ResidencyKind::Geometry,
                                                .live    = content_numbers.streams_live,
                                                .bytes   = content_numbers.stream_bytes_live,
                                                .built   = content_numbers.stream_builds,
                                                .retired = content_numbers.stream_releases });
        ledger.note(vn::graphics::ResidencyRow{ .kind    = vn::graphics::ResidencyKind::Textures,
                                                .live    = image_numbers.live,
                                                .bytes   = image_numbers.live_bytes,
                                                .built   = image_numbers.builds,
                                                .retired = image_numbers.releases });
        ledger.note(vn::graphics::ResidencyRow{ .kind = vn::graphics::ResidencyKind::Materials, .bytes = 0U });
        ledger.note(vn::graphics::ResidencyRow{ .kind    = vn::graphics::ResidencyKind::Programs,
                                                .live    = content_numbers.pipelines_live,
                                                .built   = content_numbers.pipeline_builds,
                                                .retired = content_numbers.pipeline_releases });
    }
};

Device::FrameDriver::FrameDriver(Impl& owner_device) noexcept
  : owner(owner_device)
  , recorder(owner_device.caches, scratch)
{
}

void Device::FrameDriver::setDefaultContentProgram(vn::intrusive_ptr<const vn::graphics::ShaderProgram> program)
{
    // THE VALUE, NOT THE OBJECT: what a frame records is the text that was current when the host announced it, so a
    // later edit of the program cannot change a frame that is already described.
    const std::lock_guard<std::mutex> program_held(program_mutex);
    default_program = program != nullptr ? program->description() : vn::graphics::ShaderProgramDescriptionPtr{};
}

Device::FrameDriver::CreateRefusal Device::FrameDriver::create() noexcept
{
    if (owner.device == VK_NULL_HANDLE) {
        return CreateRefusal::NoDevice;
    }
    if (command_pool == VK_NULL_HANDLE) {
        VkCommandPoolCreateInfo info{};
        info.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
        // A frame re-records ONE buffer, so the pool may reset it; the pool itself lives as long as this driver.
        info.flags            = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
        info.queueFamilyIndex = owner.queue_family;
        if (owner.device_table.vkCreateCommandPool(owner.device, &info, nullptr, &command_pool) != VK_SUCCESS) {
            command_pool = VK_NULL_HANDLE;
            return CreateRefusal::CommandPool;
        }
    }
    if (completion == VK_NULL_HANDLE) {
        VkFenceCreateInfo info{};
        info.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
        if (owner.device_table.vkCreateFence(owner.device, &info, nullptr, &completion) != VK_SUCCESS) {
            completion = VK_NULL_HANDLE;
            return CreateRefusal::Fence;
        }
    }
    // THE CLOCK WINDOW: a frame reports its completion through this id (see DeviceClock), and the park queue dates a
    // release against the watermark it feeds. The clock starts a joining window AT the current watermark, which is
    // right for a driver created with the device: it proves nothing about frames it never drove.
    if (!clock_window_live) {
        const std::lock_guard<std::mutex> clock_held(owner.clock_mutex);
        clock_window      = owner.clock.addWindow();
        owner.clock.setWindowSlots(clock_window, Impl::kFramesInFlight);
        clock_window_live = true;
    }
    return CreateRefusal::Ok;
}

bool Device::FrameDriver::createScratch() noexcept
{
    return scratch.create(owner.device_table,
                          owner.device,
                          owner.allocator,
                          owner.caches.uniformAlignment(),
                          owner.caches.contentSetLayout(),
                          owner.caches.drawSetLayout(),
                          owner.caches.screenSetLayout());
}

void Device::FrameDriver::destroy() noexcept
{
    // THE SCRATCH GOES FIRST of this driver's objects: its descriptor pool holds the cached sets, and the device's
    // layouts (which the sets were allocated against) outlive it - `Device::shutdown` destroys the scratch before the
    // caches, and this call is idempotent for the paths that reach a destroy without a shutdown.
    scratch.destroy();
    // THE WINDOW LEAVES FIRST: a device clock that still lists this driver keeps the device watermark where it is, and
    // a queue that waits for evidence from a driver that no longer exists would never release anything.
    if (clock_window_live) {
        const std::lock_guard<std::mutex> clock_held(owner.clock_mutex);
        owner.clock.removeWindow(clock_window);
        clock_window_live = false;
    }
    if (owner.device != VK_NULL_HANDLE) {
        if (completion != VK_NULL_HANDLE) {
            owner.device_table.vkDestroyFence(owner.device, completion, nullptr);
            completion = VK_NULL_HANDLE;
        }
        if (command_pool != VK_NULL_HANDLE) {
            owner.device_table.vkDestroyCommandPool(owner.device, command_pool, nullptr);
            command_pool = VK_NULL_HANDLE;
        }
        if (timestamp_pool != VK_NULL_HANDLE) {
            owner.device_table.vkDestroyQueryPool(owner.device, timestamp_pool, nullptr);
            timestamp_pool = VK_NULL_HANDLE;
        }
    }
    timestamps        = false;
    timestamp_queries = 0U;
    clock_frames      = 0U;
    books             = FrameBooks{};
    default_program   = nullptr;
}

bool Device::FrameDriver::ensureTimestampQueries(const std::uint32_t needed) noexcept
{
    if (timestamp_pool != VK_NULL_HANDLE && timestamp_queries >= needed) {
        return true;
    }
    if (timestamp_pool != VK_NULL_HANDLE) {
        owner.device_table.vkDestroyQueryPool(owner.device, timestamp_pool, nullptr);
        timestamp_pool    = VK_NULL_HANDLE;
        timestamp_queries = 0U;
    }
    VkQueryPoolCreateInfo info{};
    info.sType      = VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO;
    info.queryType  = VK_QUERY_TYPE_TIMESTAMP;
    info.queryCount = needed;
    if (owner.device_table.vkCreateQueryPool(owner.device, &info, nullptr, &timestamp_pool) != VK_SUCCESS) {
        timestamp_pool    = VK_NULL_HANDLE;
        timestamp_queries = 0U;
        return false;
    }
    timestamp_queries = needed;
    return true;
}

void Device::FrameDriver::closeFrameBooks(const ContentTotals& before, const vn::graphics::FrameCounters& counters) noexcept
{
    const ContentTotals after = owner.contentNumbers();
    books.counters            = counters;
    // `stream_releases` / `texture_releases` stay 0 for a frame: this device only lets go when a host says so, which
    // happens BETWEEN frames - and an explicit release is an announcement, not a frame's own number (see
    // FrameUploads). A texture a new revision replaced is a BUILD (it never stopped being resident).
    vn::graphics::FrameUploads frame_uploads;
    frame_uploads.stream_uploads  = after.stream_builds - before.stream_builds;
    frame_uploads.stream_bytes    = after.stream_bytes - before.stream_bytes;
    frame_uploads.texture_uploads = after.texture_builds - before.texture_builds;
    frame_uploads.texture_bytes   = after.texture_bytes - before.texture_bytes;
    books.uploads = frame_uploads;
    // WHAT THE FRAME SPENT ON THE CACHE LOCK, as the same kind of delta (see FrameLocking): the head's one hold plus
    // anything a host verb slipped in while this frame ran - a steady frame is a CONSTANT here, whatever it draws.
    vn::graphics::FrameLocking locking;
    locking.acquisitions = after.cache_lock_acquisitions - before.cache_lock_acquisitions;
    locking.waits        = after.cache_lock_waits - before.cache_lock_waits;
    books.locking        = locking;
}

PresentOutcome Device::FrameDriver::executeFrame(const FrameDescription& frame, PresentTarget* present_to)
{
    // ONE FRAME AT A TIME PER DRIVER, AS A CHECKED FACT (see FrameGate): another thread waits here, and a frame started
    // from the thread that is ALREADY driving one is refused - a re-entrant frame would corrupt this driver's command
    // buffer and fence silently. The gate is the DRIVER's, so a second driver (step 2's `createFrameDriver()`) is not
    // serialised against this one for free. The lease releases the gate on EVERY return below, which is why it is a
    // lease and not a pair of calls.
    const vn::graphics::FrameGate::Exclusive frame_lease = frames.enterExclusive();
    if (!frame_lease) {
        if (frame_reentry_report.shouldReport()) {
            owner.report(deviceDiagnostic(DiagnosticSeverity::Error,
                                              DiagnosticCategory::PassProtocolViolation,
                                              u8"vk.session.frame_reentry",
                                              u8"vk: a frame was started from the thread that is already driving one (a hook "
                                              u8"or a sink drove another engine); this frame was refused - a device drives "
                                              u8"one frame at a time, so drive the second engine from another thread or after "
                                              u8"waitForFrame"));
        }
        return PresentOutcome::Refused;
    }

    // FRAME BODIES OVERLAP (see Impl::body_gate): an OFF-SCREEN frame takes the gate SHARED, so another driver's frame
    // may record and submit at the same time - everything a frame touches is either its own (scratch, command pool,
    // fence) or under its own lock (the caches, the allocator, the target map). A frame that PRESENTS is the exception:
    // the swapchain, its semaphores and the window's report episodes are one device resource, so a window frame takes
    // the gate EXCLUSIVELY and is serialised with every other frame.
    //
    // AND A BODY ENTERED FROM INSIDE ONE IS REFUSED, NOT QUEUED (see FrameGate): a hook driving another engine of this
    // device runs on this thread, which already holds the gate - waiting there would be the thread waiting for itself.
    std::optional<vn::graphics::FrameGate::Shared>    offscreen_body;
    std::optional<vn::graphics::FrameGate::Exclusive> window_body;
    if (present_to != nullptr) {
        window_body.emplace(owner.body_gate.enterExclusive());
    }
    else {
        offscreen_body.emplace(owner.body_gate.enterShared());
    }
    const bool body_entered = present_to != nullptr ? static_cast<bool>(*window_body) : static_cast<bool>(*offscreen_body);
    if (!body_entered) {
        if (frame_reentry_report.shouldReport()) {
            owner.report(deviceDiagnostic(DiagnosticSeverity::Error,
                                          DiagnosticCategory::PassProtocolViolation,
                                          u8"vk.session.frame_reentry",
                                          u8"vk: a frame was started from a thread that is already inside a frame of this "
                                          u8"device (a hook or a sink drove another engine); this frame was refused - a "
                                          u8"thread drives one frame at a time"));
        }
        return PresentOutcome::Refused;
    }

    if (!(owner.device != VK_NULL_HANDLE && !owner.device_lost)) {
        if (owner.device_lost) {
            // The one loss report already said why and what to do (see noteDeviceLost): a frame asked after it is
            // skipped without touching the device, not re-explained per frame. A frame that expected a window still
            // says its picture did not arrive - Lost is the outcome a host acts on.
            return present_to != nullptr ? PresentOutcome::Lost : PresentOutcome::Skipped;
        }
        owner.report(deviceDiagnostic(DiagnosticSeverity::Error,
                                          DiagnosticCategory::SubmissionFailed,
                                          u8"vk.session.not_initialized",
                                          u8"vk: a frame was handed to a device that is not up"));
        return PresentOutcome::Skipped;
    }

    // THE FRAME'S BOOKS OPEN HERE AND CLOSE ON EVERY EXIT (see closeFrameBooks): the report always describes the
    // frame that just ended - an empty one included - and the upload numbers are this frame's own delta.
    vn::graphics::FrameToken frame_token{};
    {
        const std::lock_guard<std::mutex> clock_held(owner.clock_mutex);
        frame_token = owner.clock.begin();
    }
    FrameTokenCloser                        frame_close{ owner.clock, owner.retirement, owner.clock_mutex, frame_token };
    const ContentTotals    before = owner.contentNumbers();
    vn::graphics::FrameCounters   counters{};
    struct BooksCloser {
        FrameDriver*                 session;
        const ContentTotals&         before;
        vn::graphics::FrameCounters& counters;
        ~BooksCloser()
        {
            session->closeFrameBooks(before, counters);
        }
    } books_closer{ this, before, counters };
    // THIS FRAME'S ADMISSIONS (F4): the geometries it resolves are asked of the DEVICE's residence table in this
    // driver's name, a build it wins is PUBLISHED below once the fence says the fill is provably complete, and a
    // competitor's temporary copy dies with this object (see ContentCaches::FrameBuilds). Declared here, OUTSIDE the
    // head's hold, because its destructor takes the cache lock to dispose a copy.
    ContentCaches::FrameBuilds frame_builds(owner.caches, clock_window);

    // WHICH PASSES THIS BACKEND CAN RUN: off-screen passes, and WINDOW passes when the frame names one of this
    // device's own targets as its presenter. A frame that names no presenter presents nowhere (a first-class state:
    // an offline bake), and a pass that names no target while a presenter IS named draws into that window.
    WindowTarget* window = nullptr;
    if (present_to != nullptr) {
        auto* candidate = dynamic_cast<WindowTarget*>(present_to);
        const std::lock_guard<std::mutex> surfaces_held(owner.surfaces_mutex);
        for (const auto& surface : owner.surfaces) {
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
        /// The target's attachments (null for a window pass), resolved where the target was claimed - the frame head
        /// needs them to resolve pipelines, and the recording loop must not look the target up again.
        TargetObjects*                 images{ nullptr };
        /// The attachment set's pipeline compatibility: the formats AND the colour count a pipeline and its rendering
        /// instance are both stated from. Nothing in this loop names a render pass any more (see DynamicRendering).
        vn::graphics::RenderPassCompatibility compatibility{};
    };

    // THIS FRAME'S TARGET CLAIMS: one lease, and its destructor is the ONE release point every early return
    // below passes through (`P3`).
    TargetTable::Lease target_lease = owner.target_table.lease(clock_window);
    bool              claim_refused = false;

    std::vector<Runnable> runnable_passes;
    runnable_passes.reserve(frame.passes.size());
    bool window_acquired = false;
    // Whether a pass was dropped because a description this device cannot build was named (a sample count, say). The
    // refusal has already said WHY, so the generic "nothing I can run" sentence below would be the same fact twice.
    bool refused_pass = false;
    // A PASS THAT SAMPLES A TARGET READS ITS IMAGES, and a target's "sampled vs attachment" layout is per-frame state:
    // the sampler takes the same lease the drawer does (R5's other half). A conflict skips THIS pass and says so once
    // per (target, episode), exactly like a drawing conflict.
    const auto claim_sampled = [&](const RenderTarget* sampled) -> bool {
        if (sampled == nullptr || target_lease.claim(sampled)) {
            return true;
        }
        if (claim_conflicts.insert(sampled).second) {
            owner.report(deviceDiagnostic(DiagnosticSeverity::Warning,
                                          DiagnosticCategory::UnsupportedRequest,
                                          u8"vk.target.claim_conflict",
                                          u8"vk: a pass samples a target another frame driver is drawing into right now; "
                                          u8"the pass was skipped for this frame - drive the other engine on another "
                                          u8"target, or wait for its frame to finish"));
        }
        claim_refused = true;
        refused_pass  = true;
        return false;
    };
    const auto claim_pass_samples = [&](const PassDraw& draw, const FrameDescription::Pass& entry) -> bool {
        bool every = true;
        for (const DrawPacket& description : draw.packets) {
            if (description.kind == vn::graphics::DrawKind::Fullscreen && !claim_sampled(description.source)) {
                every = false;
            }
        }
        for (const ImageSource& input : entry.inputs) {
            if (!claim_sampled(input.target)) {
                every = false;
            }
        }
        return every;
    };

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
        std::uint64_t packets = 0U;
        std::uint64_t draws   = 0U;
        for (const DrawPacket& description : draw.packets) {
            packets += vn::graphics::packetDraws(description) ? 1U : 0U;
            draws += vn::graphics::drawItemsOf(description);
        }
        // COUNTED ONLY FOR A PASS THIS DEVICE CAN RUN: a draw packet is recorded, and a pass whose target could not
        // be built recorded nothing (see the sample-count refusals in TargetTable::ensure) - reporting its packets
        // would say a frame drew when nothing reached the queue.
        const bool has_effect = vn::graphics::passHasEffect(draw);
        const auto count_pass = [&counters, packets, draws, has_effect]() {
            if (has_effect) {
                ++counters.passes;
                counters.draw_packets += packets;
                counters.draws += draws;
            }
        };
        if (draw.target == nullptr) {
            if (window == nullptr || !window->ready()) {
                if (owner.window_reports.presenter.shouldReport()) {
                    owner.report(deviceDiagnostic(DiagnosticSeverity::Warning,
                                                      DiagnosticCategory::UnsupportedRequest,
                                                      u8"vk.window.presenter_missing",
                                                      u8"vk: a window pass names no presenter this device handed out; it was left out"));
                }
                refused_pass = true;
                continue;
            }
            owner.window_reports.presenter.rearm(); // the pass named a presenter this device handed out
            if (!window_acquired) {
                vn::String why;
                if (!window->acquire(&why)) {
                    if (window->deviceLost()) {
                        // The frame is over: nothing can run on a lost device, so the walk stops here instead of
                        // letting every later pass build against it. The sentence is reported once (see
                        // noteDeviceLost), and Lost is the outcome the host acts on.
                        (void)owner.note_device_lost(VK_ERROR_DEVICE_LOST, "vkAcquireNextImageKHR");
                        return PresentOutcome::Lost;
                    }
                    if (owner.window_reports.acquire.shouldReport()) {
                        owner.report(deviceDiagnostic(DiagnosticSeverity::Warning,
                                                          DiagnosticCategory::UnsupportedRequest,
                                                          u8"vk.window.acquire_failed",
                                                          why.empty() ? vn::String(u8"vk: the window's next image could not be acquired") : std::move(why)));
                    }
                    refused_pass = true;
                    continue;
                }
                owner.window_reports.acquire.rearm(); // the image arrived: this episode is over
                window_acquired = true;
            }
            if (!claim_pass_samples(draw, entry)) {
                continue;
            }
            count_pass();
            runnable_passes.push_back(Runnable{ &entry, &draw, true, nullptr, window->compatibility() });
            continue;
        }
        // ONE DRIVER AT A TIME, ONE FRAME AT A TIME (see TargetTable): the claim is taken BEFORE the
        // attachments are built, because rebuilding a target is the claim holder's verb (`R5`). A lost claim
        // skips THIS pass - the rest of the frame still runs (`G7`) - and says so once per (target, episode).
        if (!target_lease.claim(draw.target)) {
            if (claim_conflicts.insert(draw.target).second) {
                owner.report(deviceDiagnostic(DiagnosticSeverity::Warning,
                                              DiagnosticCategory::UnsupportedRequest,
                                              u8"vk.target.claim_conflict",
                                              u8"vk: a pass named a target another frame driver is drawing into right "
                                              u8"now; the pass was skipped for this frame - drive the other engine on "
                                              u8"another target, or wait for its frame to finish"));
            }
            claim_refused = true;
            refused_pass  = true; // the "nothing I can run" sentence would be the same fact twice
            continue;
        }
        if (!claim_pass_samples(draw, entry)) {
            continue;
        }
        // THE DEPTH THIS PASS ATTACHES MAY BELONG TO ANOTHER TARGET (RenderTarget::shareDepth). Claiming only the
        // borrower would let two drivers write ONE image - the lender's depth - with two submissions at once, and would
        // let both write that image's ONE layout record from two threads. The borrow edge is therefore a claim on the
        // LENDER, and a contended lender skips this pass exactly like a contended target (`G10`).
        if (const vn::raw_ptr<RenderTarget> lender = draw.target->depthSource(); lender != nullptr && !target_lease.claim(lender)) {
            if (claim_conflicts.insert(lender).second) {
                owner.report(deviceDiagnostic(DiagnosticSeverity::Warning,
                                              DiagnosticCategory::UnsupportedRequest,
                                              u8"vk.target.claim_conflict",
                                              u8"vk: a pass draws against a depth another frame driver is drawing into "
                                              u8"right now; the pass was skipped for this frame - drive the other engine "
                                              u8"on another target, or wait for its frame to finish"));
            }
            claim_refused = true;
            refused_pass  = true;
            continue;
        }
        if (!owner.target_table.ensure(*draw.target)) {
            refused_pass = true;
            continue;
        }
        count_pass();
        // THE ENTRY EXISTS NOW (the build above inserted it): the frame keeps a borrowed pointer into it, and a
        // release during its recording hands the record to the park instead of destroying it (S6c-2b).
        TargetObjects& claimed_images = *owner.target_table.lookup(draw.target);
        runnable_passes.push_back(Runnable{ &entry,
                                            &draw,
                                            false,
                                            &claimed_images,
                                            claimed_images.compatibility() });
    }

    if (runnable_passes.empty()) {
        if (!refused_pass && no_targets_report.shouldReport()) {
            owner.report(deviceDiagnostic(DiagnosticSeverity::Info,
                                          DiagnosticCategory::UnsupportedRequest,
                                          contentRefusalCode(ContentRefusalCause::NoRunnablePass),
                                          u8"vk: the frame had no pass this backend can run (off-screen targets and host windows are "
                                          u8"served); nothing was submitted"));
        }
        return PresentOutcome::Skipped;
    }
    no_targets_report.rearm(); // a pass ran: the "nothing I can run" episode is over
    if (!claim_refused) {
        claim_conflicts.clear(); // no pass lost its target this frame: the conflict episode is over
    }

    // THE ACQUIRED IMAGE IS A PAIR OR IT IS A LEAK: `vkAcquireNextImageKHR` took an image and signalled its semaphore,
    // and only a present hands it back. Everything between here and the present can fail, so the give-back is one named
    // act (`WindowTarget::abandonFrame`) that every failure path below calls instead of returning past an acquire.
    const auto giveBackWindowImage = [&]() {
        if (window_acquired && window != nullptr) {
            window->abandonFrame();
        }
    };

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
    allocate.commandPool        = command_pool;
    allocate.level              = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    allocate.commandBufferCount = 1U;

    VkCommandBuffer commands{ VK_NULL_HANDLE };
    if (owner.device_table.vkAllocateCommandBuffers(owner.device, &allocate, &commands) != VK_SUCCESS) {
        owner.report(deviceDiagnostic(DiagnosticSeverity::Error,
                                          DiagnosticCategory::SubmissionFailed,
                                          u8"vk.frame.command_buffer_missing",
                                          u8"vk: no command buffer"));
        giveBackWindowImage(); // an image is already acquired and nothing was recorded: it goes back unpresented
        return PresentOutcome::Skipped;
    }

    VkCommandBufferBeginInfo begin{};
    begin.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    if (owner.device_table.vkBeginCommandBuffer(commands, &begin) != VK_SUCCESS) {
        owner.report(deviceDiagnostic(DiagnosticSeverity::Error,
                                          DiagnosticCategory::SubmissionFailed,
                                          u8"vk.frame.begin_failed",
                                          u8"vk: vkBeginCommandBuffer failed"));
        owner.device_table.vkFreeCommandBuffers(owner.device, command_pool, 1U, &commands);
        giveBackWindowImage();
        return PresentOutcome::Skipped;
    }

    // THE DEVICE CLOCK OPENS HERE (see FrameTiming): the reset is legal outside a rendering instance, which is where this
    // is, and the pair is closed before the buffer ends below. SAMPLED, not every frame (see kTimestampSampleFrames).
    ++clock_frames;
    const bool sample_clock = timestamps && (clock_frames % kTimestampSampleFrames) == 1U;
    // THE PER-PASS BOUNDARIES NEED ONE QUERY PER PASS PLUS THE TAIL, beside the frame's own pair (see PassTiming). A
    // frame that needs more than the pool holds recreates it here, which is legal because no command buffer is in
    // flight; a device that cannot grow it keeps the frame's own clock and answers no attribution.
    const std::uint32_t pass_count     = static_cast<std::uint32_t>(runnable_passes.size());
    const std::uint32_t query_count    = 2U + pass_count + 1U;
    const bool          per_pass_clock = sample_clock && ensureTimestampQueries(query_count);
    if (per_pass_clock) {
        owner.device_table.vkCmdResetQueryPool(commands, timestamp_pool, 0U, query_count);
        owner.device_table.vkCmdWriteTimestamp(commands, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, timestamp_pool, 0U);
    }

    // ONE DESCRIPTOR POOL RESET PER FRAME: the pool's sets are the per-draw uniform bindings, and the previous frame
    // is complete by now (the device submits synchronously), so nothing still names them.
    scratch.beginFrame();
    // The pass count was taken from the frame's shape above (a pass whose draws were all skipped is still a pass
    // that ran); what the loop below adds is the draw packets this backend actually recorded.

    // ─── THE FRAME HEAD: EVERY CACHE CONSULTATION HAPPENS HERE ───────────────────────────────────────────────────
    // One hold of the cache's lock answers what this frame draws with - programs, streams, pipelines and every texture
    // upload - and the recording below uses only those values (see ContentCaches::Lock). A release another thread
    // announces while this frame records therefore cannot change what the frame already resolved, and the recording
    // path takes no lock at all (S6c; T3).
    {
        const ContentCaches::Lock cache_lock = owner.caches.lock();
        // THE DEFAULT PROGRAM IS A VALUE FOR THIS FRAME (see program_mutex): a host may announce another one while
        // this frame records, and a head that read the field twice could compile two variants of one answer.
        vn::graphics::ShaderProgramDescriptionPtr head_program;
        {
            const std::lock_guard<std::mutex> program_held(program_mutex);
            head_program = default_program;
        }
        frame_head.beginFrame(runnable_passes.size());
        for (const Runnable& item : runnable_passes) {
            const PassDraw&   pass_draw = *item.draw;
            FrameHead::Pass&  pass_head = frame_head.beginPass(pass_draw.packets.size());
            for (std::size_t draw_index = 0; draw_index < pass_draw.packets.size(); ++draw_index) {
                const DrawPacket& description = pass_draw.packets[draw_index];
                FrameHead::Call&        call        = pass_head.draws[draw_index];
                if (description.kind == vn::graphics::DrawKind::Fullscreen) {
                    call.plans.resize(1U);
                    FrameHead::DrawPlan& plan = call.plans.front();
                    (void)owner.caches.planDraw(description,
                                                pass_draw,
                                                item.compatibility,
                                                plan,
                                                call.refusal,
                                                cache_lock);
                    // The destination's stand-in and shadow sampler: a full-screen draw samples the target it names, so its
                    // slots are the source's views (assembled where the source is, below), not a material's.
                    owner.caches.resolveTextures(nullptr, commands, frame_head, plan, call.refusal, cache_lock, uploadBatch());
                    continue;
                }
                if (description.commands == nullptr) {
                    continue;
                }
                call.plans.resize(description.commands->size());
                for (std::size_t command_index = 0; command_index < description.commands->size(); ++command_index) {
                    FrameHead::DrawPlan& plan = call.plans[command_index];
                    if (!owner.caches.planDraw((*description.commands)[command_index],
                                               pass_draw,
                                               head_program.get(),
                                               item.compatibility,
                                               plan,
                                               call.refusal,
                                               cache_lock,
                                               frame_builds)) {
                        continue; // the plan stays empty and `record` skips exactly this command
                    }
                    owner.caches.resolveTextures(plan.material.get(),
                                                 commands,
                                                 frame_head,
                                                 plan,
                                                 call.refusal,
                                                 cache_lock,
                                                 uploadBatch());
                }
            }
        }
    }

    bool first_window_pass = true;
    // Whether a draw this frame named could not be recorded. The refused-content episode ends when a frame records
    // everything it was handed - a frame that still names one bad draw keeps the episode open (see ReportOnce).
    bool content_refused = false;
    std::size_t pass_position = 0;
    // THE RECORDS THIS FRAME MOVED ON ANOTHER TARGET (a sampled source's picture, a shadow map's picture): what a frame
    // that never reaches the queue has to put back (see rewindUnrunFrame below).
    std::vector<MovedLayout> moved_layouts;
    moved_layouts.reserve(runnable_passes.size() * 2U);
    // A FRAME THAT NEVER REACHED THE QUEUE MOVED NOTHING (see TargetObjects' note on records following the recorded
    // COMMANDS). Two answers, because the two kinds of record have different owners:
    // - THIS FRAME'S OWN ATTACHMENT SETS go back to `Undefined`: their content is what this frame would have written, the
    //   GPU never wrote it, and `Undefined` is the honest AND the safe statement - the next pass clears instead of
    //   loading a picture that is not there (TargetObjects::resetLayouts).
    // - SOMEONE ELSE'S IMAGES go back to EXACTLY what they said: a sampled source or a shadow map may hold a picture an
    //   earlier frame wrote and nothing else can reproduce, so discarding it would be worse than the failure.
    // - AND THE WINDOW'S ACQUIRED IMAGE IS GIVEN BACK, because it was taken from the presentation engine and may not be
    //   re-acquired with the semaphore its acquire signalled (WindowTarget::abandonFrame).
    const auto rewindUnrunFrame = [&]() {
        for (const Runnable& item : runnable_passes) {
            if (!item.window && item.images != nullptr) {
                item.images->resetLayouts();
            }
        }
        for (const MovedLayout& moved : moved_layouts) {
            if (moved.images == nullptr) {
                continue;
            }
            if (moved.depth_picture) {
                moved.images->setDepthPictureLayout(moved.previous);
            }
            else if (moved.color_index < moved.images->color_layouts.size()) {
                moved.images->color_layouts[moved.color_index] = moved.previous;
            }
        }
        giveBackWindowImage();
    };
    for (const Runnable& item : runnable_passes) {
        const std::size_t pass_index = pass_position++;
        // THE PASS' OWN BOUNDARY, written BEFORE its uploads: the transitions and copies a pass asks for exist because
        // it asked, so they are billed to it rather than to the frame (see PassTiming).
        if (per_pass_clock) {
            owner.device_table.vkCmdWriteTimestamp(commands,
                                                    VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                                                    timestamp_pool,
                                                    2U + static_cast<std::uint32_t>(pass_index));
        }
        const PassDraw& draw = *item.draw;
        // THE PASS' TEXTURE UPLOADS ALREADY RAN, at the frame head, on this same command buffer (see above): an upload
        // records image barriers and copies, neither legal inside a rendering instance - which is why the head runs
        // before the first pass begins. Nothing here consults the caches.

        // A SOURCE A SCREEN DRAW SAMPLES is about to be read: transition it to SHADER_READ_ONLY now, where an image
        // barrier is unambiguously allowed (before any rendering instance begins).
        for (const DrawPacket& description : draw.packets) {
            if (description.kind != vn::graphics::DrawKind::Fullscreen || description.source == nullptr) {
                continue;
            }
            const std::shared_ptr<TargetObjects> source_entry = owner.target_table.lookup(description.source);
            if (source_entry == nullptr || !source_entry->built()) {
                continue;
            }
            // EVERY PICTURE THE DRAW SAMPLES MOVES, each from its OWN record: one image per colour attachment, and a
            // readback may have moved one of them while the others stayed. The picture is `colors` in both shapes - the
            // resolve images on a multisampled target, whose sample buffers nothing may sample.
            TargetObjects& sampled = *source_entry;
            for (std::size_t index = 0; index < sampled.colors.size(); ++index) {
                const ImageLayout previous = sampled.color_layouts[index];
                if (previous == ImageLayout::ShaderReadOnly) {
                    continue; // an earlier pass of this frame already stated it
                }
                // THE REWIND LOG (see rewindUnrunFrame): this picture belongs to ANOTHER target, so a failed frame has to
                // put the record back exactly - not to `Undefined`, which would discard a picture an earlier frame wrote.
                moved_layouts.push_back(MovedLayout{ &sampled, index, false, previous });
                (void)transitionImage(owner.device_table,
                                      commands,
                                      sampled.colors[index],
                                      sampled.color_layouts[index],
                                      ImageLayout::ShaderReadOnly,
                                      VK_IMAGE_ASPECT_COLOR_BIT);
            }
        }

        // THE SHADOW MAP THIS PASS SAMPLES, if it declared one: resolved from the target's own statement and moved to
        // SHADER_READ_ONLY here, before the pass begins (see resolveShadow).
        ContentDraw::ShadowSource shadow{};
        (void)owner.resolveShadow(commands, item.entry->inputs, lent_depths, shadow, moved_layouts);

        TargetObjects*                                  images        = item.images;
        VkExtent2D                                     extent{};
        const vn::graphics::RenderPassCompatibility    compatibility = item.compatibility;
        const std::uint32_t                            color_count   = static_cast<std::uint32_t>(compatibility.device_color_formats.size());
        const bool                                     has_clear     = draw.clear.has_value();
        bool                                           load_color    = false;
        bool                                           load_depth    = false;

        if (item.window) {
            // A WINDOW PASS: the acquired swapchain image and the window's depth. WHAT IT LOADS is the pass' OWN policy,
            // one attachment at a time (see ClearPolicy's two independent dimensions): the first pass into the frame's
            // image can clear either or both (an image nobody wrote cannot be loaded), and a later pass loads what the
            // earlier one wrote EXCEPT the attachment it asks to clear - which is how a HUD owns the depth inside its
            // rectangle and keeps the picture underneath.
            const bool written = !first_window_pass;
            load_color         = written && (!has_clear || !draw.clear->clear_color);
            load_depth         = written && (!has_clear || !draw.clear->clear_depth);
            extent             = VkExtent2D{ window->width(), window->height() };
            first_window_pass  = false;
        }
        else {
            // A FRESH attachment set is UNDEFINED: a pass that asked to preserve something gets a CLEAR for its first
            // frame (there is nothing to load yet). A pass that announces NO clear loads both attachments.
            const bool drawn_into = !images->fresh.load(std::memory_order_relaxed);
            // ONE ANSWER PER ATTACHMENT (see ClearPolicy): a pass that clears only the depth loads the colour it is drawn
            // over and resets the depth, which is what a HUD pass asks for.
            load_color  = has_clear ? (!draw.clear->clear_color && drawn_into) : drawn_into;
            load_depth  = has_clear ? (!draw.clear->clear_depth && drawn_into) : drawn_into;
            extent      = VkExtent2D{ static_cast<std::uint32_t>(images->width()), static_cast<std::uint32_t>(images->height()) };
        }

        // PER-ATTACHMENT CLEAR (a loading attachment ignores its value): the array is in colour output-location order and
        // a value the pass does not state is transparent black - see ClearPolicy. A DEPTH-ONLY pass has no colour slot
        // at all, which is why the depth value is its own field rather than the array's last entry. THE ARRAY IS A STACK
        // VALUE sized by the API's own maximum, so a pass allocates nothing here (see kMaxColorAttachments).
        std::array<VkClearValue, kMaxColorAttachments> color_clears{};
        const Colorf              colour = draw.clear.has_value() ? draw.clear->color : Colorf(0.0F, 0.0F, 0.0F, 1.0F);
        if (color_count != 0U) {
            color_clears[0].color = { { colour.r, colour.g, colour.b, colour.a } };
        }
        for (std::size_t index = 1; index < color_count; ++index) {
            const Colorf extra = images != nullptr && draw.clear.has_value() && index - 1U < draw.clear->extra_colors.size()
                                     ? draw.clear->extra_colors[index - 1U]
                                     : Colorf(0.0F, 0.0F, 0.0F, 0.0F);
            color_clears[index].color = { { extra.r, extra.g, extra.b, extra.a } };
        }
        // Reverse-Z: every backend clears depth to its own far plane, and this camera convention's far plane is 0 (see
        // ClearPolicy: the policy states WHETHER, never to what).
        VkClearValue depth_clear{};
        depth_clear.depthStencil = { 0.0F, 0U };

        PassLoad load{};
        load.load_color   = load_color ? vn::graphics::LoadOp::Load : vn::graphics::LoadOp::Clear;
        load.load_depth   = load_depth ? vn::graphics::LoadOp::Load : vn::graphics::LoadOp::Clear;
        // WHAT THE CONTENTS ARE WORTH: this backend keeps what a pass wrote; `beginRendering` downgrades a RESOLVED role
        // to `DontCare` itself, because only it knows whether the images are consumed by a resolve (see PassLoad).
        load.store_color  = vn::graphics::StoreOp::Store;
        load.store_depth  = vn::graphics::StoreOp::Store;
        load.color_clears = std::span<const VkClearValue>(color_clears.data(), color_count); // one value per colour attachment
        load.depth        = depth_clear;

        // THE SET OWNS THE TRANSITIONS AND THE INSTANCE (see TargetObjects::beginPass / WindowTarget::beginPass): the
        // layouts each attachment is in are the owner's bookkeeping, so the recorder cannot disagree with them.
        if (item.window) {
            window->beginPass(commands, load);
        }
        else {
            images->beginPass(owner.device_table, commands, load);
        }
        // WHAT THIS PASS DRAWS: the frame head's plans and resolved bindings, in the draw packet's own order. Every
        // command inside a call becomes one GPU draw (M3: the plan selected the pipeline, one `DrawItem` = one
        // indexed draw). The frame's DRAWING-CALL count was taken from the frame's shape above - a call is the
        // description, not its commands.
        const FrameHead::Pass&  pass_head = frame_head.passes()[pass_index];
        for (std::size_t draw_index = 0; draw_index < draw.packets.size(); ++draw_index) {
            const DrawPacket& description = draw.packets[draw_index];
            const FrameHead::Call&  call        = pass_head.draws[draw_index];
            // THE HEAD'S REFUSAL IS REPORTED WHERE THE DRAW IS (see planDraw): the code travels with the sentence
            // instead of being guessed at the sink, and a call whose later commands DID resolve still records them.
            if (call.refusal.isRefused()) {
                content_refused = true;
                if (content_report.shouldReport()) {
                    owner.report(deviceDiagnostic(DiagnosticSeverity::Warning,
                                                  call.refusal.category(),
                                                  call.refusal.code(),
                                                  call.refusal.message()));
                }
            }
            ContentRefusal refusal;
            if (description.kind == vn::graphics::DrawKind::Fullscreen) {
                // THE FULL-SCREEN ABI: the source's colour attachments, bound in order. A source this device never
                // built answers an empty span, which makes the recorder report (and skip) the draw.
                ContentDraw::ScreenSource source{};
                const std::shared_ptr<TargetObjects> source_entry =
                    description.source != nullptr ? owner.target_table.lookup(description.source) : nullptr;
                if (source_entry != nullptr && source_entry->built()) {
                    source.views = source_entry->color_views;
                }
                (void)recorder.recordFullscreen(commands,
                                            static_cast<int>(extent.width),
                                            static_cast<int>(extent.height),
                                            description,
                                            draw,
                                            call.plans.front(),
                                            frame_head,
                                            source,
                                            shadow,
                                            &refusal);
            }
            else {
                (void)recorder.record(commands,
                                      static_cast<int>(extent.width),
                                      static_cast<int>(extent.height),
                                      description,
                                      draw,
                                      call.plans,
                                      frame_head,
                                      shadow,
                                      &refusal);
            }
            if (refusal.isRefused()) {
                content_refused = true;
                if (content_report.shouldReport()) {
                    // THE CAUSE WAS NAMED WHERE IT WAS KNOWN (see ContentRefusal), so this report FORWARDS the value the
                    // recorder built - the code travels with the sentence instead of being guessed at the sink.
                    const vn::graphics::RenderDiagnostic diagnostic =
                        deviceDiagnostic(DiagnosticSeverity::Warning, refusal.category(), refusal.code(), refusal.message());
                    owner.report(diagnostic);
                }
            }
        }
        // THE PASS ENDS: the attachments stay in the rendering layout and the set's own record says so (see beginPass).
        if (item.window) {
            window->endPass(commands);
        }
        else {
            images->endPass(owner.device_table, commands);
        }
    }
    if (!content_refused) {
        content_report.rearm(); // every draw the frame named was recorded: the episode is over
    }

    // THE PRESENT TRANSITION, after the window's last pass and before the command buffer ends: every window pass
    // stores into COLOR_ATTACHMENT_OPTIMAL, and vkQueuePresentKHR reads PRESENT_SRC_KHR.
    if (window_acquired) {
        window->recordPresentTransition(commands);
    }

    if (per_pass_clock) {
        // THE TAIL BOUNDARY: the last pass' span ends here, so what follows (the present transition, the frame's own
        // tail) belongs to no pass - the honest split, because no pass asked for it.
        owner.device_table.vkCmdWriteTimestamp(commands, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, timestamp_pool, 2U + pass_count);
        owner.device_table.vkCmdWriteTimestamp(commands, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, timestamp_pool, 1U);
    }

    if (owner.device_table.vkEndCommandBuffer(commands) != VK_SUCCESS) {
        owner.report(deviceDiagnostic(DiagnosticSeverity::Error,
                                          DiagnosticCategory::SubmissionFailed,
                                          u8"vk.frame.end_failed",
                                          u8"vk: vkEndCommandBuffer failed"));
        // NOTHING WAS SUBMITTED, SO NOTHING MOVED: the records this pass walk advanced are put back (see the lambdas
        // above), and the acquired window image is given back rather than left signalled and unpresented.
        rewindUnrunFrame();
        owner.caches.endFrame(uploadBatch());
        owner.device_table.vkFreeCommandBuffers(owner.device, command_pool, 1U, &commands);
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

    const VkResult submit_result = [&]() {
        // THE QUEUE IS EXTERNALLY SYNCHRONIZED (see Impl::queue_submit): another driver may be submitting on its own
        // thread right now, and the lock is taken for the driver call only - the fence wait below stays outside it.
        const std::lock_guard<std::mutex> submit_held(owner.queue_submit);
        return owner.device_table.vkQueueSubmit(owner.queue, 1U, &submit, completion);
    }();
    const bool     submitted     = submit_result == VK_SUCCESS;
    if (submitted) {
        ++books.progress.submitted;
        frame_close.submitted = true;
    }
    const VkResult fence_result = submitted ? owner.device_table.vkWaitForFences(owner.device, 1U, &completion, VK_TRUE, UINT64_MAX)
                                            : submit_result;
    const bool     completed    = fence_result == VK_SUCCESS;
    if (completed) {
        ++books.progress.completed;
        // THE COMPLETION EVIDENCE THE PARK QUEUE WAITS FOR (see Impl::releaseLater): the frame's GPU work is provably
        // done, so every object a release handed over while this frame was in flight may now go.
        {
            const std::lock_guard<std::mutex> clock_held(owner.clock_mutex);
            owner.clock.windowCompleted(clock_window, frame_token.frame);
        }
        // THE PAIR IS READABLE NOW (the fence says the frame ran): one read, one value, and the report carries it as
        // the frame's device time (see FrameTiming). A frame the clock did not sample answers DISENGAGED - never the
        // previous frame's value.
        if (per_pass_clock) {
            std::vector<std::uint64_t> stamps(query_count, 0U);
            if (owner.device_table.vkGetQueryPoolResults(owner.device,
                                                         timestamp_pool,
                                                         0U,
                                                         query_count,
                                                         stamps.size() * sizeof(std::uint64_t),
                                                         stamps.data(),
                                                         sizeof(std::uint64_t),
                                                         VK_QUERY_RESULT_64_BIT) == VK_SUCCESS &&
                stamps[1] > stamps[0]) {
                const double             to_micros = static_cast<double>(owner.timestamp_period) / 1000.0;
                vn::graphics::FrameTiming timing;
                timing.gpu_micros = static_cast<std::uint64_t>(static_cast<double>(stamps[1] - stamps[0]) * to_micros);
                // PER-PASS ATTRIBUTION, in EXECUTION order: pass i spans boundary 2+i to 2+i+1, and the name is the
                // frame's own - a report a host can match against the plan it read (see PassTiming).
                timing.passes.reserve(pass_count);
                for (std::uint32_t index = 0; index < pass_count; ++index) {
                    const std::uint64_t from = stamps[2U + index];
                    const std::uint64_t to   = stamps[2U + index + 1U];
                    vn::graphics::PassTiming entry;
                    if (runnable_passes[index].entry != nullptr && runnable_passes[index].entry->pass != nullptr) {
                        entry.pass = runnable_passes[index].entry->pass->name();
                    }
                    entry.gpu_micros = to > from ? static_cast<std::uint64_t>(static_cast<double>(to - from) * to_micros) : 0U;
                    timing.passes.push_back(std::move(entry));
                }
                books.timing = std::move(timing);
            }
            else {
                books.timing.reset();
            }
        }
        else {
            books.timing.reset();
        }
    }
    (void)owner.device_table.vkResetFences(owner.device, 1U, &completion);
    // THE FILL IS PROVABLY COMPLETE (ResidencyClaims rule 2): every geometry this frame uploaded becomes usable by
    // every driver. A frame the fence did not complete publishes nothing - its claim is withdrawn when `frame_builds`
    // dies at the end of this call, so no key is left Uploading (`P1`).
    if (completed) {
        frame_builds.publishAll();
    }
    owner.device_table.vkFreeCommandBuffers(owner.device, command_pool, 1U, &commands);
    // The per-draw uniform buffers were named by the command buffer that just finished (or failed to): the queue is
    // idle either way, so the bytes can go - and only THIS driver's texture staging, never a frame that is still
    // recording (see ContentImages::BatchId).
    owner.caches.endFrame(uploadBatch());

    if (!completed) {
        // THE FRAME DID NOT RUN, SO ITS COMMANDS MOVED NOTHING: the same rewind the end-of-buffer failure takes (see
        // rewindUnrunFrame). A submit that failed wrote no pixel; a fence that failed may have written some, and
        // treating it as "did not run" is the SAFE reading - `Undefined` discards content instead of loading a picture
        // nobody can vouch for.
        rewindUnrunFrame();
        // A LOST DEVICE GETS ITS OWN SENTENCE, ONCE (see noteDeviceLost); the generic one is for the failures a retry
        // can still fix, which is the difference that matters to a host.
        if (!owner.note_device_lost(submit_result, "vkQueueSubmit") && !owner.note_device_lost(fence_result, "vkWaitForFences")) {
            owner.report(deviceDiagnostic(DiagnosticSeverity::Error,
                                              DiagnosticCategory::SubmissionFailed,
                                              u8"vk.frame.did_not_complete",
                                              u8"vk: the frame's command buffer did not run to completion"));
        }
    }

    // THE FRAME'S TAIL: the executor is what presents the frame it ran (see RenderFrameExecutor::executeFrame).
    PresentOutcome outcome = PresentOutcome::Skipped;
    if (window_acquired) {
        outcome = completed ? window->present() : PresentOutcome::Lost;
        if (window->deviceLost()) {
            // A present that found the device gone is the same session-level fact as a lost submit: one report, and
            // the frame's Lost outcome is what the host acts on (see PresentOutcome::Lost).
            (void)owner.note_device_lost(VK_ERROR_DEVICE_LOST, "vkQueuePresentKHR");
        }
        if (outcome == PresentOutcome::Presented) {
            ++books.progress.presented;
        }
    }
    return outcome;
}

vn::graphics::PrepareReport Device::FrameDriver::prepareFrame(const FrameDescription& frame, PresentTarget* present_to)
{
    using vn::graphics::EngineNotReadyReason;
    using vn::graphics::PrepareReport;

    // THE SAME GATE AS executeFrame, because a warm-up records and may submit a transfer batch of its own (see
    // ADR-2026-11-14): a warm-up racing a frame would share this driver's command pool. `Refused` carries no reason -
    // the report says which refusal it was (see executeFrame).
    const vn::graphics::FrameGate::Exclusive frame_lease = frames.enterExclusive();
    if (!frame_lease) {
        if (frame_reentry_report.shouldReport()) {
            owner.report(deviceDiagnostic(DiagnosticSeverity::Error,
                                              DiagnosticCategory::PassProtocolViolation,
                                              u8"vk.session.frame_reentry",
                                              u8"vk: a warm-up was started from the thread that is already driving a frame "
                                              u8"(a hook or a sink drove another engine); it was refused - a device drives one "
                                              u8"frame at a time"));
        }
        return PrepareReport{ PrepareReport::Outcome::Refused };
    }

    // THE SAME GATE AS executeFrame, in its SHARED form: a warm-up records transfers only, so it may overlap another
    // driver's frame - but never a frame of this thread (both gates answer an empty lease for that), which is
    // `Refused` here because a warm-up records and may submit, exactly like a frame.
    const vn::graphics::FrameGate::Shared device_body = owner.body_gate.enterShared();
    if (!device_body) {
        if (frame_reentry_report.shouldReport()) {
            owner.report(deviceDiagnostic(DiagnosticSeverity::Error,
                                          DiagnosticCategory::PassProtocolViolation,
                                          u8"vk.session.frame_reentry",
                                          u8"vk: a warm-up was started from a thread that is already inside a frame of "
                                          u8"this device (a hook or a sink drove another engine); it was refused - a "
                                          u8"thread drives one frame at a time"));
        }
        return PrepareReport{ PrepareReport::Outcome::Refused };
    }

    if (!(owner.device != VK_NULL_HANDLE && !owner.device_lost)) {
        // "There is nothing to prebuild" and "the device is not up" are different answers (see PrepareReport).
        return PrepareReport{ PrepareReport::Outcome::NotReady, EngineNotReadyReason::NotInitialized };
    }

    // WHICH SURFACE THE FRAME NAMES - the same resolution executeFrame makes, minus everything that would touch it:
    // nothing is acquired, resized or presented, and the frame water level does not move (see
    // RenderFrameExecutor::prepareFrame, P11). The ONE command buffer this call records holds transfers only.
    WindowTarget* window = nullptr;
    if (present_to != nullptr) {
        auto* candidate = dynamic_cast<WindowTarget*>(present_to);
        for (const auto& surface : owner.surfaces) {
            if (surface.get() == candidate) {
                window = candidate;
                break;
            }
        }
        if (window == nullptr) {
            if (owner.prepare_target_report.shouldReport()) {
                owner.report(deviceDiagnostic(DiagnosticSeverity::Warning,
                                                  DiagnosticCategory::UnsupportedRequest,
                                                  u8"vk.window.foreign_target",
                                                  u8"vk: a frame named a surface target this device did not hand out; nothing was prepared"));
            }
            return PrepareReport{ PrepareReport::Outcome::Refused };
        }
        owner.prepare_target_report.rearm(); // the frame named a target this device handed out
    }

    // THE WARM-UP IS A SUBMISSION TOO (see ADR-2026-11-14): it opens a frame on the clock, so the transfer batch it may
    // submit carries a frame number the park queue can date a release against - and a warm-up that records nothing
    // spends the token without moving the submitted watermark.
    vn::graphics::FrameToken warmup_token{};
    {
        const std::lock_guard<std::mutex> clock_held(owner.clock_mutex);
        warmup_token = owner.clock.begin();
    }
    FrameTokenCloser                        warmup_close{ owner.clock, owner.retirement, owner.clock_mutex, warmup_token };
    // THE WARM-UP'S ADMISSIONS (F4): a warm-up may BUILD a geometry too, and it publishes on its own evidence - the
    // return of its transfer-only submit (see `ResidencyClaims` rule 2 and `ContentCaches::FrameBuilds`).
    ContentCaches::FrameBuilds warmup_builds(owner.caches, clock_window);
    const ContentTotals before = owner.contentNumbers();
    PrepareReport             report;
    report.outcome = PrepareReport::Outcome::Prepared;
    // THE ONE THING THIS CALL MAY SUBMIT IS A TRANSFER BATCH OF ITS OWN (see RenderFrameExecutor::prepareFrame and
    // ADR-2026-11-14): the builds that need a command buffer - the 1x1 stand-in and every material texture of a pass -
    // are recorded on one command buffer here, submitted on the device's own fence and waited for before this call
    // answers. It is NOT a frame: no swapchain image is acquired, no rendering instance opens, nothing is drawn or
    // presented,
    // and `books.progress` does not move. The buffer is allocated LAZILY, on the first pass this device can serve: a
    // frame whose passes it cannot serve records nothing and therefore allocates nothing.
    VkCommandBuffer batch{ VK_NULL_HANDLE };
    bool            batch_attempted{ false };
    // Whether a draw this call could not prepare. The refused-content episode ends when a call prepares everything it
    // was handed, exactly as the frame path ends it (see ReportOnce).
    bool content_refused{ false };

    // THE WORKLOAD THE FRAME ASKS FOR, computed by the SDK (see FrameCounts): one definition, so this backend and the
    // reference one cannot report different counts for the same frame - and the rule is the one a recorded frame's
    // `FrameCounters` follows (a pass that draws something or announces a clear).
    const vn::graphics::FrameCounts counts = vn::graphics::frameCountsOf(frame);
    report.passes       = counts.passes;
    report.draw_packets = counts.draw_packets;
    report.draws        = counts.draws;

    // A WARM-UP WRITES TARGETS TOO (an attachment build, a texture copy into one), so it takes the same
    // claims a frame does - and gives them back on every path out of here (`P3`).
    TargetTable::Lease target_lease = owner.target_table.lease(clock_window);
    bool               claim_refused = false;

    // THE WARM-UP RESOLVES UNDER THE SAME HOLD A FRAME HEAD TAKES (see ContentCaches::Lock): it and a frame may be
    // driven from two threads, and both consult the same tables. The hold ends with the pass walk: the batch's submit
    // and the staging release below take their own (see the wrappers).
    {
        const ContentCaches::Lock cache_lock = owner.caches.lock();
        vn::graphics::ShaderProgramDescriptionPtr head_program;
        {
            const std::lock_guard<std::mutex> program_held(program_mutex);
            head_program = default_program;
        }

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

            // THE ATTACHMENT SET'S COMPATIBILITY (formats and colour count): what a pipeline is built for, and the only
            // thing a pass needs to name - dynamic rendering states the attachments when the frame records them.
            vn::graphics::RenderPassCompatibility compatibility{};
            if (draw.target == nullptr) {
                // A WINDOW PASS: its shape is known only once a swapchain exists - and preparing must not create one.
                if (window == nullptr || !window->ready()) {
                    continue;
                }
                compatibility = window->compatibility();
            }
            else {
                // THE TARGET IS CLAIMED BEFORE ITS ATTACHMENTS ARE BUILT (see executeFrame): the warm-up may be
                // running while another driver frames, and a half-rebuilt target is the one thing both forbid.
                if (!target_lease.claim(draw.target)) {
                    if (claim_conflicts.insert(draw.target).second) {
                        owner.report(deviceDiagnostic(DiagnosticSeverity::Warning,
                                                      DiagnosticCategory::UnsupportedRequest,
                                                      u8"vk.target.claim_conflict",
                                                      u8"vk: a pass named a target another frame driver is drawing into right "
                                                      u8"now; the pass was skipped for this preparation - drive the other "
                                                      u8"engine on another target, or wait for its frame to finish"));
                    }
                    claim_refused = true;
                    continue;
                }
                // THE TARGET'S ATTACHMENTS ARE WHAT THE FRAME WOULD BUILD ANYWAY, so preparing them is the point.
                if (!owner.target_table.ensure(*draw.target)) {
                    continue;
                }
                TargetObjects& images = *owner.target_table.lookup(draw.target);
                compatibility         = images.compatibility();
            }

            // THE BATCH IS OPENED ON THE FIRST PASS THIS DEVICE CAN SERVE - the same condition the branch above just
            // answered, so a frame nothing can be built for never allocates a command buffer.
            if (!batch_attempted) {
                batch_attempted = true;
                VkCommandBufferAllocateInfo allocate{};
                allocate.sType              = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
                allocate.commandPool        = command_pool;
                allocate.level              = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
                allocate.commandBufferCount = 1U;
                batch                       = VK_NULL_HANDLE;
                VkCommandBufferBeginInfo begin{};
                begin.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
                begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
                if (owner.device_table.vkAllocateCommandBuffers(owner.device, &allocate, &batch) != VK_SUCCESS) {
                    batch = VK_NULL_HANDLE;
                    owner.report(deviceDiagnostic(DiagnosticSeverity::Error,
                                                      DiagnosticCategory::SubmissionFailed,
                                                      u8"vk.prepare.batch_failed",
                                                      u8"vk: the preparation's transfer batch could not be recorded; the next frame uploads the "
                                                      u8"content again"));
                }
                else if (owner.device_table.vkBeginCommandBuffer(batch, &begin) != VK_SUCCESS) {
                    owner.device_table.vkFreeCommandBuffers(owner.device, command_pool, 1U, &batch);
                    batch = VK_NULL_HANDLE;
                    owner.report(deviceDiagnostic(DiagnosticSeverity::Error,
                                                      DiagnosticCategory::SubmissionFailed,
                                                      u8"vk.prepare.batch_failed",
                                                      u8"vk: the preparation's transfer batch could not be recorded; the next frame uploads the "
                                                      u8"content again"));
                }
            }

            for (const DrawPacket& description : draw.packets) {
                ContentRefusal refusal;
                if (description.kind == vn::graphics::DrawKind::Fullscreen) {
                    FrameHead::DrawPlan plan;
                    (void)owner.caches.planDraw(description, draw, compatibility, plan, refusal, cache_lock);
                }
                else if (description.commands != nullptr) {
                    // ONE PLAN PER COMMAND, the same resolution the frame head runs (see ContentCaches::planDraw): a
                    // warm-up may not build something the frame would not ask for, and vice versa.
                    for (const vn::graphics::DrawItem& command : *description.commands) {
                        FrameHead::DrawPlan plan;
                        (void)owner.caches.planDraw(command, draw, head_program.get(), compatibility, plan, refusal, cache_lock, warmup_builds);
                    }
                }
                if (refusal.isRefused()) {
                    content_refused = true;
                    if (content_report.shouldReport()) {
                        // The same forwarding the frame path does (see above).
                        const vn::graphics::RenderDiagnostic diagnostic =
                            deviceDiagnostic(DiagnosticSeverity::Warning, refusal.category(), refusal.code(), refusal.message());
                        owner.report(diagnostic);
                    }
                }
            }

            // THE PASS' OWN UPLOADS, recorded into the batch: exactly the copies the frame's head would record on its own
            // command buffer (see ContentCaches::resolveTextures), so the frame then finds them resident and records
            // nothing. A pass whose batch could not be opened is still prepared; the next frame uploads it.
            if (batch != VK_NULL_HANDLE) {
                owner.caches.preparePass(batch, draw.packets, cache_lock);
            }
        }
    }

    // THE BATCH RUNS BEFORE THIS CALL ANSWERS: one submit on the device's frame fence, no semaphore (nothing was
    // acquired, so the queue has nothing to wait for), one wait, and the staging buffers go as soon as the copies
    // have run - the release the frame path reaches through ContentDraw::endFrame -> ContentImages::releaseFrame.
    // WHETHER THE WARM-UP'S FILL IS PROVABLE (F4): a batch that never closed or never ran publishes nothing, and the
    // geometries it claimed are withdrawn when `warmup_builds` dies - so no key is left Uploading.
    bool transfers_completed = true;
    if (batch != VK_NULL_HANDLE) {
        if (owner.device_table.vkEndCommandBuffer(batch) != VK_SUCCESS) {
            // THE BUFFER NEVER CLOSED, so whatever it recorded never ran - and it cannot be recorded again: the copies
            // were already recorded against entries this call made resident. Said plainly, because "the next frame
            // uploads it again" would be a promise the revision check does not keep.
            transfers_completed = false;
            owner.report(deviceDiagnostic(DiagnosticSeverity::Error,
                                              DiagnosticCategory::SubmissionFailed,
                                              u8"vk.prepare.batch_not_closed",
                                              u8"vk: the preparation's transfer batch could not be closed; the copies it recorded never ran"));
            owner.device_table.vkFreeCommandBuffers(owner.device, command_pool, 1U, &batch);
        }
        else if (!owner.caches.hasPendingUploads(0)) {
            // ZERO WORK, ZERO SUBMIT: an unchanged frame records no copy, so there is nothing to run and nothing is
            // submitted - the property a second prepare of the same frame is observed by (see ADR-2026-11-14).
            owner.device_table.vkFreeCommandBuffers(owner.device, command_pool, 1U, &batch);
        }
        else {
            VkSubmitInfo submit{};
            submit.sType              = VK_STRUCTURE_TYPE_SUBMIT_INFO;
            submit.commandBufferCount = 1U;
            submit.pCommandBuffers    = &batch;
            const VkResult submit_result = [&]() {
                // THE QUEUE IS EXTERNALLY SYNCHRONIZED (see Impl::queue_submit): held for the driver call only.
                const std::lock_guard<std::mutex> submit_held(owner.queue_submit);
                return owner.device_table.vkQueueSubmit(owner.queue, 1U, &submit, completion);
            }();
            const VkResult fence_result  = submit_result == VK_SUCCESS
                                               ? owner.device_table.vkWaitForFences(owner.device, 1U, &completion, VK_TRUE, UINT64_MAX)
                                               : submit_result;
            const bool     completed     = fence_result == VK_SUCCESS;
            transfers_completed          = completed;
            warmup_close.submitted       = submit_result == VK_SUCCESS;
            if (completed) {
                const std::lock_guard<std::mutex> clock_held(owner.clock_mutex);
                owner.clock.windowCompleted(clock_window, warmup_token.frame);
            }
            (void)owner.device_table.vkResetFences(owner.device, 1U, &completion);
            owner.device_table.vkFreeCommandBuffers(owner.device, command_pool, 1U, &batch);
            if (completed) {
                owner.caches.releaseUploadStaging(0);
            }
            else if (!owner.note_device_lost(submit_result, "vkQueueSubmit") && !owner.note_device_lost(fence_result, "vkWaitForFences")) {
                // The staging buffers stay: the next frame's releaseFrame lets go of them once its own queue is idle.
                // They cannot buy a re-upload, though - the entries were already made resident with what the copies were
                // going to write - so this says what actually happened rather than promising a retry: a queue that
                // refuses a submit is a device-level failure (out of memory or lost device), not a content problem.
                owner.report(deviceDiagnostic(DiagnosticSeverity::Error,
                                                  DiagnosticCategory::SubmissionFailed,
                                                  u8"vk.prepare.batch_not_run",
                                                  u8"vk: the preparation's transfer batch did not run (the queue refused it); the images it "
                                                  u8"was going to fill were never written"));
            }
        }
    }

    // THE WARM-UP'S FILL IS PROVABLY COMPLETE (ResidencyClaims rule 2): publish every geometry it built. A batch with
    // zero work submits nothing, and nothing was pending - the fills were host copies - so that path publishes too.
    if (transfers_completed) {
        warmup_builds.publishAll();
    }

    // WHAT THIS CALL BUILT, not what is resident: a second prepare of an unchanged frame answers zeros (see
    // PrepareReport). `uploads` counts BOTH kinds FrameUploads reports - the streams this call uploaded and the
    // textures it made resident (see ADR-2026-11-14). `sets` stays 0 because no descriptor set outlives a frame here.
    if (!content_refused) {
        content_report.rearm(); // every draw the call named was prepared: the episode is over
    }
    if (!claim_refused) {
        claim_conflicts.clear(); // no pass lost its target this preparation: the conflict episode is over
    }
    const ContentTotals after = owner.contentNumbers();
    report.halves    = after.program_builds - before.program_builds;
    report.pipelines = after.pipeline_builds - before.pipeline_builds;
    report.uploads   = (after.stream_builds - before.stream_builds) + (after.texture_builds - before.texture_builds);
    return report;
}

vn::graphics::FrameReport Device::FrameDriver::frameReport() const noexcept
{
    vn::graphics::FrameReport report;
    report.counters = books.counters;
    report.progress = books.progress;
    report.uploads  = books.uploads; // this frame's own delta; absent until a frame ran
    report.timing   = books.timing;  // the device's own clock; absent when the device keeps none (see FrameTiming)
    report.locking  = books.locking; // what it spent on the cache lock; absent until a frame ended

    vn::graphics::RetentionStats retention;
    retention.released_nodes = owner.released_nodes;
    // THE QUEUE IS THE ONE COUNTING (see Impl::releaseLater): a release it could not park idles the device instead, and
    // a report that kept its own number would disagree with `retirementNumbers()` about the same fact.
    {
        const std::lock_guard<std::mutex> clock_held(owner.clock_mutex);
        retention.device_waits = owner.retirement.deviceWaits();
    }
    if ((owner.device != VK_NULL_HANDLE && !owner.device_lost)) {
        // WHAT IS RESIDENT, PER KIND: the three kinds this backend retains, composed here where all three tables are
        // reachable. Material blocks and descriptor sets are per-frame objects, so no row claims them.
        vn::graphics::ResidencyLedger ledger;
        owner.noteResidency(ledger);
        retention.residency = std::move(ledger);

        // WHAT THE POOL HOLDS NOW (see DeviceMemory): the byte account the ledger deliberately does not total. It is
        // the DEVICE's figure, not one driver's - a second driver reads the same pool - and the pool answers under its
        // own lock, so this is safe while another session's frame runs. Only the five byte figures travel: the block
        // and slice counters are the witness a test reads from `memoryStats()`, not a host fact.
        const MemoryStats          pool = owner.allocator.stats();
        vn::graphics::DeviceMemory memory;
        memory.used_bytes       = pool.used_bytes;
        memory.reserved_bytes   = pool.reserved_bytes;
        memory.peak_used_bytes  = pool.peak_used_bytes;
        memory.free_bytes       = pool.free_bytes;
        memory.largest_free_run = pool.largest_free_run;
        report.memory           = memory;
    }
    report.retention = std::move(retention);
    return report;
}

Device::Device(const DeviceDesc& desc)
  : impl(std::make_unique<Impl>())
{
    // THE DRIVER IS CREATED WITH THE DEVICE, not with the Vulkan session: it holds the bookkeeping a report reads
    // before `initialize()` and after `shutdown()`, and `create()` is what gives it objects.
    impl->session = std::make_unique<FrameDriver>(*impl);
    impl->desc    = desc;
    // ONE REPORT AT A TIME AT THE HOST'S SINK: two drivers' frames report on two threads (S6c-2), and the sink is the
    // host's - it is not asked to be thread-safe. A diagnostic is never on a frame's path (it is a refusal or a loss).
    impl->report           = [this](const vn::graphics::RenderDiagnostic& diagnostic) {
        const std::lock_guard<std::recursive_mutex> report_held(impl->report_mutex);
        reportDiagnostic(diagnostic);
    };
    // THE HOST-REENTRY SENTENCE (see body_gate): one per device, naming the verb, because every host verb answers the
    // same refusal and a host needs to hear it once rather than discover a call that did nothing.
    impl->report_host_reentry = [this](const char8_t* verb) {
        bool first = false;
        {
            const std::lock_guard<std::mutex> reentry_held(impl->reentry_mutex);
            first = impl->host_reentry_report.shouldReport();
        }
        if (!first) {
            return;
        }
        impl->report(deviceDiagnostic(DiagnosticSeverity::Error,
                                      DiagnosticCategory::PassProtocolViolation,
                                      u8"vk.device.reentry",
                                      vn::String(u8"vk: ") + vn::String(verb) +
                                          u8" was called from inside a frame body (a hook or a sink called back into "
                                          u8"the device); it did nothing - the verb needs the frames in flight to "
                                          u8"end, so ask between frames"));
    };
    impl->note_device_lost = [this](const VkResult result, const char* what) { return noteDeviceLost(result, what); };
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
        reportDiagnostic(deviceDiagnostic(DiagnosticSeverity::Error,
                                          DiagnosticCategory::InitFailed,
                                          u8"vk.init.loader_missing",
                                          u8"vk: no Vulkan loader (libvulkan / vulkan-1 could not be opened)"));
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
            reportDiagnostic(deviceDiagnostic(probe ? DiagnosticSeverity::Error : DiagnosticSeverity::Warning,
                                              probe ? DiagnosticCategory::InitFailed : DiagnosticCategory::UnsupportedRequest,
                                              u8"vk.window.system_missing",
                                              why.empty() ? vn::String(u8"vk: no window system is available, so this device presents nothing") : std::move(why)));
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
            reportDiagnostic(deviceDiagnostic(DiagnosticSeverity::Error,
                                              DiagnosticCategory::InitFailed,
                                              u8"vk.window.own_window_failed",
                                              why.empty() ? vn::String(u8"vk: the device-owned (Probe) window could not be brought up") : std::move(why)));
            shutdown();
            return false;
        }
        impl->surfaces.push_back(std::make_unique<WindowTarget>(*impl->window_system, impl->window_system->ownWindow()));
    }
    if (impl->presentation_wanted && impl->desc.presentation == DevicePresentation::Handle && impl->desc.native_handle == nullptr &&
        impl->surfaces.empty()) {
        reportDiagnostic(deviceDiagnostic(DiagnosticSeverity::Error,
                                          DiagnosticCategory::UnsupportedRequest,
                                          u8"vk.window.handle_missing",
                                          u8"vk: DevicePresentation::Handle needs a native window handle; this device presents nothing"));
        impl->presentation_wanted = false;
    }
    if (impl->presentation_wanted && (!has_surface_extension || !has_platform_extension)) {
        reportDiagnostic(deviceDiagnostic(DiagnosticSeverity::Warning,
                                          DiagnosticCategory::UnsupportedRequest,
                                          u8"vk.window.present_unsupported",
                                          u8"vk: the instance does not offer the surface extensions a window needs; this device presents nothing"));
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
    application.apiVersion         = VK_API_VERSION_1_3;

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
        reportDiagnostic(deviceDiagnostic(DiagnosticSeverity::Error,
                                          DiagnosticCategory::InitFailed,
                                          u8"vk.init.instance_failed",
                                          u8"vk: vkCreateInstance failed"));
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
            if (!surface->createArea(impl->instance, &why)) {
                reportDiagnostic(deviceDiagnostic(DiagnosticSeverity::Warning,
                                                  DiagnosticCategory::UnsupportedRequest,
                                                  u8"vk.window.surface_failed",
                                                  why.empty() ? vn::String(u8"vk: the host window's surface could not be created") : std::move(why)));
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
        reportDiagnostic(deviceDiagnostic(DiagnosticSeverity::Error,
                                          DiagnosticCategory::InitFailed,
                                          u8"vk.init.no_device",
                                          u8"vk: no Vulkan physical device"));
        shutdown();
        return false;
    }
    std::vector<VkPhysicalDevice> physical_devices(physical_count);
    if (vkEnumeratePhysicalDevices(impl->instance, &physical_count, physical_devices.data()) != VK_SUCCESS) {
        reportDiagnostic(deviceDiagnostic(DiagnosticSeverity::Error,
                                          DiagnosticCategory::InitFailed,
                                          u8"vk.init.no_device",
                                          u8"vk: vkEnumeratePhysicalDevices failed on its second call"));
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
        reportDiagnostic(deviceDiagnostic(DiagnosticSeverity::Error,
                                          DiagnosticCategory::InitFailed,
                                          u8"vk.init.no_queue",
                                          u8"vk: no physical device has a graphics queue family"));
        shutdown();
        return false;
    }

    // THE CHOSEN DEVICE'S PROPERTIES, ONCE: the extension choice below, the pipeline cache's key and the capabilities
    // all describe the SAME device, so they read one value (see PipelineCacheKey).
    VkPhysicalDeviceProperties properties{};
    vkGetPhysicalDeviceProperties(impl->physical, &properties);

    // THE API VERSION IS A FLOOR, NOT A PREFERENCE (`ADR-2026-11-23` D4, step 1 in T54 and step 2 in `T11`): the
    // barrier table records synchronisation 2, and dynamic rendering replaces the render-pass objects, both core in 1.3.
    // A device below it would have to be served by a second recording path - the legacy one this backend deliberately
    // does not keep - so it is refused with its own code instead.
    if (properties.apiVersion < VK_API_VERSION_1_3) {
        reportDiagnostic(deviceDiagnostic(DiagnosticSeverity::Error,
                                          DiagnosticCategory::InitFailed,
                                          u8"vk.init.api_version_low",
                                          u8"vk: this backend needs a Vulkan 1.3 device (dynamic rendering and "
                                          u8"synchronisation 2 are core there), and this device is older"));
        shutdown();
        return false;
    }

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
    bool has_extended_dynamic_state  = false;
    bool has_extended_dynamic_state2 = false;
    bool has_extended_dynamic_state3 = false;
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
            if (std::strcmp(extension.extensionName, VK_EXT_EXTENDED_DYNAMIC_STATE_EXTENSION_NAME) == 0) {
                has_extended_dynamic_state = true;
            }
            if (std::strcmp(extension.extensionName, VK_EXT_EXTENDED_DYNAMIC_STATE_2_EXTENSION_NAME) == 0) {
                has_extended_dynamic_state2 = true;
            }
            if (std::strcmp(extension.extensionName, VK_EXT_EXTENDED_DYNAMIC_STATE_3_EXTENSION_NAME) == 0) {
                has_extended_dynamic_state3 = true;
            }
        }
    }
    std::vector<const char*> device_extension_names;
    if (impl->presentation_wanted) {
        if (has_swapchain) {
            device_extension_names.push_back("VK_KHR_swapchain");
        }
        else {
            reportDiagnostic(deviceDiagnostic(DiagnosticSeverity::Warning,
                                              DiagnosticCategory::UnsupportedRequest,
                                              u8"vk.window.present_unsupported",
                                              u8"vk: the device does not offer VK_KHR_swapchain; this device presents nothing"));
            impl->presentation_wanted = false;
        }
    }
    if (has_creation_feedback && properties.apiVersion < VK_API_VERSION_1_3) {
        device_extension_names.push_back("VK_EXT_pipeline_creation_feedback");
    }
    impl->creation_feedback = has_creation_feedback;

    // THE RASTER FLOOR (`T54`, `ADR-2026-11-23` D4): cull, polygon mode, depth test/write/compare and blend are
    // DYNAMIC state, so they are no longer part of a pipeline's identity and a state change costs commands instead of
    // a compile. On a 1.3 device cull and the depth items are CORE dynamic state; POLYGON MODE and COLOUR BLEND remain
    // `VK_EXT_extended_dynamic_state3`, which is therefore the one extension this floor requires - a device without it
    // would have to bake silently, and the picture would be wrong in a way no test could name.
    //
    // THE 1.3 FEATURES ARE FEATURES, NOT VERSIONS: `synchronization2` (the barrier table records `vkCmdPipelineBarrier2`)
    // and `dynamicRendering` (step 2 of the batch) must both be ENABLED, and a 1.3 device that does not offer them is
    // refused. A version alone is not evidence that a feature is on.
    VkPhysicalDeviceVulkan13Features     vk13_available{};
    vk13_available.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES;
    VkPhysicalDeviceExtendedDynamicState3FeaturesEXT dynamic_state3_available{};
    dynamic_state3_available.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTENDED_DYNAMIC_STATE_3_FEATURES_EXT;
    VkPhysicalDeviceFeatures2 dynamic_state_query{};
    dynamic_state_query.sType     = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2;
    dynamic_state_query.pNext     = &vk13_available;
    vk13_available.pNext          = &dynamic_state3_available;
    vkGetPhysicalDeviceFeatures2(impl->physical, &dynamic_state_query);
    if (vk13_available.synchronization2 == VK_FALSE || vk13_available.dynamicRendering == VK_FALSE) {
        reportDiagnostic(deviceDiagnostic(DiagnosticSeverity::Error,
                                          DiagnosticCategory::InitFailed,
                                          u8"vk.init.device_features_missing",
                                          u8"vk: this backend needs the Vulkan 1.3 synchronization2 and dynamicRendering "
                                          u8"features, and this device does not offer them"));
        shutdown();
        return false;
    }
    const bool dynamic_raster_floor = has_extended_dynamic_state3 &&
                                      dynamic_state3_available.extendedDynamicState3PolygonMode == VK_TRUE &&
                                      dynamic_state3_available.extendedDynamicState3ColorBlendEnable == VK_TRUE &&
                                      dynamic_state3_available.extendedDynamicState3ColorBlendEquation == VK_TRUE;
    if (!dynamic_raster_floor) {
        reportDiagnostic(deviceDiagnostic(DiagnosticSeverity::Error,
                                          DiagnosticCategory::InitFailed,
                                          u8"vk.init.dynamic_state_missing",
                                          u8"vk: this backend needs VK_EXT_extended_dynamic_state3 with polygon mode and "
                                          u8"colour blend (cull / depth / polygon / blend are dynamic state), and this device "
                                          u8"does not offer them"));
        shutdown();
        return false;
    }
    device_extension_names.push_back(VK_EXT_EXTENDED_DYNAMIC_STATE_3_EXTENSION_NAME);
    VkPhysicalDeviceExtendedDynamicState3FeaturesEXT dynamic_state3_enable{};
    dynamic_state3_enable.sType                                   = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTENDED_DYNAMIC_STATE_3_FEATURES_EXT;
    dynamic_state3_enable.extendedDynamicState3PolygonMode        = VK_TRUE;
    dynamic_state3_enable.extendedDynamicState3ColorBlendEnable   = VK_TRUE;
    dynamic_state3_enable.extendedDynamicState3ColorBlendEquation = VK_TRUE;
    VkPhysicalDeviceVulkan13Features vk13_enable{};
    vk13_enable.sType            = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES;
    vk13_enable.synchronization2 = VK_TRUE;
    vk13_enable.dynamicRendering = VK_TRUE;
    vk13_enable.pNext            = &dynamic_state3_enable;
    device_info.pNext            = &vk13_enable;
    if (!device_extension_names.empty()) {
        device_info.enabledExtensionCount   = static_cast<std::uint32_t>(device_extension_names.size());
        device_info.ppEnabledExtensionNames = device_extension_names.data();
    }

    if (vkCreateDevice(impl->physical, &device_info, nullptr, &impl->device) != VK_SUCCESS) {
        reportDiagnostic(deviceDiagnostic(DiagnosticSeverity::Error,
                                          DiagnosticCategory::InitFailed,
                                          u8"vk.init.device_failed",
                                          u8"vk: vkCreateDevice failed"));
        shutdown();
        return false;
    }
    volkLoadDevice(impl->device);
    volkLoadDeviceTable(&impl->device_table, impl->device);
    impl->device_table.vkGetDeviceQueue(impl->device, impl->queue_family, 0U, &impl->queue);

    // THE FRAME DRIVER: a session owns the command machinery a frame needs, and this device drives its frames through
    // it (see Device::FrameDriver). Its clock window is registered here too, which is what arms the park queue.
    impl->clock = vn::graphics::DeviceClock{};
    switch (impl->session->create()) {
    case FrameDriver::CreateRefusal::Ok:
        break;
    case FrameDriver::CreateRefusal::CommandPool:
        reportDiagnostic(deviceDiagnostic(DiagnosticSeverity::Error,
                                          DiagnosticCategory::InitFailed,
                                          u8"vk.init.command_machinery_failed",
                                          u8"vk: vkCreateCommandPool failed"));
        shutdown();
        return false;
    case FrameDriver::CreateRefusal::Fence:
        reportDiagnostic(deviceDiagnostic(DiagnosticSeverity::Error,
                                          DiagnosticCategory::InitFailed,
                                          u8"vk.init.command_machinery_failed",
                                          u8"vk: vkCreateFence failed"));
        shutdown();
        return false;
    case FrameDriver::CreateRefusal::NoDevice:
        shutdown();
        return false;
    }

    vkGetPhysicalDeviceMemoryProperties(impl->physical, &impl->memory);

    // THE MEMORY POOL COMES FIRST OF EVERYTHING THAT OWNS MEMORY: every resource below - the window's depth image, the
    // attachments, the textures, the streams and the ring - asks it for a slice, and it is the one place that answers
    // which memory type serves a purpose (see MemoryAllocator). `bufferImageGranularity` is the device's rule about
    // which slices may share a page, so it is a device fact, read here and nowhere else.
    if (!impl->allocator.open(impl->device_table, impl->device, impl->memory, properties.limits.bufferImageGranularity)) {
        reportDiagnostic(deviceDiagnostic(DiagnosticSeverity::Error,
                                          DiagnosticCategory::InitFailed,
                                          u8"vk.init.memory_pool_failed",
                                          u8"vk: the device's memory pool could not be opened"));
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
                reportDiagnostic(deviceDiagnostic(DiagnosticSeverity::Warning,
                                                  DiagnosticCategory::UnsupportedRequest,
                                                  u8"vk.window.attach_failed",
                                                  why.empty() ? vn::String(u8"vk: the host window's swapchain could not be created") : std::move(why)));
                impl->presentation_wanted = false;
                break;
            }
        }
    }

    // CAPABILITIES ARE WHAT THIS BACKEND CAN SERVE, not what the device can do: M1 builds off-screen colour targets and
    // reads them back, and still presents nothing (M5).

    // DEPTH RESOLVE IS ASKED OF THE DEVICE, NOT ASSUMED FROM THE FLOOR: the SHAPE is core in Vulkan 1.3 (one
    // `resolveMode` field on the depth attachment), but the MODE this backend states is `SAMPLE_ZERO` - the only one no
    // multisampled draw can change - and which modes a device supports is its own answer
    // (`VkPhysicalDeviceDepthStencilResolveProperties::supportedDepthResolveModes`). A device without that mode refuses
    // the one shape, once, instead of being handed a resolve the spec does not allow.
    {
        VkPhysicalDeviceDepthStencilResolveProperties depth_resolve{};
        depth_resolve.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DEPTH_STENCIL_RESOLVE_PROPERTIES;
        VkPhysicalDeviceProperties2 properties2{};
        properties2.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2;
        properties2.pNext = &depth_resolve;
        vkGetPhysicalDeviceProperties2(impl->physical, &properties2);
        impl->depth_resolve_supported = (depth_resolve.supportedDepthResolveModes & VK_RESOLVE_MODE_SAMPLE_ZERO_BIT) != 0U;
    }

    // THE DEVICE CLOCK (see FrameTiming): the frame's own pair to start; the per-pass boundaries are added by the first
    // frame that needs them, between frames (see ensureTimestampQueries).
    impl->timestamp_period = properties.limits.timestampPeriod;
    if (properties.limits.timestampComputeAndGraphics != VK_FALSE && impl->timestamp_period > 0.0F) {
        impl->session->timestamps = impl->session->ensureTimestampQueries(2U);
    }

    impl->caps                         = Capabilities{};
    impl->caps.render_targets          = true;
    impl->caps.presentation            = impl->presentation_wanted;
    impl->caps.features.bits           = 0U;
    // THE FEATURE BITS ARE WHAT WAS ENABLED, not what the device might do: each one is set from the SAME value
    // `pEnabledFeatures` carried, so a switch a host sees advertised is a switch the driver will accept.
    // Anisotropy additionally requires a limit above 1: a bit saying "anisotropy is available" beside a factor of 1
    // would advertise a quality setting that cannot do anything.
    if (wanted_features.fillModeNonSolid == VK_TRUE) {
        impl->caps.features.bits |= vn::graphics::Features::bit(vn::graphics::Features::Feature::Wireframe);
    }
    if (wanted_features.samplerAnisotropy == VK_TRUE && properties.limits.maxSamplerAnisotropy > 1.0F) {
        impl->caps.features.bits |= vn::graphics::Features::bit(vn::graphics::Features::Feature::AnisotropicFiltering);
    }
    // GPU TIMINGS IS A DEVICE FACT, not a promise this backend measures anything yet (see Feature::GpuTimings): the
    // device can write timestamps for graphics and compute work.
    if (properties.limits.timestampComputeAndGraphics != VK_FALSE) {
        impl->caps.features.bits |= vn::graphics::Features::bit(vn::graphics::Features::Feature::GpuTimings);
    }
    // THE POOL ACCOUNTS FOR ITSELF (see Feature::MemoryUsage): every block it reserves and every slice it hands out is
    // this backend's own bookkeeping, so "how much memory does this device hold for me" is a measurement rather than
    // an estimate, and the numbers reach the host through `FrameReport::memory`.
    impl->caps.features.bits |= vn::graphics::Features::bit(vn::graphics::Features::Feature::MemoryUsage);
    // ONE target has ONE sample count, and its colour and depth attachments must both support it, so the set this
    // device offers is the INTERSECTION of the two (a device whose depth tops out lower is the binding side). That
    // intersection is a `VkSampleCountFlags` BIT SET, while `Capabilities::limits.max_samples` states the largest COUNT a
    // target may ask for - the number a host hands to `RenderTarget::setSamples` - so the limit is read OUT of the
    // set rather than being the set. The same set is what `TargetTable::ensure` clamps a request against.
    impl->supported_sample_counts      = static_cast<std::uint32_t>(properties.limits.framebufferColorSampleCounts & properties.limits.framebufferDepthSampleCounts);
    impl->caps.limits.max_samples             = largestSampleCount(impl->supported_sample_counts);
    // THE LIMITS TRAVEL BESIDE THE BITS, and each is the device's own answer: "anisotropy exists" and "how much of
    // it" are different facts, and so are "wide lines are allowed" and "how wide".
    impl->caps.limits.max_anisotropy          = impl->caps.features.supports(vn::graphics::Features::Feature::AnisotropicFiltering) ? properties.limits.maxSamplerAnisotropy : 1.0F;
    impl->caps.limits.max_line_width          = properties.limits.lineWidthRange[1] > 0.0F ? properties.limits.lineWidthRange[1] : 1.0F;
    // THE DEVICE'S OWN COUNT: an off-full-screen pass here can reference as many colour attachments as the device
    // takes, and capping that below what the device offers would be an amputation nothing in this backend needs.
    impl->caps.limits.max_color_attachments   = properties.limits.maxColorAttachments;

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
    if (!impl->caches.create(impl->device_table,
                              impl->device,
                              impl->allocator,
                              properties.limits.minUniformBufferOffsetAlignment,
                              initial_pipeline_cache)) {
        reportDiagnostic(deviceDiagnostic(DiagnosticSeverity::Error,
                                          DiagnosticCategory::InitFailed,
                                          u8"vk.init.content_pool_failed",
                                          u8"vk: the content layout / descriptor pool could not be created"));
        shutdown();
        return false;
    }
    // THE TWO RASTERISATION LIMITS THE RECORDERS HAVE TO KNOW: a line wider than the device's range is an invalid
    // command, and an anisotropy factor past the device's maximum is an invalid sampler - both are clamped to the
    // numbers the capabilities just reported, so a host asking for more gets the device's best answer instead of a
    // validation error.
    impl->caches.setLineWidthLimit(impl->caps.limits.max_line_width);
    impl->caches.setMaxAnisotropy(impl->caps.limits.max_anisotropy);
    impl->caches.setCreationFeedback(impl->creation_feedback);

    // EVERY DRIVER COMES UP WITH THE DEVICE, not only the device's own: a session borrowed before initialize()
    // (R1 - an engine attaches first) gets its command machinery and its frame scratch HERE, where the layouts
    // exist. A driver that cannot hold its constants is not a driver that can draw, so the device refuses to
    // come up rather than answer frames with nothing.
    const auto bring_up = [this](FrameDriver& driver) -> bool {
        if (driver.create() != FrameDriver::CreateRefusal::Ok) {
            reportDiagnostic(deviceDiagnostic(DiagnosticSeverity::Error,
                                              DiagnosticCategory::InitFailed,
                                              u8"vk.init.command_machinery_failed",
                                              u8"vk: a frame driver's command pool / fence could not be created"));
            return false;
        }
        if (!driver.createScratch()) {
            reportDiagnostic(deviceDiagnostic(DiagnosticSeverity::Error,
                                              DiagnosticCategory::InitFailed,
                                              u8"vk.init.content_pool_failed",
                                              u8"vk: a frame driver's uniform ring / descriptor pool could not be created"));
            return false;
        }
        return true;
    };
    if (!bring_up(*impl->session)) {
        shutdown();
        return false;
    }
    for (std::unique_ptr<FrameDriver>& extra : impl->extra_sessions) {
        if (!bring_up(*extra)) {
            shutdown();
            return false;
        }
    }

    // THE PARK QUEUE'S WINDOW, from the first frame on: the driver registered its clock window in create(), so the
    // queue knows how many frames may be in flight and every released object waits for that watermark (see
    // Impl::releaseLater).
    impl->retirement.setSlots(impl->clock.widestSlots());
    // THE PARK SEAM, not the queue itself: the caches ask the DEVICE "when may this go?", and the device answers with
    // its clock and its queue (see Impl::releaseLater). That is what lets two drivers share one clock safely.
    impl->caches.setRetirement([impl = impl.get()](std::function<void()> destroy) { impl->releaseLater(std::move(destroy)); });

    // THE TARGET TABLE'S SESSION (see TargetTable::open): the same park seam the caches got, plus the device facts a
    // build compares against. A release parks THROUGH the table, so "the entry left the map" and "the images wait for
    // the watermark" are one call and no caller can do half of it.
    impl->target_table.open(TargetTable::Device{ impl->device, &impl->device_table, &impl->allocator },
                            TargetTable::Limits{ impl->supported_sample_counts, impl->depth_resolve_supported },
                            [this](const vn::graphics::RenderDiagnostic& diagnostic) { reportDiagnostic(diagnostic); },
                            [impl = impl.get()](std::shared_ptr<TargetObjects> doomed) {
                                impl->releaseLater([table = impl->device_table,
                                                    device = impl->device,
                                                    allocator = &impl->allocator,
                                                    doomed = std::move(doomed)]() mutable {
                                    destroyTarget(table, device, *allocator, *doomed);
                                });
                            });

    // WHAT THE RECORDER DID WITH THE FILE, recorded where it will outlive the session (see pipelineCacheReport): the
    // hand-over and any refusal happen ONCE, at creation, and `content.destroy()` is what would erase them.
    impl->cache_books.hits_measured   = impl->creation_feedback;
    impl->cache_books.preload_handed  = impl->caches.pipelineCachePreloadHanded();
    impl->cache_books.preload_refused = impl->caches.pipelineCachePreloadRefused();

    return true;
}

void Device::shutdown()
{
    // SHUTDOWN IS THE ONE HOST VERB THAT OWNS THE WHOLE DEVICE: every frame in flight must be over before anything is
    // destroyed, so it takes the gate exclusively (see Impl::body_gate). Called from inside a frame body it does
    // nothing: the device keeps what it has, which is the only safe answer (see host_reentry_report).
    const vn::graphics::FrameGate::Exclusive device_lock = impl->body_gate.enterExclusive();
    if (!device_lock) {
        impl->report_host_reentry(u8"shutdown()");
        return;
    }
    if (impl->device != VK_NULL_HANDLE) {
        // A counted stop: every queue is idle before the objects it recorded against go.
        (void)impl->device_table.vkDeviceWaitIdle(impl->device);

        // WHAT THE RELEASES PARKED NOW GOES: the idle above IS the evidence every parked release was waiting for, so
        // the queue runs out of turn and the objects die while their device is still alive. A device that skipped this
        // would leak every object a release had parked - and the pool below would keep the blocks alive.
        impl->caches.setRetirement({});
        {
            const std::lock_guard<std::mutex> clock_held(impl->clock_mutex);
            impl->retirement.releaseAll();
        }

        // THE SESSION'S PIPELINE CACHE GOES TO THE HOST'S DIRECTORY FIRST: `vkGetPipelineCacheData` needs the cache
        // object, which `content.destroy()` below is about to destroy. A write that fails is a cold start next time,
        // never a failed shutdown (the file is an optimisation).
        impl->cache_books.cache_hits = impl->caches.pipelineCacheHits();
        if (!impl->cache_directory.empty()) {
            const std::vector<std::byte> pipeline_cache = impl->caches.pipelineCacheBytes();
            if (PipelineCacheFile::write(impl->cache_directory, impl->cache_key, pipeline_cache)) {
                impl->cache_books.saved_bytes = pipeline_cache.size();
            }
        }

        // EVERY OTHER DRIVER GOES FIRST for the same reason the device's own scratch does: its pool holds cached
        // sets and its ring is a slice, while the layouts belong to the caches below.
        for (std::unique_ptr<FrameDriver>& extra : impl->extra_sessions) {
            extra->destroy();
        }
        impl->extra_sessions.clear();

        // THE FRAME SCRATCH GOES BEFORE THE CACHES: its pool holds the cached sets and its ring is a slice of the
        // allocator, while the layouts it allocated against belong to the caches - so the borrowed half goes first.
        impl->session->scratch.destroy();

        // Pipelines are keyed by the attachment-set COMPATIBILITY, so they hold no render-pass handle; destroying the content
        // caches first is still the order that keeps teardown simple (the queue is idle, nothing names them).
        impl->caches.destroy();

        // THE TARGETS' RECORDS GO BEFORE THE POOL THEY SLICED: the queue was idle above, so nothing names
        // them. `drain()` takes the map AND the claims: "the targets are gone" and "nobody holds them" are one
        // fact, which is the reason they live in one table.
        for (const std::shared_ptr<TargetObjects>& images : impl->target_table.drain()) {
            destroyTarget(impl->device_table, impl->device, impl->allocator, *images);
        }
        impl->target_table.close();

        // The window targets first: their destructors use the device table/instance this block is about to destroy.
        impl->surfaces.clear();
        impl->presentation_wanted = false;

        // THE DRIVER'S OBJECTS GO AFTER THE RESOURCES THEY RECORDED AGAINST and before the device they borrowed: the
        // queue was idle above, so nothing names them.
        // THE DRIVER STAYS (its objects go): a report read after `shutdown()` must answer "this device measured
        // nothing", not crash - the same reason the content recorder outlives its own `destroy()`.
        impl->session->destroy();

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
    // THE NEXT SESSION STARTS WITH NOTHING REPORTED: these episodes are about one session's wiring and one session's
    // answers, so the rebuild a lost device needs must be able to tell its own story (see ReportOnce). The target
    // table's six episodes are re-armed by its own `open()`.
    impl->prepare_target_report.rearm();
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
    // The frame's books went with the driver (see FrameDriver::destroy): the next initialize() starts a device that
    // measured nothing yet.
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
    bool first = false;
    {
        const std::lock_guard<std::mutex> held(impl->loss_mutex);
        first = impl->device_lost_report.shouldReport();
    }
    if (first) {
        // THE SENTENCE SAYS WHAT TO DO, not only what happened: this is the one failure a retry cannot fix.
        const std::string sentence = std::string("vk: ") + (what != nullptr ? what : "a device call") +
                                     " answered VK_ERROR_DEVICE_LOST; this session cannot continue - rebuild it with "
                                     "shutdown() and initialize()";
        reportDiagnostic(deviceDiagnostic(DiagnosticSeverity::Error,
                                          DiagnosticCategory::SubmissionFailed,
                                          u8"vk.session.device_lost",
                                          vn::String::fromUtf8(sentence)));
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
    // A HOST VERB OWNS THE GATE (see Impl::body_gate): it waits for the frames in flight instead of racing them. Called
    // from a frame body's thread it does nothing instead of waiting for itself (see host_reentry_report).
    const vn::graphics::FrameGate::Exclusive device_lock = impl->body_gate.enterExclusive();
    if (!device_lock) {
        impl->report_host_reentry(u8"setMemoryBlockCreator()");
        return;
    }
    // A TEST SEAM, NOT A POLICY (see the header): blocks are created lazily, so this is read by the first target or
    // texture that needs one, and empty means the device table's own `vkAllocateMemory`.
    impl->allocator.setBlockCreator(std::move(creator));
}

void Device::setDefaultContentProgram(vn::intrusive_ptr<const vn::graphics::ShaderProgram> program)
{
    // A HOST VERB OWNS THE GATE (see Impl::body_gate): it waits for the frames in flight instead of racing them. Called
    // from a frame body's thread it does nothing instead of waiting for itself (see host_reentry_report).
    const vn::graphics::FrameGate::Exclusive device_lock = impl->body_gate.enterExclusive();
    if (!device_lock) {
        impl->report_host_reentry(u8"setDefaultContentProgram()");
        return;
    }
    // THE DRIVER'S SETTING (see RenderFrameExecutor::setDefaultContentProgram): this device runs its own frames through
    // its own session, so a caller that drives the device directly keeps the shape it had - and a real second driver
    // would be told its own program, not this one's.
    impl->session->setDefaultContentProgram(std::move(program));
}

vn::graphics::RenderFrameExecutor* Device::createFrameDriver()
{
    if (impl->device_lost) {
        return nullptr;
    }
    // A DRIVER BORROWED BEFORE initialize() IS BUILT BY IT (R1: an engine attaches first); one borrowed from a device
    // that is ALREADY up must come up here, or it would answer frames with no command pool. A HOST VERB, so it owns
    // the gate; from a frame body's thread it answers null (creating drivers from a sink is wiring this layer cannot
    // serve).
    const vn::graphics::FrameGate::Exclusive guard = impl->body_gate.enterExclusive();
    if (!guard) {
        impl->report_host_reentry(u8"createFrameDriver()");
        return nullptr;
    }
    std::unique_ptr<FrameDriver>      session = std::make_unique<FrameDriver>(*impl);
    if (impl->device != VK_NULL_HANDLE) {
        if (session->create() != FrameDriver::CreateRefusal::Ok) {
            session->destroy();
            impl->report(deviceDiagnostic(DiagnosticSeverity::Error,
                                          DiagnosticCategory::InitFailed,
                                          u8"vk.init.command_machinery_failed",
                                          u8"vk: a borrowed frame driver could not create its command pool / fence"));
            return nullptr;
        }
        if (!session->createScratch()) {
            session->destroy();
            impl->report(deviceDiagnostic(DiagnosticSeverity::Error,
                                          DiagnosticCategory::InitFailed,
                                          u8"vk.init.content_pool_failed",
                                          u8"vk: a borrowed frame driver's uniform ring / descriptor pool could not be created"));
            return nullptr;
        }
    }
    impl->extra_sessions.push_back(std::move(session));
    return impl->extra_sessions.back().get();
}

void Device::releaseFrameDriver(vn::graphics::RenderFrameExecutor& session)
{
    if (&session == impl->session.get()) {
        return; // the device's own driver is not a host's to return (see the header)
    }
    const vn::graphics::FrameGate::Exclusive guard = impl->body_gate.enterExclusive();
    if (!guard) {
        // From a frame body's thread the answer is "nothing was released": destroying a driver a frame is running on
        // would be a use-after-free, and waiting would be the thread waiting for itself.
        impl->report_host_reentry(u8"releaseFrameDriver()");
        return;
    }
    const auto found = std::find_if(impl->extra_sessions.begin(), impl->extra_sessions.end(), [&session](const std::unique_ptr<FrameDriver>& candidate) {
        return candidate.get() == &session;
    });
    if (found == impl->extra_sessions.end()) {
        return; // not one this device handed out
    }
    // THE CALLER GUARANTEES NOTHING IS DRIVING IT (see the header): the body lock above is what makes that
    // guarantee cheap to honour - no frame body can be in flight while these objects go.
    if (impl->device != VK_NULL_HANDLE) {
        (*found)->destroy();
    }
    impl->extra_sessions.erase(found);
}

vn::graphics::RenderFrameExecutor& Device::executor()
{
    // THE DEVICE'S OWN DRIVER (see Device::FrameDriver): the frames a host drives through the device are this object's
    // frames, and step 2 of the ADR is what makes `createFrameDriver()` hand out the others.
    return *impl->session;
}

void Device::setRenderTarget(RenderTarget* target)
{
    // A HOST VERB OWNS THE GATE (see Impl::body_gate): it waits for the frames in flight instead of racing them. Called
    // from a frame body's thread it does nothing instead of waiting for itself (see host_reentry_report).
    const vn::graphics::FrameGate::Exclusive device_lock = impl->body_gate.enterExclusive();
    if (!device_lock) {
        impl->report_host_reentry(u8"setRenderTarget()");
        return;
    }
    // THE VERB BUILDS WHAT IT ANNOUNCES (BC-09, see RenderDevice::setRenderTarget and the reference backend):
    // "a target can be built without a frame" is one ability in both backends, so a later frame that names this
    // target finds its attachments built and builds nothing. The table reports its own refusal once and answers
    // false; the announcement is then not taken (the host learns at the call site, not on the first frame).
    if (target != nullptr) {
        (void)impl->target_table.ensure(*target);
    }
}

void Device::releaseRenderTarget(RenderTarget* target)
{
    // NO BODY GATE (R6, S6c-2b): this verb does not wait for the frames in flight, it removes the entry and PARKS the
    // images. A frame that had already resolved this target keeps a pointer into the entry, and the park keeps the
    // object alive until the watermark has passed that frame - so the release is safe without stopping anything.
    // THE PARK IS THE TABLE'S OWN SEAM (see TargetTable::open): "the entry leaves the map" and "the images wait for
    // the watermark" are one call, so no caller can do half of it.
    if (target == nullptr) {
        return;
    }
    (void)impl->target_table.release(target);
}

vn::raw_ptr<vn::graphics::WindowPresentTarget> Device::createSurfaceTarget(void* native_handle, SurfaceTargetResult* why)
{
    // A HOST VERB OWNS THE GATE (see Impl::body_gate): it appends to the device's window list, which every frame body
    // reads - so it waits for the frames in flight. Called from a frame body's thread it answers UnsupportedSurface
    // rather than waiting for itself.
    const vn::graphics::FrameGate::Exclusive device_lock = impl->body_gate.enterExclusive();
    if (!device_lock) {
        impl->report_host_reentry(u8"createSurfaceTarget()");
        if (why != nullptr) {
            *why = SurfaceTargetResult::UnsupportedSurface;
        }
        return nullptr;
    }
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
                reportDiagnostic(deviceDiagnostic(DiagnosticSeverity::Warning,
                                                  DiagnosticCategory::UnsupportedRequest,
                                                  u8"vk.window.handle_missing",
                                                  u8"vk: DevicePresentation::Handle needs a host window handle; this device brings up none"));
            }
            return nullptr;
        }
        // THE CONNECTION AND THE WINDOW ARE TWO FACTS: a display that cannot be reached and a window that would not
        // come up have different repairs, so each is its own episode (see WindowReports).
        vn::String why;
        if (!ensureWindowSystem(&why)) {
            answer(SurfaceTargetResult::UnsupportedSurface);
            if (impl->window_reports.window_system.shouldReport()) {
                reportDiagnostic(deviceDiagnostic(DiagnosticSeverity::Warning,
                                                  DiagnosticCategory::UnsupportedRequest,
                                                  u8"vk.window.system_missing",
                                                  why.empty() ? vn::String(u8"vk: no window system is available, so no window surface can be created") : std::move(why)));
            }
            return nullptr;
        }
        if (!impl->window_system->openOwnWindow(kProbeWindowWidth, kProbeWindowHeight, &why)) {
            answer(SurfaceTargetResult::UnsupportedSurface);
            if (impl->window_reports.own_window.shouldReport()) {
                reportDiagnostic(deviceDiagnostic(DiagnosticSeverity::Warning,
                                                  DiagnosticCategory::UnsupportedRequest,
                                                  u8"vk.window.own_window_failed",
                                                  why.empty() ? vn::String(u8"vk: the device-owned window could not be brought up") : std::move(why)));
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
                reportDiagnostic(deviceDiagnostic(DiagnosticSeverity::Warning,
                                                  DiagnosticCategory::UnsupportedRequest,
                                                  u8"vk.window.system_missing",
                                                  why.empty() ? vn::String(u8"vk: no window system is available, so no window surface can be created") : std::move(why)));
            }
            return nullptr;
        }
    }
    for (const auto& surface : impl->surfaces) {
        if (surface->nativeHandle() == window_handle) {
            // THE ASK IS REFUSED, and the host already holds the target the first ask answered (see RenderDevice):
            // one window carries one live swapchain, so a second target would be a second owner of the same surface.
            answer(SurfaceTargetResult::WindowOccupied);
            // AND THE HOST HAS TO HEAR IT: createSurfaceTarget promises that the two refusals which are not "this
            // device presents nothing" are said once per episode, and a host that asked has to learn why the picture
            // it expected never appears.
            if (impl->window_reports.occupied.shouldReport()) {
                reportDiagnostic(deviceDiagnostic(DiagnosticSeverity::Warning,
                                                  DiagnosticCategory::UnsupportedRequest,
                                                  u8"vk.window.occupied",
                                                  u8"vk: this device already serves that window - one window carries one live swapchain, so the second ask hands out no target"));
            }
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
                reportDiagnostic(deviceDiagnostic(DiagnosticSeverity::Warning,
                                                  DiagnosticCategory::UnsupportedRequest,
                                                  u8"vk.window.attach_failed",
                                                  reason.empty() ? vn::String(u8"vk: the window target could not be created") : std::move(reason)));
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

Capabilities Device::capabilities() const noexcept
{
    return impl->caps;
}

PresentOutcome Device::executeFrame(const FrameDescription& frame, PresentTarget* present_to)
{
    // THE FRAME IS THE DRIVER'S (see Device::FrameDriver): this verb exists because a device is also a RenderFrameExecutor
    // (RenderBackend is the aggregate), so it answers the driver `executor()` hands out.
    return impl->session->executeFrame(frame, present_to);
}

bool Device::releaseContent(vn::raw_ptr<const vn::Object> content)
{
    // NO BODY GATE (R6, S6c-2b): a release does not wait for the frames in flight. What makes it safe is the cache
    // lock (a frame head resolves under it, so the tables are never read mid-erase) plus the park (a frame that
    // already resolved these objects keeps them alive until the watermark). It waits at most for ONE cache
    // operation, never for a whole frame.
    if (content == nullptr || !initialized()) {
        return false;
    }

    // ONE TYPE DISPATCH: the SDK names the entry with Object because the four content types have no common narrower
    // base (see graphics-content-lifecycle.md). No two of them are related, and a Texture2D / CubeMap answers through
    // Texture.
    if (const auto* geometry = vn::obj_cast<const vn::graphics::Geometry>(content)) {
        const vn::graphics::GeometryDescriptionPtr description = geometry->description();
        const std::size_t dropped = description != nullptr ? impl->caches.releaseGeometry(description.get()) : 0U;
        if (dropped > 0U) {
            ++impl->released_nodes;
        }
        return true;
    }
    if (const auto* program = vn::obj_cast<const vn::graphics::ShaderProgram>(content)) {
        const vn::graphics::ShaderProgramDescriptionPtr description = program->description();
        const std::size_t dropped = description != nullptr ? impl->caches.releaseProgram(description.get()) : 0U;
        if (dropped > 0U) {
            ++impl->released_nodes;
        }
        return true;
    }
    if (const auto* material = vn::obj_cast<const vn::graphics::MaterialValues>(content)) {
        // RECOGNIZED, AND NOTHING TO DROP: a material's values are packed into each draw's uniform buffer, and no
        // per-material GPU state is kept. Answering false here would say the object is not content - it is.
        (void)material;
        return true;
    }
    if (const auto* texture = vn::obj_cast<const vn::graphics::Texture>(content)) {
        const std::uint64_t before = impl->caches.imageProgress().releases;
        const bool          answered = impl->caches.releaseTexture(*texture);
        if (answered && impl->caches.imageProgress().releases != before) {
            ++impl->released_nodes;
        }
        return answered;
    }
    return false;
}

bool Device::releaseContent(const vn::Buffer<float>& buffer)
{
    // NO BODY GATE (R6, S6c-2b): a release does not wait for the frames in flight. What makes it safe is the cache
    // lock (a frame head resolves under it, so the tables are never read mid-erase) plus the park (a frame that
    // already resolved these objects keeps them alive until the watermark). It waits at most for ONE cache
    // operation, never for a whole frame.
    if (!initialized()) {
        return false;
    }
    // The element type is the caller's way of saying WHICH stream; the residency keys on the buffer's ADDRESS (see
    // ContentDraw::releaseStreams), so the two overloads share one rule and one body would.
    const std::size_t dropped = impl->caches.releaseStreams(&buffer);
    if (dropped > 0U) {
        ++impl->released_nodes;
    }
    return true;
}

bool Device::releaseContent(const vn::Buffer<std::uint32_t>& buffer)
{
    // NO BODY GATE (R6, S6c-2b): a release does not wait for the frames in flight. What makes it safe is the cache
    // lock (a frame head resolves under it, so the tables are never read mid-erase) plus the park (a frame that
    // already resolved these objects keeps them alive until the watermark). It waits at most for ONE cache
    // operation, never for a whole frame.
    if (!initialized()) {
        return false;
    }
    const std::size_t dropped = impl->caches.releaseStreams(&buffer);
    if (dropped > 0U) {
        ++impl->released_nodes;
    }
    return true;
}

vn::graphics::PrepareReport Device::prepareFrame(const FrameDescription& frame, PresentTarget* present_to)
{
    // The same forward the frame takes: a warm-up submits a transfer batch of its own, so it is a driver verb too.
    return impl->session->prepareFrame(frame, present_to);
}

vn::graphics::FrameReport Device::frameReport() const noexcept
{
    // A report about THIS device's frames is its driver's books plus the device-level retention and residency numbers.
    return impl->session->frameReport();
}

vn::graphics::ReadbackOutcome Device::readColorBuffer(const vn::graphics::ImageRef& image, std::vector<std::uint8_t>& outPixels)
{
    const auto fail = [](const ReadbackResult reason) { return vn::graphics::ReadbackOutcome{ reason }; };

    if (image.kind() != vn::graphics::ImageRef::Kind::Color) {
        return fail(ReadbackResult::Invalid); // the wrong verb for this image
    }
    const vn::graphics::RenderTarget* target     = image.target();
    const int                         attachment = image.attachment();

    // A READBACK ASKED FROM INSIDE A FRAME WOULD WAIT FOR THAT SAME FRAME (see Impl::body_gate): it is refused, not
    // hung. `Failed` says the verb did not run; a hook or a sink calling back into the device is a host protocol
    // violation, not a device fact worth a report on the frame's diagnostic channel.
    if (impl->body_gate.heldByThisThread()) {
        return fail(ReadbackResult::Failed);
    }
    if (!initialized()) {
        return fail(ReadbackResult::Failed);
    }
    if (target == nullptr) {
        return fail(ReadbackResult::Invalid);
    }

    // THE REFUSALS THAT NEED NO WAIT COME FIRST (the reference backend's rule, and the same answers): a target this
    // device never built, an attachment that does not exist, a format the readback contract does not serve. A frame may
    // rebuild the target while this verb waits below, so every answer is checked AGAIN under the gate - this pass only
    // spares the device the stop.
    {
        // A VALUE, NOT THE RECORD (see TargetTable::peek): the record is rebuilt in place and destroyed outside the
        // table, so a field read through a borrowed pointer has no synchronisation to point at (audit A3-01). One
        // snapshot under the table's own lock answers every refusal below, and nothing here keeps a piece of the
        // record across the wait that follows.
        const TargetTable::Peek peek = impl->target_table.peek(target);
        if (!peek.built) {
            return fail(ReadbackResult::NotReady);
        }
        if (attachment < 0 || static_cast<std::size_t>(attachment) >= peek.shape.color_formats.size()) {
            return fail(ReadbackResult::Invalid);
        }
        if (peek.fresh) {
            // BUILT IS NOT DRAWN: the images exist now that `setRenderTarget` builds the announce, but their
            // contents are UNDEFINED until a pass writes them, and "undefined" is not a picture. Asked after the
            // range check, so an index the target does not have is Invalid whether or not anything drew.
            return fail(ReadbackResult::NotReady);
        }
        if (peek.shape.color_formats[static_cast<std::size_t>(attachment)] !=
            vn::graphics::RenderTarget::ColorFormat::RGBA8) {
            return fail(ReadbackResult::Unsupported);
        }
    }

    // THE WAIT THE DEVICE NAMES FOR THE HOST, AND IT IS COUNTED (see Impl::body_gate): with several drivers, "between
    // frames" is not a moment the host can see, so the gate says "no frame body runs while this verb runs" - it waits
    // for the frames in flight, not for a session. THE COMMAND MACHINERY IS THE READBACK'S OWN (see ReadbackStaging):
    // nothing here borrows a driver's command pool, fence or ring.
    const vn::graphics::FrameGate::Exclusive device_lock = impl->body_gate.enterExclusive();
    if (!device_lock) {
        return fail(ReadbackResult::Failed); // the guard above covers this thread; a refused gate is still a refusal
    }
    {
        // ONE COUNTED DEVICE STOP per readback (F7): this is the counter the host reads, and the same counter the
        // reference backend raises with its readback idle (G6). A refusal answered above never reaches this line.
        const std::lock_guard<std::mutex> clock_held(impl->clock_mutex);
        impl->retirement.noteDeviceWait();
    }
    if (!initialized()) {
        return fail(ReadbackResult::Failed); // the device went down while this verb waited for the gate
    }

    // THE AUTHORITATIVE LOOK: the frames that ran while this verb waited may have rebuilt or replaced the target.
    const std::shared_ptr<TargetObjects> entry = impl->target_table.lookup(target);
    if (entry == nullptr || !entry->built()) {
        return fail(ReadbackResult::NotReady);
    }
    TargetObjects& images = *entry;
    if (attachment < 0 || static_cast<std::size_t>(attachment) >= images.colorCount()) {
        return fail(ReadbackResult::Invalid);
    }
    if (images.fresh.load(std::memory_order_relaxed)) {
        return fail(ReadbackResult::NotReady); // built but never drawn (see the pre-gate check's note)
    }
    const std::size_t   attachment_index = static_cast<std::size_t>(attachment);
    const VkImage       color_image      = images.colors[attachment_index];
    if (images.colorFormat(attachment_index) != VK_FORMAT_R8G8B8A8_UNORM) {
        // The readback contract is packed RGBA8; another colour format would need a conversion this step does not write.
        return fail(ReadbackResult::Unsupported);
    }

    // THE COPY MECHANISM IS ReadbackStaging's (its own command pool and fence, the staging buffer, submit, wait,
    // mapping); this function says WHAT to copy and what the bytes mean. Where each picture is stays this backend's
    // knowledge (the per-image `color_layouts` records), recorded below through the staging command buffer.
    const VkDeviceSize bytes = static_cast<VkDeviceSize>(images.width()) * static_cast<VkDeviceSize>(images.height()) * 4U;

    ReadbackStaging staging;
    const StagingRefusal opened =
        staging.open(impl->device_table, impl->device, impl->allocator, impl->queue_family, bytes);
    if (opened == StagingRefusal::Unsupported) {
        return fail(ReadbackResult::Unsupported); // no host-visible coherent memory: this device cannot copy to the host
    }
    if (opened != StagingRefusal::Ok) {
        return fail(ReadbackResult::Failed);
    }

    // RECORD: the image's OWN layout -> transfer source -> copy -> THE SAME LAYOUT AGAIN. A readback is a device verb,
    // not a pass: it reads ONE image and leaves it exactly where it found it, so no record changes and no other
    // attachment of the set moves. The `fresh` check above is what rules out `Undefined` here - a set no pass has drawn
    // into has nothing to read - so the starting layout is the colour attachment layout or the sampled one.
    const ImageLayout colour_now = images.color_layouts[attachment_index];

    const VkImageMemoryBarrier2 to_transfer =
        barrierBetween(color_image, stateOf(colour_now), transferReadState(), VK_IMAGE_ASPECT_COLOR_BIT);
    recordBarrier(impl->device_table, staging.commands(), to_transfer);

    staging.recordCopy(color_image,
                       VK_IMAGE_ASPECT_COLOR_BIT,
                       VkExtent3D{ static_cast<std::uint32_t>(images.width()), static_cast<std::uint32_t>(images.height()), 1U });

    const VkImageMemoryBarrier2 restored =
        barrierBetween(color_image, transferReadState(), stateOf(colour_now), VK_IMAGE_ASPECT_COLOR_BIT);
    recordBarrier(impl->device_table, staging.commands(), restored);

    const StagingRun run = staging.run(impl->queue, impl->queue_submit);
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
    return vn::graphics::ReadbackOutcome{ ReadbackResult::Ok };
}

vn::graphics::ReadbackOutcome Device::readDepthBuffer(const vn::graphics::ImageRef& image, std::vector<float>& outDepths)
{
    const auto fail = [](const ReadbackResult reason) { return vn::graphics::ReadbackOutcome{ reason }; };

    if (image.kind() != vn::graphics::ImageRef::Kind::Depth) {
        return fail(ReadbackResult::Invalid); // the wrong verb for this image
    }
    const vn::graphics::RenderTarget* target = image.target();

    // A READBACK ASKED FROM INSIDE A FRAME WOULD WAIT FOR THAT SAME FRAME (see Impl::body_gate): refused, not hung (the
    // colour readback says the same; `Failed` means the verb did not run).
    if (impl->body_gate.heldByThisThread()) {
        return fail(ReadbackResult::Failed);
    }
    if (!initialized()) {
        return fail(ReadbackResult::Failed);
    }
    if (target == nullptr) {
        return fail(ReadbackResult::Invalid);
    }

    // THE REFUSALS THAT NEED NO WAIT COME FIRST, and every one of them is checked again under the gate below (a frame
    // may rebuild the target while this verb waits). A BORROWED DEPTH IS READ THROUGH THE SOURCE (the SDK's rule, see
    // RenderBackend::readDepthBuffer): the image belongs to the lender, and this target never promised it - so that
    // refusal comes before any format question. D24 is Vulkan's COMBINED depth/stencil format with no plain depth
    // copy, so it is refused rather than decoded into numbers that look like depths and are not (the same table the
    // copy and the decoder use is the SDK's `depthReadbackOf`).
    {
        // THE SAME VALUE-OF-RECORD RULE as readColorBuffer's phase one (see TargetTable::peek), and the depth format
        // is the one the ATTACHMENTS were built with rather than whatever the host object declares now: the hint has
        // to describe the image a copy would read.
        const TargetTable::Peek peek = impl->target_table.peek(target);
        if (!peek.built) {
            return fail(ReadbackResult::NotReady);
        }
        if (target->depthSource() != nullptr) {
            return fail(ReadbackResult::Unsupported);
        }
        if (!peek.shape.depth_format.has_value()) {
            return fail(ReadbackResult::Invalid);
        }
        if (peek.fresh) {
            return fail(ReadbackResult::NotReady); // built but never drawn: the depth is UNDEFINED
        }
        if (!depthReadbackOf(*peek.shape.depth_format).readable) {
            return fail(ReadbackResult::Unsupported);
        }
    }

    // THE WAIT THE DEVICE NAMES FOR THE HOST, AND IT IS COUNTED, exactly as in readColorBuffer: the frames in flight
    // are what the gate waits for, and the command machinery below is the readback's own (see ReadbackStaging).
    const vn::graphics::FrameGate::Exclusive device_lock = impl->body_gate.enterExclusive();
    if (!device_lock) {
        return fail(ReadbackResult::Failed); // the guard above covers this thread; a refused gate is still a refusal
    }
    {
        const std::lock_guard<std::mutex> clock_held(impl->clock_mutex);
        impl->retirement.noteDeviceWait();
    }
    if (!initialized()) {
        return fail(ReadbackResult::Failed); // the device went down while this verb waited for the gate
    }

    // THE AUTHORITATIVE LOOK: the frames that ran while this verb waited may have rebuilt or replaced the target.
    const std::shared_ptr<TargetObjects> entry = impl->target_table.lookup(target);
    if (entry == nullptr || !entry->built()) {
        return fail(ReadbackResult::NotReady);
    }
    if (target->depthSource() != nullptr) {
        return fail(ReadbackResult::Unsupported);
    }
    TargetObjects& images = *entry;
    if (!images.hasDepth()) {
        return fail(ReadbackResult::Invalid); // no depth attachment at all: the request names nothing
    }
    if (images.fresh.load(std::memory_order_relaxed)) {
        return fail(ReadbackResult::NotReady); // built but never drawn (see the pre-gate check's note)
    }

    // ONE TABLE SAYS WHAT A DEPTH READBACK IS - the SDK's, the same one `decodeDepth` reads. The per-texel size below
    // is the table's too, so the copy and the decoder cannot disagree.
    const vn::graphics::ReadbackFormat packed = depthReadbackOf(target->depthFormat());
    if (!packed.readable) {
        return fail(ReadbackResult::Unsupported);
    }

    const VkImage    depth_image = images.depthPicture();
    const VkExtent3D extent{ static_cast<std::uint32_t>(images.width()), static_cast<std::uint32_t>(images.height()), 1U };
    const VkDeviceSize bytes = static_cast<VkDeviceSize>(extent.width) * static_cast<VkDeviceSize>(extent.height) *
                               static_cast<VkDeviceSize>(packed.bytes_per_texel);

    ReadbackStaging staging;
    const StagingRefusal opened =
        staging.open(impl->device_table, impl->device, impl->allocator, impl->queue_family, bytes);
    if (opened == StagingRefusal::Unsupported) {
        return fail(ReadbackResult::Unsupported); // no host-visible coherent memory: this device cannot copy to the host
    }
    if (opened != StagingRefusal::Ok) {
        return fail(ReadbackResult::Failed);
    }

    // RECORD: the PICTURE's own layout -> transfer source -> copy -> THE SAME LAYOUT AGAIN. Like the colour readback this
    // is not a pass: it reads one image and leaves it where it found it, so the record does not change and the sample
    // buffer of a multisampled target is never named. `fresh` rules out `Undefined`, so the starting layout is the depth
    // attachment layout or the sampled one a shadow pass left behind.
    const ImageLayout picture_now = images.depthPictureLayout();

    // A BARRIER NAMES BOTH HALVES of a combined depth/stencil image (see depthAspectMask); the COPY names only the
    // aspect it reads, because a depth readback is the depth half - the stencil half is not a depth and nothing here
    // decodes it (which is also why a combined format is refused above: this backend serves the formats the SDK's
    // table can decode, and that table has no combined entry).
    const VkImageMemoryBarrier2 to_transfer =
        barrierBetween(depth_image, stateOf(picture_now), transferReadState(), depthAspectMask(images.depthFormat()));
    recordBarrier(impl->device_table, staging.commands(), to_transfer);

    staging.recordCopy(depth_image, VK_IMAGE_ASPECT_DEPTH_BIT, extent);

    const VkImageMemoryBarrier2 restored =
        barrierBetween(depth_image, transferReadState(), stateOf(picture_now), depthAspectMask(images.depthFormat()));
    recordBarrier(impl->device_table, staging.commands(), restored);

    const StagingRun run = staging.run(impl->queue, impl->queue_submit);
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
    return vn::graphics::ReadbackOutcome{ ReadbackResult::Ok };
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
    // A HOST VERB OWNS THE GATE (see Impl::body_gate). Called from a frame body's thread it answers zero rather than
    // waiting for itself: no pipeline count is a better answer than a hung sink (see host_reentry_report).
    const vn::graphics::FrameGate::Exclusive device_lock = impl->body_gate.enterExclusive();
    if (!device_lock) {
        impl->report_host_reentry(u8"contentPipelines()");
        return 0U;
    }
    return impl->caches.pipelinesCreated();
}

std::uint64_t Device::frameHeadStorageGrows() const noexcept
{
    return impl->session != nullptr ? impl->session->frame_head.storageGrows() : 0U;
}

Device::ContentCacheCounters Device::contentCacheCounters() const noexcept
{
    // A HOST VERB OWNS THE GATE (see Impl::body_gate). Called from a frame body's thread it answers an empty set of
    // counters rather than waiting for itself (see host_reentry_report).
    const vn::graphics::FrameGate::Exclusive device_lock = impl->body_gate.enterExclusive();
    if (!device_lock) {
        impl->report_host_reentry(u8"contentCacheCounters()");
        return ContentCacheCounters{};
    }
    // TWO OWNERS, ONE ANSWER: the ring and the descriptor sets belong to the device's own driver (a second driver would
    // have its own), while the caches answer the rest. `Device` reports its own session's frame scratch, which is the
    // same answer a single-session tree has always given.
    return ContentCacheCounters{ impl->session->scratch.ringBuilds(),
                                 impl->session->scratch.setBuilds(),
                                 impl->session->scratch.setsLive(),
                                 impl->caches.lockAcquisitions(),
                                 impl->caches.lockWaits() };
}

MemoryStats Device::memoryStats() const noexcept
{
    // A HOST VERB OWNS THE GATE (see Impl::body_gate). Called from a frame body's thread it answers empty stats rather
    // than waiting for itself (see host_reentry_report).
    const vn::graphics::FrameGate::Exclusive device_lock = impl->body_gate.enterExclusive();
    if (!device_lock) {
        impl->report_host_reentry(u8"memoryStats()");
        return MemoryStats{};
    }
    return impl->allocator.stats();
}

TargetTable& Device::targetTable() noexcept
{
    return impl->target_table;
}

const TargetTable& Device::targetTable() const noexcept
{
    return impl->target_table;
}

ResidencyClaims& Device::residence() noexcept
{
    return impl->caches.residence();
}

const ResidencyClaims& Device::residence() const noexcept
{
    return impl->caches.residence();
}

Device::RetirementNumbers Device::retirementNumbers() const noexcept
{
    // A HOST VERB OWNS THE GATE (see Impl::body_gate). Called from a frame body's thread it answers zeros rather than
    // waiting for itself (see host_reentry_report).
    const vn::graphics::FrameGate::Exclusive device_lock = impl->body_gate.enterExclusive();
    if (!device_lock) {
        impl->report_host_reentry(u8"retirementNumbers()");
        return RetirementNumbers{};
    }
    // AND THE QUEUE'S OWN LOCK: a release no longer waits for the frames (R6), so a park may be appending while this
    // verb reads the numbers.
    const std::lock_guard<std::mutex> clock_held(impl->clock_mutex);
    RetirementNumbers numbers;
    numbers.pending      = impl->retirement.pending();
    numbers.released     = impl->retirement.released();
    numbers.device_waits = impl->retirement.deviceWaits();
    return numbers;
}

Device::PipelineCacheReport Device::pipelineCacheReport() const noexcept
{
    // A HOST VERB OWNS THE GATE (see Impl::body_gate). Called from a frame body's thread it answers an empty report
    // rather than waiting for itself (see host_reentry_report).
    const vn::graphics::FrameGate::Exclusive device_lock = impl->body_gate.enterExclusive();
    if (!device_lock) {
        impl->report_host_reentry(u8"pipelineCacheReport()");
        return PipelineCacheReport{};
    }
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
        report.cache_hits = impl->caches.pipelineCacheHits();
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
    const std::lock_guard<std::recursive_mutex> report_held(impl->report_mutex);
    reportDiagnostic(deviceDiagnostic(severity, DiagnosticCategory::InitFailed, u8"vk.driver.message", messageOf(text)));
}

VN_BACKEND_VK_NS_END
