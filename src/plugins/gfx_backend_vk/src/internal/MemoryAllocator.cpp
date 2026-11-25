#include <vine/backend_vk/internal/MemoryAllocator.hpp>

#include <algorithm>
#include <mutex>
#include <cstddef>
#include <memory>
#include <optional>
#include <utility>
#include <vector>

#include <vine/String.hpp>

VN_BACKEND_VK_NS_BEGIN

namespace
{

/// @brief The alignment a block's own size is rounded to (it also keeps a slice of a small resource from sharing a page).
constexpr VkDeviceSize kBlockAlignment = 64U * 1024U;

/**
 * @brief Rounds @p value up to a multiple of @p alignment.
 *
 * @param value     The value to round.
 * @param alignment The multiple (1 and 0 round nothing).
 * @return The rounded value.
 */
[[nodiscard]] constexpr VkDeviceSize alignedUp(const VkDeviceSize value, const VkDeviceSize alignment) noexcept
{
    if (alignment <= 1U) {
        return value;
    }
    return ((value + alignment - 1U) / alignment) * alignment;
}

/**
 * @brief Finds a memory type that both the resource allows and the usage needs.
 *
 * @details
 * - THE USAGE IS THE WHOLE SEARCH (see MemoryUsage): `DeviceLocal` needs `DEVICE_LOCAL`, `HostVisible` needs
 *   `HOST_VISIBLE|HOST_COHERENT` - this backend never flushes, so a type that would need it is not a candidate.
 * - The first match wins: the memory type order is the device's own preference, and re-ranking it here would be a
 *   policy nobody asked for.
 *
 * @param memory The device's memory properties.
 * @param bits   The type bits the resource reports.
 * @param usage  What the memory is for.
 * @return The type index, or nullopt when the device has none.
 */
[[nodiscard]] std::optional<std::uint32_t> findMemoryType(const VkPhysicalDeviceMemoryProperties& memory,
                                                          const std::uint32_t                     bits,
                                                          const MemoryUsage                        usage) noexcept
{
    constexpr VkMemoryPropertyFlags kDeviceLocal = VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT;
    constexpr VkMemoryPropertyFlags kHostVisible =
        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
    const VkMemoryPropertyFlags wanted = usage == MemoryUsage::DeviceLocal ? kDeviceLocal : kHostVisible;

    for (std::uint32_t index = 0; index < memory.memoryTypeCount; ++index) {
        if ((bits & (1U << index)) != 0U && (memory.memoryTypes[index].propertyFlags & wanted) == wanted) {
            return index;
        }
    }
    return std::nullopt;
}

/**
 * @brief Gets how many bytes a memory type's heap holds (0 when the properties do not name one).
 *
 * @param memory The device's memory properties.
 * @param type   The memory type index.
 * @return The heap's size, or 0.
 */
[[nodiscard]] VkDeviceSize heapBytesOf(const VkPhysicalDeviceMemoryProperties& memory, const std::uint32_t type) noexcept
{
    if (type >= memory.memoryTypeCount) {
        return 0U;
    }
    const std::uint32_t heap = memory.memoryTypes[type].heapIndex;
    return heap < memory.memoryHeapCount ? memory.memoryHeaps[heap].size : 0U;
}

/**
 * @brief Classifies `vkAllocateMemory`'s answer into the classes a caller ACTS ON (see MemoryRefusal).
 *
 * @details
 * - THE POINT OF THE SPLIT IS THE FIRST TWO CASES: a lost device ends the session (and is the device's one
 *   classification point to handle), while an exhausted heap is a fact a host can act on by releasing content. They
 *   used to be one value, so a lost device was reported as "out of memory" and the session kept running on a dead
 *   device.
 * - Everything else is `DriverRefused` WITH THE RAW RESULT KEPT (see MemoryRefusalReport::driver_result): the sentence
 *   can then name `VK_ERROR_TOO_MANY_OBJECTS` instead of guessing, which is the difference between a report a host can
 *   act on and one it cannot.
 *
 * @param result `vkAllocateMemory`'s answer.
 * @return The class that answer belongs to.
 */
[[nodiscard]] MemoryRefusal refusalOf(const VkResult result) noexcept
{
    switch (result) {
    case VK_SUCCESS: return MemoryRefusal::Ok;
    case VK_ERROR_DEVICE_LOST: return MemoryRefusal::DeviceLost;
    case VK_ERROR_OUT_OF_DEVICE_MEMORY:
    case VK_ERROR_OUT_OF_HOST_MEMORY: return MemoryRefusal::OutOfMemory;
    default: return MemoryRefusal::DriverRefused;
    }
}

} // namespace

