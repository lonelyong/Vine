#pragma once

#include <cstddef>
#include <cstdint>
#include <list>
#include <unordered_map>
#include <vector>

#include <vine/vsg/vsg_global.hpp>

/**
 * @brief The persistent storage of material blocks: one slot per material, one copy per frame that may still
 * read it, and a write only when the material's revision says its bytes changed.
 *
 * WHY MATERIALS GET THEIR OWN STORAGE. Every other per-draw value is written every frame (a view block per
 * pass, a matrix and an opacity per draw) because it changes every frame. Material bytes do not: an
 * application edits a material a few times in a session, and a scene with N drawables sharing one material
 * must not pay N writes per frame for it. Giving materials their own arena - written only when the upstream
 * revision moves - is what makes "a steady scene transfers nothing" a property of the design rather than of
 * how carefully the frame path avoids writing.
 *
 * WHY COPIES, AND WHY THE ROTATION MATTERS. A write while an earlier frame is still reading the block is a
 * data race the GPU will happily commit (it renders half of the old and half of the new value). Each slot
 * therefore holds as many copies as frames may be in flight, and the frame's copy rotates - the same
 * arithmetic the retirement queue uses to know which frames are done. A caller that writes to the copy the
 * current frame reads is correct by construction; a caller that writes to the copy an IN-FLIGHT frame reads
 * is the bug this layout exists to make impossible to express.
 *
 * WHAT THIS TYPE DOES NOT OWN. No GPU object and no buffer: it is the layout and the accounting (which slot,
 * which copy, how many 64-byte writes happened), so its rules are testable without a device. The API layer
 * allocates one buffer of @ref capacityBytes() and uses @ref offsetOf() for the dynamic offset.
 *
 * NOT thread-safe: it is used from the frame's own thread, like the rest of the backend.
 */
V_VSG_NS_BEGIN

namespace core
{

/** @brief Persistent material storage: slots, in-flight copies and write accounting. */
class MaterialArena
{
  public:
    /** @brief The arena's shape, all of it known before anything is written. */
    struct Layout
    {
        std::uint32_t block_bytes{64};       ///< Bytes one material block occupies (the engine's material ABI).
        std::uint32_t copies{3};             ///< Copies per slot: one per frame that may still read it.
        std::uint32_t capacity{256};         ///< Slots; the oldest entry is evicted past it (the cache bound).
    };

    /** @brief How a note was answered. */
    enum class WriteKind : std::uint8_t
    {
        Allocated,  ///< The material had no slot: one was allocated and its block written.
        Rewritten,  ///< The revision moved: the new bytes were written into the material's copy.
        Unchanged,  ///< The revision matched what the storage holds: nothing was written.
    };

    /** @brief The answer of @ref note. */
    struct Write
    {
        WriteKind     kind{WriteKind::Unchanged};  ///< What happened.
        std::uint32_t slot{0};                     ///< Slot the material occupies (meaningful for Allocated / Rewritten).
        std::uint32_t copy{0};                     ///< In-flight copy written (meaningful for Allocated / Rewritten).
        std::uint64_t bytes{0};                    ///< Bytes written (0 for Unchanged).
    };

  public:
    /** @brief Constructs an arena with the given layout.
     *
     * @param layout Block size, copy count and slot capacity; every field is clamped to at least 1.
     */
    explicit MaterialArena(const Layout& layout);

    MaterialArena(const MaterialArena&) = delete;
    MaterialArena& operator=(const MaterialArena&) = delete;

  public:
    /** @brief Begins the next frame: advances the copy rotation and the frame-local write dedupe.
     *
     * The first call begins frame 0, so a test (or a session) that calls this at the top of every frame
     * sees the frames numbered from zero.
     */
    void beginFrame() noexcept;

