#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>

#include <vsg/commands/BindIndexBuffer.h>
#include <vsg/commands/BindVertexBuffers.h>
#include <vsg/core/Data.h>
#include <vsg/core/ref_ptr.h>

#include <vine/Buffer.hpp>
#include <vine/graphics/backend/FrameTimeline.hpp>
#include <vine/graphics/backend/RetirementQueue.hpp>
#include <vine/graphics/backend/Streams.hpp>
#include <vine/backend_vsg/internal/ContentFacts.hpp>
#include <vine/backend_vsg/backend_vsg_global.hpp>

/**
 * @brief The device-facing half of geometry aliasing: one bind (and therefore one upload) per stream identity.
 *
 * WHY THE BIND IS THE UNIT. A bind command owns its `BufferInfo`, and a `BufferInfo` is what becomes one
 * device buffer plus its upload. Two drawables reading the same stream, each with their own bind, therefore
 * upload the same bytes twice; handing both the SAME bind command is what makes the stream exist once on the
 * GPU and be read twice. That is the whole mechanism this store wraps.
 *
 * WHAT DECIDES LIFETIME: CONTENT, NOT TIME. An entry lives exactly while some LIVE content declares its bytes.
 * The declaration is PUBLISHED by the facts the frame was recorded against (`ContentFacts::declarations` - an
 * index the content store maintains as its geometry rows move), so the question "is this stream still wanted?"
 * is answered by asking who is alive, not by remembering who named it last. The consequences are the ones the
 * aliasing rules promise:
 *
 *   * a geometry that is CULLED or hidden keeps its uploads - it is still tracked, so its row (and therefore
 *     its pin) is still there, and the frame after it comes back re-uploads nothing;
 *   * a geometry that LEAVES the tree loses them - the content store parks the row's removal for the same
 *     `slots + 1` window every replaced GPU object gets, and this store's next frame sees the pin gone;
 *   * a buffer whose bytes were REFILLED (`Buffer::edit()`) retires its previous upload without the geometry
 *     speaking at all, because the pin carries the buffer's own revision - read live from the row's buffer
 *     handle at enumeration time, exactly as `streamKey()` reads it.
 *
 * The pins are two O(1) facts, not a walk of the content tables. The ADDRESS comes from the content layer: the
 * facts publish the set of buffers the live rows declare (`ContentDeclarations`, refcounted, so one arena named by
 * several geometries stays declared until the last of them goes). The REVISION comes from the entry itself: it
 * holds the buffer it uploaded from, so "are these still that buffer's current bytes?" is one compare against
 * `owner->revision()` - and a buffer edit retires the old upload without the geometry speaking. Nothing here
 * walks the content rows, which is what keeps a frame that draws nothing as cheap as a busy one.
 *
 * WHAT IS DELETED, AND WHY. The previous spelling remembered "the last frame that named this" and dropped
 * entries older than a grace window (`slots + 1`), with a capacity bound on top. Both were guesses about
 * lifetime: culling looked exactly like leaving, so a mesh that merely went out of view re-uploaded when it
 * came back, and the bound could drop a stream a live geometry still declared. @ref beginFrame now sweeps
 * against content, and there is no capacity and no clock in this store at all.
 *
 * WHAT MAY BE SHARED (and what is refused instead):
 *
 *   * the four CANONICAL vertex channels, when their bytes are the model's own (positions, normals,
 *     texcoords, the loc2 colour). A custom channel's bind is one command whose identity is the whole custom
 *     LAYOUT, and a channel the backend BUILDS carries this drawable's own values (the white opacity carrier
 *     even carries its opacity) - sharing either would draw every peer with the first drawable's data, so
 *     both are refused here and the caller builds a private bind;
 *   * a channel whose buffer handle the caller does NOT hand over. The entry's key names the buffer by ADDRESS,
 *     and only the entry's own hold on that address keeps a new buffer from landing on it; a borrowed identity
 *     has nothing to hold, so its upload would be a snapshot served to whoever lands there next. Refused rather
 *     than trusted - production always has the handle (see ContentPass);
 *   * the INDEX stream, keyed on the whole buffer it aliases. The DRAW states which span it reads
 *     (first index / count), so two geometries slicing one index arena share one index upload - which is why
 *     the index key is normalised here (offset 0, count = the array's element count) and not taken from the
 *     caller's span. The array passed in must alias the whole buffer for that to hold.
 *
 * WHAT DROPPING AN ENTRY DOES NOT DO. An entry holds the bind, and the bind holds the arrays the upload was
 * built from - so an entry leaving the map never frees bytes a frame may still be reading. Departures are
 * parked through the retirement queue anyway (`ResourceTable::retireUnpinned`), so the bind a submitted frame
 * named stays alive for exactly the window that frame is in flight; this store is a LOOKUP, not the owner of
 * last resort.
 *
 * AN ENTRY ALSO HOLDS ITS MODEL BUFFER. The key names the buffer by ADDRESS, and a buffer nobody holds hands
 * that address back to the allocator: a new buffer landing there - same kind, same slice, same revision, which
 * fresh buffers all carry - would then be answered with THIS entry's uploaded bytes, i.e. another geometry's
 * vertices, silently. (Same family as the program rows' address key, see .ai/bugs/vsgb-content-address-recycled-
 * before-rows.md.) So an acquire takes the buffer handle and the entry keeps that share until it itself leaves:
 * the address cannot be recycled while it is cached, which is exactly as long as its key can be looked up.
 *
 * NOT thread-safe: it is used from the frame's own thread, like the rest of the backend.
 */
