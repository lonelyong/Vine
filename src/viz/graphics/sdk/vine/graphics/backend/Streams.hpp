#pragma once

#include <cstddef>
#include <cstdint>

#include <vine/graphics/backend/BackendHalfGuard.hpp>
#include <vine/graphics/graphics_global.hpp>

/**
 * @brief Content identity: which geometry streams two draws read, and whether they can share one upload.
 *
 * @details
 * - THE PROBLEM THIS ANSWERS. A scene draws the same mesh from several slots, several passes may read
 *   one target, and an application edits meshes while the GPU is still reading the uploads. "Same
 *   geometry" and "changed geometry" are therefore questions about BYTES and SLICES, not about
 *   objects: a mesh split across one arena buffer yields streams that differ only in their offset, and
 *   a refilled buffer yields a stream whose bytes changed while its address did not. Geometry aliasing
 *   (one upload read by N draws) is only safe if the thing that decides it is exactly this identity.
 * - WHY IT DOES NOT OWN THE BYTES. This file answers "which bytes is this, and may two readers share one
 *   upload?" - never "when may the upload die". Ownership stays with the layer that owns the GPU object
 *   (`StreamUploads`, and the retirement queue behind its table), because "the map dropped the entry" and
 *   "the bytes may be freed" are different facts: an upload whose entry left is still read by everything that
 *   bound it. Keeping that split is what makes this file device-free and its rules testable without a GPU.
 */
VN_GRAPHICSBACKEND_NS_BEGIN

/** @brief Which stream a key describes. */
enum class StreamKind : std::uint8_t
{
    Vertex, // One vertex channel (an attribute location).
    Index,  // The index stream a draw reads.
};

/**
 * @brief Identity of one stream: a buffer slice, plus the revision that makes it current.
 *
 * @details
 * - THE SLICE IS PART OF THE IDENTITY. One buffer may hold several geometries' data, so two reads of
 *   it at different offsets are two streams; treating them as one would feed a draw another
 *   geometry's bytes.
 * - THE REVISION IS THE BUFFER'S OWN (`Buffer::revision()`), and there is no second one: a host edited
 *   bytes in place and said so on the buffer, or it did not, in which case the bytes are considered
 *   UNCHANGED - the renderer never compares bytes itself, and a geometry's own revision does not stand in
 *   for an announcement nobody made (see Geometry::setRevision, which speaks about the geometry's SHAPE).
 *   A stream that reads a shared arena re-keys for every geometry reading it, because the bytes are shared.
 * - The backend never invents a revision by comparing bytes it happens to read: a refilled buffer is a
 *   different key, never a stale copy.
 * - A derived channel - one the backend builds itself because the model authors none (a white
 *   opacity carrier, zero UVs, computed normals) - has a null @ref buffer. Such a channel is not a
 *   stream and can never be shared: its bytes belong to one geometry and change with that geometry's
 *   own revision.
 */
struct VN_GRAPHICS_API StreamKey {
    StreamKind    kind{ StreamKind::Vertex }; // Vertex channel or index stream.
    std::uint32_t location{ 0 };              // Attribute location; 0 for an index stream.
    std::uint32_t components{ 0 };            // Components per element (the packed stride); 1 for indices.
    const void*   buffer{ nullptr };          // Buffer the slice lives in; null for a derived channel.
    std::uint64_t revision{ 0 };              // The buffer's own revision (Buffer::revision()); 0 when derived.
    std::uint64_t offset{ 0 };                // First element of the slice.
    std::uint64_t count{ 0 };                 // Elements the slice reads.

    /** @brief Compares the whole key: this is what "the same stream" means. */
    [[nodiscard]] bool operator==(const StreamKey& other) const noexcept;

    /** @brief Gets whether the channel is built by the backend rather than aliased from a buffer. */
    [[nodiscard]] bool derived() const noexcept;
};

/** @brief Hashes a StreamKey for the sharing table's map. */
struct VN_GRAPHICS_API StreamKeyHash {
    /** @brief Hashes @p key. */
    std::size_t operator()(const StreamKey& key) const noexcept;
};

VN_GRAPHICSBACKEND_NS_END
