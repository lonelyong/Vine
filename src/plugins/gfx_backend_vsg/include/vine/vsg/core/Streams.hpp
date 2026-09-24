#pragma once

#include <cstddef>
#include <cstdint>
#include <list>
#include <optional>
#include <unordered_map>
#include <vector>

#include <vine/vsg/vsg_global.hpp>

/**
 * @brief Content identity: which geometry streams two draws read, whether they can share one upload, and what
 * a data edit actually changed.
 *
 * THE PROBLEM THIS ANSWERS. A scene draws the same mesh from several slots, several passes may read one
 * target, and an application edits meshes while the GPU is still reading the uploads. "Same geometry" and
 * "changed geometry" are therefore questions about BYTES and SLICES, not about objects: a mesh split across
 * one arena buffer yields streams that differ only in their offset, and a refilled buffer yields a stream
 * whose bytes changed while its address did not. Geometry aliasing (one upload read by N draws) is only safe
 * if the thing that decides it is exactly this identity.
 *
 * WHY IT IS NOT A CACHE OF UPLOADS. The registry below (`SharedStreams`) knows which stream identities have
 * been offered and how many readers each has; it deliberately does NOT own the bytes. Ownership stays with
 * the API layer's retained nodes, because "the map dropped the entry" and "the bytes may be freed" are
 * different facts - a shared upload whose map entry was evicted is still read by everyone who bound it, and
 * the layer that owns the GPU object is the only one that may decide when it dies. Keeping that split is
 * what makes this file device-free and its rules testable without a GPU.
 *
 * THE PLAN IS A PURE FUNCTION. `planGeometry` takes two snapshots and returns what has to happen: nothing,
 * an in-place refresh of named streams, or a rebuild. It cannot read a buffer, so "the app wrote through a
 * raw pointer and only bumped the geometry-level revision" reaches it as a fact it is told, and its answer
 * for that case - rebuild, and do not reuse a shared upload - is the rule the previous implementation earned
 * by shipping the opposite one.
 */
V_VSG_NS_BEGIN

namespace core
{

/** @brief Which stream a key describes. */
enum class StreamKind : std::uint8_t
{
    Vertex,  ///< One vertex channel (an attribute location).
    Index,   ///< The index stream a draw reads.
};

/**
 * @brief Identity of one stream: a buffer slice, plus the revision that makes it current.
 *
 * THE SLICE IS PART OF THE IDENTITY. One buffer may hold several geometries' data, so two reads of it at
 * different offsets are two streams; treating them as one would feed a draw another geometry's bytes.
 *
 * `revision` comes from the UPSTREAM resource (`Geometry::revision()` / `Buffer::revision()`); the backend
 * never invents one by comparing bytes it happens to read. A refilled buffer is therefore a different key,
 * never a stale copy.
 *
 * A derived channel - one the backend builds itself because the model authors none (a white opacity
 * carrier, zero UVs, computed normals) - has a null @ref buffer. Such a channel is not a stream and can
 * never be shared: its bytes belong to one geometry and change with that geometry's own revision.
 */
struct StreamKey
{
    StreamKind    kind{StreamKind::Vertex};  ///< Vertex channel or index stream.
    std::uint32_t location{0};               ///< Attribute location; 0 for an index stream.
    std::uint32_t components{0};             ///< Components per element (the packed stride); 1 for indices.
    const void*   buffer{nullptr};           ///< Buffer the slice lives in; null for a derived channel.
    std::uint64_t revision{0};               ///< Upstream content revision.
    std::uint64_t offset{0};                 ///< First element of the slice.
    std::uint64_t count{0};                  ///< Elements the slice reads.

    /** @brief Compares the whole key: this is what "the same stream" means. */
    [[nodiscard]] bool operator==(const StreamKey& other) const noexcept;