struct MemoryAllocator::Impl {
    ///\n    /// THE POOL'S LOCK. Every session draws its resources from this one pool, and two frame drivers grow their
    /// rings and build their targets on two threads (S6c-2): a block list mutated by one while the other walks it is
    /// the coarsest possible race. It is never held across a frame, only across one allocation or release.
    mutable std::mutex lock{};

    /**
     * @brief One contiguous stretch of a block: either handed to a resource (live) or available.
     *
     * @details
     * - A BLOCK KEEPS EVERY RANGE, LIVE AND FREE, in one list sorted by offset - and therefore ALTERNATING: two free
     *   neighbours are always merged into one, so a live range is only ever between two free ones (or the block's
     *   ends). That is what makes a release local: it looks at its two neighbours and merges, no search.
     * - `kind` is the granularity class of a LIVE range (see MemoryKind); a free range's is unused.
     */
    struct Range {
        VkDeviceSize offset{ 0 };
        VkDeviceSize size{ 0 };
        MemoryKind   kind{ MemoryKind::Optimal };
        bool         live{ false };

        /** @brief Gets the first byte past this range. */
        [[nodiscard]] VkDeviceSize end() const noexcept
        {
            return offset + size;
        }
    };

    /// @brief One `VkDeviceMemory`: the allocator's unit of allocation, a resource's unit of binding is a slice of it.
    struct Block {
        std::uint32_t      id{ 0 };           // The allocator's own id (slices carry it; never reused).
        VkDeviceMemory     memory{ VK_NULL_HANDLE };
        std::uint32_t      type_index{ 0 };
        VkDeviceSize       size{ 0 };
        void*              mapped{ nullptr }; // Non-null once the block was mapped (see map).
        std::vector<Range> ranges{};
    };

    const VolkDeviceTable*           table{ nullptr };
    VkDevice                          device{ VK_NULL_HANDLE };
    VkPhysicalDeviceMemoryProperties  memory{};
    VkDeviceSize                      granularity{ 1 };
    MemoryBlockSizes                  block_sizes{};
    std::vector<std::unique_ptr<Block>> blocks{};
    std::uint32_t                     next_block_id{ 1 };

    /// The numbers stats() answers, kept live rather than recomputed (release is the hot path, not this).
    /// @brief Gets the pool's numbers WITH the lock held (the caller owns it; see @c lock).
    [[nodiscard]] MemoryStats statsLocked() const noexcept
    {
        // THE ROOM NUMBERS ARE WALKED, NOT MAINTAINED: `stats` is asked by a report or a test, never per allocation, and
        // keeping a running "largest free run" correct across every split and merge would put bookkeeping on the hot
        // path for a number nobody reads there.
        MemoryStats out = numbers;
        for (const auto& block : blocks) {
            for (const Range& range : block->ranges) {
                if (range.live) {
                    continue;
                }
                out.free_bytes += range.size;
                out.largest_free_run = std::max(out.largest_free_run, range.size);
            }
        }
        return out;
    }

    /// @brief Answers whether the pool can allocate WITH the lock held (the caller owns it).
    [[nodiscard]] bool openedLocked() const noexcept
    {
        return table != nullptr && device != VK_NULL_HANDLE;
    }
    MemoryStats numbers{};

    /// Where every allocation's outcome goes (see setFailureSink); empty keeps the classification in the return value.
    MemoryFailureSink sink{};
    /// How a block's memory is created (see setBlockCreator); empty is `vkAllocateMemory`.
    MemoryBlockCreator creator{};

    /** @brief Finds a block by the id a slice carries (null when this allocator does not own it). */
    [[nodiscard]] Block* blockOf(const std::uint32_t id) noexcept
    {
        for (const auto& block : blocks) {
            if (block->id == id) {
                return block.get();
            }
        }
        return nullptr;
    }