VN_BACKEND_VSG_NS_BEGIN

/** @brief Shared stream uploads, keyed by `core::StreamKey` and pinned by live geometry (see the file note). */
class StreamUploads {
  public:
    /** @brief What an acquire did. */
    enum class Action : std::uint8_t
    {
        Uploaded, // A bind was built for this stream (the bytes will be uploaded once).
        Aliased,  // An existing bind serves this stream.
        Refused,  // This stream may not be shared (see the file note); the caller builds a private bind.
    };

    /** @brief The answer of @ref acquireVertex. */
    struct VertexResult {
        Action                                   action{ Action::Refused }; // What happened.
        ::vsg::ref_ptr<::vsg::BindVertexBuffers> bind;                      // The bind (null when refused).
    };

    /** @brief The answer of @ref acquireIndex. */
    struct IndexResult {
        Action                                 action{ Action::Refused }; // What happened.
        ::vsg::ref_ptr<::vsg::BindIndexBuffer> bind;                      // The bind (null when refused).
    };

    /** @brief Sentinel: a shader location that has no canonical binding (a custom channel). */
    static constexpr std::uint32_t kNoBinding = ~std::uint32_t{ 0 };

  public:
    StreamUploads();
    ~StreamUploads();

    StreamUploads(const StreamUploads&)            = delete;
    StreamUploads& operator=(const StreamUploads&) = delete;

  public:
    /** @brief Gets the binding a canonical shader location is fed at, or @ref kNoBinding.
     *
     * This is the ONE spelling of the mapping (positions 0, normals 1, texcoords 2, loc2 colour 3 - the
     * shader contract's locations), so a layout builder and this store cannot disagree about which binding a
     * shared stream belongs to.
     *
     * @param location Shader location of the attribute.
     * @return First-binding index, or @ref kNoBinding for a custom channel.
     */
    [[nodiscard]] static std::uint32_t bindingOfCanonical(std::uint32_t location) noexcept;

    /** @brief Gets (or builds) the shared bind for a canonical vertex stream.
     *
     * @details
     * - The BYTES are handed over as a view and are only read when the stream must actually be uploaded: a
     *   sharing hit touches neither the buffer nor its memory, so a steady frame pays nothing for them.
     * - The array the bind reads ALIASES @p owner's memory (see `VsgBufferView`): the renderer keeps the model's own
     *   bytes instead of a second copy of every channel, and the entry's hold on @p owner is what keeps them alive. A
     *   caller that passes no owner (a borrowed identity) has nothing to alias and gets a snapshot instead.
     *
     * @param key     Stream identity; its kind must be Vertex and its location canonical.
     * @param scalars The channel's scalars as they are right now (read on a miss).
     * @param owner   The buffer the key's address names, held by the entry for as long as it is cached (see
     *                the file note). It is REQUIRED: a shareable stream without its handle has nothing that
     *                reserves the address, and is refused.
     * @return The action and the bind (null when the stream may not be shared).
     */
    [[nodiscard]] VertexResult acquireVertex(const core::StreamKey& key, std::span<const float> scalars, vn::intrusive_ptr<const vn::Buffer<float>> owner = {});

