#pragma once

#include <cstdint>
#include <functional>
#include <memory>

#include <vine/backend_vk/backend_vk_global.hpp>

#include <volk.h>

#include <vine/String.hpp>

VN_BACKEND_VK_NS_BEGIN

/**
 * @brief What a slice of device memory is FOR: the one fact the allocator turns into a Vulkan memory TYPE.
 *
 * @details
 * - A CALLER STATES A PURPOSE, NOT PROPERTY FLAGS. Every allocation site in this backend used to search
 *   `VkPhysicalDeviceMemoryProperties` for its own set of bits (`DEVICE_LOCAL`, or `HOST_VISIBLE|HOST_COHERENT`),
 *   which put the same rule in five files and let them drift. The purpose is what the code actually knows; the type
 *   that serves it is the device's answer (see MemoryAllocator::open).
 * - `HostVisible` additionally REQUIRES `HOST_COHERENT`: this backend never flushes or invalidates a range, so a
 *   memory type that needs either is not a type it can use (the search says so instead of silently picking one).
 */
enum class MemoryUsage : std::uint8_t
{
    DeviceLocal, ///< Read and written by the GPU: render-target attachments, texture images, vertex/index/uniform buffers.
    HostVisible, ///< Also written (or read) by the host: staging buffers, the uniform ring, a readback's bytes.
};

/**
 * @brief What KIND of resource a slice holds.
 *
 * @details
 * - THIS EXISTS FOR ONE RULE. Vulkan's `bufferImageGranularity` forbids a LINEAR resource (a buffer) and a NON-LINEAR
 *   one (an optimally tiled image) from sharing a granularity-sized page of the same `VkDeviceMemory`, so an allocator
 *   that packs both kinds into one block has to know which is which (see MemoryAllocator::allocate). It is the whole
 *   of what the allocator needs to know about a resource beyond its `VkMemoryRequirements`.
 * - `Linear` is also the right spelling for a linearly tiled image. This backend creates none today; a future one
 *   states `Linear`, which is what the rule asks about.
 */
enum class MemoryKind : std::uint8_t
{
    Linear,  ///< A buffer (or a linearly tiled image).
    Optimal, ///< A resource with an implementation-defined layout: every image this backend creates is optimal-tiled.
};

/** @brief Why an allocation was refused (see MemoryAllocator::allocate). */
enum class MemoryRefusal : std::uint8_t
{
    Ok,             ///< The slice exists.
    NoType,         ///< No memory type the resource allows serves the requested usage - this device cannot host it.
    DeviceLost,     ///< `VK_ERROR_DEVICE_LOST`: the session is over (the device's ONE classification point handles it).
    OutOfMemory,    ///< `VK_ERROR_OUT_OF_DEVICE_MEMORY` / `VK_ERROR_OUT_OF_HOST_MEMORY`: releasing content may help.
    DriverRefused,  ///< Any other `VkResult` from `vkAllocateMemory`: a device limit, a bad handle, a driver bug.
    InvalidRequest, ///< The request itself is unusable (a zero-byte resource): a defect in the caller.
    NotOpen,        ///< `allocate` ran before `open`: a wiring defect, not a device fact.
    Internal,       ///< The pool contradicted its own invariant (a placement failed in a block it had just made).
};

/** @brief The memory type a request resolved to when it resolved to none (see MemoryRefusalReport). */
inline constexpr std::uint32_t kNoMemoryType = ~0U;

/**
 * @brief A resource's own piece of device memory: an OFFSET into a block the allocator owns.
 *
 * @details
 * - WHAT IT REPLACES. Every resource in this backend used to own a whole `VkDeviceMemory` and bind it at offset 0.
 *   The slice is the same idea one level down: the resource owns a RANGE, the allocator owns the allocation. Binding
 *   is `vkBind*Memory(handle, slice.memory, slice.offset)` - the offset is not decoration, it is the fix.
 * - `memory` AND `block` IDENTIFY THE OWNER. `memory` is what Vulkan is handed; `block` is the allocator's own id for
 *   the block, which is what `release` needs and Vulkan does not have a word for.
 * - A SLICE IS SINGLE-OWNER, like the handle it replaces: one `release` per slice, and `release` clears it (see
 *   MemoryAllocator::release). Copying a slice does not create a second allocation - and releasing both is a
 *   double-free the allocator refuses by refusing the second one.
 * - `valid()` answers whether it names memory; `size` is the allocation the block carved, which may exceed a
 *   resource's own `VkMemoryRequirements::size` only by the alignment the block had to honour.
 */
