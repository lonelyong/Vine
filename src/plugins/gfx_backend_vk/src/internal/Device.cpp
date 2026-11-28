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
#include <vine/backend_vk/internal/DeviceChoice.hpp>
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

using vn::backend_vk::ImageLayout;
using vn::graphics::Capabilities;
using vn::graphics::colorBytesPerTexel;
using vn::graphics::decodeDepth;
using vn::graphics::depthBytesPerTexel;
using vn::graphics::DeviceDesc;
using vn::graphics::DevicePresentation;
using vn::graphics::DiagnosticCategory;
using vn::graphics::DiagnosticSeverity;
using vn::graphics::DrawPacket;
using vn::graphics::FrameDescription;
using vn::graphics::ImageSource;
using vn::graphics::PassDraw;
using vn::graphics::planTarget;
using vn::graphics::PresentOutcome;
using vn::graphics::PresentTarget;
using vn::graphics::ReadbackStatus;
using vn::graphics::RenderTarget;
using vn::graphics::RepairReason;
using vn::graphics::SurfaceTargetResult;
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
 * - SUBMITTING IS NOT COMPLETING. The success path reports completion separately, after the fence (see the callers),
 *   and it reports it to the clock as a SUBMISSION's proof (`DeviceClock::noteCompletion`), not as a frame number:
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
        retirement.advance(clock);
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
    /// What the last frame cost on the device (see FrameTiming): DISENGAGED until a frame's own fence made a sampled
    /// pair readable, so a frame that did not run (or was not sampled) answers "not measured", never a stale number.
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
 * @brief ONE FRAME'S OWN STATE, kept by its driver: what it claimed, which records it moved, and what it must undo.
 *
 * @details
 * - WHY THE DRIVER OWNS IT (`G1` / `G6`). Every member is a fact about ONE frame's recording, and the phases of the
 *   frame body are steps that read and write the same value - so it is one value they share rather than a dozen locals
 *   threaded between them (`G3`: the flow reads in one pass because its state is named in one place).
 * - WHAT DOES NOT BELONG HERE: the passes and their resolved plans (see FrameHead), the constants ring and the
 *   descriptor pool (see FrameScratch), the report (see FrameBooks). This is only what the frame body itself decides
 *   and must be able to UNDO when its commands never reached the queue (see `rewindUnrunFrame`).
 * - THE READ SET IS NOT HERE EITHER (`G1`): each `FrameHead::Pass` keeps the record its full-screen packets sample and
 *   the one its shadow map comes from, resolved at the claim (see `resolveReadSet`). A second list beside those slots
 *   would be the same fact in two places, and the release window (R6: a host may release at any moment) is closed by
 *   the slot that already shares ownership of what it draws into.
 * - `clear()` KEEPS THE CAPACITY, so a steady frame stops allocating these - the promise this storage exists for.
 */
struct FrameRecording {
    /// The presenter this frame draws into (resolved from `present_to`), or null.
    WindowTarget* window{ nullptr };
    /// Whether the window's image is STILL HELD, and therefore must be given back on every path that does not present.
    bool window_acquired{ false };
    /// Whether this frame's window pass found no image (`acquire` failed once): a later window pass in the same frame
    /// neither asks again (the rebuild it would start is the one that just failed) nor draws.
    bool window_refused{ false };
    /// Whether this frame OWES the surface a picture: set with the acquire (which is the promise a present keeps), and
    /// NOT cleared by the give-back - see `giveBackWindowImage` and `unrunOutcome`.
    bool picture_owed{ false };
    /// A pass lost its target claim this frame (see TargetTable::Lease): the conflict episode stays open.
    bool claim_refused{ false };
    /// A pass was left out for a reason that has already been reported (the "nothing I can run" sentence would repeat it).
    bool refused_pass{ false };
    /// Whether a pass has already drawn into the acquired image (the first one may clear what it finds there).
    bool window_written{ false };
    /// Whether a draw this frame named could not be recorded (the refused-content episode stays open).
    bool content_refused{ false };
    /// What the frame states it ran (see FrameCounters): counted from the frame's SHAPE, not from the commands.
    vn::graphics::FrameCounters counters{};
    /// The depths THIS frame lends (`RenderTarget::shareDepth`), so the map a pass samples skips them. A frame lends a
    /// handful at most, so this is a small vector scanned linearly - an `unordered_set` would allocate a node per entry
    /// on the recording path, which is exactly what this value exists to stop.
    std::vector<const RenderTarget*> lent_depths{};
    /// The layout records this frame moved on ANOTHER target, with the value each stated before (see MovedLayout).
    std::vector<MovedLayout> moved_layouts{};
    /// The clock's sample buffer (see FrameTiming): resized per sampled frame, never rebuilt.
    std::vector<std::uint64_t> stamp_values{};
    /// Whether this frame's GPU time is sampled at all (see ensureTimestampQueries).
    bool per_pass_clock{ false };
    /// The clock's queries for this frame: the frame's pair plus one boundary per pass.
    std::uint32_t query_count{ 0 };
    /// Whether the frame's debug-utils region is open (and must be closed before the command buffer ends).
    bool frame_labelled{ false };

    /**
     * @brief Drops the layout log this frame wrote.
     *
     * @details
     * - The log exists so a frame that never reached the queue can put every record back (`rewindUnrunFrame`); once
     *   the frame is over it is stale. The records it NAMES are held by the pass slots, not by this log, so the hold
     *   ends with `FrameHead::clear`.
     */
    void clearMovedLayouts() noexcept
    {
        moved_layouts.clear();
    }

    /**
     * @brief Gives the presentation engine's image back without presenting it (see WindowTarget::abandonFrame).
     *
     * @details
     * - THE ACQUIRE IS A PAIR OR IT IS A LEAK: `vkAcquireNextImageKHR` took an image and signalled its semaphore, and
     *   only a present hands it back - so every path that will not present calls this ONE act.
     * - IT CLEARS THE HOLD, NOT THE PROMISE: `window_acquired` answers "the image is still owed back", `picture_owed`
     *   answers "this frame promised the surface a picture". The frame's answer is `unrunOutcome`, which reads the
     *   SECOND - so a give-back that erased both would make the one answer a host must act on unreachable on exactly
     *   the paths it exists for (review 19 `Z19`).
     */
    void giveBackWindowImage() noexcept
    {
        if (window_acquired && window != nullptr) {
            window->abandonFrame();
            window_acquired = false;
        }
    }

    /**
     * @brief Answers a frame that will not present: `Lost` when it owed the surface a picture, `Skipped` when it never
     *        had one to deliver.
     *
     * @details
     * - ONE DECISION, EVERY EXIT THAT DOES NOT PRESENT (see `Device::FrameDriver::executeFrame`): the buffer that will
     *   not begin, the buffer that will not end, and the submission the fence never confirmed. Each calls this, so
     *   "what does a frame that did not run answer" is stated once instead of being re-derived per failure path.
     * - `Skipped` is `PresentOutcome`'s "there was nothing to present": a frame with no window at all (an off-screen
     *   target), and one whose walk claimed no pass before it ever reached the queue (the walk withdraws the promise
     *   itself - a rebuild cannot fix a content refusal, so it must not be asked for).
     *
     * @return #PresentOutcome::Lost or #PresentOutcome::Skipped, per the frame's own promise.
     */
    [[nodiscard]] vn::graphics::PresentOutcome unrunOutcome() const noexcept
    {
        return picture_owed ? vn::graphics::PresentOutcome::Lost : vn::graphics::PresentOutcome::Skipped;
    }

