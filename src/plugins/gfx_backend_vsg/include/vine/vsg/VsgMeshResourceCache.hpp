#pragma once
#include "vsg_global.hpp"

#include <cstddef>
#include <cstdint>
#include <memory>

#include <vsg/commands/BindIndexBuffer.h>
#include <vsg/commands/BindVertexBuffers.h>
#include <vsg/core/ref_ptr.h>

#include <vine/raw_ptr.hpp>

namespace vine::graphics
{
class Geometry;
}

V_VSG_NS_BEGIN

/**
 * @brief Shares the bind commands of VERTEX/INDEX streams whose bytes are the model's own.
 *
 * WHY. A bind command OWNS its BufferInfo, and a BufferInfo is what vsg turns into one device buffer plus
 * its upload (`BindVertexBuffers::compile()` → `createBufferAndTransferData`). So N geometries that read the
 * same mesh stream — instanced meshes, a model shared between drawables, the same scene drawn by two slots —
 * each built their own bind and therefore uploaded the same bytes N times. Handing them ONE bind makes the
 * stream upload once and be read from one device buffer by every drawable.
 *
 * WHAT IS SHAREABLE. Only channels the builder ALIASES verbatim from a `vine::Buffer`: positions, authored
 * normals, authored texcoords, an authored four-component loc2 colour and the index stream. The channels the
 * backend BUILDS are not: the white opacity carrier is per-drawable by definition (its alpha is the
 * drawable's opacity), zero UVs and derived normals are computed per geometry, and a THREE-component loc2
 * colour is packed into a vec4 per geometry. The custom channels (location >= 3) are bound through one
 * command whose identity is the whole layout, so they stay per node as well.
 *
 * WHO MAY SHARE. A shared bind holds a copy of the bytes as of its insertion, and the key cannot tell a
 * buffer that was written through a pointer the buffer itself cannot see (see `Geometry::setRevision`: the
 * geometry-level revision is what reports that). This cache is therefore only consulted for a build whose
 * revision a STREAM accounts for — a caller that cannot attribute what moved passes a null cache and takes
 * its own copies (see SceneBridge::buildGeometryData).
 *
 * THE KEY IS THE STREAM, NOT THE GEOMETRY: binding + component count + the buffer's address + its content
 * revision + the SLICE the array reads (first scalar and length). A refilled buffer carries a new revision
 * and an arena's segments differ in their offset, and both are DIFFERENT keys — so the entry served is always
 * one whose BufferInfo was built from exactly those bytes, never a stale copy.
 *
 * WHAT IT SHARES WITH THE VERTEX SLICE. The bind is per (stream, slice): the vertex array aliases the slice,
 * so two geometries reading different segments of one arena must not share the command. The INDEX bind is the
 * exception by design — it aliases the whole buffer and the DRAW states the span (see SceneBridge), so every
 * geometry slicing one index arena resolves to the same key and shares one index upload.
 *
 * LIFETIME. An entry holds the bind, and the bind holds the array, which holds the model buffer (see
 * VsgBufferView): a shared stream therefore keeps its bytes alive, and releaseAbandoned() drops the entries
 * whose last user is gone (the cache being the only holder left). Capacity is a FIFO bound on top, so a
 * session that keeps generating meshes cannot grow it without limit.
 *
 * NOT thread-safe: it is used from the frame's own thread, like the rest of the bridge.
 */
class V_VSG_API VsgMeshResourceCache
{
  public:
    /** @brief Upper bound on cached shared binds (FIFO past it, like the other retained caches). */
    static constexpr std::size_t kMaxEntries = 512;

    /** @brief Identity of one aliased stream (what a shared bind reads). */
    struct ChannelKey
    {
        std::uint32_t binding = 0;
        std::uint32_t components = 0;
        const void*   buffer = nullptr;   ///< The `vine::Buffer` the array aliases.
        std::uint64_t revision = 0;       ///< Its content revision (a refill is a different stream).
        std::size_t   offset = 0;         ///< First scalar the array reads (an arena's segments differ here).
        std::size_t   count = 0;          ///< Scalars (vertex channels) or indices the stream reads.

        /** @brief Whether two keys describe the same stream. */
        bool operator==(const ChannelKey& other) const noexcept
        {
            return binding == other.binding && components == other.components && buffer == other.buffer &&
                   revision == other.revision && offset == other.offset && count == other.count;
        }
    };

    /** @brief Hashes a ChannelKey for the cache's map. */
    struct ChannelKeyHash
    {
        /** @brief Hashes @p key. */
        std::size_t operator()(const ChannelKey& key) const noexcept;
    };

  public:
    VsgMeshResourceCache();
    ~VsgMeshResourceCache();

    VsgMeshResourceCache(const VsgMeshResourceCache&) = delete;
    VsgMeshResourceCache& operator=(const VsgMeshResourceCache&) = delete;

  public:
    /**
     * @brief Gets (or creates) the bind command one aliased vertex channel is bound through.
     *
     * @param key   Stream identity (binding, components, buffer, revision, count).
     * @param array The array that reads it. It is whatever the caller would have bound anyway, so the cache
     *              never has to know how a channel's element type is chosen; the FIRST array for a key is
     *              the one that gets bound.
     * @return The shared bind, never null (a null @p array yields a bind over an empty list).
     */
    ::vsg::ref_ptr<::vsg::BindVertexBuffers> getOrCreateVertexBind(const ChannelKey&                  key,
                                                                  const ::vsg::ref_ptr<::vsg::Data>& array);

    /**
     * @brief Gets (or creates) the bind command one aliased index stream is read through.
     *
     * @param key     Stream identity (buffer, revision, index count).
     * @param indices The index array that reads it.
     * @return The shared bind, never null.
     */
    ::vsg::ref_ptr<::vsg::BindIndexBuffer> getOrCreateIndexBind(const ChannelKey&                  key,
                                                               const ::vsg::ref_ptr<::vsg::Data>& indices);

    /** @brief Drops the entries nothing references any more (see the class comment).
     *
     * @return Number of released entries.
     */
    std::size_t releaseAbandoned();

    /** @brief Gets the number of cached shared binds (vertex and index together). */
    std::size_t count() const;

    /** @brief Releases every cached bind. */
    void clear();

  private:
    struct Data;
    // Owns the cache through RAII (the repo's "avoid raw owning pointers" rule); declared after Data so the
    // out-of-line destructor is the only place that needs the complete type.
    std::unique_ptr<Data> d;
};

V_VSG_NS_END