struct MemorySlice
{
    VkDeviceMemory memory{ VK_NULL_HANDLE }; ///< The block's memory object (owned by the allocator, valid until close).
    VkDeviceSize   offset{ 0 };              ///< Where the range starts inside that block.
    VkDeviceSize   size{ 0 };                ///< How many bytes the range is.
    std::uint32_t  block{ 0 };               ///< The allocator's id of the block (see MemoryAllocator::release).

    /** @brief Answers whether this slice names memory (a cleared slice, or one a refused allocate returned, does not). */
    [[nodiscard]] bool valid() const noexcept
    {
        return memory != VK_NULL_HANDLE;
    }
};

/**
 * @brief What the allocator did: the witness a test reads instead of counting driver calls from outside.
 *
 * @details
 * - CUMULATIVE VERSUS LIVE, the same split `ContentImages::Progress` follows: `blocks` / `allocations` / `releases`
 *   describe the session, `blocks_live` / `reserved_bytes` / `used_bytes` describe NOW.
 * - THE TWO NUMBERS THAT SAY "SUBALLOCATION WORKS" ARE `allocations` AND `blocks`: a scene that builds a thousand
 *   resources asks for a thousand allocations and must not create a thousand blocks. `maxMemoryAllocationCount` is a
 *   per-device limit (the spec's floor is 4096), so the count of BLOCKS is the number that has to stay small.
 * - `reserved_bytes - used_bytes` is what the pool holds beyond what resources need: alignment rounding, the tail of
 *   a block nobody has reached yet, and blocks kept warm after a release. It is a real cost, not a leak - `used_bytes`
 *   answers 0 once everything was released.
 */
struct MemoryStats
{
    std::uint64_t blocks{ 0 };          ///< Blocks created over this allocator's lifetime.
    std::uint32_t blocks_live{ 0 };     ///< Blocks owned NOW.
    std::uint64_t reserved_bytes{ 0 };  ///< Bytes those blocks hold NOW.
    std::uint64_t used_bytes{ 0 };      ///< Bytes handed out and not released NOW.
    std::uint64_t peak_used_bytes{ 0 }; ///< The high-water mark of `used_bytes`.
    std::uint64_t allocations{ 0 };     ///< Slices handed out over this allocator's lifetime.
    std::uint64_t releases{ 0 };        ///< Slices released over this allocator's lifetime.
};

/**
 * @brief What one allocation asks the REPORTING layer to say, in numbers (see MemoryAllocator::setFailureSink).
 *
 * @details
 * - WHY THIS IS NOT THE RETURN VALUE. The returned slice answers "may I use this range" - control flow. This value
 *   answers "why not, and with which numbers" - reporting. They have different consumers, and the defect this split
 *   removes is real: five allocation sites used to be handed a reason and threw it away, so a **lost device read as
 *   out of memory** all the way to the host. A report that travels to a sink cannot be forgotten by a call site,
 *   because no call site sees it.
 * - `Ok` IS REPORTED TOO: it is how the sink learns that the condition ended and re-arms its episodes, which is the
 *   repository's rule for `ReportOnce` (re-arm where the condition ends, not at shutdown).
 * - IT IS A STACK VALUE, built per allocation and only read when a sink is installed: no allocation, no virtual call
 *   on the success path beyond the sink itself (see the class note's cost model).
 */