    /** @brief Gets whether the channel is built by the backend rather than aliased from a buffer. */
    [[nodiscard]] bool derived() const noexcept;
};

/** @brief Hashes a StreamKey for the registry's map. */
struct StreamKeyHash
{
    /** @brief Hashes @p key. */
    std::size_t operator()(const StreamKey& key) const noexcept;
};

/** @brief Gets whether two keys describe the same channel with the same SHAPE (bytes excluded).
 *
 * Shape is what decides how a node is assembled - the location, the component count and the element count.
 * Two keys that differ only in their buffer, revision or offset are exactly the case an in-place refresh
 * serves; a shape difference is a rebuild.
 *
 * @param a First key.
 * @param b Second key.
 * @return true when both describe the same shape.
 */
[[nodiscard]] bool sameShape(const StreamKey& a, const StreamKey& b) noexcept;

/** @brief Gets whether two keys describe the same stream (shape and bytes).
 *
 * @param a First key.
 * @param b Second key.
 * @return true when both describe the same stream.
 */
[[nodiscard]] bool sameStream(const StreamKey& a, const StreamKey& b) noexcept;

/**
 * @brief Everything one retained data node reads: its vertex channels plus its index stream.
 *
 * `channels` is in ascending location order - the order the builder walks, and the order that makes two
 * snapshots comparable entry by entry. An ABSENT index stream is a different geometry from an empty one:
 * the first draws unindexed, the second draws zero triangles, and the difference is a shape change.
 */
struct GeometryStreams
{
    std::vector<StreamKey>    channels;  ///< Vertex channels, ascending location.
    std::optional<StreamKey>  index;     ///< The index stream, when the geometry draws indexed.
};

/** @brief A retained node's inputs: what it was built from, and the revision the model reported then. */
struct GeometrySnapshot
{
    GeometryStreams streams;             ///< The streams themselves.
    std::uint64_t   revision{0};         ///< Upstream geometry revision at the time of the build.
};

/** @brief What has to happen to keep a retained data node current. */
enum class GeometryAction : std::uint8_t
{
    None,     ///< Nothing changed: the retained node already reads exactly these streams.
    Refresh,  ///< Re-point the named streams in place; the node's shape is unchanged.
    Rebuild,  ///< Re-materialise the whole node.
};

/** @brief The answer of @ref planGeometry. */
struct GeometryPlan
{
    GeometryAction              action{GeometryAction::Rebuild};  ///< What to do.
    std::vector<std::uint32_t>  refreshed_locations;              ///< Channels whose bytes moved (ascending).
    bool                        index_refreshed{false};           ///< The index stream needs re-pointing.
    bool                        shared_uploads_allowed{true};     ///< Whether shared uploads may serve this build.
    bool                        unexplained_revision{false};      ///< The revision moved and no stream did.
};

/** @brief Decides how a retained data node reacts to the model's current state.
 *
 * The rules, in the order they are applied:
 *
 *   * nothing was built yet -> rebuild (every stream is new, so sharing is allowed);
 *   * the channel shape, the channel set or the presence of an index stream changed -> rebuild;
 *   * a channel's bytes moved, or the index buffer was replaced with the same span -> refresh exactly
 *     those streams;
 *   * the INDEX SPAN moved -> rebuild: the assembled node states first index and count, so a different span
 *     is a different draw, not a different buffer;
 *   * the revision moved while every stream stayed identical -> rebuild with sharing REFUSED. That
 *     announcement is one no stream accounts for (the app wrote through a pointer the buffer cannot see),
 *     and a shared upload holds the bytes as of its own insertion, so it cannot be vouched for here.
 *
 * @param built  What the retained node was built from (empty when nothing was built yet).
 * @param now    What the model reports now.
 * @return The plan (never throws; a rebuild is the safe answer for anything unclear).
 */
[[nodiscard]] GeometryPlan planGeometry(const GeometrySnapshot& built, const GeometrySnapshot& now) noexcept;

/**
 * @brief The registry of streams more than one draw reads: the geometry aliasing decision, without the bytes.
 *
 * WHAT IT IS FOR. The same mesh drawn by two slots must upload once. The decision belongs here because it is
 * pure bookkeeping - is this exact stream identity already offered, and may the caller alias it - while the
 * upload itself belongs to the layer that owns GPU objects.
 *
 * LIFETIME IS USAGE, NOT A READER COUNT. The first spelling kept a `readers` count and a `release()` the last
 * reader was supposed to call - and it could never fire: a draw acquires its streams once per FRAME (the
 * node it records into is rebuilt every frame), so the count became a cumulative tally of acquires and "the
 * last reader let go" was unreachable. MEASURED: `release()` had no production caller at all, and the only
 * thing that ever removed an entry was the capacity bound. What an entry's lifetime really follows is which
 * frame last NAMED it, so that is what an entry carries: @ref acquire stamps the frame it is called from, and
 * @ref releaseUnseen drops everything no frame has named for more than a grace window (the window the
 * executor parks replaced GPU objects for - `slots + 1`). A stream a frame still names is never dropped, and
 * a stream nothing names - a mesh the host stopped drawing, a revision that has moved on - leaves on its own.
 * A content-removal hook could not do that job: the store keeps a TRACKED object's rows whether or not any
 * frame draws it, so "the rows left" and "nothing names the stream" are different facts.
 *
 * WHAT EVICTION DOES NOT MEAN. The capacity is a HARD BOUND on the map, and what leaves when it is reached is
 * the STALEST entry (the one no frame named for longest) rather than the oldest inserted: a scene that
 * rotates its content re-names old entries, and there those are two different rows. What leaves the map is
 * only a lookup - the readers that already bound the stream hold their own reference to the bind, which is
 * why an entry leaving the map is not a release. `evictions()` counts exactly those departures so a phase can
 * see the bound being hit.
 *
 * NOT thread-safe: it is used from the frame's own thread, like the rest of the backend.
 */
class SharedStreams
{
  public:
    /** @brief Upper bound on the registry's map (FIFO past it, like the retained caches it replaces). */
    static constexpr std::size_t kDefaultCapacity = 512;