    /** @brief Notes that a material is in use this frame, telling the caller whether to write its bytes.
     *
     * A material with no slot is allocated one and written (Allocated); a material whose revision moved is
     * written once per frame (Rewritten) - the SECOND note in the same frame asks one block from an
     * application that edited a value twice, and answering it twice would double the frame's traffic for
     * bytes the GPU can only ever see one of.
     *
     * @param material Material identity (a raw pointer: the arena stores no reference to it).
     * @param revision Upstream revision of the material's bytes; the arena never invents one.
     * @return What happened, and where the block lives (see @ref offsetOf).
     */
    [[nodiscard]] Write note(const void* material, std::uint64_t revision);

    /** @brief Drops a material's slot, when the application itself is done with it.
     *
     * @param material Material identity.
     * @return true when the arena had a slot for it.
     */
    bool release(const void* material) noexcept;

    /** @brief Gets the byte offset of one block, which is also the dynamic offset a descriptor binds with.
     *
     * @param slot Slot index (see the Write returned by @ref note).
     * @param copy In-flight copy.
     * @return Offset in bytes from the start of the arena's buffer.
     */
    [[nodiscard]] std::uint64_t offsetOf(std::uint32_t slot, std::uint32_t copy) const noexcept;

  public:
    /** @brief Gets the buffer size the API layer has to allocate for this layout. */
    [[nodiscard]] std::uint64_t capacityBytes() const noexcept;

    /** @brief Gets the bytes one material block occupies. */
    [[nodiscard]] std::uint32_t blockBytes() const noexcept;

    /** @brief Gets the number of in-flight copies per slot. */
    [[nodiscard]] std::uint32_t copies() const noexcept;

    /** @brief Gets the number of slots the arena may hold. */
    [[nodiscard]] std::uint32_t capacity() const noexcept;

    /** @brief Gets the current frame number (0 before the first @ref beginFrame). */
    [[nodiscard]] std::uint64_t frame() const noexcept;

    /** @brief Gets the number of materials with a slot. */
    [[nodiscard]] std::size_t live() const noexcept;

    /** @brief Gets the number of blocks written (allocations and rewrites together). */
    [[nodiscard]] std::uint64_t writes() const noexcept;

    /** @brief Gets the number of blocks written for a material the arena had not seen. */
    [[nodiscard]] std::uint64_t allocations() const noexcept;

    /** @brief Gets the number of notes answered without a write. */
    [[nodiscard]] std::uint64_t hits() const noexcept;

    /** @brief Gets the number of entries dropped because the capacity was reached. */
    [[nodiscard]] std::uint64_t evictions() const noexcept;

    /** @brief Gets the total bytes written since construction. */
    [[nodiscard]] std::uint64_t bytesWritten() const noexcept;

    /** @brief Drops every material, keeping the counters (a session ends; the numbers did happen). */
    void clear() noexcept;

  private:
    struct Entry
    {
        const void*   material{nullptr};    ///< The identity this slot belongs to.
        std::uint64_t revision{0};          ///< Revision whose bytes the slot holds.
        std::uint32_t slot{0};              ///< Slot index.
        std::uint64_t last_write_frame{0};  ///< Frame of the last write (the per-frame dedupe).
        std::uint32_t last_copy{0};         ///< Copy the last write went to.
    };

    /** @brief Gets the copy the current frame writes to. */
    [[nodiscard]] std::uint32_t currentCopy() const noexcept;

  private:
    Layout        layout_;
    bool          started_{false};
    std::uint64_t frame_{0};
    std::uint32_t next_slot_{0};
    std::vector<std::uint32_t> free_slots_;

    std::list<Entry>                                            order_;  ///< FIFO order (insertion).
    std::unordered_map<const void*, std::list<Entry>::iterator> index_;  ///< Material -> its entry.

    std::uint64_t writes_{0};
    std::uint64_t allocations_{0};
    std::uint64_t hits_{0};
    std::uint64_t evictions_{0};
    std::uint64_t bytes_written_{0};
};

}  // namespace core

V_VSG_NS_END