struct MemoryRefusalReport
{
    MemoryRefusal refusal{ MemoryRefusal::Ok };      ///< What kind of answer this is.
    VkResult      driver_result{ VK_SUCCESS };       ///< `vkAllocateMemory`'s own answer when a block was attempted.
    MemoryUsage   usage{ MemoryUsage::DeviceLocal }; ///< The purpose the caller stated.
    MemoryKind    kind{ MemoryKind::Optimal };       ///< The kind the caller stated.
    VkDeviceSize  requested_bytes{ 0 };              ///< The resource's `VkMemoryRequirements::size`.
    std::uint32_t type_index{ kNoMemoryType };       ///< The memory type it resolved to, or `kNoMemoryType`.
    MemoryStats   stats{};                           ///< The pool's numbers AT this refusal (see MemoryStats).
};

/** @brief Receives every allocation's outcome: the ONE place a refusal becomes a sentence (see MemoryAllocator). */
using MemoryFailureSink = std::function<void(const MemoryRefusalReport&)>;

/**
 * @brief Creates one block's `VkDeviceMemory`: the ONE device call this unit makes, as an injectable seam.
 *
 * @details
 * - WHY IT EXISTS. Every failure class of this unit - a lost device, an exhausted heap, a driver that refuses for its
 *   own reason - is UNREACHABLE on a healthy machine, so the tests that prove the classification cannot exist without
 *   a way to make the call fail. An empty creator is the production path (`vkAllocateMemory` through the device
 *   table); a test installs one that answers the result it wants to see classified.
 * - IT IS THE SAME SHAPE AS THE `VkDevice` HANDLE THIS UNIT ALREADY HOLDS: a borrowed capability, not an owned
 *   dependency. It costs one indirect call per BLOCK (not per resource), which is a handful per session.
 */
using MemoryBlockCreator = std::function<VkResult(VkDevice, const VkMemoryAllocateInfo&, VkDeviceMemory*)>;

/**
 * @brief The starting size of a block, per usage.
 *
 * @details
 * - A BLOCK IS A CACHE, and these are its first-fill amounts: large enough that a scene of hundreds of small
 *   resources reaches the pool a handful of times, small enough that an idle session does not reserve memory it
 *   never uses. `open` caps each at an eighth of the heap the memory type belongs to.
 * - A request larger than its base gets a block sized to the request, so a 256 MiB target does not force every
 *   later block of that usage to be 256 MiB.
 */
struct MemoryBlockSizes
{
    VkDeviceSize device_local{ 64U * 1024U * 1024U }; ///< The first device-local block.
    VkDeviceSize host_visible{ 16U * 1024U * 1024U }; ///< The first host-visible block.
};

/**
 * @brief Device memory as a DEVICE-LEVEL POOL: one owner that creates `VkDeviceMemory` blocks and carves slices out of
 *        them.
 *
 * @details
 * - WHY THIS TYPE EXISTS. `vkAllocateMemory` is not free of limits: the spec's floor for `maxMemoryAllocationCount`
 *   is 4096, and the failure a renderer meets when it reaches that number is `VK_ERROR_OUT_OF_DEVICE_MEMORY` - which
 *   reads as "the GPU is full" while the GPU may be nearly empty. A resource per allocation makes the count scale
 *   with the SCENE (the exact thing a scene is allowed to grow); a block per pool makes it scale with the number of
 *   (memory type, usage) combinations a session actually uses, which is a handful.
 * - WHAT IT OWNS. Every `VkDeviceMemory` it created, the free ranges inside each block, and the mappings of the
 *   host-visible blocks. What a caller owns is a MemorySlice: `release` gives the range back to the pool, `close`
 *   destroys the memory itself.
 * - PLACEMENT IS A RULE, NOT A SEARCH TUNED FOR SPEED. Blocks keep one sorted list of ranges (live and free, always
 *   alternating), so a release merges with its free neighbours and an allocation is a first fit that respects two
 *   constraints: the resource's own `alignment`, and - between ranges of different MemoryKind - the device's
 *   `bufferImageGranularity` (a linear and a non-linear resource may not share one granularity-sized page). That is
 *   the whole algorithm; it is deterministic, and a test can predict every offset it produces.
 * - HOST-VISIBLE MEMORY IS MAPPED ONCE PER BLOCK, not once per slice (see @ref map): the blocks of that usage must be
 *   `HOST_COHERENT` (see MemoryUsage), so a mapping needs no flush and no invalidation, and the one mapping is the
 *   pointer every slice of that block is reached through.
 * - BLOCK SIZES ARE A POLICY, not a rule. A block is the larger of the caller's stated `BlockSizes` entry and what
 *   the request needs (a single request larger than the base gets a block of its own), rounded to 64 KiB; the base is
 *   capped at an eighth of the memory type's heap so a small device does not see a block it can never fill. A block
 *   that becomes entirely free is destroyed when another block of its type is left, which bounds what a one-off
 *   large resource (a big readback) keeps reserved for the rest of the session.
 *
 * @warning
 * - Not thread-safe: one frame at a time, on the thread that drives the device (the backend's contract).
 * - A slice must be released before @ref close; close frees the blocks regardless, so a missed release is a leak the
 *   validation layer reports at device destruction.
 */
