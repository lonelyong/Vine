#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>

#include <vsg/commands/BindIndexBuffer.h>
#include <vsg/commands/BindVertexBuffers.h>
#include <vsg/core/Data.h>
#include <vsg/core/ref_ptr.h>

#include <vine/vsg/core/Streams.hpp>
#include <vine/vsg/vsg_global.hpp>

/**
 * @brief The device-facing half of geometry aliasing: one bind (and therefore one upload) per stream identity.
 *
 * WHY THE BIND IS THE UNIT. A bind command owns its `BufferInfo`, and a `BufferInfo` is what becomes one
 * device buffer plus its upload. Two drawables reading the same stream, each with their own bind, therefore
 * upload the same bytes twice; handing both the SAME bind command is what makes the stream exist once on the
 * GPU and be read twice. That is the whole mechanism this store wraps.
 *
 * WHAT IT DELEGATES. Which identity was already offered, and which frame last named it, is
 * `core::SharedStreams` - the decision is bookkeeping, and it is testable without a GPU. This class only
 * owns the objects and keeps its map in step with the registry: a key that leaves the registry (the capacity
 * bound, or a sweep that found nothing naming it any more) drops its bind here too - the readers that already
 * bound it hold their own reference, so nothing they read disappears.
 *
 * LIFETIME: A FRAME NAMES IT, OR IT GOES. The frame calls @ref beginFrame once (the driver of the frame's
 * content - see api/ContentAssembly), every acquire stamps the entry, and the frame's last step calls
 * @ref releaseUnseen: what no frame named for the grace window leaves, map entry and bind together. There is
 * deliberately no reader count: a draw acquires its streams once per FRAME, so such a count could only ever
 * go up, and its `release()` had no production caller at all (see core::SharedStreams's note, which is where
 * that spelling is written down as the defect it was).
 *
 * WHAT MAY BE SHARED (and what is refused instead):
 *
 *   * the four CANONICAL vertex channels, when their bytes are the model's own (positions, normals,
 *     texcoords, the loc2 colour). A custom channel's bind is one command whose identity is the whole custom
 *     LAYOUT, and a channel the backend BUILDS carries this drawable's own values (the white opacity carrier
 *     even carries its opacity) - sharing either would draw every peer with the first drawable's data, so
 *     both are refused here and the caller builds a private bind;
 *   * the INDEX stream, keyed on the whole buffer it aliases. The DRAW states which span it reads
 *     (first index / count), so two geometries slicing one index arena share one index upload - which is why
 *     the index key is normalised here (offset 0, count = the array's element count) and not taken from the
 *     caller's span. The array passed in must alias the whole buffer for that to hold.
 *
 * WHAT DROPPING AN ENTRY DOES NOT DO. An entry holds the bind, and the bind holds the arrays, which alias
 * the model buffers - so an entry leaving the map (the sweep above, or the capacity bound) never frees bytes a
 * frame may still be reading: the frame that bound it holds its own reference to the bind, and what the
 * executor's retirement queue keeps alive is that node (see the design's D4/D7). This store is a LOOKUP, not
 * the owner of last resort, which is why nothing here is parked.
 *
 * NOT thread-safe: it is used from the frame's own thread, like the rest of the backend.
 */
VN_VSG_NS_BEGIN

/** @brief Shared stream uploads, keyed by `core::StreamKey`. */
class StreamUploads
{
  public:
    /** @brief What an acquire did. */
    enum class Action : std::uint8_t
    {
        Uploaded,  ///< A bind was built for this stream (the bytes will be uploaded once).
        Aliased,   ///< An existing bind serves this stream.
        Refused,   ///< This stream may not be shared (see the file note); the caller builds a private bind.
    };

    /** @brief The answer of @ref acquireVertex. */
    struct VertexResult
    {
        Action                                       action{Action::Refused};  ///< What happened.
        ::vsg::ref_ptr<::vsg::BindVertexBuffers>     bind;                    ///< The bind (null when refused).
    };