    /**
     * @brief Finds where @p size bytes with @p alignment fit inside the free range at @p index, or says there is no room.
     *
     * @details
     * - TWO CONSTRAINTS, BOTH ABOUT THE RANGE'S NEIGHBOURS: the resource's alignment, and - only against a neighbour of
     *   the OTHER MemoryKind - `bufferImageGranularity`, because a linear and a non-linear resource may not share one
     *   granularity-sized page of the same memory. Granularity is applied by starting at the page after the previous
     *   range and ending the page before the next one, and alignment is re-applied afterwards (a driver is free to
     *   make one a multiple of the other, not obliged to).
     * - WHEN BOTH CANNOT HOLD AT ONCE the answer is "no room in this range", not a compromise: a placement that
     *   violates the granularity rule is a validation error on every real driver.
     *
     * @param block     The block whose range is being filled.
     * @param index     The free range's index in `block.ranges`.
     * @param size      Bytes wanted.
     * @param alignment The resource's alignment.
     * @param kind      The resource's kind (see MemoryKind).
     * @return The offset the resource would start at, or nullopt when the range cannot hold it.
     */
    [[nodiscard]] std::optional<VkDeviceSize> placeAt(const Block&     block,
                                                      const std::size_t index,
                                                      const VkDeviceSize size,
                                                      const VkDeviceSize alignment,
                                                      const MemoryKind  kind) const noexcept
    {
        const Range&       free = block.ranges[index];
        const bool         after_other = index > 0 && block.ranges[index - 1U].kind != kind;
        const bool         before_other = index + 1U < block.ranges.size() && block.ranges[index + 1U].kind != kind;
        const VkDeviceSize after_limit = after_other ? alignedUp(block.ranges[index - 1U].end(), granularity) : 0U;

        VkDeviceSize offset = alignedUp(std::max(free.offset, after_limit), alignment);
        if (offset < after_limit) {
            return std::nullopt; // alignment pushed the start back into the previous range's page
        }
        if (offset + size > free.end()) {
            return std::nullopt;
        }
        if (before_other && alignedUp(offset + size, granularity) > block.ranges[index + 1U].offset) {
            return std::nullopt; // the end would share the next range's page
        }
        return offset;
    }

    /**
     * @brief Writes the live range at @p offset into the free range at @p index, leaving the remainders free.
     *
     * @details
     * - THREE SHAPES, and the invariant (alternating live/free) is what they restore: the live range may start at the
     *   free range's start, end at its end, or sit strictly inside it - in which case the free range splits in two.
     *
     * @param block  The block to fill.
     * @param index  The free range's index.
     * @param offset The placement `placeAt` answered.
     * @param size   Bytes taken.
     * @param kind   The resource's kind.
     */
    static void takeAt(Block& block, const std::size_t index, const VkDeviceSize offset, const VkDeviceSize size, const MemoryKind kind)
    {
        const Range free = block.ranges[index];
        const Range live{ offset, size, kind, true };

        std::vector<Range> replacement;
        if (offset > free.offset) {
            replacement.push_back(Range{ free.offset, offset - free.offset, kind, false });
        }
        replacement.push_back(live);
        if (live.end() < free.end()) {
            replacement.push_back(Range{ live.end(), free.end() - live.end(), kind, false });
        }
        block.ranges.erase(block.ranges.begin() + static_cast<std::ptrdiff_t>(index));
        block.ranges.insert(block.ranges.begin() + static_cast<std::ptrdiff_t>(index), replacement.begin(), replacement.end());
    }

