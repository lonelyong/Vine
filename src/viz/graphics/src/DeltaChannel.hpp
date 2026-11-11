#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <type_traits>
#include <vector>

#include <vine/math/Matrix4x4.hpp>

#include "DrawListBake.hpp"

VN_GRAPHICS_NS_BEGIN

namespace detail
{

/**
 * @brief What one delta says changed.
 *
 * @details
 * - THE VOCABULARY GROWS BY KIND, not by channel: a new kind is a new enumerator PLUS the applier that serves it, so
 *   the content's thread and the engine's cannot disagree about how a change travels (`.ai/design/graphics-threading.md`
 *   F2). A kind without an applier is a lie about what the other side can do, so it is not added.
 * - THE THREE THAT EXIST TODAY each have their applier: `RowsReplaced` is the handshake and `RenderWorld::adopt` takes
 *   it, `Placement` is one row and `RenderWorld::place` takes it, and `Terminate` is how the engine side is told to
 *   stop. The finer kinds the design names (attach / detach / reparent / state / description / box) arrive WITH their
 *   appliers (see `../backlog/graphics.md` T31's phases).
 */
enum class DeltaKind : std::uint8_t
{
    RowsReplaced, ///< The content's rows as of this moment; carries the table (see OwnedDelta::rows).
    Attached,     ///< A subtree now hangs under a row: `identity` is the PARENT, the block rides along (child_index).
    Detached,     ///< A subtree is gone from the table: `identity` is the detached subtree's ROOT.
    Placement,    ///< One row's own placement; carries its local matrix.
    State,        ///< One row's own numeric values (opacity / layers / draw order); see StateValues.
    Description,  ///< One row's PAYLOAD (geometry description, draw facts, its handles); carries a one-row block.
    Terminate,    ///< The engine side should stop: the last delta this channel ever carries.
};

/**
 * @brief One row's own NUMERIC values: what a node declares about itself, without a handle in it.
 *
 * @details
 * - WHY ONLY NUMBERS. A StateNode contributes state + material + program together, and two of those are handles: a
 *   handle-carrying edit belongs to the description delta (design R5 / F5), so that one fact is never split across two
 *   deltas. What is left is exactly the numeric half - and the half an editor changes interactively (dimming a part,
 *   moving it to another layer, restacking it).
 * - VISIBILITY IS NOT HERE: in this model a hidden subtree has no rows at all (the build skips it), so showing and
 *   hiding is a STRUCTURE change (attach / detach), not a value one.
 */
struct StateValues {
    float                        opacity{ 1.0F }; ///< The row's own opacity (its subtree multiplies it).
    std::optional<std::uint32_t> layers{};        ///< The row's own layer declaration, or nothing (inherit).
    std::optional<std::int32_t>  draw_order{};    ///< The row's own draw order, or nothing (inherit).
};

static_assert(std::is_trivially_copyable_v<StateValues>, "a state delta carries no share either: it must cross as bytes");

/**
 * @brief ONE delta's own value: fixed size, trivially copyable, no share.
 *
 * @details
 * - This is the half that has to be cheap: a per-frame pose is 28~70 of these, so it must cross as bytes and never
 *   allocate (`.ai/design/graphics-threading.md` R15). A delta that OWNS content instead travels through
 *   @ref OwnedDelta, which is the same value plus the share it carries.
 */
struct Delta {
    DeltaKind     kind{ DeltaKind::Placement };
    const void*   identity{ nullptr }; ///< The content object it is about: opaque here, and never dereferenced.
    Mat4d         matrix{};            ///< Placement: the row's own local matrix.
    StateValues   state{};             ///< State: the row's own numeric values.
    std::uint32_t child_index{ 0 };    ///< Attached: where among the parent's children the block goes (0 = first).
};

static_assert(std::is_trivially_copyable_v<Delta>, "a delta is a value: it has to cross a thread as bytes");

/**
 * @brief One delta together with the content it owns.
 *
 * @details
 * - THE TWO HALVES OF A CHANGE. The numbers say WHAT changed, and the share (when there is one) is the content that
 *   cannot be copied into a fixed-size record: the rows a rebuild produced. They travel together so the engine side
 *   cannot receive the announcement without the values it describes.
 * - Ownership is the point: the share was built on the content's thread and is read-only from the moment it is posted,
 *   so the engine side reads it without a lock and without touching the content's graph.
 */
struct OwnedDelta {
    Delta                            delta{};
    /// The table a `RowsReplaced` delta carries, or the BLOCK an `Attached` one does: built on the content's thread
    /// and read-only on the engine's. Empty for the kinds that own nothing.
    std::shared_ptr<const SceneBake> rows{};
};

/**
 * @brief THE DELTA CHANNEL: what the content's thread tells the engine's, delivered once per frame.
 *
 * @details
 * - WHY A CHANNEL AND NOT A CALLBACK. The two sides run on different threads, and the engine side asks exactly one
 *   question per frame - "what changed since the last one" - which a batch answers in one drain. Nothing here knows
 *   what a delta means; the applier does (see DeltaKind).
 * - ORDER IS THE CONTRACT. The batch is delivered in post order, so two deltas about one row apply in the order they
 *   were produced. The DRAIN is the frame boundary: the engine applies the whole batch before it expands, so no frame
 *   sees half a change.
 * - REFUSAL IS EXPLICIT AND RECOVERABLE. The channel is bounded, and a post that does not fit is REFUSED (it returns
 *   false and is counted) instead of being dropped quietly or growing the queue. Losing a delta is safe by
 *   construction: the coarsest delta, `RowsReplaced`, is a superset of everything finer, so a full channel is
 *   recovered by posting a fresh table - which is exactly what the producer does when it sees a refusal.
 * - ONE DRAINER, MANY PRODUCERS. The producer side is the content's thread (one today); the consumer side is the
 *   engine's, and it is the only one that drains. A mutex is the whole synchronisation: the critical sections are a
 *   push into a preallocated slot and a swap, and the queue is fixed-capacity, so a steady-state frame allocates
 *   nothing on either side.
 * - WHAT IS NOT HERE. Threads, waiting, and wake-ups: the channel is a mailbox, so the caller decides when to drain
 *   (the worker drains at frame start, and `RenderWorker`'s loop is what waits). Keeping the waiting out of the
 *   channel is what lets the same mailbox run in the inline mode, where there is no thread at all.
 *
 * @warning Not copyable: two threads share one instance, they do not own two.
 */
class DeltaChannel {
  public:
    /// The default bound: enough for a robot's whole pose (28~70 joints) plus structure, with room to spare.
    inline static constexpr std::size_t kDefaultCapacity = 256;