class VN_BACKEND_VK_API MemoryAllocator final
{
  public:
    /** @brief Builds a closed allocator: no block exists until @ref open. */
    MemoryAllocator();

    /** @brief Releases everything @ref close would (see @ref close). */
    ~MemoryAllocator();

    MemoryAllocator(const MemoryAllocator&)            = delete;
    MemoryAllocator& operator=(const MemoryAllocator&) = delete;
    MemoryAllocator(MemoryAllocator&&)                 = delete;
    MemoryAllocator& operator=(MemoryAllocator&&)      = delete;

  public:
    /**
     * @brief Binds the allocator to a logical device; blocks are created lazily by @ref allocate.
     *
     * @details
     * - THE DEVICE FACTS IT KEEPS are the entry points, the handle, the memory properties (to answer which type serves
     *   a usage, and how large that type's heap is) and `bufferImageGranularity` (the placement rule). None of them is
     *   owned: the allocator must be closed before the device is destroyed.
     * - A SECOND `open` WITHOUT `close` is refused (it would orphan the blocks of the first).
     *
     * @param table        The device's entry points (volk table, borrowed).
     * @param device       The logical device (borrowed).
     * @param memory       The device's memory properties.
     * @param granularity  `VkPhysicalDeviceLimits::bufferImageGranularity`; 0 is read as 1.
     * @param block_sizes  The first-fill sizes per usage (see MemoryBlockSizes).
     * @return true when the allocator is open; false when it already was, or the device is null.
     */
    [[nodiscard]] bool open(const VolkDeviceTable&                  table,
                            VkDevice                                device,
                            const VkPhysicalDeviceMemoryProperties& memory,
                            VkDeviceSize                            granularity,
                            MemoryBlockSizes                        block_sizes = MemoryBlockSizes{});

    /**
     * @brief Unmaps and frees every block, leaving the allocator closed (idempotent).
     *
     * @warning
     * - Every slice must have been released: the memory goes away here whatever the resources still name.
     */
    void close() noexcept;

    /** @brief Answers whether @ref open has succeeded and @ref close has not run since. */
    [[nodiscard]] bool opened() const noexcept;

    /**
     * @brief Carves a range that satisfies @p requirements out of a block, creating a block when none fits.
     *
     * @details
     * - THE SEARCH, IN ORDER: a block of the memory type the usage resolves to that has a free range the request fits
     *   in (first fit, honouring alignment and - between different kinds - `bufferImageGranularity`); otherwise a new
     *   block, which is then guaranteed to fit. A block that fits is always preferred, because creating one is a
     *   device call and reaching into one is not.
     * - A request larger than the usage's base size gets a block sized to the request, so one huge resource does not
     *   change the policy for the next small one.
     *
     * @param requirements What Vulkan said the resource needs (size, alignment, allowed type bits).
     * @param usage        What the memory is for (decides the memory type).
     * @param kind         Whether the resource is linear or optimally tiled (decides the granularity rule).
     * @param refusal      Receives why the answer is empty, or null when the caller does not branch on the reason.
     * @return The slice, or an invalid one (`valid()` is false).
     *
     * @note
     * - THE REASON IS ALSO SENT TO THE FAILURE SINK (see setFailureSink), which is where it becomes a diagnostic. A
     *   caller that passes @p refusal branches on it; a caller that does not still cannot lose it.
     */
    [[nodiscard]] MemorySlice allocate(const VkMemoryRequirements& requirements,
                                       MemoryUsage                 usage,
                                       MemoryKind                  kind,
                                       MemoryRefusal*              refusal = nullptr);