    /**
     * @brief Creates a block of @p type and @p usage that can hold @p size bytes, and records it.
     *
     * @param type     The memory type index.
     * @param usage    What the block is for.
     * @param size     The smallest useful block (a request larger than the base policy).
     * @param refusal  Receives the class of answer when no block could be created.
     * @param driver_result Receives `vkAllocateMemory`'s own answer (or the creator's).
     * @return The new block, or null.
     */
    [[nodiscard]] Block* createBlock(const std::uint32_t type,
                                     const MemoryUsage   usage,
                                     const VkDeviceSize  size,
                                     MemoryRefusal&      refusal,
                                     VkResult&           driver_result)
    {
        const VkDeviceSize base = usage == MemoryUsage::HostVisible ? block_sizes.host_visible : block_sizes.device_local;
        const VkDeviceSize heap = heapBytesOf(memory, type);
        // THE BASE IS CAPPED AT AN EIGHTH OF THE HEAP: a device with 128 MiB of device-local memory must not see this
        // pool reserve 64 MiB of it for the first small image. A request larger than the cap still gets its block.
        const VkDeviceSize capped = heap > 0U ? std::min(base, heap / 8U) : base;
        const VkDeviceSize wanted = alignedUp(std::max(size, capped), kBlockAlignment);

        VkMemoryAllocateInfo allocate{};
        allocate.sType           = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
        allocate.allocationSize  = wanted;
        allocate.memoryTypeIndex = type;

        auto block       = std::make_unique<Block>();
        block->id        = next_block_id++;
        block->type_index = type;
        block->size       = wanted;
        block->ranges.push_back(Range{ 0U, wanted, MemoryKind::Optimal, false });

        // THE ONE DEVICE CALL THIS UNIT MAKES, AND THE ONLY PLACE IT CAN FAIL: through the injectable creator when a test
        // installed one (see MemoryBlockCreator), through the device table otherwise. Its answer is CLASSIFIED, not
        // collapsed: a lost device and an exhausted heap need different repairs, and the raw result is carried so the
        // sentence can name it (see refusalOf).
        driver_result = creator ? creator(device, allocate, &block->memory)
                                : table->vkAllocateMemory(device, &allocate, nullptr, &block->memory);
        if (driver_result != VK_SUCCESS) {
            block->memory = VK_NULL_HANDLE;
            refusal       = refusalOf(driver_result);
            return nullptr;
        }        blocks.push_back(std::move(block));
        ++numbers.blocks;
        numbers.reserved_bytes += wanted;
        numbers.blocks_live = static_cast<std::uint32_t>(blocks.size());
        return blocks.back().get();
    }
};

MemoryAllocator::MemoryAllocator()
  : impl(std::make_unique<Impl>())
{
}

MemoryAllocator::~MemoryAllocator()
{
    close();
}

bool MemoryAllocator::open(const VolkDeviceTable&                  table,
                           const VkDevice                          device,
                           const VkPhysicalDeviceMemoryProperties& memory,
                           const VkDeviceSize                      granularity,
                           const MemoryBlockSizes                  block_sizes)
{
    const std::lock_guard<std::mutex> held(impl->lock);
    if (impl->table != nullptr) {
        return false; // a second open would orphan the first session's blocks
    }
    if (device == VK_NULL_HANDLE) {
        return false;
    }
    impl->table       = &table;
    impl->device      = device;
    impl->memory      = memory;
    impl->granularity = granularity > 1U ? granularity : 1U;
    impl->block_sizes = block_sizes;
    impl->numbers     = MemoryStats{};
    return true;
}

void MemoryAllocator::close() noexcept
{
    const std::lock_guard<std::mutex> held(impl->lock);
    if (impl->table != nullptr) {
        for (const auto& block : impl->blocks) {
            if (block->mapped != nullptr) {
                impl->table->vkUnmapMemory(impl->device, block->memory);
                block->mapped = nullptr;
            }
            if (block->memory != VK_NULL_HANDLE) {
                impl->table->vkFreeMemory(impl->device, block->memory, nullptr);
                block->memory = VK_NULL_HANDLE;
            }
        }
    }
    impl->blocks.clear();
    impl->next_block_id = 1;
    impl->numbers       = MemoryStats{};
    impl->table         = nullptr;
    impl->device        = VK_NULL_HANDLE;
    impl->memory        = VkPhysicalDeviceMemoryProperties{};
    impl->granularity   = 1;
}

bool MemoryAllocator::opened() const noexcept
{
    const std::lock_guard<std::mutex> held(impl->lock);
    return impl->openedLocked();
}

bool MemoryAllocator::supportsHostVisible() const noexcept
{
    // The properties are the device's own and immutable once open, but `close()` resets them, so the read is taken
    // under the same lock every other accessor uses. No allocation, no device call: this is the answer a readback needs
    // BEFORE it stops the device.
    const std::lock_guard<std::mutex> held(impl->lock);
    return impl->openedLocked() && findMemoryType(impl->memory, ~0U, MemoryUsage::HostVisible).has_value();
}

void MemoryAllocator::setFailureSink(MemoryFailureSink sink)
{
    const std::lock_guard<std::mutex> held(impl->lock);
    impl->sink = std::move(sink);
}

void MemoryAllocator::setBlockCreator(MemoryBlockCreator creator)
{
    const std::lock_guard<std::mutex> held(impl->lock);
    impl->creator = std::move(creator);
}