    /** @brief Gets (or builds) the shared bind for an index stream.
     *
     * @details
     * - The bind covers the WHOLE buffer (the draw states the span), so the array aliases @p owner from offset 0 and
     *   the whole length - a span that is not that whole buffer cannot be aliased and is snapshotted instead.
     *
     * @param key     Stream identity; its kind must be Index.
     * @param indices The WHOLE index buffer as it is right now (the bind aliases all of it, and the draw
     *                states the span; read on a miss, like acquireVertex's bytes).
     * @param owner   The buffer the key's address names, held by the entry (REQUIRED, like acquireVertex's).
     * @return The action and the bind (null when the stream may not be shared).
     */
    [[nodiscard]] IndexResult
    acquireIndex(const core::StreamKey& key, std::span<const std::uint32_t> indices, vn::intrusive_ptr<const vn::Buffer<std::uint32_t>> owner = {});

    /**
     * @brief Opens a frame: the facts it was recorded against decide which streams stay.
     *
     * @details
     * - The declarations are the ones `ContentStore::tablesFor` just published (`ContentFacts::declarations`), so
     *   "does anything alive still name this buffer?" is one hash lookup per entry - no walk of the rows, no
     *   second walk of the scene or the plan.
     * - Every entry whose buffer is no longer declared, or whose bytes are no longer the buffer's current ones
     *   (`owner->revision()`), is PARKED through @p retirement; one the queue cannot park yet is retained, never
     *   freed early (see ResourceTable::retireUnpinned).
     * - Calling it with empty facts means "nothing is declared" and retires every entry.
     *
     * @param facts      The frame's facts (its declaration index is what this reads).
     * @param timeline   The frame clock parks are dated against.
     * @param retirement Where departed entries are parked.
     * @return How many entries left (each one's bind holding until the park comes due).
     */
    std::uint64_t beginFrame(const ContentFacts& facts, core::FrameTimeline& timeline, core::RetirementQueue& retirement);

    /**
     * @brief Parks every stream read from @p buffer: the host's EXPLICIT release, over an entry content still pins.
     *
     * @details
     * - THE HOST'S WORD OUTRANKS CONTENT. A stream normally leaves when no live content row declares its buffer any
     *   more (see @ref beginFrame); this makes the entries leave NOW, while the geometry that declares them stays.
     *   Only the GPU copy goes: the host's buffer is untouched, and the next frame that draws the geometry uploads
     *   again - that re-upload is what an explicit release costs (the same trade every other `releaseContent`
     *   makes, see the ADR-2026-11-10 pin model).
     * - DEPARTURES ARE PARKED, not destroyed, like every other departure here: a submitted frame may still bind
     *   the binds. A queue with no window yet keeps the entries and counts them as retained - memory, never a
     *   command buffer naming freed objects.
     * - O(entries), not O(1): an explicit release is a host announcement between frames, never a per-frame cost.
     *
     * @param buffer     The buffer a stream was uploaded from (the address a `StreamKey` names). Null changes nothing.
     * @param timeline   Frame clock the parks are dated against.
     * @param retirement Where departed entries are parked.
     * @return How many entries left the table (parked; a retained one is still resident and not counted).
     */
    std::uint64_t releaseBuffer(const void* buffer, core::FrameTimeline& timeline, core::RetirementQueue& retirement);

  public:
    /** @brief Gets the number of entries the store holds. */
    [[nodiscard]] std::size_t live() const noexcept;

    /** @brief Gets the number of binds built (one upload each). */
    [[nodiscard]] std::uint64_t uploads() const noexcept;

    /** @brief Gets the bytes those binds have copied so far (cumulative; a frame's own number is a delta).
     *
     * @details
     * - The size of the array a built bind reads: an aliased bind reads the model's own bytes and is counted at its
     *   real size too - the device copy happens either way, it is only the host-side staging that is shared.
     *
     * @return Bytes uploaded since construction.
     */
    [[nodiscard]] std::uint64_t uploadedBytes() const noexcept;

    /** @brief Gets the number of acquires an existing bind answered. */
    [[nodiscard]] std::uint64_t aliases() const noexcept;

    /** @brief Gets the number of acquires refused because the stream may not be shared. */
    [[nodiscard]] std::uint64_t refusals() const noexcept;

    /** @brief Gets the number of entries that left, parked (the sweep's departures and explicit releases). */
    [[nodiscard]] std::uint64_t retired() const noexcept;

  private:
    struct Data;
    // Lexically after Data so the out-of-line destructor is the only place that needs the complete type.
    std::unique_ptr<Data> d;
};

VN_BACKEND_VSG_NS_END