    /**
     * @brief Returns @p slice to its block, merging it with free neighbours; clears the slice (idempotent).
     *
     * @details
     * - A SLICE THAT IS ALREADY CLEAR IS NOT AN ERROR: destructors and failure paths may release unconditionally, and
     *   `valid()` is exactly the test they make. A slice naming a block this allocator does not own is ignored too -
     *   it can only come from a copy released twice, and refusing it loudly would be a new crash path in teardown.
     * - A BLOCK THAT BECOMES ENTIRELY FREE is destroyed when the pool keeps another block of the same memory type; the
     *   last one of its type stays, so the common "build, release, build" cycle does not allocate memory twice.
     *
     * @param slice The slice to release (cleared before this returns).
     */
    void release(MemorySlice& slice) noexcept;

    /**
     * @brief Gets the host address @p slice starts at, mapping its block on first use.
     *
     * @details
     * - THE WHOLE BLOCK IS MAPPED, ONCE (see the class note): a `HOST_COHERENT` mapping needs no flush, so one mapping
     *   per block serves every slice of it, and callers never see an offset - they get the address of their own bytes.
     * - A mapping that fails is answered as null and not retried on this call path: the next call tries again, because
     *   "the driver was busy" is a real answer and a permanently poisoned block would be worse.
     *
     * @param slice The slice to reach (must be memory this allocator owns, of a `HOST_VISIBLE` memory type).
     * @return The address of the slice's first byte, or null when the slice is not host-visible or the map failed.
     */
    [[nodiscard]] void* map(const MemorySlice& slice) noexcept;

    /** @brief Gets the pool's numbers (see MemoryStats); zeros once closed. */
    [[nodiscard]] MemoryStats stats() const noexcept;

    /**
     * @brief Installs the one consumer of every allocation's outcome: the reporting layer (see MemoryRefusalReport).
     *
     * @details
     * - WHY THE POOL REPORTS INSTEAD OF RETURNING A REASON TO EACH CALLER. `allocate` has six call sites and five of
     *   them only need "did it work"; handing the reason back made "why" something each site had to remember to use,
     *   and they did not - a device lost during an allocation was reported as an out-of-memory, or not at all. With a
     *   sink the reason has exactly one consumer, the device, which is the only party that knows what a lost device
     *   means (see `Device::noteDeviceLost`).
     * - IT IS CALLED FOR EVERY ALLOCATION, `Ok` included: that is how the consumer re-arms an episode when the
     *   condition ends. The cost is one indirect call per allocation, and allocations are a miss-path event (a steady
     *   frame allocates nothing - see `R24`).
     * - The sink is borrowed and must outlive this allocator's allocations; an empty sink keeps the classification in
     *   the return value only.
     *
     * @param sink Consumer of every report, or empty to clear.
     */
    void setFailureSink(MemoryFailureSink sink);

    /**
     * @brief Installs the seam that creates a block's memory (see MemoryBlockCreator).
     *
     * @details
     * - Empty is the production path. A test installs one to reach the failure classes this unit classifies - a lost
     *   device, an exhausted heap, a refused driver call - which a healthy machine never produces.
     * - It is read when a BLOCK is created, so it may be installed after @ref open and before the first resource.
     *
     * @param creator The block creator, or empty for `vkAllocateMemory`.
     */
    void setBlockCreator(MemoryBlockCreator creator);

  private:
    struct Impl;
    std::unique_ptr<Impl> impl;
};

VN_BACKEND_VK_NS_END