MemorySlice MemoryAllocator::allocate(const VkMemoryRequirements& requirements,
                                      const MemoryUsage           usage,
                                      const MemoryKind            kind,
                                      MemoryRefusal*              refusal_out)
{
    const std::lock_guard<std::mutex> held(impl->lock);
    // ONE REPORT PER ALLOCATION, BUILT HERE SO BOTH ANSWERS CARRY THE SAME FACTS (see MemoryRefusalReport): what was
    // asked, which memory type served it, and the pool's numbers at this moment. It is a stack value - nothing is
    // allocated to report - and nothing is reported unless a sink is installed.
    MemoryRefusalReport report;
    report.usage           = usage;
    report.kind            = kind;
    report.requested_bytes = requirements.size;

    const auto report_and = [this, &report, refusal_out](const MemoryRefusal refusal, const VkResult driver_result) {
        report.refusal       = refusal;
        report.driver_result = driver_result;
        // THE ROOM NUMBERS ARE WALKED, so they are filled only when something was REFUSED: a success path pays the
        // running counters it already has, and the refusal - the one case a host reads - gets the free/largest-run
        // pair that makes "out of memory" actionable (see MemoryStats).
        report.stats         = refusal == MemoryRefusal::Ok ? impl->numbers : impl->statsLocked();
        if (refusal_out != nullptr) {
            *refusal_out = refusal;
        }
        if (impl->sink) {
            impl->sink(report);
        }
    };
    const auto refuse = [&report_and](const MemoryRefusal refusal, const VkResult driver_result) -> MemorySlice {
        report_and(refusal, driver_result);
        return MemorySlice{};
    };
    const auto succeed = [&report_and](const MemorySlice& slice) -> MemorySlice {
        report_and(MemoryRefusal::Ok, VK_SUCCESS);
        return slice;
    };

    if (!impl->openedLocked()) {
        // A WIRING DEFECT, NOT A DEVICE FACT: saying "out of memory" here would send a host looking for memory to free.
        return refuse(MemoryRefusal::NotOpen, VK_SUCCESS);
    }
    if (requirements.size == 0U) {
        return refuse(MemoryRefusal::InvalidRequest, VK_SUCCESS);
    }

    const std::optional<std::uint32_t> type = findMemoryType(impl->memory, requirements.memoryTypeBits, usage);
    if (!type.has_value()) {
        return refuse(MemoryRefusal::NoType, VK_SUCCESS);
    }
    report.type_index = *type;

    // A BLOCK THAT FITS IS ALWAYS PREFERRED: reaching into one is a few comparisons; creating one is a device call.
    for (const auto& candidate : impl->blocks) {
        if (candidate->type_index != *type) {
            continue;
        }
        for (std::size_t index = 0; index < candidate->ranges.size(); ++index) {
            if (candidate->ranges[index].live) {
                continue;
            }
            const std::optional<VkDeviceSize> offset = impl->placeAt(*candidate, index, requirements.size, requirements.alignment, kind);
            if (!offset.has_value()) {
                continue;
            }
            Impl::takeAt(*candidate, index, *offset, requirements.size, kind);
            ++impl->numbers.allocations;
            impl->numbers.used_bytes += requirements.size;
            impl->numbers.peak_used_bytes = std::max(impl->numbers.peak_used_bytes, impl->numbers.used_bytes);
            return succeed(MemorySlice{ candidate->memory, *offset, requirements.size, candidate->id });
        }
    }

    MemoryRefusal refusal       = MemoryRefusal::Ok;
    VkResult      driver_result = VK_SUCCESS;
    Impl::Block*  block         = impl->createBlock(*type, usage, requirements.size, refusal, driver_result);
    if (block == nullptr) {
        return refuse(refusal, driver_result);
    }

    // A FRESH BLOCK HOLDS EXACTLY ONE FREE RANGE, so the search has to succeed unless the allocator contradicts itself:
    // a placement at offset 0 satisfies every alignment, and there is no neighbour to share a page with.
    const std::optional<VkDeviceSize> offset = impl->placeAt(*block, 0U, requirements.size, requirements.alignment, kind);
    if (!offset.has_value()) {
        return refuse(MemoryRefusal::Internal, VK_SUCCESS);
    }
    Impl::takeAt(*block, 0U, *offset, requirements.size, kind);
    ++impl->numbers.allocations;
    impl->numbers.used_bytes += requirements.size;
    impl->numbers.peak_used_bytes = std::max(impl->numbers.peak_used_bytes, impl->numbers.used_bytes);
    return succeed(MemorySlice{ block->memory, *offset, requirements.size, block->id });
}

