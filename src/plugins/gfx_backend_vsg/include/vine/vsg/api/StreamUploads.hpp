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
 * WHAT IT DELEGATES. Which identity was already offered, and how many readers it has, is
 * `core::SharedStreams` - the decision is bookkeeping, and it is testable without a GPU. This class only
 * owns the objects and keeps its map in step with the registry: when an acquire reports that the oldest
 * entry left the map, the corresponding bind is dropped here too (the readers that already bound it hold
 * their own reference, so nothing they read disappears).
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
 * LIFETIME. An entry holds the bind, and the bind holds the arrays, which alias the model buffers. Dropping
 * an entry therefore never frees bytes a frame may still be reading unless nobody else holds the bind - and a
 * retained node that IS holding it is what the executor's retirement queue keeps alive until its frames are
 * done (see the design's D4/D7). The store is a lookup, not an owner of last resort.
 *
 * NOT thread-safe: it is used from the frame's own thread, like the rest of the backend.
 */
V_VSG_NS_BEGIN

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

    /** @brief Gives up one reader of a stream.
     *
     * @param key Stream identity; an index key is normalised the same way @ref acquireIndex normalises it, so
     *            the draw's span does not matter here.
     * @return true when this was the last reader, so the store dropped the bind.
     */
    bool release(const core::StreamKey& key);

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

V_VSG_NS_END