    /** @brief What a caller gets when it offers a stream. */
    enum class Action : std::uint8_t
    {
        Upload,  ///< No entry had this identity: the caller uploads, and the registry now remembers it.
        Alias,   ///< An entry had this identity: the caller reads the existing upload.
    };

    /** @brief The answer of @ref acquire. */
    struct Decision
    {
        Action                   action{Action::Upload};  ///< Whether to upload or to alias.
        std::optional<StreamKey> evicted;                 ///< The entry that made room, when the bound was hit.
    };

  public:
    /** @brief Constructs a registry with the given capacity.
     *
     * @param capacity Maximum entries the map holds; 0 is clamped to 1 so the registry stays usable.
     */
    explicit SharedStreams(std::size_t capacity = kDefaultCapacity);

    SharedStreams(const SharedStreams&) = delete;
    SharedStreams& operator=(const SharedStreams&) = delete;

  public:
    /** @brief Offers a stream from @p frame, telling the caller whether to upload it or to read what exists.
     *
     * NAMING IT IS WHAT KEEPS IT. The call stamps the entry with @p frame, which is the whole lifetime rule
     * (see the class note): an entry a frame names is never released, however many frames ago it was created.
     *
     * @param key   Stream identity (kind, location, components, buffer, revision, offset, count).
     * @param frame The frame this offer comes from (see core::FrameTimeline).
     * @return Whether this caller uploads or aliases, and - when this call pushed the STALEST entry out - the
     *         key that left the map. Reporting it is what lets a layer that OWNS one object per entry stay in
     *         step with this registry: it is the only moment the two can be reconciled, and doing it here keeps
     *         the registry free of callbacks.
     */
    [[nodiscard]] Decision acquire(const StreamKey& key, std::uint64_t frame);

    /** @brief Drops every entry no frame has named for more than @p grace frames.
     *
     * THE SWEEP'S HALF OF THE LIFETIME RULE, and the reason this registry needs no reader bookkeeping: what a
     * frame names survives (@ref acquire), and what nothing names leaves here. An entry named in @p frame has
     * an age of 0 and is kept whatever @p grace is; one named @p grace + 1 frames ago is dropped.
     *
     * @param frame   The frame the sweep runs in.
     * @param grace   How many frames an entry survives without being named (the caller's window).
     * @param dropped Receives the keys that left, in the order they were inserted; cleared first, and its
     *                capacity is kept - a caller that owns one object per entry sweeps its own maps with it.
     * @return How many entries left.
     */
    std::uint64_t releaseUnseen(std::uint64_t frame, std::uint64_t grace, std::vector<StreamKey>& dropped);

    /** @brief Gets the number of entries the map holds. */
    [[nodiscard]] std::size_t live() const noexcept;

    /** @brief Gets the number of identities that had to be uploaded (first reader of each entry). */
    [[nodiscard]] std::uint64_t uploads() const noexcept;

    /** @brief Gets the number of offers served by an existing entry. */
    [[nodiscard]] std::uint64_t aliases() const noexcept;

    /** @brief Gets the number of entries that left the map because the capacity was reached. */
    [[nodiscard]] std::uint64_t evictions() const noexcept;

    /** @brief Gets the number of entries that left because no frame named them any more. */
    [[nodiscard]] std::uint64_t unused() const noexcept;

    /** @brief Drops every entry, keeping the counters. */
    void clear() noexcept;

  private:
    struct Entry
    {
        StreamKey     key;            ///< The identity this entry stands for.
        std::uint64_t last_frame{0};  ///< The last frame that named it (the whole lifetime rule, see the class note).
    };

    std::size_t                                                         capacity_;
    std::list<Entry>                                                    order_;  ///< Insertion order (the index into it).
    std::unordered_map<StreamKey, std::list<Entry>::iterator, StreamKeyHash> index_;
    std::uint64_t                                                       uploads_{0};
    std::uint64_t                                                       aliases_{0};
    std::uint64_t                                                       evictions_{0};
    std::uint64_t                                                       unused_{0};
};

}  // namespace core

V_VSG_NS_END