void MemoryAllocator::release(MemorySlice& slice) noexcept
{
    const std::lock_guard<std::mutex> held(impl->lock);
    if (!slice.valid() || impl->table == nullptr) {
        slice = MemorySlice{};
        return;
    }
    Impl::Block* const block = impl->blockOf(slice.block);
    if (block == nullptr) {
        slice = MemorySlice{};
        return;
    }

    // THE LIVE RANGE IS FOUND BY ITS OFFSET: the list is sorted, and a slice names exactly one range.
    std::size_t index = 0U;
    bool        found = false;
    for (; index < block->ranges.size(); ++index) {
        const Impl::Range& range = block->ranges[index];
        if (range.live && range.offset == slice.offset && range.size == slice.size) {
            found = true;
            break;
        }
    }
    if (!found) {
        slice = MemorySlice{};
        return;
    }

    block->ranges[index].live = false;
    ++impl->numbers.releases;
    impl->numbers.used_bytes -= slice.size;

    // MERGE INTO THE FREE NEIGHBOURS (the list alternates, so they are the two adjacent ranges when they are free).
    if (index > 0U && !block->ranges[index - 1U].live) {
        block->ranges[index - 1U].size += block->ranges[index].size;
        block->ranges.erase(block->ranges.begin() + static_cast<std::ptrdiff_t>(index));
        --index;
    }
    while (index + 1U < block->ranges.size() && !block->ranges[index + 1U].live) {
        block->ranges[index].size += block->ranges[index + 1U].size;
        block->ranges.erase(block->ranges.begin() + static_cast<std::ptrdiff_t>(index + 1U));
    }

    // A BLOCK THAT BECAME ENTIRELY FREE IS DESTROYED WHEN ANOTHER OF ITS TYPE REMAINS: what a one-off large resource
    // (a big readback, a texture a host released) reserved does not stay reserved for the rest of the session. The
    // last block of a type stays warm, so "build, release, build" does not allocate twice.
    if (block->ranges.size() == 1U && !block->ranges[0].live) {
        const bool another = std::count_if(impl->blocks.begin(), impl->blocks.end(), [type = block->type_index](const auto& other) {
                                 return other->type_index == type;
                             }) > 1;
        if (another) {
            if (block->mapped != nullptr) {
                impl->table->vkUnmapMemory(impl->device, block->memory);
                block->mapped = nullptr;
            }
            impl->table->vkFreeMemory(impl->device, block->memory, nullptr);
            impl->numbers.reserved_bytes -= block->size;
            const auto position = std::find_if(impl->blocks.begin(), impl->blocks.end(), [id = block->id](const auto& other) {
                return other->id == id;
            });
            impl->blocks.erase(position);
            impl->numbers.blocks_live = static_cast<std::uint32_t>(impl->blocks.size());
        }
    }

    slice = MemorySlice{};
}

void* MemoryAllocator::map(const MemorySlice& slice) noexcept
{
    const std::lock_guard<std::mutex> held(impl->lock);
    if (!slice.valid() || impl->table == nullptr) {
        return nullptr;
    }
    Impl::Block* const block = impl->blockOf(slice.block);
    if (block == nullptr) {
        return nullptr;
    }
    // HOST-VISIBLE IS A PROPERTY OF THE MEMORY TYPE, not of the purpose a block was first used for: a device whose
    // device-local type is also host-visible (a ReBAR GPU, lavapipe) serves both usages from ONE type, so a block
    // created for an attachment can legitimately hold a staging slice - and the only question that decides whether a
    // mapping exists is whether the memory itself can be mapped.
    if (block->type_index >= impl->memory.memoryTypeCount ||
        (impl->memory.memoryTypes[block->type_index].propertyFlags & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT) == 0U) {
        return nullptr;
    }
    if (block->mapped == nullptr) {
        void* mapped = nullptr;
        if (impl->table->vkMapMemory(impl->device, block->memory, 0U, VK_WHOLE_SIZE, 0U, &mapped) != VK_SUCCESS) {
            return nullptr;
        }
        block->mapped = mapped;
    }
    return static_cast<std::byte*>(block->mapped) + slice.offset;
}

MemoryStats MemoryAllocator::stats() const noexcept
{
    const std::lock_guard<std::mutex> held(impl->lock);
    return impl->statsLocked();
}

VN_BACKEND_VK_NS_END
