#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <span>
#include <vector>

#include <vine/geom/spatial/BvhTopology.hpp>
#include <vine/robotics/io/robot_io_global.hpp>

#include "IoUtils.hpp"

VN_ROBOTICS_IO_NS_BEGIN

namespace detail
{

/// Bytes of the header every triangle-tree cache starts with.
inline constexpr std::size_t kBvhCacheHeaderSize = 24;
/// Magic at the start of a cache file: "Vine BoTree".
inline constexpr std::array<char, 4> kBvhCacheMagic{ 'V', 'B', 'T', 'P' };
/// Layout version this build writes.
inline constexpr std::uint16_t kBvhCacheVersion = 1;
/// Bytes one node takes: its `offset` and its `count`.
inline constexpr std::size_t kBvhCacheNodeSize = 8;

/**
 * @brief What a cache file says about itself.
 *
 * @details
 * - The counts are inside the file, so nothing outside it has to agree with what was written: the reader checks them
 *   against the mesh that is actually there, and a mismatch simply means "this cache is not for this mesh".
 * - `flags` is reserved for statements about the whole file (the mesh format keeps its block bits and its format bits
 *   apart the same way); a bit this build has no name for makes the file unreadable rather than half-read.
 */
struct BvhCacheHeader {
    std::uint16_t version{ 0 };        // Format version; anything but kBvhCacheVersion is refused.
    std::uint16_t flags{ 0 };          // Reserved; must be zero.
    std::uint32_t triangle_count{ 0 }; // Triangles the stored tree indexes; also how many order entries follow.
    std::uint32_t node_count{ 0 };     // Node slots that follow.
    std::uint32_t depth{ 0 };          // Height of the stored tree, the root at 0.
};

/**
 * @brief Writes the header into its 24 bytes.
 *
 * @param out The bytes to fill; it has to hold kBvhCacheHeaderSize of them.
 * @param header The header to write.
 */
inline void writeBvhCacheHeader(std::span<char> out, const BvhCacheHeader& header) noexcept
{
    std::memset(out.data(), 0, kBvhCacheHeaderSize);
    std::memcpy(out.data(), kBvhCacheMagic.data(), kBvhCacheMagic.size());
    // Every number is little-endian, written by the format rather than by the machine.
    char* const version_at = out.data() + 4;
    version_at[0]          = static_cast<char>(header.version & 0xFFu);
    version_at[1]          = static_cast<char>((header.version >> 8) & 0xFFu);
    char* const flags_at   = out.data() + 6;
    flags_at[0]            = static_cast<char>(header.flags & 0xFFu);
    flags_at[1]            = static_cast<char>((header.flags >> 8) & 0xFFu);
    putU32Le(out.data() + 8, header.triangle_count);
    putU32Le(out.data() + 12, header.node_count);
    putU32Le(out.data() + 16, header.depth);
    putU32Le(out.data() + 20, 0u); // reserved
}

/**
 * @brief Reads a header, refusing anything this build does not understand.
 *
 * @param bytes The first kBvhCacheHeaderSize bytes of a cache file.
 * @param out Receives the header; untouched when this returns false.
 * @return true when the bytes are a cache file of a version this build reads.
 */
inline bool parseBvhCacheHeader(std::span<const char> bytes, BvhCacheHeader& out) noexcept
{
    if (bytes.size() < kBvhCacheHeaderSize || std::memcmp(bytes.data(), kBvhCacheMagic.data(), kBvhCacheMagic.size()) != 0) {
        return false;
    }
    BvhCacheHeader header;
    header.version = static_cast<std::uint16_t>(static_cast<unsigned char>(bytes[4]) | (static_cast<unsigned char>(bytes[5]) << 8));
    header.flags   = static_cast<std::uint16_t>(static_cast<unsigned char>(bytes[6]) | (static_cast<unsigned char>(bytes[7]) << 8));
    header.triangle_count = getU32Le(bytes.data() + 8);
    header.node_count     = getU32Le(bytes.data() + 12);
    header.depth          = getU32Le(bytes.data() + 16);
    if (header.version != kBvhCacheVersion || header.flags != 0u) {
        return false; // a layout or a whole-file statement this build has no name for
    }
    // One leaf per triangle is the smallest tree a build makes, and a node slot holds at most one leaf: twice the
    // triangle count is the widest legal node array (each leaf and each internal node take a slot). Compared in 64 bits:
    // a header claiming more triangles than a `u32` can double is refused rather than wrapped into a small bound.
    if (header.triangle_count == 0u || header.node_count == 0u ||
        static_cast<std::uint64_t>(header.node_count) > 2ull * header.triangle_count || header.depth > vn::geom::spatial::kBvhMaxDepth) {
        return false;
    }
    out = header;
    return true;
}

/**
 * @brief Returns how many bytes a stored shape takes: one header, its nodes, its primitive order.
 *
 * @details
 * - The number a caller sizes its buffer with, and the number the reader compares a file's length against: one
 *   definition, so a writer cannot allocate one size and a reader expect another.
 *
 * @param shape The shape to store.
 * @return The total byte count.
 */
[[nodiscard]] inline std::size_t bvhCacheByteCount(vn::geom::spatial::BvhTopologyView shape) noexcept
{
    return kBvhCacheHeaderSize + shape.nodes.size() * kBvhCacheNodeSize + shape.primitiveCount() * sizeof(std::uint32_t);
}

/**
 * @brief Encodes a shape into cache bytes.
 *
 * @details
 * - Only the shape is written: every box is recomputed from the positions on the way back in. That keeps the file at
 *   about 8 bytes per triangle and, more importantly, keeps it from ever disagreeing with the mesh's own bytes: a stale
 *   cache produces a worse tree, never a wrong answer.
 * - It takes a VIEW, so a caller that already holds the shape - decoded from a file, or read out of a tree's `topology()`
 *   - writes it without a second conversion.
 *
 * @param shape The shape to encode.
 * @param out   Receives the bytes; replaced, sized by bvhCacheByteCount().
 * @return true when the shape had something to store.
 */
[[nodiscard]] inline bool encodeBvhCache(vn::geom::spatial::BvhTopologyView shape, std::vector<unsigned char>& out)
{
    if (shape.empty() || shape.primitiveCount() > UINT32_MAX || shape.nodes.size() > UINT32_MAX) {
        return false;
    }
    BvhCacheHeader header;
    header.version        = kBvhCacheVersion;
    header.triangle_count = static_cast<std::uint32_t>(shape.primitiveCount());
    header.node_count     = static_cast<std::uint32_t>(shape.nodes.size());
    header.depth          = shape.depth;

    out.resize(bvhCacheByteCount(shape));
    char* const raw = reinterpret_cast<char*>(out.data());
    writeBvhCacheHeader(std::span<char>(raw, kBvhCacheHeaderSize), header);
    char* cursor = raw + kBvhCacheHeaderSize;
    for (const vn::geom::spatial::BvhNodeTopology& node : shape.nodes) {
        putU32Le(cursor + 0, node.offset);
        putU32Le(cursor + 4, node.count);
        cursor += kBvhCacheNodeSize;
    }
    for (const std::uint32_t id : shape.order) {
        putU32Le(cursor, id);
        cursor += sizeof(std::uint32_t);
    }
    return true;
}

/**
 * @brief Decodes cache bytes into a shape, refusing anything that does not add up.
 *
 * @details
 * - The file has to hold exactly what its header says: a header whose byte count disagrees with the buffer's size is a
 *   truncated or padded file, and reading it as far as it happens to go is how a half-restored tree would enter the
 *   process. Whether the numbers describe a particular triangle set is the caller's check, since only it knows one.
 *
 * @param bytes The whole file.
 * @param out   Receives the decoded shape; untouched when this returns false.
 * @return true when the bytes were read in full.
 */
[[nodiscard]] inline bool decodeBvhCache(std::span<const unsigned char> bytes, vn::geom::spatial::BvhTopology& out) noexcept
{
    if (bytes.size() < kBvhCacheHeaderSize) {
        return false;
    }
    const char* const raw = reinterpret_cast<const char*>(bytes.data());
    BvhCacheHeader    header;
    if (!parseBvhCacheHeader(std::span<const char>(raw, kBvhCacheHeaderSize), header)) {
        return false;
    }
    const std::uint64_t expected = kBvhCacheHeaderSize + static_cast<std::uint64_t>(header.node_count) * kBvhCacheNodeSize +
                                   static_cast<std::uint64_t>(header.triangle_count) * sizeof(std::uint32_t);
    if (bytes.size() != expected) {
        return false; // the file does not hold what its own header says
    }

    vn::geom::spatial::BvhTopology decoded;
    decoded.depth = header.depth;
    decoded.nodes.resize(header.node_count);
    decoded.order.resize(header.triangle_count);
    const char* cursor = raw + kBvhCacheHeaderSize;
    for (vn::geom::spatial::BvhNodeTopology& node : decoded.nodes) {
        node.offset = getU32Le(cursor + 0);
        node.count  = getU32Le(cursor + 4);
        cursor += kBvhCacheNodeSize;
    }
    for (std::uint32_t& id : decoded.order) {
        id = getU32Le(cursor);
        cursor += sizeof(std::uint32_t);
    }
    out = std::move(decoded);
    return true;
}

} // namespace detail

VN_ROBOTICS_IO_NS_END