    /** @brief The answer of @ref acquireIndex. */
    struct IndexResult
    {
        Action                                       action{Action::Refused};  ///< What happened.
        ::vsg::ref_ptr<::vsg::BindIndexBuffer>       bind;                    ///< The bind (null when refused).
    };

    /** @brief Sentinel: a shader location that has no canonical binding (a custom channel). */
    static constexpr std::uint32_t kNoBinding = ~std::uint32_t{ 0 };

  public:
    /** @brief Constructs a store with the given capacity.
     *
     * @param capacity Maximum shared entries; 0 is clamped to 1 (see `core::SharedStreams`).
     */
    explicit StreamUploads(std::size_t capacity = core::SharedStreams::kDefaultCapacity);
    ~StreamUploads();

    StreamUploads(const StreamUploads&) = delete;
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
     * @param key   Stream identity; its kind must be Vertex and its location canonical.
     * @param array The array that reads the stream (whatever the caller would have bound anyway).
     * @return The action and the bind (null when the stream may not be shared).
     */
    [[nodiscard]] VertexResult acquireVertex(const core::StreamKey& key, ::vsg::ref_ptr<::vsg::Data> array);

    /** @brief Gets (or builds) the shared bind for an index stream.
     *
     * @param key     Stream identity; its kind must be Index.
     * @param indices The array aliasing the WHOLE index buffer (see the file note).
     * @return The action and the bind (null when the stream may not be shared).
     */
    [[nodiscard]] IndexResult acquireIndex(const core::StreamKey& key, ::vsg::ref_ptr<::vsg::Data> indices);

    /** @brief Opens a frame: the stamp every acquire from it carries (see the file note).
     *
     * @param frame The frame being recorded (`core::FrameTimeline::submittedFrame`).
     * @param grace How many frames an entry survives without being named (`slots + 1`: the same window the
     *              executor parks a replaced GPU object for).
     */
    void beginFrame(std::uint64_t frame, std::uint32_t grace) noexcept;

    /** @brief Drops the entries no frame has named for the grace window, their binds included.
     *
     * The frame's last step for streams (see the file note): it is what lets a stream the host stopped
     * drawing leave on its own, and it is idempotent - a second call in the same frame releases nothing.
     *
     * @return How many entries left (each one's bind went with it).
     */
    std::uint64_t releaseUnseen();
  public:
    /** @brief Gets the number of entries the registry holds. */
    [[nodiscard]] std::size_t live() const noexcept;

    /** @brief Gets the number of binds the store holds. */
    [[nodiscard]] std::size_t objects() const noexcept;

    /** @brief Gets the number of binds built (one upload each). */
    [[nodiscard]] std::uint64_t uploads() const noexcept;

    /** @brief Gets the number of acquires an existing bind answered. */
    [[nodiscard]] std::uint64_t aliases() const noexcept;

    /** @brief Gets the number of acquires refused because the stream may not be shared. */
    [[nodiscard]] std::uint64_t refusals() const noexcept;

    /** @brief Gets the number of entries that left because the capacity was reached. */
    [[nodiscard]] std::uint64_t evictions() const noexcept;

    /** @brief Gets the number of entries that left because no frame had named them any more. */
    [[nodiscard]] std::uint64_t unused() const noexcept;

    /** @brief Checks the store's own map against the registry that decides for it.
     *
     * The two are written at different places - an entry exists because the registry said so, an object
     * exists because this store built it - so a bug can make one of them drift. Where they describe the same
     * thing ("one entry, one bind") they are asserted equal instead of believed.
     *
     * @return true when the store holds exactly the entries the registry does.
     */
    [[nodiscard]] bool agreesWithRegistry() const noexcept;

    /** @brief Drops every entry and bind, keeping the counters (a session was replaced). */
    void clear();

  private:
    struct Data;
    // Lexically after Data so the out-of-line destructor is the only place that needs the complete type.
    std::unique_ptr<Data> d;
};

VN_VSG_NS_END
