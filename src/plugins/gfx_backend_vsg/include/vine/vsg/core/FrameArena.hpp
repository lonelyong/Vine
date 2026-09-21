#pragma once

#include <cstddef>
#include <memory>
#include <span>
#include <vector>

#include <vine/vsg/vsg_global.hpp>

/**
 * @brief The storage a frame's plan lives in, owned by the frame and nothing else.
 *
 * WHY THIS TYPE EXISTS. A frame plan is a graph of small records - pass scopes, resolved inputs,
 * lights, draws - that must stay valid from the call that collected it (inside RenderBackend::render,
 * where every argument is BORROWED) until the executor has recorded it at the end of the frame. Spans
 * into the host's containers do not survive that window: the engine's own resolved-input vector is a
 * reused member that is cleared and refilled per pass, so a plan holding a span into it would be
 * reading the NEXT pass's data by the time it is recorded. One arena per frame, reset at beginFrame(),
 * makes the rule mechanical: every span in the plan points here, and the only moment a span may die
 * is the frame boundary.
 *
 * GROWTH ADDS A CHUNK, IT NEVER MOVES ONE. A single resizable buffer would be simpler and wrong: the
 * moment it reallocated, every span handed out so far would point into freed memory - and the whole
 * point of the arena is that a plan's spans stay readable until the frame ends. So the storage is a
 * list of chunks: an allocation that does not fit the current chunk uses the next one, or starts a new
 * one, and the blocks already handed out never move. `allocations()` counts the chunks STARTED since
 * construction (a steady frame starts none); only reset() invalidates a span, and that is the frame
 * boundary by definition.
 *
 * ALIGNMENT LIMIT: chunks come from `new std::byte[]`, so the arena serves alignments up to
 * `alignof(std::max_align_t)`. Anything more is refused rather than served misaligned.
 */
V_VSG_NS_BEGIN

namespace core
{

/**
 * @brief A bump allocator for one frame's plan (see the file note for the ownership rule it enforces).
 */
class FrameArena
{
  public:
    /** @brief Creates an arena whose chunks are @p chunk_size bytes each.
     *
     * The first chunk is created here as a RESERVATION and does not count as an allocation, so a frame
     * that fits in it leaves allocations() at 0.
     *
     * @param chunk_size Bytes in the first chunk, and the minimum size of any chunk started later.
     */
    explicit FrameArena(std::size_t chunk_size = 256 * 1024);

    FrameArena(const FrameArena&)            = delete;
    FrameArena& operator=(const FrameArena&) = delete;
    ~FrameArena();

    /** @brief Ends the frame: the allocation cursor returns to the start.
     *
     * The ONE place a span handed out earlier may be invalidated. Capacity is kept, so the next frame
     * reuses the same memory.
     */
    void reset() noexcept;

    /** @brief Reserves @p bytes aligned to @p alignment.
     *
     * @param bytes Number of bytes to reserve; 0 returns nullptr.
     * @param alignment Requested alignment, a power of two.
     * @return The reserved block, valid until the next reset().
     */
    [[nodiscard]] void* allocate(std::size_t bytes, std::size_t alignment = alignof(std::max_align_t));

    /** @brief Reserves an array of @p count value-initialized T.
     *
     * @param count Number of elements.
     * @return The array, valid until the next reset().
     */
    template <typename T>
    [[nodiscard]] std::span<T> makeArray(std::size_t count)
    {
        if (count == 0)
        {
            return {};
        }
        void* raw = allocate(count * sizeof(T), alignof(T));
        // The first element is `raw`, NOT what the construction helper returns: it returns the pointer
        // PAST the constructed range, and a span built from that would point out of the block.
        T* first = static_cast<T*>(raw);
        std::uninitialized_value_construct_n(first, count);
        return std::span<T>(first, count);
    }

    /** @brief Copies @p source into the arena.
     *
     * The way a borrowed span enters a plan: the host may destroy its own storage as soon as the call
     * returns, so anything a plan needs to keep is copied here.
     *
     * @param source Values to copy.
     * @return The copy, valid until the next reset().
     */
    template <typename T>
    [[nodiscard]] std::span<const T> copy(std::span<const T> source)
    {
        if (source.empty())
        {
            return {};
        }
        std::span<T> destination = makeArray<T>(source.size());
        for (std::size_t i = 0; i < source.size(); ++i)
        {
            destination[i] = source[i];
        }
        return destination;
    }

    /** @brief Copies one borrowed value into the arena.
     *
     * @param value Value to copy.
     * @return A pointer to the copy, valid until the next reset(), or nullptr for nothing.
     */
    template <typename T>
    [[nodiscard]] const T* copyValue(const T& value)
    {
        const std::span<const T> copy_of = copy(std::span<const T>(&value, 1));
        return copy_of.empty() ? nullptr : &copy_of.front();
    }

    /** @brief Gets the bytes handed out this frame. */
    [[nodiscard]] std::size_t bytesUsed() const noexcept;

    /** @brief Gets the bytes reserved (what reset() keeps). */
    [[nodiscard]] std::size_t bytesReserved() const noexcept;

    /** @brief Gets how many chunks the arena STARTED after the one it was constructed with.
     *
     * The steady-state gate: a frame whose content did not change must leave this unchanged.
     */
    [[nodiscard]] std::size_t allocations() const noexcept;


  private:
    /** @brief One block of storage. Blocks already handed out are never moved between chunks. */
    struct Chunk
    {
        std::unique_ptr<std::byte[]> storage;       ///< The block itself.
        std::size_t                  capacity{0};   ///< Bytes in the block.
        std::size_t                  used{0};       ///< Bytes handed out of it this frame.
    };

    /** @brief Starts a chunk able to hold at least @p bytes, and counts it. */
    void addChunk(std::size_t bytes);

    std::vector<Chunk> chunks_;         ///< Storage, in the order it was started.
    std::size_t        chunk_size_{0};  ///< Size of a new chunk when a request fits in one.
    std::size_t        cursor_{0};      ///< Chunk the next allocation comes from.
    std::size_t        allocations_{0}; ///< Chunks started after construction.
};

}  // namespace core

V_VSG_NS_END