  public:
    /**
     * @brief Creates a channel that holds up to @p capacity deltas between two drains.
     *
     * @param capacity Deltas that may wait at once; clamped to at least one.
     */
    explicit DeltaChannel(std::size_t capacity = kDefaultCapacity);

    DeltaChannel(const DeltaChannel&)            = delete;
    DeltaChannel& operator=(const DeltaChannel&) = delete;

  public:
    /**
     * @brief Posts one delta that owns nothing.
     *
     * @param delta The change to announce.
     * @return false when the channel is full (the delta was NOT queued; see the class note on recovery).
     */
    bool post(const Delta& delta);

    /**
     * @brief Posts one delta together with the content it owns (a table for `RowsReplaced`).
     *
     * @param delta The change to announce.
     * @param rows  The rows it carries; may be null for a delta that only names a change.
     * @return false when the channel is full (nothing was queued and the share was not taken).
     */
    bool post(const Delta& delta, std::shared_ptr<const SceneBake> rows);

    /**
     * @brief Takes everything posted so far, in post order, into @p into.
     *
     * @details
     * - @p into must be a buffer the caller is DONE with: it is recycled as the producer's storage, which is what keeps
     *   a steady-state frame allocation-free. Pass the same vector every frame and the channel never allocates.
     * - One swap under the lock: the engine side holds the batch for as long as it needs it, and the content side keeps
     *   posting into the other buffer.
     *
     * @param into Receives the pending deltas (replacing its contents); must not be the channel's own storage.
     */
    void drain(std::vector<OwnedDelta>& into);

  public:
    /** @brief Gets how many deltas are waiting for the next drain. */
    [[nodiscard]] std::size_t pending() const;

    /** @brief Gets how many deltas were accepted since construction. */
    [[nodiscard]] std::uint64_t posted() const;

    /** @brief Gets how many were refused because the channel was full. */
    [[nodiscard]] std::uint64_t refused() const;

    /** @brief Gets how many deltas may wait between two drains. */
    [[nodiscard]] std::size_t capacity() const noexcept;

  private:
    mutable std::mutex      mutex_;
    std::vector<OwnedDelta> pending_;
    std::size_t             capacity_{ kDefaultCapacity };
    std::uint64_t           posted_{ 0 };
    std::uint64_t           refused_{ 0 };
};

} // namespace detail

VN_GRAPHICS_NS_END