    /** @brief Forgets the last frame's entries and keeps their capacity. */
    void clear() noexcept
    {
        window           = nullptr;
        window_acquired  = false;
        window_refused   = false;
        picture_owed     = false;
        claim_refused    = false;
        refused_pass     = false;
        window_written   = false;
        content_refused  = false;
        per_pass_clock   = false;
        query_count      = 0U;
        frame_labelled   = false;
        counters         = {};
        lent_depths.clear();
        moved_layouts.clear();
        stamp_values.clear();
    }
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
        Ok,            ///< Both objects exist (or already existed).
        NoDevice,      ///< There is no Vulkan device to create them on.
        CommandPool,   ///< `vkCreateCommandPool` refused.
        CommandBuffer, ///< `vkAllocateCommandBuffers` refused.
        Fence,         ///< `vkCreateFence` refused.
    };

  public:
    explicit FrameDriver(Impl& owner_device) noexcept;

    FrameDriver(const FrameDriver&)            = delete;
    FrameDriver& operator=(const FrameDriver&) = delete;

  public:
    /// @brief What one pass walk is FOR (see claimPasses): a recorded frame, or a warm-up's build.
    enum class PassMode : std::uint8_t
    {
        Record,  ///< `executeFrame`: acquire the window's image, count what runs, open a rendering instance per pass.
        Prepare, ///< `prepareFrame`: build what a later frame needs, acquiring and presenting nothing.
    };

    /// @brief What a pass walk answered (see claimPasses).
    enum class PassWalk : std::uint8_t
    {
        Ready,      ///< At least one pass was claimed (see FrameHead::passes); the window's image, if any, is held.
        Nothing,    ///< No pass this device can run; an acquired image has already been given back.
        DeviceLost, ///< The device was lost while acquiring the window's image: the frame is over.
    };

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
     * - ONE BATCH PER DRIVER (`C2`): a frame and its warm-up record their copies into the same book and wait on the
     *   same fence, so the batch whose completion publishes them is the driver's.
     *
     * @return `clock_window + 1`, so a driver's batch is never 0 ("nobody") and two drivers never share one.
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
    /**
     * @brief Claims the passes this frame will run, and states WHAT EACH ONE DRAWS INTO.
     *
     * @details
     * - ONE WALKER FOR BOTH FRAME VERBS (`G1` / `G5`). "Which passes can this device run" and "which claims does each
     *   pass take" used to be answered twice - once for a recorded frame, once for a warm-up - and the warm-up's copy
     *   had already lost three of the four claims (the depth LENDER, a full-screen source, a declared input). A pass
     *   walk is one rule, so it is one function, and @p mode states only what the WALKER does with its answer.
     * - WHAT IT CLAIMS, AND WHY EACH ONE. The target a pass draws into (rebuilding a target is the claim holder's verb);
     *   every target it SAMPLES (a full-screen source, a declared input - the sampler takes the same lease the drawer
     *   does); and the LENDER of a borrowed depth (two drivers writing one image is what a claim prevents, and the
     *   lender's ONE layout record would otherwise be written from two threads). A lost claim skips THAT pass, reports
     *   once per episode, and leaves the rest of the frame running.
     * - THE WINDOW'S IMAGE IS ACQUIRED HERE, in @ref PassMode::Record only, and the walk GIVES IT BACK on every answer
     *   that is not @ref PassWalk::Ready: an acquire is a pair with a present, and a walk that claimed no pass has
     *   nothing that would present it.
     * - IT WRITES THE FRAME FACTS INTO @p head (`FrameHead::Pass`: the entry, the draw, the pass target) and counts what
     *   the frame states it ran (`FrameCounters`, from the frame's SHAPE - a pass with no effect is not a scope).
     *
     * @param frame      The frame whose passes to claim (borrowed).
     * @param window     The presenter this device handed out (see resolvePresenter), or null for no window.
     * @param mode       Whether this walk is for a recorded frame or a warm-up.
     * @param lease      This frame's target claims (the caller owns it and its release point).
     * @param head       Receives one slot per runnable pass, in the frame's order.
     * @param recording  Receives the frame's own state (window, counters, refusals).
     * @return @ref PassWalk::Ready with at least one pass claimed, @ref PassWalk::Nothing, or @ref PassWalk::DeviceLost.
     */
    [[nodiscard]] PassWalk claimPasses(const vn::graphics::FrameDescription& frame,
                                       WindowTarget* const                   window,
                                       PassMode                              mode,
                                       TargetTable::Lease&                   lease,
                                       FrameHead&                            head,
                                       FrameRecording&                       recording) noexcept;

    /**
     * @brief Resolves the frame's READ SET - each full-screen packet's source record and each pass' shadow map - into
     *        the claimed pass slots.
     *
     * @details
     * - ONE RESOLUTION PER FRAME, WITH THE WRITE SET (`K7`, review 9). `claimPasses` already CLAIMS what a pass reads
     *   (a full-screen source, a declared input), so another driver cannot draw into it meanwhile; what it did not do is
     *   KEEP the record, so the record loop asked the target map again - per pass and per source, under the map's lock,
     *   on the one path the head/record split exists to keep lock-free - and a release landing between the claim and
     *   that second call took the picture away from a frame that had already claimed it.
     * - IT RUNS AFTER THE WHOLE WALK, not inside it, because two facts are frame-wide: the record an earlier pass
     *   samples may be the one a LATER pass' `ensure` built (a ping-pong pass reads last frame's picture), and a depth a
     *   LATER pass lends decides whether an EARLIER pass' declared input is a sampleable map (`lent_depths`).
     * - THE PASS SLOT OWNS WHAT IT RESOLVED (`FrameHead::Pass::sources` / `shadow_map`), so the park keeps the record
     *   alive until the frame ends and the record loop reads values instead of the table.
     *
     * @param head      The claimed passes (their read slots are filled).
     * @param recording The frame's state: the lends are collected here first, and the read set is resolved against them.
     */
    void resolveReadSet(FrameHead& head, FrameRecording& recording) noexcept;

    /**
     * @brief Resolves @p present_to to a window THIS device handed out, or null.
     *
     * @details
     * - WHAT IT ANSWERS: the target whose acquired image a pass that names no target draws into. A frame that names no
     *   presenter presents nowhere (a first-class state: an offline bake); one that names a target this device never
     *   handed out is a wiring defect each verb reports its own way.
     * - THE SURFACE LIST IS PROTECTED BY THE BODY GATE (see Impl): every caller of this holds at least a SHARED lease,
     *   and every writer holds it EXCLUSIVE.
     *
     * @param present_to The target the frame named, or null.
     * @return The window, or null when the frame names none or a foreign one.
     */
    [[nodiscard]] WindowTarget* resolvePresenter(vn::graphics::PresentTarget* present_to) const noexcept;

    /**
     * @brief Resolves every claimed pass' plans and texture uploads, under ONE hold of the cache lock.
     *
     * @details
     * - THE ONE RESOLUTION POINT (`R23`): a frame and a warm-up ask the caches the same question through this call, so
     *   a warm-up cannot build something the frame would not ask for - and a texture a warm-up uploaded is found
     *   resident by the frame that follows.
     * - EVERY UPLOAD IS RECORDED HERE, into this driver's command buffer, BEFORE any rendering instance begins (see
     *   C5): a buffer-to-image copy and its layout transition are not legal inside one. The warm-up's batch buffer is
     *   the same buffer, which is why it must be begun before this call.
     * - THE DEFAULT PROGRAM IS A VALUE FOR ONE RESOLUTION (see program_mutex): a host may announce another one while
     *   this runs, and a plan that read the field twice could compile two variants of one answer.
     *
     * @param head         The claimed passes (their plans are filled) and the frame-level texture facts.
     * @param frame_builds This frame's geometry admissions (see ContentCaches::FrameBuilds).
     */
    void resolveFrameHead(FrameHead& head, ContentCaches::FrameBuilds& frame_builds) noexcept;

  public:
    VkCommandPool   command_pool{ VK_NULL_HANDLE };
    ///
    /// THE FRAME'S COMMAND BUFFER, and therefore the driver's: a frame BEGINS it (the pool resets it), records into it
    /// and submits it - one buffer per driver, one frame at a time, exactly like the fence and the query pool beside it
    /// (`G1`: the frame machinery is one object's). Allocating one per frame was a driver call pair per frame and a
    /// failure branch a host could do nothing about.
    VkCommandBuffer commands{ VK_NULL_HANDLE };
    VkFence       completion{ VK_NULL_HANDLE };
    /// The GPU clock's query pool for THIS driver's frames (see ensureTimestampQueries).
    VkQueryPool   timestamp_pool{ VK_NULL_HANDLE };
    bool          timestamps{ false };
    std::uint64_t clock_frames{ 0 }; ///< Frames this driver has begun (the sampling counter).
    std::uint32_t timestamp_queries{ 0 };
    ///
    /// THE BOOKS THIS FRAME IS WRITING, owned by the thread that runs the frame (see FrameBooks): the frame body
    /// increments them as it goes - a submit, a fence, a present, a clock read - and `closeFrameBooks` publishes the
    /// whole value as the frame's answer. A host never reads this field.
    FrameBooks books{};
    ///
    /// THE FRAME'S ANSWER TO "HOW DID IT GO", as the host reads it (see frameReport): the last CLOSED frame's books,
    /// copied out under `books_mutex` by `closeFrameBooks`. It exists because the alternative is what this backend did
    /// before - a host reading the live `books` while the frame thread incremented them - and "read it only after
    /// waitForFrame" was a rule every host had to know, with nothing in the interface saying so (`G7`: the frame's
    /// numbers are the frame's answer, so they are published with it, exactly like PresentRecord and the pass costs).
    FrameBooks answered_books{};
    /// Guards `answered_books`; the frame thread writes it once per frame, any host thread reads it.
    mutable std::mutex books_mutex{};
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
    /// Debug-utils regions this driver opened, cumulative (see Device::recordingNumbers).
    std::uint64_t debug_labels{ 0U };
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
    ///
    /// WHAT THIS FRAME TRACKS ABOUT ITS OWN RECORDING (see FrameRecording): what it claimed, what it moved, what it
    /// lends and the clock's sample buffer - cleared per frame, kept in capacity.
    FrameRecording recording{};
    ///
    /// WHAT THE RECORDING PATH SPENT, cumulative: the dependencies it stated and the barriers inside them (see
    /// `RecordingTally`). The witness for "a pass' entry is ONE dependency" (see Device::recordingNumbers).
    RecordingTally barrier_tally{};
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
    /// EVERY access to `surfaces` happens under the DEVICE'S BODY GATE, which is where the list is protected: the
    /// writers (initialize, createSurfaceTarget, shutdown) hold it EXCLUSIVE and the readers (a window frame, a warm-up)
    /// hold it SHARED, so they cannot overlap. A second lock over one of those readers would protect nothing and would
    /// be the only place that got it right - which is how a reader that forgot it looks correct.
    DeviceDesc      desc{};
    Capabilities    caps{};

    VkInstance               instance{ VK_NULL_HANDLE };
    VkDebugUtilsMessengerEXT messenger{ VK_NULL_HANDLE };
    VkPhysicalDevice         physical{ VK_NULL_HANDLE };
    VkDevice                 device{ VK_NULL_HANDLE };
    VkQueue                  queue{ VK_NULL_HANDLE };
    std::uint32_t            queue_family{ 0U };

    bool validation{ false };
    ///
    /// THE DEBUG-UTILS ENTRY POINTS this session could resolve (see Device::debugMarkers): null when the loader has no
    /// `VK_EXT_debug_utils`, which is the only case in which the frame path names nothing to a capture tool. They are the
    /// SAME extension the validation messenger uses, so a session that reports is exactly a session that can label.
    PFN_vkCmdBeginDebugUtilsLabelEXT debug_label_begin{ nullptr };
    PFN_vkCmdEndDebugUtilsLabelEXT   debug_label_end{ nullptr };
    PFN_vkSetDebugUtilsObjectNameEXT debug_object_name{ nullptr };
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
        /// A window pass named THIS device's presenter, but the window cannot present right now (see
        /// WindowTarget::presentationState). Its own code, because the repair is the window's and the sentence used to
        /// accuse the host of a foreign presenter (review 19 `Z14`).
        vn::graphics::ReportOnce not_ready;
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
            not_ready.rearm();
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
     * @brief The device's submission clock and its park queue: WHEN an object a host released may really be destroyed.
     *
     * @details
     * - ONE CLOCK, ONE PARK QUEUE, like the reference backend: whether a buffer or an image may be destroyed is a
     *   question about the WHOLE device, because every frame reads the same objects - so the answer cannot live in a
     *   session.
     * - THE CLOCK IS THE DEVICE'S, NOT A SESSION'S: every submit takes the next number from it (inside the queue's
     *   external-synchronization lock, so the numbers follow the queue's own order) and reports that number's proof
     *   after the fence. Nothing here is per-driver, which is what makes an idle driver harmless (see DeviceClock).
     * - Frames in flight on this device today: one, because `executeFrame` submits and waits inside the call. The
     *   park's window is derived from that number (see `retirement.setSlots`).
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
     * @brief Runs @p destroy once NO frame may name the object: every submitted command buffer that named it has
     *        completed, and no value this device lent still holds it.
     *
     * @details
     * - TWO WINDOWS, ONE DECISION (ADR-2026-11-24-residency-borrowing). The park answers "has the work that named it
     *   completed" (the device's submission watermark); @p named answers "does a value still name it" - a pass handle,
     *   a record a frame reads, an entry a frame resolved, a host verb's own share. The second window is real and not
     *   academic: OFF-SCREEN frame bodies overlap (see `executeFrame`), so one engine's frame can complete - and run
     *   this - while another is still recording.
     * - THE PARK'S OWN SHARE IS NOT A HOLDER. The queued act holds the object, so it sees "one reference" when nobody
     *   but the park holds it, and it asks again at a later frame end when it sees more - there is no spin, because
     *   the only owners are frame bodies and host verbs, and every frame end is another chance to ask.
     * - WITHOUT EVIDENCE IT IDLES: a release with no armed queue has no parking window it could trust, so any body
     *   still recording is waited out (the body gate) and the device is idled - and the idle is COUNTED. A queue that
     *   is armed never takes this path.
     *
     * @param named   A weak view of the object; empty means "the park's own share is the last one by construction".
     * @param destroy What to run when the object may go (it keeps the object alive for the parked act).
     */
    void releaseLater(std::function<void()> destroy)
    {
        releaseLater(std::weak_ptr<const void>{}, std::move(destroy));
    }

    void releaseLater(std::weak_ptr<const void> named, std::function<void()> destroy)
    {
        // THE QUEUE IS THE EVIDENCE: armed (a known slot count) means it can date a park, and it needs no driver to
        // exist - an idle device still proves its own last submission (see DeviceClock). The act is handed over BY
        // COPY, because a queue that cannot park must not consume what the fallback below still has to run.
        if (retirement.parkingAvailable()) {
            const std::lock_guard<std::mutex> held(clock_mutex);
            if (parkRelease(named, destroy, clock)) {
                return;
            }
        }
        // NO EVIDENCE, SO NO PARKING: the honest answer to "we cannot prove it" is to stop the device (and count it),
        // never to guess a window that would destroy an object a submitted command buffer still names. The idle is a
        // host call on every queue, so it takes the queue's external-synchronization lock (see queue_submit): a frame
        // on another thread may be inside its submit right now (a release runs beside frames, R6). ANY BODY STILL
        // RECORDING IS WAITED OUT FIRST: a body holds a frame window from its first line, so "no window" already
        // means "no body" - the gate is how this device SAYS that, and a refusal can only mean this thread is inside
        // a body, which is the contradiction just ruled out.
        const vn::graphics::FrameGate::Exclusive bodies = body_gate.enterExclusive();
        (void)bodies;
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

    /**
     * @brief Parks @p destroy behind both windows (see releaseLater); called with the clock lock held.
     *
     * @param named    A weak view of the object, or empty.
     * @param destroy  The release act (keeps the object alive while it is parked).
     * @param when     The device clock this window is dated against.
     * @return true when the act was parked; false when the queue cannot park at all (see parkingAvailable).
     */
    [[nodiscard]] bool parkRelease(std::weak_ptr<const void> named, std::function<void()> destroy, const vn::graphics::DeviceClock& when)
    {
        return retirement.retire(when, [this, named, destroy](const vn::graphics::DeviceClock* at) mutable {
            if (at != nullptr && named.use_count() > 1) {
                // STILL NAMED BY A VALUE THIS DEVICE LENT (a pass handle, a frame's read, an entry a frame resolved,
                // a host verb's share): another turn. The park keeps its own share, so the object cannot die before
                // the next turn.
                (void)this->parkRelease(named, destroy, *at);
                return;
            }
            destroy();
        });
    }

    /// True once a queue/acquire/present call answered `VK_ERROR_DEVICE_LOST`: the session is over (see
    /// Device::noteDeviceLost), so `initialized()` stops answering true and the host rebuilds it. ATOMIC because a
    /// host reads it while another session's frame is the thread that writes it.
    std::atomic<bool> device_lost{ false };
    ///
    /// WHETHER THE NEXT SUBMISSION ANSWERS A PLAIN FAILURE (see `Device::refuseNextSubmit`): the reaching-into-a-driver
    /// door exists because "the queue refused it" is otherwise unreachable, and the contracts that follow from it are
    /// the ones a failed frame owes a host.
    std::atomic<bool> refuse_submit{ false };

    /** @brief Consumes a pending `refuseNextSubmit` (one submission only). */
    [[nodiscard]] bool takeSubmitRefusal() noexcept
    {
        return refuse_submit.exchange(false, std::memory_order_relaxed);
    }
    /// One report for the loss itself, whatever number of calls saw it afterwards (the session is the episode).
    vn::graphics::ReportOnce device_lost_report{};
    /// Guards the loss episode: two frames may both see the loss on two threads (S6c-2); the atomic bool says WHETHER,
    /// this says WHO reports it.
    mutable std::mutex loss_mutex{};

    /// How many nodes a release announcement let go, cumulative (a frame's own number is 0 - see FrameUploads).
    /// ATOMIC because two host threads may release at the same time now that releases do not take the body gate.
    std::atomic<std::uint64_t> released_nodes{ 0 };

    /**
     * @brief Opens a named region on @p commands, when this session resolved the extension (see `Device::debugMarkers`).
     *
     * @details
     * - WHAT IT IS FOR: a capture tool builds its pass tree out of these labels; without them the frame is one anonymous
     *   command stream and every conclusion a host reads from `FrameReport` has to be matched to commands by hand. The
     *   names are the frame's own (`RenderPass::name`), so the tool's tree and the host's report agree.
     * - NOTHING HAPPENS WITHOUT THE EXTENSION: no label, no count, and the caller's cost is one comparison.
     *
     * @param commands The command buffer being recorded.
     * @param name     The region's name (null matches nothing).
     * @param color    The region's colour, RGBA in [0, 1].
     * @return true when a label was opened, so the caller closes it (and counts it).
     */
    bool beginLabel(const VkCommandBuffer commands, const char8_t* const name, const float (&color)[4]) const noexcept
    {
        if (debug_label_begin == nullptr || commands == VK_NULL_HANDLE || name == nullptr) {
            return false;
        }
        VkDebugUtilsLabelEXT label{};
        label.sType      = VK_STRUCTURE_TYPE_DEBUG_UTILS_LABEL_EXT;
        label.pLabelName = reinterpret_cast<const char*>(name);
        for (int index = 0; index < 4; ++index) {
            label.color[index] = color[index];
        }
        debug_label_begin(commands, &label);
        return true;
    }

    /** @brief Closes the region @ref beginLabel opened (a no-op without the extension). */
    void endLabel(const VkCommandBuffer commands) const noexcept
    {
        if (debug_label_end != nullptr && commands != VK_NULL_HANDLE) {
            debug_label_end(commands);
        }
    }

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
     * @brief Resolves the SHADOW MAP a pass samples out of what it declared - the facts only, no commands.
     *
     * @details
     * - WHICH INPUT: the first one whose TARGET says it is a shadow map (`RenderTarget::shadowOf`), whose depth this
     *   device actually built and whose host left promotable, and whose producer published how to read it
     *   (`hasProducerViewProjection`) - that is the SDK's rule, restated here because this backend consumes
     *   the frame value directly. A G-buffer's depth is not a shadow: a
     *   target nobody stated a light for is skipped, which is the defect the rule exists to prevent.
     * - WHY IT TAKES NO COMMANDS (`K7`, review 9): this is the RESOLUTION - which record, which view, whose light, which
     *   matrix - and it runs where the frame CLAIMS its passes (see `FrameDriver::claimPasses`), with the frame's lends
     *   in hand and under the target map's own lock. The transition the picture needs IS a command, so it stays in the
     *   record loop, where the pass slot already owns the record and no lock is taken.
     * - IT ANSWERS THE RECORD TOO: the pass slot keeps the map alive, so a release announced after the claim parks the
     *   map instead of taking it away from a frame that is still going to sample it (R6).
     *
     * @param inputs The pass' resolved inputs, in declaration order.
     * @param lent   The depths THIS frame lends to another target (see RenderTarget::shareDepth); such an image cannot
     *               be a sampled picture and a pass' depth attachment at once, so it is skipped. A small span, scanned
     *               linearly: a frame lends a handful at most, and the claim path allocates nothing.
     * @param record Receives the record the map is read from; cleared when the pass samples no readable map.
     * @param out    Receives the binding; empty (`view == VK_NULL_HANDLE`) when the pass samples no readable map.
     * @return true when a map was resolved.
     */
    [[nodiscard]] bool resolveShadowSource(std::span<const ImageSource>    inputs,
                                           std::span<const RenderTarget*>  lent,
                                           std::shared_ptr<TargetObjects>& record,
                                           ShadowSource&                   out) const
    {
        record.reset();
        out = ShadowSource{};
        for (const ImageSource& input : inputs) {
            if (input.target == nullptr || !input.isProduced()) {
                continue;
            }
            const RenderTarget& map = *input.target;
            const std::shared_ptr<TargetObjects> map_entry = target_table.lookup(&map);
            if (map_entry == nullptr || !map_entry->built() || !map_entry->hasDepth()) {
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
            if (std::find(lent.begin(), lent.end(), &map) != lent.end()) {
                // LENDING REVOKES SAMPLING: another target in this frame draws against this map's depth, so the image
                // has to stay a depth attachment - a shader cannot sample it and a pass cannot attach it in the same
                // frame (see RenderTarget::shareDepth). Skipped rather than transitioned, because the transition the
                // borrower's pass depends on is the one already in effect.
                continue;
            }
            // A MAP THAT BORROWS ITS DEPTH OWNS NO PICTURE, so there is nothing for a shader to read: sampling it would
            // bind a null view with the switch on. The borrower's depth belongs to its lender, which is what the pass
            // that SAMPLES would have to name instead.
            const VkImageView picture = map_entry->depthPictureView();
            if (picture == VK_NULL_HANDLE) {
                continue;
            }

            out.view                = picture;
            out.has_view_projection = true;
            out.view_projection     = map.producerViewProjection();
            out.light               = light;
            record                  = map_entry;
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
    // THE FRAME'S COMMAND BUFFER IS PART OF THIS MACHINERY, not of a frame: the pool carries
    // `VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT`, so beginning it per frame resets what the last one recorded.
    if (commands == VK_NULL_HANDLE) {
        VkCommandBufferAllocateInfo info{};
        info.sType              = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
        info.commandPool        = command_pool;
        info.level              = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        info.commandBufferCount = 1U;
        if (owner.device_table.vkAllocateCommandBuffers(owner.device, &info, &commands) != VK_SUCCESS) {
            commands = VK_NULL_HANDLE;
            return CreateRefusal::CommandBuffer;
        }
    }
    // THE CLOCK WINDOW: the identity this driver keys its own tables by (see DeviceClock). It is NOT completion
    // evidence - the device's watermark counts SUBMISSIONS, so a driver that stops framing stalls nothing.
    if (!clock_window_live) {
        const std::lock_guard<std::mutex> clock_held(owner.clock_mutex);
        clock_window      = owner.clock.addDriver();
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
                          owner.caches.fullscreenSetLayout());
}

void Device::FrameDriver::destroy() noexcept
{
    // THE SCRATCH GOES FIRST of this driver's objects: its descriptor pool holds the cached sets, and the device's
    // layouts (which the sets were allocated against) outlive it - `Device::shutdown` destroys the scratch before the
    // caches, and this call is idempotent for the paths that reach a destroy without a shutdown.
    scratch.destroy();
    // THE IDENTITY LEAVES FIRST: a device clock that still lists this driver lets it key a table nothing will read
    // again; the completion watermark never consulted it, so nothing about released objects changes here.
    if (clock_window_live) {
        const std::lock_guard<std::mutex> clock_held(owner.clock_mutex);
        owner.clock.removeDriver(clock_window);
        clock_window_live = false;
    }
    if (owner.device != VK_NULL_HANDLE) {
        if (completion != VK_NULL_HANDLE) {
            owner.device_table.vkDestroyFence(owner.device, completion, nullptr);
            completion = VK_NULL_HANDLE;
        }
        if (command_pool != VK_NULL_HANDLE) {
            // THE BUFFER GOES WITH ITS POOL: freeing it first keeps the two handles in step when only one of them
            // exists (a create that failed after the pool), and the pool's own destruction would free it anyway.
            if (commands != VK_NULL_HANDLE) {
                owner.device_table.vkFreeCommandBuffers(owner.device, command_pool, 1U, &commands);
                commands = VK_NULL_HANDLE;
            }
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
    {
        // THE ANSWER GOES WITH THE SESSION: a report read after this driver is destroyed must not describe a frame
        // whose objects no longer exist.
        const std::lock_guard<std::mutex> held(books_mutex);
        answered_books = FrameBooks{};
    }
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
    // THE FRAME IS ANSWERED (see answered_books): everything it measured - the counters, this frame's uploads, the lock's
    // cost, and the device time its fence wait made readable - becomes the value a host reads, in ONE copy under the
    // lock. After this point the frame thread touches `books` no more, which is what makes the answer a closed value
    // rather than a table someone may still be filling.
    {
        const std::lock_guard<std::mutex> held(books_mutex);
        answered_books = books;
    }
}

WindowTarget* Device::FrameDriver::resolvePresenter(vn::graphics::PresentTarget* const present_to) const noexcept
{
    if (present_to == nullptr) {
        return nullptr;
    }
    auto* candidate = dynamic_cast<WindowTarget*>(present_to);
    for (const auto& surface : owner.surfaces) {
        if (surface.get() == candidate) {
            return candidate;
        }
    }
    return nullptr;
}

Device::FrameDriver::PassWalk Device::FrameDriver::claimPasses(const FrameDescription& frame,
                                                              WindowTarget* const     window,
                                                              const PassMode          mode,
                                                              TargetTable::Lease&     lease,
                                                              FrameHead&              head,
                                                              FrameRecording&         recording) noexcept
{
    // WHERE A PASS THAT NAMES NO TARGET DRAWS: the acquired image of the presenter the frame named (see
    // resolvePresenter). A pass that samples a target takes a claim on it below, exactly like a pass that draws into one.
    recording.window = window;

    // A PASS THAT SAMPLES A TARGET READS ITS IMAGES, and a target's "sampled vs attachment" layout is per-frame state:
    // the sampler takes the same lease the drawer does (R5's other half). A conflict skips THIS pass and says so once
    // per (target, episode), exactly like a drawing conflict - and it says the same sentence for a frame and a
    // preparation, because the repair is the same and the code names which fact it was.
    const auto claim_sampled = [&](const RenderTarget* const sampled) -> bool {
        if (sampled == nullptr || lease.claim(sampled)) {
            return true;
        }
        if (claim_conflicts.insert(sampled).second) {
            owner.report(deviceDiagnostic(DiagnosticSeverity::Warning,
                                          DiagnosticCategory::UnsupportedRequest,
                                          u8"vk.target.claim_conflict",
                                          u8"vk: a pass samples a target another frame driver is drawing into right now; "
                                          u8"the pass was skipped - drive the other engine on another target, or wait "
                                          u8"for its frame to finish"));
        }
        recording.claim_refused = true;
        recording.refused_pass  = true;
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
    // THE WINDOW'S OWN SENTENCE, in ONE place (see WindowTarget::presentationState): the three states mean three
    // different things with three different repairs, so the report names which one stopped the pass instead of accusing
    // the host of a presenter it never named (review 19 `Z14`).
    const auto report_window_state = [this](const WindowTarget& target) {
        const WindowTarget::PresentationState state = target.presentationState();
        owner.report(deviceDiagnostic(
            DiagnosticSeverity::Warning,
            DiagnosticCategory::UnsupportedRequest,
            u8"vk.window.not_ready",
            state == WindowTarget::PresentationState::Detached
                ? vn::String(u8"vk: a window pass names this device's presenter, but the window is detached (its host window "
                             u8"is gone, or the move to a new handle could not be rebuilt); the pass was left out")
                : state == WindowTarget::PresentationState::NotAttached
                    ? vn::String(u8"vk: a window pass names this device's presenter, but it was never attached to this device "
                                 u8"(or its attach was refused); the pass was left out")
                    : vn::String(u8"vk: a window pass names this device's presenter, but it has no live swapchain - the last "
                                 u8"(re)build produced none, and its own refusal was reported when it happened; the pass was "
                                 u8"left out")));
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
        // with no draws). The reference recorder states the same rule, which is what makes the two reports comparable;
        // ONLY A RECORDED FRAME COUNTS, because a warm-up reports the shape it read from the frame itself.
        std::uint64_t packets = 0U;
        std::uint64_t draws   = 0U;
        for (const DrawPacket& description : draw.packets) {
            packets += vn::graphics::packetDraws(description) ? 1U : 0U;
            draws += vn::graphics::drawItemsOf(description);
        }
        const bool has_effect = vn::graphics::passHasEffect(draw);
        const auto count_pass = [&recording, mode, packets, draws, has_effect]() {
            if (mode != PassMode::Record || !has_effect) {
                return;
            }
            ++recording.counters.passes;
            recording.counters.draw_packets += packets;
            recording.counters.draws += draws;
        };

        if (draw.target == nullptr) {
            // ONE QUESTION, THREE ANSWERS (`G4`, review 19 Z14): "no presenter this device handed out", "the presenter
            // has no live swapchain" and "the presenter is fine" used to be two answers, and the second was reported as
            // the first - so a host whose own window lost its chain was told it had passed a foreign presenter.
            if (recording.window == nullptr) {
                if (owner.window_reports.presenter.shouldReport()) {
                    owner.report(deviceDiagnostic(DiagnosticSeverity::Warning,
                                                  DiagnosticCategory::UnsupportedRequest,
                                                  u8"vk.window.presenter_missing",
                                                  u8"vk: a window pass names no presenter this device handed out; it was left out"));
                }
                recording.refused_pass = true;
                continue;
            }
            // THE WALK ASKS THE QUESTION ITS MODE NEEDS, AND NEITHER ONE IS "DOES A CHAIN EXIST?" (`G1`, review 20
            // `Z23`). `ready()` answers whether a chain exists RIGHT NOW, and this backend's rebuild destroys the old
            // chain before it builds the new one - so a rebuild that failed (a zero extent, a refused
            // `vkCreateSwapchainKHR`, a refused depth-image allocation) leaves an attached target with no chain. Asked
            // as a guard here, that fact ended the window for the session: the only call that could repair it - the
            // acquire that rebuilds - never ran.
            //   - A RECORDED FRAME needs an IMAGE, and `acquire` is the verb that gets one, including building the
            //     chain the target owes: it is asked FIRST, and once per frame.
            //   - A WARM-UP acquires nothing (`P11`), so it asks the chain: preparing a pass against one that does not
            //     exist has nothing to prepare.
            if (mode != PassMode::Record && !recording.window->ready()) {
                if (owner.window_reports.not_ready.shouldReport()) {
                    report_window_state(*recording.window);
                }
                recording.refused_pass = true;
                continue;
            }
            if (mode == PassMode::Record && !recording.window_acquired) {
                vn::String why;
                if (!recording.window->acquire(&why)) {
                    if (recording.window->deviceLost()) {
                        // The frame is over: nothing can run on a lost device, so the walk stops here instead of
                        // letting every later pass build against it. The sentence is reported once (see
                        // noteDeviceLost), and Lost is the outcome the host acts on.
                        (void)owner.note_device_lost(VK_ERROR_DEVICE_LOST, "vkAcquireNextImageKHR");
                        return PassWalk::DeviceLost;
                    }
                    const WindowTarget::PresentationState state = recording.window->presentationState();
                    if (state == WindowTarget::PresentationState::Detached || state == WindowTarget::PresentationState::NotAttached) {
                        // THE WINDOW IS NOT A PLACE TO PRESENT: its own state is the answer, and the acquire's sentence
                        // would only repeat it (review 19 `Z14`).
                        if (owner.window_reports.not_ready.shouldReport()) {
                            report_window_state(*recording.window);
                        }
                    }
                    else if (owner.window_reports.acquire.shouldReport()) {
                        // AN IMAGE THIS FRAME COULD NOT HAVE, with the cause the target itself reported: a chain that
                        // exists but handed none out (a timeout, a surface error), or a rebuild that failed and named
                        // the allocation it was refused.
                        owner.report(deviceDiagnostic(DiagnosticSeverity::Warning,
                                                      DiagnosticCategory::UnsupportedRequest,
                                                      u8"vk.window.acquire_failed",
                                                      why.empty() ? vn::String(u8"vk: the window's next image could not be acquired") : std::move(why)));
                    }
                    recording.window_refused = true;
                    recording.refused_pass   = true;
                    continue;
                }
                owner.window_reports.acquire.rearm(); // the image arrived: this episode is over
                // THE PROMISE AND THE HOLD, TWO FACTS (see FrameRecording::picture_owed): taking the image commits this
                // frame to a present, and that answer survives the give-back every failure path below performs.
                recording.window_acquired = true;
                recording.picture_owed    = true;
            }
            if (mode == PassMode::Record && !recording.window_acquired) {
                // A LATER WINDOW PASS IN A FRAME WHOSE ACQUIRE ALREADY FAILED: the refusal was reported once, and there
                // is still no image to draw into.
                recording.refused_pass = true;
                continue;
            }
            // THIS PASS FOUND A PRESENTER IT CAN DRAW INTO: the two episodes that name the ways it could not are over
            // (see ReportOnce - a condition is re-armed where it ends, not at shutdown).
            owner.window_reports.presenter.rearm();
            owner.window_reports.not_ready.rearm();
            if (!claim_pass_samples(draw, entry)) {
                continue;
            }
            count_pass();
            FrameHead::Pass& pass_head = head.beginPass(draw.packets.size());
            pass_head.entry            = &entry;
            pass_head.draw             = &draw;
            pass_head.target           = PassTarget::forWindow(*recording.window);
            continue;
        }
        // ONE DRIVER AT A TIME, ONE FRAME AT A TIME (see TargetTable): the claim is taken BEFORE the attachments are
        // built, because rebuilding a target is the claim holder's verb (`R5`). A lost claim skips THIS pass - the rest
        // of the frame still runs (`G7`) - and says so once per (target, episode).
        if (!lease.claim(draw.target)) {
            if (claim_conflicts.insert(draw.target).second) {
                owner.report(deviceDiagnostic(DiagnosticSeverity::Warning,
                                              DiagnosticCategory::UnsupportedRequest,
                                              u8"vk.target.claim_conflict",
                                              u8"vk: a pass named a target another frame driver is drawing into right "
                                              u8"now; the pass was skipped - drive the other engine on another target, "
                                              u8"or wait for its frame to finish"));
            }
            recording.claim_refused = true;
            recording.refused_pass  = true; // the "nothing I can run" sentence would be the same fact twice
            continue;
        }
        if (!claim_pass_samples(draw, entry)) {
            continue;
        }
        // THE DEPTH THIS PASS ATTACHES MAY BELONG TO ANOTHER TARGET (RenderTarget::shareDepth). Claiming only the
        // borrower would let two drivers write ONE image - the lender's depth - with two submissions at once, and would
        // let both write that image's ONE layout record from two threads. The borrow edge is therefore a claim on the
        // LENDER, and a contended lender skips this pass exactly like a contended target (`G10`). A WARM-UP TAKES THE
        // SAME CLAIMS: a build that reads a lender's view without holding the lender is exactly the drift this one
        // walker exists to prevent.
        if (const vn::raw_ptr<RenderTarget> lender = draw.target->depthSource(); lender != nullptr && !lease.claim(lender)) {
            if (claim_conflicts.insert(lender).second) {
                owner.report(deviceDiagnostic(DiagnosticSeverity::Warning,
                                              DiagnosticCategory::UnsupportedRequest,
                                              u8"vk.target.claim_conflict",
                                              u8"vk: a pass draws against a depth another frame driver is drawing into "
                                              u8"right now; the pass was skipped - drive the other engine on another "
                                              u8"target, or wait for its frame to finish"));
            }
            recording.claim_refused = true;
            recording.refused_pass  = true;
            continue;
        }
        // ONE CALL, ONE ANSWER, AND THE HANDLE KEEPS IT (see TargetTable::ensure and PassTarget::forTarget): the
        // record the build left behind is what this pass draws into, and the handle shares ownership of it, so a
        // release announced while the frame records - which does not wait for it (R6) - can park that record but
        // cannot take it away from a frame that has already resolved it.
        std::shared_ptr<TargetObjects> claimed_record = owner.target_table.ensure(*draw.target);
        if (claimed_record == nullptr) {
            recording.refused_pass = true;
            continue;
        }
        count_pass();
        FrameHead::Pass& pass_head = head.beginPass(draw.packets.size());
        pass_head.entry            = &entry;
        pass_head.draw             = &draw;
        pass_head.target           = PassTarget::forTarget(std::move(claimed_record));
    }

    // THE FRAME'S READ SET IS RESOLVED WITH ITS WRITE SET (`K7`, review 9): which record each full-screen packet samples
    // and which record a pass' shadow comes from are values the frame OWNS from the claim on - so a release announced
    // while the frame records parks the record instead of taking the picture away, and the record loop takes no
    // target-map lock (see resolveReadSet).
    resolveReadSet(head, recording);

    if (head.passes().empty()) {
        // A WALK THAT TOOK THE WINDOW'S IMAGE RETURNS IT (see FrameRecording::giveBackWindowImage): an acquire is a
        // pair with a present, and no pass was claimed to present it. The PROMISE is withdrawn with it - the walk took
        // the image before it knew it could draw nothing into it, and no picture was ever recorded - so the frame
        // answers `Skipped` and not the `Lost` a failure earns: a swapchain rebuild cannot fix a refused pass, and
        // asking for one per frame would buy a device wait for a content defect.
        recording.giveBackWindowImage();
        recording.picture_owed = false;
        return PassWalk::Nothing;
    }
    return PassWalk::Ready;
}

void Device::FrameDriver::resolveReadSet(FrameHead& head, FrameRecording& recording) noexcept
{
    // THE DEPTHS THIS FRAME LENDS, collected once: every target another pass' target borrows its depth from
    // (RenderTarget::shareDepth resolves to one pointer per borrower, and the borrower was built by the walk above).
    // `resolveShadowSource` skips them - an image cannot be a shader's picture and a pass' depth attachment in the same
    // frame - and collecting them HERE is what lets every pass' shadow be resolved before the record loop opens.
    for (const FrameHead::Pass& item : head.passes()) {
        if (item.draw->target != nullptr) {
            if (const vn::raw_ptr<RenderTarget> lender = item.draw->target->depthSource(); lender != nullptr) {
                recording.lent_depths.push_back(lender);
            }
        }
    }
    for (FrameHead::Pass& pass_head : head.passes()) {
        const PassDraw& draw = *pass_head.draw;
        for (std::size_t index = 0; index < draw.packets.size(); ++index) {
            const DrawPacket& description = draw.packets[index];
            if (description.kind != vn::graphics::DrawKind::Fullscreen || description.source == nullptr) {
                continue;
            }
            // THE SOURCE'S RECORD, RESOLVED ONCE: the draw was claimed by the walk, so what is left is the record the
            // recorder binds - and the pass slot now keeps it (a release parks it rather than emptying the frame's
            // answer). An unbuilt source stays null and the recorder reports its own refusal, with its own code.
            const std::shared_ptr<TargetObjects> source = owner.target_table.lookup(description.source);
            if (source == nullptr || !source->built()) {
                continue;
            }
            pass_head.sources[index] = source;
        }
        if (pass_head.entry != nullptr) {
            (void)owner.resolveShadowSource(pass_head.entry->inputs, recording.lent_depths, pass_head.shadow_map, pass_head.shadow);
        }
    }
}

void Device::FrameDriver::resolveFrameHead(FrameHead& head, ContentCaches::FrameBuilds& frame_builds) noexcept
{    // ─── THE FRAME HEAD: EVERY CACHE CONSULTATION HAPPENS HERE ───────────────────────────────────────────────────
    // One hold of the cache's lock answers what this frame draws with - programs, streams, pipelines and every texture
    // upload - and the recording uses only those values (see ContentCaches::Lock). A release another thread announces
    // while this frame records therefore cannot change what the frame already resolved, and the recording path takes no
    // lock at all (S6c; T3).
    const ContentCaches::Lock cache_lock = owner.caches.lock();
    // THE DEFAULT PROGRAM IS A VALUE FOR THIS RESOLUTION (see program_mutex): a host may announce another one while
    // this runs, and a head that read the field twice could compile two variants of one answer.
    vn::graphics::ShaderProgramDescriptionPtr head_program;
    {
        const std::lock_guard<std::mutex> program_held(program_mutex);
        head_program = default_program;
    }
    for (FrameHead::Pass& pass_head : head.passes()) {
        const PassDraw& pass_draw = *pass_head.draw;
        for (std::size_t packet_index = 0; packet_index < pass_draw.packets.size(); ++packet_index) {
            const DrawPacket&  description = pass_draw.packets[packet_index];
            FrameHead::Packet& packet      = pass_head.packets[packet_index];
            if (description.kind == vn::graphics::DrawKind::Fullscreen) {
                packet.plans.resize(1U);
                FrameHead::DrawPlan& plan = packet.plans.front();
                (void)owner.caches.planFullscreen(description, pass_draw, pass_head.target.compatibility(), plan, packet.refusal, cache_lock, frame_builds);
                // The destination's stand-in and shadow sampler: a full-screen draw samples the target it names, so its
                // slots are the source's views (assembled where the source is), not a material's.
                owner.caches.resolveTextures(nullptr, commands, head, plan, packet.refusal, cache_lock, uploadBatch(), frame_builds, &barrier_tally);
                continue;
            }
            if (description.commands == nullptr) {
                continue;
            }
            packet.plans.resize(description.commands->size());
            for (std::size_t command_index = 0; command_index < description.commands->size(); ++command_index) {
                FrameHead::DrawPlan& plan = packet.plans[command_index];
                if (!owner.caches.planCommand((*description.commands)[command_index],
                                              pass_draw,
                                              head_program.get(),
                                              pass_head.target.compatibility(),
                                              plan,
                                              packet.refusal,
                                              cache_lock,
                                              frame_builds))
                {
                    continue; // the plan stays empty and `record` skips exactly this command
                }
                owner.caches.resolveTextures(plan.material.get(), commands, head, plan, packet.refusal, cache_lock, uploadBatch(), frame_builds, &barrier_tally);
            }
        }
    }
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
    // A FRAME'S DEVICE TIME STARTS DISENGAGED (see FrameTiming): only a reading of this frame's own pair may engage it,
    // so a frame that never reached the queue - or reached it and did not finish - answers "not measured" instead of
    // the previous frame's number. Stated at the START rather than on the failure path because every exit has to answer
    // the same way, and one statement true before all of them makes that true by construction (review 19 `Z20`).
    books.timing.reset();
    // the frame's counters live in its recording value (see FrameRecording): the books read them when the frame ends.
    struct BooksCloser {
        FrameDriver*                 session;
        const ContentTotals&         before;
        vn::graphics::FrameCounters& counters;
        ~BooksCloser()
        {
            session->closeFrameBooks(before, counters);
        }
    } books_closer{ this, before, recording.counters };
    // THIS FRAME'S ADMISSIONS (F4): the geometries it resolves are asked of the DEVICE's residence table in this
    // driver's name, a build it wins is PUBLISHED below once the fence says the fill is provably complete, and a
    // competitor's temporary copy dies with this object (see ContentCaches::FrameBuilds). Declared here, OUTSIDE the
    // head's hold, because its destructor takes the cache lock to dispose a copy.
    ContentCaches::FrameBuilds frame_builds(owner.caches, clock_window);

    // THE PASSES THIS FRAME WILL RUN ARE CLAIMED BY ONE WALKER, SHARED WITH THE WARM-UP (see claimPasses): "which
    // passes can this device run, and which claims does each take" has ONE answer, so a warm-up cannot miss a claim a
    // frame must take - or build what a frame would not run (`G1` / `G5`).
    frame_head.beginFrame(frame.passes.size());
    recording.clear();

    // THE FRAME'S HOLD ON ITS TARGETS ENDS WITH THE FRAME (see PassTarget and FrameHead::Pass): the pass slots keep the
    // records this frame draws into AND the ones it reads (its full-screen sources, its shadow map), and the park's turn
    // runs at a frame end - dropping them here (on every return below) is what lets that turn destroy a released target
    // instead of carrying it a whole frame longer.
    struct FrameHoldCloser {
        FrameHead&      head;
        FrameRecording& recording;

        ~FrameHoldCloser()
        {
            head.clear();
            recording.clearMovedLayouts();
        }
    } hold_closer{ frame_head, recording };
    // THIS FRAME'S TARGET CLAIMS: one lease, and its destructor is the ONE release point every early return below
    // passes through (`P3`).
    TargetTable::Lease target_lease = owner.target_table.lease(clock_window);
    switch (claimPasses(frame, resolvePresenter(present_to), PassMode::Record, target_lease, frame_head, recording)) {
    case PassWalk::DeviceLost:
        // The one loss report already said why (see noteDeviceLost): nothing can run on a lost device.
        return PresentOutcome::Lost;
    case PassWalk::Nothing:
        if (!recording.refused_pass && no_targets_report.shouldReport()) {
            owner.report(deviceDiagnostic(DiagnosticSeverity::Info,
                                          DiagnosticCategory::UnsupportedRequest,
                                          contentRefusalCode(ContentRefusalCause::NoRunnablePass),
                                          u8"vk: the frame had no pass this backend can run (off-screen targets and host windows are "
                                          u8"served); nothing was submitted"));
        }
        // THE WALK RETURNED THE WINDOW'S IMAGE AND WITHDREW THE PROMISE (see claimPasses): a frame that claims no pass
        // has nothing to present, so the answer is `Skipped` - and it is read from the promise like every other
        // frame-level answer after the acquire, so the two cannot disagree.
        return recording.unrunOutcome();
    case PassWalk::Ready:
        break;
    }
    no_targets_report.rearm(); // a pass ran: the "nothing I can run" episode is over
    if (!recording.claim_refused) {
        claim_conflicts.clear(); // no pass lost its target this frame: the conflict episode is over
    }

    // ONE COMMAND BUFFER, ONE FENCE, ONE WAIT. The buffer is THIS DRIVER'S (see create): beginning it resets what the
    // last frame recorded, so a frame pays no allocate/free pair and has no "could not get a buffer" branch - the
    // object either exists (the device came up) or this device was never initialized. The frame is submitted and
    // finished before the call returns, which is what makes "read the pixels right after" mean what it says.
    //
    // AND A FRAME THAT STOPS HERE STILL OWES ITS ANSWER (see FrameRecording::unrunOutcome): it took the surface's
    // image, so the picture did not arrive and the host is told `Lost` - the give-back returns the IMAGE, never the
    // promise. The same two lines close the end-of-buffer and submission failures below; `Skipped` is left to a frame
    // with no surface, and to the walk above that claimed no pass.
    VkCommandBufferBeginInfo begin{};
    begin.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    if (owner.device_table.vkBeginCommandBuffer(commands, &begin) != VK_SUCCESS) {
        owner.report(deviceDiagnostic(DiagnosticSeverity::Error,
                                          DiagnosticCategory::SubmissionFailed,
                                          u8"vk.frame.begin_failed",
                                          u8"vk: vkBeginCommandBuffer failed"));
        recording.giveBackWindowImage();
        return recording.unrunOutcome();
    }

    // A FRAME AND EVERY PASS IN IT ARE NAMED TO A CAPTURE TOOL (see Device::debugMarkers and Impl::beginLabel): without
    // labels a RenderDoc/NSight frame is one anonymous command stream, and every name needed is already here (the
    // frame's own pass names). The frame's region opens here, so the head's texture uploads are inside it too.
    constexpr float kFrameLabelColor[4]{ 0.28F, 0.47F, 0.86F, 1.0F };
    recording.frame_labelled = owner.beginLabel(commands, u8"vk frame", kFrameLabelColor);
    if (recording.frame_labelled) {
        ++debug_labels;
    }

    // THE DEVICE CLOCK OPENS HERE (see FrameTiming): the reset is legal outside a rendering instance, which is where this
    // is, and the pair is closed before the buffer ends below. SAMPLED, not every frame (see kTimestampSampleFrames).
    ++clock_frames;
    const bool sample_clock = timestamps && (clock_frames % kTimestampSampleFrames) == 1U;
    // ONE QUERY PER PASS BOUNDARY, beside the frame's own pair (see PassTiming): a boundary is the END of a pass and it
    // is written at `ALL_COMMANDS`, because a timestamp answers "when did everything before it finish" - a boundary that
    // does not wait (the 1.0 `TOP_OF_PIPE` habit) measures when the command processor ARRIVED, which is not what a pass
    // spent. A frame that needs more than the pool holds recreates it here, which is legal because no command buffer is
    // in flight; a device that cannot grow it keeps the frame's own clock and answers no attribution.
    const std::uint32_t pass_count      = static_cast<std::uint32_t>(frame_head.passes().size());
    recording.query_count               = 2U + pass_count;
    recording.per_pass_clock            = sample_clock && ensureTimestampQueries(recording.query_count);
    if (recording.per_pass_clock) {
        owner.device_table.vkCmdResetQueryPool(commands, timestamp_pool, 0U, recording.query_count);
        // THE FRAME'S START takes no wait (every earlier command of this buffer is about to be recorded), which is what
        // `TOP_OF_PIPE` means for a timestamp - `NONE` would be the same statement spelled for a barrier.
        owner.device_table.vkCmdWriteTimestamp2(commands, kFrameClockStartStage, timestamp_pool, 0U);
    }

    // THE RING'S CURSOR RESTARTS HERE; THE SET CACHE DOES NOT (see FrameScratch::beginFrame): the previous frame's
    // submission is complete by now (the device submits synchronously), but a CACHED descriptor set has to outlive the
    // frame that built it - the pool is never reset, entries are freed one at a time and swept after many frames
    // (`N1`). What restarts is the uniform ring's write cursor, not the pool.
    scratch.beginFrame();
    // AND A NEW COMMAND BUFFER HOLDS NO STATE (see ContentDraw::beginFrame): the recorder's shadow of what is already
    // stated starts empty, so the first draw of every frame states its own.
    recorder.beginFrame();
    // The pass count was taken from the frame's shape above (a pass whose draws were all skipped is still a pass
    // that ran); what the loop below adds is the draw packets this backend actually recorded.

    // THE HEAD RESOLVES EVERY PLAN AND RECORDS EVERY UPLOAD (see resolveFrameHead): one hold of the cache lock, and the
    // recording below uses only the values it produced - so it takes no lock and names no cache entry.
    resolveFrameHead(frame_head, frame_builds);

    // THE RECORDS THIS FRAME MOVED ON ANOTHER TARGET (a sampled source's picture, a shadow map's picture): what a frame
    // that never reaches the queue has to put back (see rewindUnrunFrame below). It is the DRIVER's storage and only
    // `clear()`ed per frame, for the same reason `FrameHead`'s slots are (see FrameRecording) - a steady frame allocates
    // nothing here.
    std::vector<MovedLayout>& moved_layouts = recording.moved_layouts;
    // A FRAME THAT NEVER REACHED THE QUEUE MOVED NOTHING (see TargetObjects' note on records following the recorded
    // COMMANDS). Two answers, because the two kinds of record have different owners:
    // - THIS FRAME'S OWN ATTACHMENT SETS go back to `Undefined`: their content is what this frame would have written, the
    //   GPU never wrote it, and `Undefined` is the honest AND the safe statement - the next pass clears instead of
    //   loading a picture that is not there (TargetObjects::resetLayouts).
    // - SOMEONE ELSE'S IMAGES go back to EXACTLY what they said: a sampled source or a shadow map may hold a picture an
    //   earlier frame wrote and nothing else can reproduce, so discarding it would be worse than the failure.
    // - AND THE WINDOW'S ACQUIRED IMAGE IS GIVEN BACK, because it was taken from the presentation engine and may not be
    //   re-acquired with the semaphore its acquire signalled (FrameRecording::giveBackWindowImage, which returns the
    //   image and keeps the frame's promise - that promise is what the caller's answer reads).
    const auto rewindUnrunFrame = [&]() {
        for (const FrameHead::Pass& item : frame_head.passes()) {
            // A WINDOW PASS OWES NOTHING HERE: its image is given back below, and the next acquire states its own
            // starting layout (see PassTarget::rewind).
            item.target.rewind();
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
        recording.giveBackWindowImage();
    };
    const std::span<const FrameHead::Pass> passes = frame_head.passes();
    constexpr float kPassLabelColor[4]{ 0.62F, 0.62F, 0.62F, 1.0F };
    for (std::size_t pass_index = 0; pass_index < passes.size(); ++pass_index) {
        const FrameHead::Pass& pass_run = passes[pass_index];
        const PassDraw& draw = *pass_run.draw;
        // THE PASS' REGION OPENS BEFORE ITS ENTRY BARRIERS and closes after its instance: a capture tool then shows
        // exactly what the pass caused, which is the same split the clock reports (see PassTiming).
        const bool pass_labelled = pass_run.entry != nullptr && pass_run.entry->pass != nullptr &&
                                   owner.beginLabel(commands, pass_run.entry->pass->name().c_str(), kPassLabelColor);
        if (pass_labelled) {
            ++debug_labels;
        }
        // THE PASS' TEXTURE UPLOADS ALREADY RAN, at the frame head, on this same command buffer (see above): an upload
        // records image barriers and copies, neither legal inside a rendering instance - which is why the head runs
        // before the first pass begins. Nothing here consults the caches.

        // A SOURCE A FULL-SCREEN DRAW SAMPLES is about to be read: transition it to SHADER_READ_ONLY now, where an image
        // barrier is unambiguously allowed (before any rendering instance begins). ONE SOURCE, ONE DEPENDENCY: its
        // pictures move together (`BarrierBatch`), so a pass sampling a four-attachment G-buffer states one command
        // rather than four - and every record it writes stays in step with the barrier that moves it.
        for (std::size_t draw_index = 0; draw_index < draw.packets.size(); ++draw_index) {
            // THE RECORD IS THE CLAIM'S, NOT A LOOKUP (`K7`, review 9): `resolveReadSet` resolved it with the write set
            // and the pass slot keeps it, so this loop reads a value and takes no target-map lock - and a release that
            // landed after the claim cannot make the picture disappear between here and the draw.
            const std::shared_ptr<TargetObjects>& source_entry = pass_run.sources[draw_index];
            if (source_entry == nullptr) {
                continue;
            }
            std::array<VkImageMemoryBarrier2, kMaxColorAttachments> sampled_moves{};
            BarrierBatch sampled_batch(owner.device_table, commands, sampled_moves, &barrier_tally);
            // EVERY PICTURE THE DRAW SAMPLES MOVES, each from its OWN record: one image per colour attachment, and a
            // readback may have moved one of them while the others stayed. The picture is `colors` in both shapes - the
            // resolve images on a multisampled target, whose sample buffers nothing may sample.
            TargetObjects& sampled = *source_entry;
            for (std::size_t index = 0; index < sampled.colors.size(); ++index) {
                const ImageLayout previous = sampled.color_layouts[index];
                if (!sampled_batch.move(sampled.colors[index], sampled.color_layouts[index], ImageLayout::ShaderReadOnly, VK_IMAGE_ASPECT_COLOR_BIT)) {
                    continue; // an earlier pass of this frame already stated it
                }
                // THE REWIND LOG (see rewindUnrunFrame): this picture belongs to ANOTHER target, so a failed frame has to
                // put the record back exactly - not to `Undefined`, which would discard a picture an earlier frame wrote.
                moved_layouts.push_back(MovedLayout{ &sampled, index, false, previous });
            }
            sampled_batch.flush();
        }

        // THE SHADOW MAP THIS PASS SAMPLES: the CLAIM resolved it (see resolveShadowSource) and the pass slot keeps the
        // record, so what is left here is the one command it needs - the picture moves to SHADER_READ_ONLY before the
        // pass begins, because a barrier is not legal inside a rendering instance. IT IS THE PICTURE that moves: on a
        // multisampled map that is the resolved depth image, which is also the only one a shader may sample.
        ContentDraw::ShadowSource shadow = pass_run.shadow;
        if (pass_run.shadow_map != nullptr) {
            TargetObjects& sampled = *pass_run.shadow_map;
            const ImageLayout previous = sampled.depthPictureLayout();
            if (previous != ImageLayout::ShaderReadOnly) {
                // THE REWIND LOG (see rewindUnrunFrame): this picture belongs to ANOTHER target, so a frame that never
                // reaches the queue has to put the record back exactly - not to `Undefined`, which would discard a
                // picture an earlier frame wrote.
                moved_layouts.push_back(MovedLayout{ &sampled, 0U, true, previous });
                (void)sampled.transitionDepthPicture(owner.device_table, commands, ImageLayout::ShaderReadOnly, &barrier_tally);
            }
        }

        // WHAT THIS PASS LOADS AND CLEARS: ONE RULE, ONE HOME (see DynamicRendering::resolveAttachmentOps). The only input that
        // differs between the two kinds of target is "does this set already hold a picture to keep": an off-screen set
        // answers from its own record, and a window from what THIS frame has already written into the acquired image (its
        // content before that belongs to the presentation engine, not to the host).
        std::array<ResolvedAttachment, kMaxColorAttachments> color_ops{};
        const PassTarget& target   = pass_run.target;
        const bool        preserve = target.isWindow() ? recording.window_written : target.holdsPicture();
        const PassAttachmentOps                          load =
            resolveAttachmentOps(preserve, draw.attachment_policy.has_value() ? &*draw.attachment_policy : nullptr, color_ops, target.colorCount());

        // THE SET OWNS THE TRANSITIONS AND THE INSTANCE (see PassTarget::begin): the layouts each attachment is in are the
        // owner's bookkeeping, so the recorder cannot disagree with them.
        target.begin(owner.device_table, commands, load, &barrier_tally);
        if (target.isWindow()) {
            recording.window_written = true; // from here on, a later window pass keeps what this one draws
        }
        // WHAT THIS PASS RECORDS WITH (see DrawScope): the command buffer, its rectangle, the frame's head and the map it
        // samples - stated once per pass instead of once per draw.
        const ContentDraw::DrawScope scope = ContentDraw::DrawScope::forPass(commands, target.extent(), frame_head, shadow, draw);
        // WHAT THIS PASS DRAWS: the frame head's plans and resolved bindings, in the draw packet's own order. Every
        // command inside a packet becomes one GPU draw (M3: the plan selected the pipeline, one `DrawItem` = one
        // indexed draw). The frame's DRAWING-CALL count was taken from the frame's shape above - a packet is the
        // description, not its commands.
        const FrameHead::Pass& pass_head = passes[pass_index];
        for (std::size_t packet_index = 0; packet_index < draw.packets.size(); ++packet_index) {
            const DrawPacket&        description = draw.packets[packet_index];
            const FrameHead::Packet& packet      = pass_head.packets[packet_index];
            // THE HEAD'S REFUSAL IS REPORTED WHERE THE DRAW IS (see planCommand): the code travels with the sentence
            // instead of being guessed at the sink, and a packet whose later commands DID resolve still records them.
            if (packet.refusal.isRefused()) {
                recording.content_refused = true;
                if (content_report.shouldReport()) {
                    owner.report(deviceDiagnostic(DiagnosticSeverity::Warning, packet.refusal.category(), packet.refusal.code(), packet.refusal.message()));
                }
            }
            ContentRefusal refusal;
            if (description.kind == vn::graphics::DrawKind::Fullscreen) {
                // THE FULL-SCREEN ABI: the source's colour attachments, bound in order - from the record the CLAIM
                // resolved (see resolveReadSet), which the pass slot keeps alive for the whole pass. A source this
                // device never built stays null and answers an empty span, which makes the recorder report (and skip)
                // the draw.
                ContentDraw::FullscreenSource source{};
                if (const std::shared_ptr<TargetObjects>& source_entry = pass_run.sources[packet_index]; source_entry != nullptr) {
                    source.views = source_entry->color_views;
                }
                recorder.recordFullscreen(scope, description, packet.plans.front(), source, &refusal);
            }
            else {
                recorder.record(scope, description, packet.plans, &refusal);
            }
            if (refusal.isRefused()) {
                recording.content_refused = true;
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
        target.end(owner.device_table, commands);
        // THE PASS' BOUNDARY, written AFTER it: `ALL_COMMANDS` waits for the pass' own work, so segment i is
        // `boundary i - boundary i-1` (the frame's start for the first pass). The transitions and uploads a pass asks
        // for are recorded before this point, so they are billed to the pass that asked (see PassTiming and C13).
        if (recording.per_pass_clock) {
            owner.device_table.vkCmdWriteTimestamp2(commands,
                                                    kPassClockBoundaryStage,
                                                    timestamp_pool,
                                                    2U + static_cast<std::uint32_t>(pass_index));
        }
        if (pass_labelled) {
            owner.endLabel(commands);
        }
    }
    if (!recording.content_refused) {
        content_report.rearm(); // every draw the frame named was recorded: the episode is over
    }

    // THE PRESENT TRANSITION, after the window's last pass and before the command buffer ends: every window pass
    // stores into COLOR_ATTACHMENT_OPTIMAL, and vkQueuePresentKHR reads PRESENT_SRC_KHR.
    if (recording.window_acquired && recording.window != nullptr) {
        recording.window->recordPresentTransition(commands);
    }

    if (recording.per_pass_clock) {
        // THE FRAME'S END waits for everything, like every other boundary: what follows the last pass (the present
        // transition, the frame's own tail) belongs to no pass - the honest split, because no pass asked for it.
        owner.device_table.vkCmdWriteTimestamp2(commands, kPassClockBoundaryStage, timestamp_pool, 1U);
    }
    if (recording.frame_labelled) {
        owner.endLabel(commands);
    }

    if (owner.device_table.vkEndCommandBuffer(commands) != VK_SUCCESS) {
        owner.report(deviceDiagnostic(DiagnosticSeverity::Error,
                                          DiagnosticCategory::SubmissionFailed,
                                          u8"vk.frame.end_failed",
                                          u8"vk: vkEndCommandBuffer failed"));
        // NOTHING WAS SUBMITTED, SO NOTHING MOVED: the records this pass walk advanced are put back (see the lambdas
        // above), and the acquired window image is given back rather than left signalled and unpresented. The command
        // buffer stays this driver's - the next frame's begin resets it. The picture this frame promised did not
        // arrive, and its answer says so (see FrameRecording::unrunOutcome).
        rewindUnrunFrame();
        owner.caches.endFrame(uploadBatch(), false);
        return recording.unrunOutcome();
    }

    // THE SUBMISSION IS THE 1.3 FORM, like every other queue-shaped statement this backend makes (see §2.7): the wait
    // stage travels ON the semaphore, so no 1.0 `VkPipelineStageFlags` is paired with a 1.3 command buffer.
    VkCommandBufferSubmitInfo command_info{};
    command_info.sType         = VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO;
    command_info.commandBuffer = commands;

    // THE WINDOW'S SEMAPHORES: the acquire signalled image_available, the submit waits on it and signals
    // render_finished, and the present waits on that. One frame at a time (the device waits on its fence below), so
    // one pair per window target is enough.
    VkSemaphoreSubmitInfo wait_info{};
    VkSemaphoreSubmitInfo signal_info{};
    if (recording.window_acquired) {
        wait_info.sType       = VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO;
        wait_info.semaphore   = recording.window->imageAvailableSemaphore();
        wait_info.stageMask   = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT;
        signal_info.sType     = VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO;
        signal_info.semaphore = recording.window->renderFinishedSemaphore();
        // A signal's stage mask is not consulted (only a wait's is), but it must be a valid value.
        signal_info.stageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
    }

    VkSubmitInfo2 submit{};
    submit.sType                  = VK_STRUCTURE_TYPE_SUBMIT_INFO_2;
    submit.commandBufferInfoCount = 1U;
    submit.pCommandBufferInfos    = &command_info;
    if (recording.window_acquired) {
        submit.waitSemaphoreInfoCount   = 1U;
        submit.pWaitSemaphoreInfos      = &wait_info;
        submit.signalSemaphoreInfoCount = 1U;
        submit.pSignalSemaphoreInfos    = &signal_info;
    }

    std::uint64_t  submission{ 0 };
    const VkResult submit_result = [&]() {
        // THE QUEUE IS EXTERNALLY SYNCHRONIZED (see Impl::queue_submit): another driver may be submitting on its own
        // thread right now, and the lock is taken for the driver call only - the fence wait below stays outside it.
        const std::lock_guard<std::mutex> submit_held(owner.queue_submit);
        if (owner.takeSubmitRefusal()) {
            return VK_ERROR_OUT_OF_HOST_MEMORY; // the test door (see Device::refuseNextSubmit): a failure, not a loss
        }
        // THE SUBMISSION'S NUMBER, taken where the queue is handed the buffer (see DeviceClock): inside this lock the
        // numbers follow the order the queue sees, which is what makes "every submission up to N is complete" true.
        {
            const std::lock_guard<std::mutex> clock_held(owner.clock_mutex);
            submission = owner.clock.noteSubmission();
        }
        return owner.device_table.vkQueueSubmit2(owner.queue, 1U, &submit, completion);
    }();
    const bool     submitted     = submit_result == VK_SUCCESS;
    if (submitted) {
        ++books.progress.submitted;
        frame_close.submitted = true;
    }
    else if (submission != 0) {
        // THE QUEUE NEVER TOOK IT, so nothing the command buffer named will ever execute: the number is proven at
        // once, or the device's watermark would wait forever for a submission that does not exist. (A submission the
        // queue DID take whose fence never signalled can only be a lost device, and its teardown releases everything.)
        const std::lock_guard<std::mutex> clock_held(owner.clock_mutex);
        owner.clock.noteCompletion(submission);
    }
    const VkResult fence_result = submitted ? owner.device_table.vkWaitForFences(owner.device, 1U, &completion, VK_TRUE, UINT64_MAX)
                                            : submit_result;
    const bool     completed    = fence_result == VK_SUCCESS;
    if (completed) {
        ++books.progress.completed;
        // THE COMPLETION EVIDENCE THE PARK QUEUE WAITS FOR (see Impl::releaseLater): this submission's GPU work is
        // provably done, so every object a release handed over while it was in flight may now go - once every earlier
        // submission is proven too, which is what the device's watermark folds (see DeviceClock).
        {
            const std::lock_guard<std::mutex> clock_held(owner.clock_mutex);
            owner.clock.noteCompletion(submission);
        }
        // THE PAIR IS READABLE NOW (the fence says the frame ran): one read, one value, and the report carries it as
        // the frame's device time (see FrameTiming). THE FIELD OPENED DISENGAGED (see where the books open), and this
        // read is the only thing that engages it - so a frame the clock did not sample, and a read the driver would
        // not answer, both stay absent rather than repeating a number this frame never measured.
        if (recording.per_pass_clock) {
            recording.stamp_values.resize(recording.query_count, 0U);
            const std::vector<std::uint64_t>& stamps = recording.stamp_values;
            if (owner.device_table.vkGetQueryPoolResults(owner.device,
                                                         timestamp_pool,
                                                         0U,
                                                         recording.query_count,
                                                         recording.stamp_values.size() * sizeof(std::uint64_t),
                                                         recording.stamp_values.data(),
                                                         sizeof(std::uint64_t),
                                                         VK_QUERY_RESULT_64_BIT) == VK_SUCCESS &&
                stamps[1] > stamps[0]) {
                const double             to_micros = static_cast<double>(owner.timestamp_period) / 1000.0;
                vn::graphics::FrameTiming timing;
                timing.gpu_micros = static_cast<std::uint64_t>(static_cast<double>(stamps[1] - stamps[0]) * to_micros);
                // PER-PASS ATTRIBUTION, in EXECUTION order: pass i spans the boundary that ENDED the pass before it to
                // its own (the frame's start for the first), and the name is the frame's own - a report a host can match
                // against the plan it read (see PassTiming).
                timing.passes.reserve(pass_count);
                for (std::uint32_t index = 0; index < pass_count; ++index) {
                    const std::uint64_t from = index == 0U ? stamps[0] : stamps[1U + index];
                    const std::uint64_t to   = stamps[2U + index];
                    vn::graphics::PassTiming entry;
                    if (passes[index].entry != nullptr && passes[index].entry->pass != nullptr) {
                        entry.pass = passes[index].entry->pass->name();
                    }
                    entry.gpu_micros = to > from ? static_cast<std::uint64_t>(static_cast<double>(to - from) * to_micros) : 0U;
                    timing.passes.push_back(std::move(entry));
                }
                books.timing = std::move(timing);
            }
        }
    }
    (void)owner.device_table.vkResetFences(owner.device, 1U, &completion);
    // THE FILL IS PROVABLY COMPLETE (ResidencyClaims rule 2): every geometry this frame uploaded becomes usable by
    // every driver. A frame the fence did not complete publishes nothing - its claim is withdrawn when `frame_builds`
    // dies at the end of this call, so no key is left Uploading (`P1`).
    if (completed) {
        frame_builds.publishAll();
    }
    // The per-draw uniform buffers were named by the command buffer that just finished (or failed to): the queue is
    // idle either way, so the bytes can go - and only THIS driver's texture staging, never a frame that is still
    // recording (see ContentImages::BatchId).
    owner.caches.endFrame(uploadBatch(), completed);

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
    // THE ANSWER IS THE FRAME'S PROMISE, NOT THE IMAGE'S HOLD (see FrameRecording::picture_owed): a frame that took the
    // surface's image owes it a picture, and only a present keeps that promise. The give-back above returns the IMAGE
    // and deliberately keeps the promise, so the one failure a host must act on (`PresentOutcome::Lost`) is reachable
    // on the paths it exists for - and a frame that never took an image answers `Skipped`, which is "there was nothing
    // to present" (see PresentOutcome).
    PresentOutcome outcome = PresentOutcome::Skipped;
    if (recording.picture_owed && recording.window != nullptr) {
        outcome = completed ? recording.window->present() : recording.unrunOutcome();
        if (recording.window->deviceLost()) {
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

    // WHICH SURFACE THE FRAME NAMES - the same resolution executeFrame makes (see resolvePresenter), minus everything
    // that would touch it: nothing is acquired, resized or presented, and the frame water level does not move (see
    // RenderFrameExecutor::prepareFrame, P11). The ONE command buffer this call records holds transfers only.
    WindowTarget* window = resolvePresenter(present_to);
    if (present_to != nullptr) {
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

    // THE WARM-UP OPENS A FRAME ON THE CLOCK (see ADR-2026-11-14) so a release announced while it runs has a frame
    // body to be told apart from - and if it submits its transfer batch, that submit takes a SUBMISSION number like
    // any other (see DeviceClock). A warm-up that records nothing spends the token without becoming a submission.
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
    // presented, and `books.progress` does not move. The buffer is the DRIVER's (see create) and is begun only when the
    // walk claimed a pass.

    // THE WORKLOAD THE FRAME ASKS FOR, computed by the SDK (see FrameCounts): one definition, so this backend and the
    // reference one cannot report different counts for the same frame - and the rule is the one a recorded frame's
    // `FrameCounters` follows (a pass that draws something or announces a clear).
    const vn::graphics::FrameCounts counts = vn::graphics::frameCountsOf(frame);
    report.passes       = counts.passes;
    report.draw_packets = counts.draw_packets;
    report.draws        = counts.draws;

    // A WARM-UP WALKS THE SAME PASSES A FRAME WOULD (see claimPasses): one rule for "which passes can this device run,
    // and which claims does each take", so a preparation cannot build what a frame would not run - or skip a claim a
    // frame must take (the depth LENDER among them, which the old second walker had lost). @ref PassMode::Prepare is the
    // only difference: nothing is acquired, presented or counted.
    frame_head.beginFrame(frame.passes.size());
    recording.clear();

    // A WARM-UP DROPS ITS STRONG REFERENCES TOO (see the recorded frame's closer): it is the same walk and the same
    // heads, so the same rule - the frame's hold ends with the frame.
    struct FrameHoldCloser {
        FrameHead&      head;
        FrameRecording& recording;

        ~FrameHoldCloser()
        {
            head.clear();
            recording.clearMovedLayouts();
        }
    } hold_closer{ frame_head, recording };
    TargetTable::Lease target_lease = owner.target_table.lease(clock_window);
    (void)claimPasses(frame, window, PassMode::Prepare, target_lease, frame_head, recording);

    // THE BATCH IS OPENED BEFORE THE HEAD RESOLVES, because the head RECORDS the copies (see resolveFrameHead): a
    // buffer-to-image copy and its transition may not be recorded inside a rendering instance, and a warm-up's only
    // command buffer is this one. A frame nothing can be built for never begins it.
    bool batch_open{ false };
    if (!frame_head.passes().empty()) {
        VkCommandBufferBeginInfo begin{};
        begin.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
        begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        if (owner.device_table.vkBeginCommandBuffer(commands, &begin) == VK_SUCCESS) {
            batch_open = true;
        }
        else {
            owner.report(deviceDiagnostic(DiagnosticSeverity::Error,
                                          DiagnosticCategory::SubmissionFailed,
                                          u8"vk.prepare.batch_failed",
                                          u8"vk: the preparation's transfer batch could not be recorded; nothing was prepared"));
        }
    }
    if (batch_open) {
        // THE SAME RESOLUTION A RECORDED FRAME RUNS (see resolveFrameHead): programs, streams, pipelines and every
        // texture upload, so the frame that follows finds all of it resident and records nothing.
        resolveFrameHead(frame_head, warmup_builds);
        // WHAT THE HEAD COULD NOT RESOLVE IS REPORTED HERE, the way the frame path reports it where the draw is.
        for (const FrameHead::Pass& pass_head : frame_head.passes()) {
            for (const FrameHead::Packet& packet : pass_head.packets) {
                if (!packet.refusal.isRefused()) {
                    continue;
                }
                recording.content_refused = true;
                if (content_report.shouldReport()) {
                    owner.report(deviceDiagnostic(DiagnosticSeverity::Warning, packet.refusal.category(), packet.refusal.code(), packet.refusal.message()));
                }
            }
        }
    }

    // THE BATCH RUNS BEFORE THIS CALL ANSWERS: one submit on the device's frame fence, no semaphore (nothing was
    // acquired, so the queue has nothing to wait for), one wait, and the staging buffers go as soon as the copies
    // have run - the release the frame path reaches through ContentDraw::endFrame -> ContentImages::releaseFrame.
    // WHETHER THE WARM-UP'S FILL IS PROVABLE (F4): a batch that never closed or never ran publishes nothing, and the
    // geometries it claimed are withdrawn when `warmup_builds` dies - so no key is left Uploading.
    bool transfers_completed = true;
    if (batch_open) {
        if (owner.device_table.vkEndCommandBuffer(commands) != VK_SUCCESS) {
            // THE BUFFER NEVER CLOSED, so whatever it recorded never ran - and it cannot be recorded again: the copies
            // were already recorded against entries this call made resident. Said plainly, because "the next frame
            // uploads it again" would be a promise the revision check does not keep.
            transfers_completed = false;
            owner.report(deviceDiagnostic(DiagnosticSeverity::Error,
                                              DiagnosticCategory::SubmissionFailed,
                                              u8"vk.prepare.batch_not_closed",
                                              u8"vk: the preparation's transfer batch could not be closed; the copies it recorded never ran"));
        }
        else if (!owner.caches.hasPendingUploads(uploadBatch())) {
            // ZERO WORK, ZERO SUBMIT: an unchanged frame records no copy, so there is nothing to run and nothing is
            // submitted - the property a second prepare of the same frame is observed by (see ADR-2026-11-14).
        }
        else {
            VkCommandBufferSubmitInfo command_info{};
            command_info.sType         = VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO;
            command_info.commandBuffer = commands;
            VkSubmitInfo2 submit{};
            submit.sType                  = VK_STRUCTURE_TYPE_SUBMIT_INFO_2;
            submit.commandBufferInfoCount = 1U;
            submit.pCommandBufferInfos    = &command_info;
            std::uint64_t  submission{ 0 };
            const VkResult submit_result = [&]() {
                // THE QUEUE IS EXTERNALLY SYNCHRONIZED (see Impl::queue_submit): held for the driver call only.
                const std::lock_guard<std::mutex> submit_held(owner.queue_submit);
                if (owner.takeSubmitRefusal()) {
                    return VK_ERROR_OUT_OF_HOST_MEMORY; // the test door (see Device::refuseNextSubmit)
                }
                // THE SUBMISSION'S NUMBER (see DeviceClock): a warm-up's transfer batch is submitted work, and work
                // submitted here may name a released object just like a frame's may.
                {
                    const std::lock_guard<std::mutex> clock_held(owner.clock_mutex);
                    submission = owner.clock.noteSubmission();
                }
                return owner.device_table.vkQueueSubmit2(owner.queue, 1U, &submit, completion);
            }();
            const VkResult fence_result  = submit_result == VK_SUCCESS
                                               ? owner.device_table.vkWaitForFences(owner.device, 1U, &completion, VK_TRUE, UINT64_MAX)
                                               : submit_result;
            const bool     completed     = fence_result == VK_SUCCESS;
            transfers_completed          = completed;
            warmup_close.submitted       = submit_result == VK_SUCCESS;
            {
                const std::lock_guard<std::mutex> clock_held(owner.clock_mutex);
                if (submission != 0) {
                    // A batch the queue refused never ran (nothing it named will execute); one that completed is
                    // proven. A batch that was taken and never signalled can only be a lost device.
                    owner.clock.noteCompletion(submission);
                }
            }
            (void)owner.device_table.vkResetFences(owner.device, 1U, &completion);
            if (!completed && !owner.note_device_lost(submit_result, "vkQueueSubmit") && !owner.note_device_lost(fence_result, "vkWaitForFences")) {
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

    // THE BATCH IS CLOSED ON EVERY PATH (see ContentCaches::endFrame): what ran is published, what did not is parked -
    // and a call that never opened a command buffer has no batch to close (the call is a no-op then). THE BATCH IS THE
    // DRIVER'S, not the verb's: a warm-up records its copies into the same book its frames use, so the fence it waits
    // for is the evidence for both (@ref FrameDriver::uploadBatch).
    owner.caches.endFrame(uploadBatch(), transfers_completed);

    // THE WARM-UP'S FILL IS PROVABLY COMPLETE (ResidencyClaims rule 2): publish every geometry it built. A batch with
    // zero work submits nothing, and nothing was pending - the fills were host copies - so that path publishes too.
    if (transfers_completed) {
        warmup_builds.publishAll();
    }

    // WHAT THIS CALL BUILT, not what is resident: a second prepare of an unchanged frame answers zeros (see
    // PrepareReport). `uploads` counts BOTH kinds FrameUploads reports - the streams this call uploaded and the
    // textures it made resident (see ADR-2026-11-14). `sets` stays 0 because no descriptor set outlives a frame here.
    if (!recording.content_refused) {
        content_report.rearm(); // every draw the call named was prepared: the episode is over
    }
    if (!recording.claim_refused) {
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
    //
    // THE FRAME'S ANSWER, READ AS ONE VALUE (see answered_books): the books are the frame's, and a host asking "how did
    // it go" while the next frame runs gets the frame that ENDED, whole - never this frame's counters beside last
    // frame's timings, and never a torn read of a field the frame thread is incrementing. What was closed is closed:
    // the copy below is the only thing a reader and a frame body share.
    {
        const std::lock_guard<std::mutex> held(books_mutex);
        report.counters = answered_books.counters;
        report.progress = answered_books.progress;
        report.uploads  = answered_books.uploads; // the closed frame's own delta; disengaged until a frame ran
        report.timing   = answered_books.timing;  // the device's own clock; disengaged when it keeps none (see FrameTiming)
        report.locking  = answered_books.locking; // what that frame spent on the cache lock
    }

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
    // THE LABEL ENTRY POINTS, resolved once when the extension is enabled (see Device::debugMarkers): the frame path
    // names every pass to whatever capture tool is attached, and a session without the extension names nothing rather
    // than guessing a function pointer.
    if (want_debug) {
        impl->debug_label_begin = reinterpret_cast<PFN_vkCmdBeginDebugUtilsLabelEXT>(
            vkGetInstanceProcAddr(impl->instance, "vkCmdBeginDebugUtilsLabelEXT"));
        impl->debug_label_end = reinterpret_cast<PFN_vkCmdEndDebugUtilsLabelEXT>(
            vkGetInstanceProcAddr(impl->instance, "vkCmdEndDebugUtilsLabelEXT"));
        impl->debug_object_name = reinterpret_cast<PFN_vkSetDebugUtilsObjectNameEXT>(
            vkGetInstanceProcAddr(impl->instance, "vkSetDebugUtilsObjectNameEXT"));
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

    // A CANDIDATE IS CHOSEN IN TWO STEPS, IN THIS ORDER: FILTER, THEN RATE (`K3`, review 9). A device that cannot serve
    // this session's floor is dropped, and the SURVIVORS alone are ranked by type - so the answer is "the fastest device
    // that can run this backend", not "the fastest device this machine has". Ranking first made the second question
    // unaskable: a two-GPU machine whose top-ranked device was one driver revision behind lost the session while a
    // capable device sat in the list, and the refusal described the backend's choice instead of the machine. The peers
    // answer it the same way (`vk-bootstrap`'s `PhysicalDeviceSelector`, Diligent's adapter enumeration): see
    // DeviceChoice.hpp, which owns the predicate and is testable with fabricated offers.
    struct Candidate {
        VkPhysicalDevice device{ VK_NULL_HANDLE };
        std::uint32_t    queue_family{ 0 };
        int              rank{ -1 };
        FloorRefusal     refusal{ FloorRefusal::None };
        bool             has_swapchain{ false };
        bool             has_creation_feedback{ false };
    };
    const auto report_floor = [this](const FloorRefusal refusal) {
        switch (refusal) {
        case FloorRefusal::ApiVersion:
            reportDiagnostic(deviceDiagnostic(DiagnosticSeverity::Error,
                                              DiagnosticCategory::InitFailed,
                                              u8"vk.init.api_version_low",
                                              u8"vk: this backend needs a Vulkan 1.3 device (dynamic rendering and "
                                              u8"synchronisation 2 are core there), and this device is older"));
            break;
        case FloorRefusal::CoreFeatures:
            reportDiagnostic(deviceDiagnostic(DiagnosticSeverity::Error,
                                              DiagnosticCategory::InitFailed,
                                              u8"vk.init.device_features_missing",
                                              u8"vk: this backend needs the Vulkan 1.3 synchronization2 and dynamicRendering "
                                              u8"features, and this device does not offer them"));
            break;
        case FloorRefusal::DynamicState3:
            reportDiagnostic(deviceDiagnostic(DiagnosticSeverity::Error,
                                              DiagnosticCategory::InitFailed,
                                              u8"vk.init.dynamic_state_missing",
                                              u8"vk: this backend needs VK_EXT_extended_dynamic_state3 with polygon mode and "
                                              u8"colour blend (cull / depth / polygon / blend are dynamic state), and this device "
                                              u8"does not offer them"));
            break;
        case FloorRefusal::None:
            break;
        }
    };

    std::uint32_t queue_candidates = 0U; ///< Devices a graphics (and, with a surface, presenting) queue family was found on.
    Candidate     chosen{};              ///< The best SURVIVOR (null until one is found).
    /// The highest-ranked device the floor turned away. It is assigned TOGETHER with `refused_rank`, which is why the
    /// one branch that reads it (`chosen` empty) always finds a real refusal in it.
    Candidate     best_refused{};
    int           best_rank    = -1;
    int           refused_rank = -1;
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
        ++queue_candidates;

        // WHAT THE CANDIDATE OFFERS, in the terms the floor is stated in (see DeviceChoice.hpp). Every query is per
        // DEVICE and happens before anything is created, so a rejected candidate costs three driver calls and no state.
        VkPhysicalDeviceProperties candidate_properties{};
        vkGetPhysicalDeviceProperties(candidate, &candidate_properties);
        Candidate facts;
        facts.device       = candidate;
        facts.queue_family = serving_family;
        facts.rank         = deviceTypeRank(candidate_properties.deviceType);

        VkPhysicalDeviceVulkan13Features candidate_vk13{};
        candidate_vk13.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES;
        VkPhysicalDeviceExtendedDynamicState3FeaturesEXT candidate_dynamic_state3{};
        candidate_dynamic_state3.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTENDED_DYNAMIC_STATE_3_FEATURES_EXT;
        VkPhysicalDeviceFeatures2 candidate_features{};
        candidate_features.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2;
        candidate_features.pNext = &candidate_vk13;
        candidate_vk13.pNext     = &candidate_dynamic_state3;
        vkGetPhysicalDeviceFeatures2(candidate, &candidate_features);

        DeviceOffer offer;
        offer.api_version          = candidate_properties.apiVersion;
        offer.synchronization2     = candidate_vk13.synchronization2 == VK_TRUE;
        offer.dynamic_rendering    = candidate_vk13.dynamicRendering == VK_TRUE;
        offer.polygon_mode         = candidate_dynamic_state3.extendedDynamicState3PolygonMode == VK_TRUE;
        offer.color_blend_enable   = candidate_dynamic_state3.extendedDynamicState3ColorBlendEnable == VK_TRUE;
        offer.color_blend_equation = candidate_dynamic_state3.extendedDynamicState3ColorBlendEquation == VK_TRUE;

        // THE EXTENSION LIST, scanned ONCE per candidate: it answers whether the EDS3 floor's extension exists AND which
        // of the two OPTIONAL extensions this device offers (the swap chain, and creation feedback on a device below
        // 1.3 - which the floor refuses anyway, so the flag is kept truthful rather than assumed).
        std::uint32_t                      available = 0;
        std::vector<VkExtensionProperties> extensions;
        if (vkEnumerateDeviceExtensionProperties(candidate, nullptr, &available, nullptr) == VK_SUCCESS && available > 0U) {
            extensions.resize(available);
            if (vkEnumerateDeviceExtensionProperties(candidate, nullptr, &available, extensions.data()) != VK_SUCCESS) {
                extensions.clear();
            }
        }
        bool creation_feedback_extension = false;
        for (const VkExtensionProperties& extension : extensions) {
            if (std::strcmp(extension.extensionName, "VK_KHR_swapchain") == 0) {
                facts.has_swapchain = true;
            }
            else if (std::strcmp(extension.extensionName, "VK_EXT_pipeline_creation_feedback") == 0) {
                creation_feedback_extension = true;
            }
            else if (std::strcmp(extension.extensionName, VK_EXT_EXTENDED_DYNAMIC_STATE_3_EXTENSION_NAME) == 0) {
                offer.extended_dynamic_state3 = true;
            }
        }
        // CREATION FEEDBACK IS CORE 1.3 (`VkPipelineCreationFeedbackCreateInfo`): a device this backend accepts always
        // answers it, and the extension only exists for devices the floor refuses.
        facts.has_creation_feedback = candidate_properties.apiVersion >= VK_API_VERSION_1_3 || creation_feedback_extension;

        facts.refusal = candidateRefusal(offer);
        if (facts.refusal != FloorRefusal::None) {
            // FILTERED OUT, AND KEPT FOR THE SENTENCE BELOW: a refused candidate is not the machine's answer while
            // another may serve, so nothing is reported here - but if NOBODY serves, the refusal of the highest-ranked
            // device is what the host is told, because that is the device the session would have used.
            if (facts.rank > refused_rank) {
                refused_rank = facts.rank;
                best_refused = facts;
            }
            continue;
        }

        // THE BEST SURVIVOR, not the first one (see deviceTypeRank) and not the fastest device overall (see above): a
        // machine that lists its integrated GPU first must not be the reason the slower device draws every frame.
        if (facts.rank > best_rank) {
            best_rank = facts.rank;
            chosen    = facts;
        }
    }
    if (queue_candidates == 0U) {
        reportDiagnostic(deviceDiagnostic(DiagnosticSeverity::Error,
                                          DiagnosticCategory::InitFailed,
                                          u8"vk.init.no_queue",
                                          u8"vk: no physical device has a graphics queue family"));
        shutdown();
        return false;
    }
    if (chosen.device == VK_NULL_HANDLE) {
        // NO CANDIDATE SERVES: the diagnostic names the floor the BEST device missed - a fact about the machine - and
        // not "this device is older" about a device the backend picked. A machine with a capable second GPU never
        // reaches this branch any more; it is chosen.
        report_floor(best_refused.refusal);
        shutdown();
        return false;
    }
    impl->physical     = chosen.device;
    impl->queue_family = chosen.queue_family;

    // THE CHOSEN DEVICE'S PROPERTIES, ONCE: the pipeline cache's key and the capabilities all describe the SAME device,
    // so they read one value (see PipelineCacheKey).
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

    // THE EXTENSIONS THE CHOSEN DEVICE IS ASKED FOR, from what the filter proved about it (see the candidate scan):
    //  - VK_KHR_swapchain, only when a window is to be served. Without it this device presents nothing: a warning, and
    //    a device that still runs off-screen frames.
    //  - VK_EXT_extended_dynamic_state3, which IS the raster floor's extension (`T54`).
    // Creation feedback needs no extension: it is core 1.3, and the floor is 1.3 - `impl->creation_feedback` carries
    // what the session can rely on, and a 1.3 device always can.
    std::vector<const char*> device_extension_names;
    if (impl->presentation_wanted) {
        if (chosen.has_swapchain) {
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
    impl->creation_feedback = chosen.has_creation_feedback;

    // WHAT THE FILTER PROVED IS WHAT GETS ENABLED (`G4`, see DeviceChoice.hpp): the 1.3 features the recording path uses
    // (`vkCmdPipelineBarrier2`, dynamic rendering) and the EDS3 switches the raster state commands. "Accepted" and
    // "enabled" are therefore one statement - a device cannot pass the filter and then be created without them, which
    // was a second chance for the two to disagree when the checks ran after selection.
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
    case FrameDriver::CreateRefusal::CommandBuffer:
        reportDiagnostic(deviceDiagnostic(DiagnosticSeverity::Error,
                                          DiagnosticCategory::InitFailed,
                                          u8"vk.init.command_machinery_failed",
                                          u8"vk: vkAllocateCommandBuffers failed"));
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

    // THE PARK QUEUE'S WINDOW: a parked object must outlive the slot that could still name it, so the queue needs the
    // number of frames this device may have in flight. No driver has to exist for that - the watermark counts the
    // device's own submissions (see DeviceClock).
    impl->retirement.setSlots(Impl::kFramesInFlight);
    // THE PARK SEAM, not the queue itself: the caches ask the DEVICE "when may this go?", and the device answers with
    // its clock and its queue (see Impl::releaseLater). That is what lets two drivers share one clock safely.
    impl->caches.setRetirement(
        [impl = impl.get()](std::weak_ptr<const void> named, std::function<void()> destroy) { impl->releaseLater(std::move(named), std::move(destroy)); });

    // THE TARGET TABLE'S SESSION (see TargetTable::open): the same park seam the caches got, plus the device facts a
    // build compares against - the sample counts, whether a multisampled depth resolves, and WHICH COLOUR FORMATS this
    // device backs as targets. The format answer is asked ONCE here (`vkGetPhysicalDeviceFormatProperties` per value),
    // so a build's check is an AND against the mask rather than a driver call on the frame path. A release parks
    // THROUGH the table, so "the entry left the map" and "the images wait for the watermark" are one call and no
    // caller can do half of it.
    impl->target_table.open(
        TargetTable::Device{ impl->device, &impl->device_table, &impl->allocator },
        TargetTable::Limits{ impl->supported_sample_counts, impl->depth_resolve_supported, buildableColorFormats(impl->physical) },
        [this](const vn::graphics::RenderDiagnostic& diagnostic) { reportDiagnostic(diagnostic); },
        [impl = impl.get()](std::shared_ptr<TargetObjects> doomed) {
            // THE RECORD IS THE SUBJECT OF THE RELEASE (see Impl::releaseLater): a pass handle or a
            // record some frame is reading may still name it, so the parked act asks before it
            // destroys - a release removes the target from the table, it does not pull the images
            // out from under a frame that already resolved them.
            const std::weak_ptr<const void> named = doomed;
            impl->releaseLater(named, [table = impl->device_table, device = impl->device, allocator = &impl->allocator, doomed = std::move(doomed)]() mutable {
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

void Device::refuseNextSubmit() noexcept
{
    impl->refuse_submit.store(true, std::memory_order_relaxed);
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
    // THIS VERB BUILDS WHAT IT ANNOUNCES (BC-09, see RenderDevice::setRenderTarget and the reference backend):
    // "a target can be built without a frame" is one ability in both backends, so a later frame that names this
    // target finds its attachments built and builds nothing. The table reports its own refusal once and answers no
    // record; the announcement is then not taken (the host learns at the call site, not on the first frame).
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

std::optional<std::span<const std::byte>>
Device::copyImageToHost(ReadbackStaging& staging, VkImage image, VkImageAspectFlags aspect, const ImageLayout current, const VkExtent3D extent)
{
    // RECORD: the image's OWN layout -> transfer source -> copy -> THE SAME LAYOUT AGAIN. A readback is a device verb,
    // not a pass: it reads ONE image and leaves it exactly where it found it, so no record changes and no other
    // attachment of the set moves. The starting layout is never `Undefined` - the caller refused a target no pass has
    // drawn into before it got here.
    recordBarrier(impl->device_table, staging.commands(), barrierBetween(image, stateOf(current), transferReadState(), aspect));
    staging.recordCopy(image, aspect, extent);
    recordBarrier(impl->device_table, staging.commands(), barrierBetween(image, transferReadState(), stateOf(current), aspect));

    const StagingRun run = staging.run(impl->queue, impl->queue_submit);
    if (!run.ran()) {
        // The mechanism hands back the VkResults so the session-level fact is classified where every other queue call
        // is (see noteDeviceLost); the read itself answers Failed like any other failed transfer.
        (void)noteDeviceLost(run.submit, "vkQueueSubmit");
        (void)noteDeviceLost(run.wait, "vkWaitForFences");
        return std::nullopt;
    }
    return staging.bytes();
}

vn::graphics::ColorReadback Device::readColorImage(const vn::graphics::ImageRef& image)
{
    // ONE ANSWER: a refusal is a status with NO picture, so a refused read cannot hand out bytes it never wrote.
    const auto fail = [](const ReadbackStatus reason) { return vn::graphics::ColorReadback{ reason, {} }; };

    if (image.kind() != vn::graphics::ImageRef::Kind::Color) {
        return fail(ReadbackStatus::Invalid); // the wrong verb for this image
    }
    const vn::graphics::RenderTarget* target     = image.target();
    const int                         attachment = image.attachment();

    // A READBACK ASKED FROM INSIDE A FRAME WOULD WAIT FOR THAT SAME FRAME (see Impl::body_gate): it is refused, not
    // hung. `Failed` says the verb did not run; a hook or a sink calling back into the device is a host protocol
    // violation, not a device fact worth a report on the frame's diagnostic channel.
    if (impl->body_gate.heldByThisThread()) {
        return fail(ReadbackStatus::Failed);
    }
    if (!initialized()) {
        return fail(ReadbackStatus::Failed);
    }
    if (target == nullptr) {
        return fail(ReadbackStatus::Invalid);
    }

    // THE ANSWERS A SNAPSHOT CAN GIVE COME BEFORE THE WAIT: a target this device never built, an attachment that does
    // not exist, a picture no pass has drawn, a format the readback contract does not serve - and the device's own
    // ability to copy to the host at all, which is a property of its memory types rather than of this target. A frame
    // may rebuild the target while this verb waits below, so every answer is checked AGAIN under the gate; this pass
    // only spares the device the stop, which is the rule a refusal owes a caller.
    {
        // A VALUE, NOT THE RECORD (see TargetTable::peek): the record is rebuilt in place and destroyed outside the
        // table, so a field read through a borrowed pointer has no synchronisation to point at (audit A3-01). One
        // snapshot under the table's own lock answers every refusal below, and nothing here keeps a piece of the
        // record across the wait that follows.
        const TargetTable::Peek peek = impl->target_table.peek(target);
        if (!peek.built) {
            return fail(ReadbackStatus::NotReady);
        }
        if (attachment < 0 || static_cast<std::size_t>(attachment) >= peek.shape.color_formats.size()) {
            return fail(ReadbackStatus::Invalid);
        }
        if (peek.fresh) {
            // BUILT IS NOT DRAWN: the images exist now that `setRenderTarget` builds the announce, but their
            // contents are UNDEFINED until a pass writes them, and "undefined" is not a picture. Asked after the
            // range check, so an index the target does not have is Invalid whether or not anything drew.
            return fail(ReadbackStatus::NotReady);
        }
        if (!colorBytesPerTexel(peek.shape.color_formats[static_cast<std::size_t>(attachment)]).has_value()) {
            return fail(ReadbackStatus::Unsupported);
        }
        if (!impl->allocator.supportsHostVisible()) {
            return fail(ReadbackStatus::Unsupported); // no host-visible coherent type: this device cannot copy to the host
        }
    }

    // THE WAIT THE DEVICE NAMES FOR THE HOST, AND IT IS COUNTED (see Impl::body_gate): with several drivers, "between
    // frames" is not a moment the host can see, so the gate says "no frame body runs while this verb runs" - it waits
    // for the frames in flight, not for a session. THE COMMAND MACHINERY IS THE READBACK'S OWN (see ReadbackStaging):
    // nothing here borrows a driver's command pool, fence or ring.
    const vn::graphics::FrameGate::Exclusive device_lock = impl->body_gate.enterExclusive();
    if (!device_lock) {
        return fail(ReadbackStatus::Failed); // the guard above covers this thread; a refused gate is still a refusal
    }
    {
        // ONE COUNTED DEVICE STOP per readback (F7): this is the counter the host reads, and the same counter the
        // retired backend raised with its readback idle (G6). A refusal answered above never reaches this line.
        const std::lock_guard<std::mutex> clock_held(impl->clock_mutex);
        impl->retirement.noteDeviceWait();
    }
    if (!initialized()) {
        return fail(ReadbackStatus::Failed); // the device went down while this verb waited for the gate
    }

    // THE AUTHORITATIVE LOOK: the frames that ran while this verb waited may have rebuilt or replaced the target.
    const std::shared_ptr<TargetObjects> entry = impl->target_table.lookup(target);
    if (entry == nullptr || !entry->built()) {
        return fail(ReadbackStatus::NotReady);
    }
    TargetObjects& images = *entry;
    if (attachment < 0 || static_cast<std::size_t>(attachment) >= images.colorCount()) {
        return fail(ReadbackStatus::Invalid);
    }
    if (images.fresh.load(std::memory_order_relaxed)) {
        return fail(ReadbackStatus::NotReady); // built but never drawn (see the pre-gate check's note)
    }

    // ONE TABLE SAYS WHAT A COLOUR READBACK IS, and it is the SDK's - the same one a host indexes the answer with, so
    // the copy and the reader cannot disagree about a texel's width. An 8-bit attachment is one byte per channel (1, 2
    // or 4), so a mask reads back like a picture does; a float format has no packing there and is refused before the
    // device stops.
    const std::size_t                  attachment_index = static_cast<std::size_t>(attachment);
    const std::optional<std::uint32_t> texel_width      = colorBytesPerTexel(images.desc.shape.color_formats[attachment_index]);
    if (!texel_width.has_value()) {
        return fail(ReadbackStatus::Unsupported);
    }

    const VkExtent3D   extent{ static_cast<std::uint32_t>(images.width()), static_cast<std::uint32_t>(images.height()), 1U };
    const VkDeviceSize bytes = static_cast<VkDeviceSize>(extent.width) * static_cast<VkDeviceSize>(extent.height) * static_cast<VkDeviceSize>(*texel_width);

    // THE COPY MECHANISM IS ReadbackStaging's (its own command pool and fence, the staging buffer, submit, wait,
    // mapping); this function says WHAT to copy and what the bytes mean. Where each picture is stays this backend's
    // knowledge (the per-image `color_layouts` records), which `copyImageToHost` reads.
    ReadbackStaging staging;
    const StagingRefusal opened =
        staging.open(impl->device_table, impl->device, impl->allocator, impl->queue_family, bytes);
    if (opened == StagingRefusal::Unsupported) {
        return fail(ReadbackStatus::Unsupported); // no host-visible coherent memory: this device cannot copy to the host
    }
    if (opened != StagingRefusal::Ok) {
        return fail(ReadbackStatus::Failed);
    }

    const std::optional<std::span<const std::byte>> copied =
        copyImageToHost(staging, images.colors[attachment_index], VK_IMAGE_ASPECT_COLOR_BIT, images.color_layouts[attachment_index], extent);
    if (!copied.has_value() || copied->size() != static_cast<std::size_t>(bytes)) {
        return fail(ReadbackStatus::Failed); // a copy that is not a whole picture is not a picture of anything
    }

    // THE ANSWER OWNS THE PICTURE, and it is built only once there IS one: a refusal hands back an empty vector, so a
    // caller cannot read bytes this call never wrote (the SDK's rule, see ReadbackResult).
    std::vector<std::uint8_t> pixels(static_cast<std::size_t>(bytes));
    std::memcpy(pixels.data(), copied->data(), static_cast<std::size_t>(bytes));
    return vn::graphics::ColorReadback{ ReadbackStatus::Ok, std::move(pixels) };
}

vn::graphics::DepthReadback Device::readDepthImage(const vn::graphics::ImageRef& image)
{
    // ONE ANSWER, like the colour readback: a refusal carries no picture at all (see RenderDevice::readDepthImage).
    const auto fail = [](const ReadbackStatus reason) { return vn::graphics::DepthReadback{ reason, {} }; };

    if (image.kind() != vn::graphics::ImageRef::Kind::Depth) {
        return fail(ReadbackStatus::Invalid); // the wrong verb for this image
    }
    const vn::graphics::RenderTarget* target = image.target();

    // A READBACK ASKED FROM INSIDE A FRAME WOULD WAIT FOR THAT SAME FRAME (see Impl::body_gate): refused, not hung (the
    // colour readback says the same; `Failed` means the verb did not run).
    if (impl->body_gate.heldByThisThread()) {
        return fail(ReadbackStatus::Failed);
    }
    if (!initialized()) {
        return fail(ReadbackStatus::Failed);
    }
    if (target == nullptr) {
        return fail(ReadbackStatus::Invalid);
    }

    // THE ANSWERS A SNAPSHOT CAN GIVE COME BEFORE THE WAIT, and every one of them is checked again under the gate below
    // (a frame may rebuild the target while this verb waits). A BORROWED DEPTH NAMES NOTHING ON THIS TARGET - the image
    // belongs to the lender (RenderTarget::shareDepth) - so the caller's fix is to read the SOURCE target, and the
    // answer is `Invalid` rather than "this device cannot read depth". D24 is Vulkan's COMBINED depth/stencil format
    // with no plain depth copy, so it is refused rather than decoded into numbers that look like depths and are not.
    {
        // THE SAME VALUE-OF-RECORD RULE as readColorImage's phase one (see TargetTable::peek), and the depth format is
        // the one the ATTACHMENTS were built with rather than whatever the host object declares now: the hint has to
        // describe the image a copy would read.
        const TargetTable::Peek peek = impl->target_table.peek(target);
        if (!peek.built) {
            return fail(ReadbackStatus::NotReady);
        }
        if (target->depthSource() != nullptr) {
            return fail(ReadbackStatus::Invalid);
        }
        if (!peek.shape.depth_format.has_value()) {
            return fail(ReadbackStatus::Invalid);
        }
        if (peek.fresh) {
            return fail(ReadbackStatus::NotReady); // built but never drawn: the depth is UNDEFINED
        }
        if (!depthBytesPerTexel(*peek.shape.depth_format).has_value()) {
            return fail(ReadbackStatus::Unsupported);
        }
        if (!impl->allocator.supportsHostVisible()) {
            return fail(ReadbackStatus::Unsupported); // no host-visible coherent type: this device cannot copy to the host
        }
    }

    // THE WAIT THE DEVICE NAMES FOR THE HOST, AND IT IS COUNTED, exactly as in readColorImage: the frames in flight are
    // what the gate waits for, and the command machinery below is the readback's own (see ReadbackStaging).
    const vn::graphics::FrameGate::Exclusive device_lock = impl->body_gate.enterExclusive();
    if (!device_lock) {
        return fail(ReadbackStatus::Failed); // the guard above covers this thread; a refused gate is still a refusal
    }
    {
        const std::lock_guard<std::mutex> clock_held(impl->clock_mutex);
        impl->retirement.noteDeviceWait();
    }
    if (!initialized()) {
        return fail(ReadbackStatus::Failed); // the device went down while this verb waited for the gate
    }

    // THE AUTHORITATIVE LOOK: the frames that ran while this verb waited may have rebuilt or replaced the target.
    const std::shared_ptr<TargetObjects> entry = impl->target_table.lookup(target);
    if (entry == nullptr || !entry->built()) {
        return fail(ReadbackStatus::NotReady);
    }
    if (target->depthSource() != nullptr) {
        return fail(ReadbackStatus::Invalid); // a borrowed depth is the lender's image (see the pre-gate note)
    }
    TargetObjects& images = *entry;

    // ONE SOURCE FOR THE DEPTH FORMAT: the shape the ATTACHMENTS were built with, never `RenderTarget::depthFormat()`
    // (that is the host's current declaration, which may have changed since the build while the image did not). The
    // same value decides the texel width, the aspect mask and the decode, so the three cannot disagree.
    const std::optional<vn::graphics::RenderTarget::DepthFormat> format = images.desc.shape.depth_format;
    if (!format.has_value()) {
        return fail(ReadbackStatus::Invalid); // no depth attachment at all: the request names nothing
    }
    if (images.fresh.load(std::memory_order_relaxed)) {
        return fail(ReadbackStatus::NotReady); // built but never drawn (see the pre-gate check's note)
    }
    const std::optional<std::uint32_t> texel_width = depthBytesPerTexel(*format);
    if (!texel_width.has_value()) {
        return fail(ReadbackStatus::Unsupported);
    }

    const VkExtent3D extent{ static_cast<std::uint32_t>(images.width()), static_cast<std::uint32_t>(images.height()), 1U };
    const VkDeviceSize bytes = static_cast<VkDeviceSize>(extent.width) * static_cast<VkDeviceSize>(extent.height) * static_cast<VkDeviceSize>(*texel_width);

    ReadbackStaging staging;
    const StagingRefusal opened =
        staging.open(impl->device_table, impl->device, impl->allocator, impl->queue_family, bytes);
    if (opened == StagingRefusal::Unsupported) {
        return fail(ReadbackStatus::Unsupported); // no host-visible coherent memory: this device cannot copy to the host
    }
    if (opened != StagingRefusal::Ok) {
        return fail(ReadbackStatus::Failed);
    }

    // THE PICTURE IS THE RESOLVED SINGLE-SAMPLE IMAGE (see TargetObjects::depthPicture), never the sample buffer of a
    // multisampled target - multisampling cannot change the depths a reader sees. A BARRIER NAMES BOTH HALVES of a
    // combined image (see depthAspectMask) while the COPY names only the depth half; a combined format has no plain
    // depth copy and was refused above, so the mask is the depth bit in practice.
    const std::optional<std::span<const std::byte>> copied =
        copyImageToHost(staging, images.depthPicture(), depthAspectMask(images.depthFormat()), images.depthPictureLayout(), extent);
    if (!copied.has_value()) {
        return fail(ReadbackStatus::Failed);
    }

    // THE DECODER IS THE SDK'S TOO: one table says how many bytes a texel is, one function turns them into the
    // normalised values a probe answers with (D32/D32F arrive as their float, D16 divided by its full scale). A copy
    // that is not a whole image is not a picture of anything, so a short decode is a failure - and the answer carries
    // the values only once there IS a picture.
    std::vector<float> values = decodeDepth(*format, *copied);
    if (values.size() != static_cast<std::size_t>(extent.width) * static_cast<std::size_t>(extent.height)) {
        return fail(ReadbackStatus::Failed);
    }
    return vn::graphics::DepthReadback{ ReadbackStatus::Ok, std::move(values) };
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

Device::RecordingNumbers Device::recordingNumbers() const noexcept
{
    RecordingNumbers numbers;
    if (impl->session == nullptr) {
        return numbers;
    }
    numbers.barrier_dependencies = impl->session->barrier_tally.dependencies;
    numbers.barriers             = impl->session->barrier_tally.barriers;
    numbers.state_commands       = impl->session->recorder.stateCommands();
    numbers.debug_labels         = impl->session->debug_labels;
    return numbers;
}

bool Device::debugMarkers() const noexcept
{
    return impl->debug_label_begin != nullptr;
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
