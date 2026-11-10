#pragma once

#include <cstdint>
#include <span>
#include <vector>

#include <vine/Buffer.hpp>
#include <vine/graphics/ShaderAbi.hpp>
#include <vine/graphics/backend/Streams.hpp>
#include <vine/intrusive_ptr.hpp>
#include <vine/vsg/internal/StreamUploads.hpp>

/**
 * @brief Uploads the streams a test drawable reads, with the MODEL BUFFERS production always hands over.
 *
 * WHY THIS EXISTS. `StreamUploads` shares a bind only when the caller hands over the buffer its key names: the
 * entry's hold on that buffer is what reserves the address for as long as the key can be looked up, and it is
 * also the handle the entry compares its own revision against. A test that passes a bare vsg array plus a
 * stand-in address describes a caller production does not have - and the store refuses it (correctly: a
 * snapshot served to whatever buffer lands on that address next is a wrong picture with no refusal anywhere).
 * These fixtures therefore build the model buffers a real drawable would have and upload through them.
 *
 * The returned handles are the caller's: the store keeps its own shares, and the binds alias these buffers.
 */
namespace vn::test
{

/** @brief The model buffers a test drawable reads, and the store's answers for their streams. */
struct UploadedStreams {
    vn::intrusive_ptr<const vn::Buffer<float>>         positions;  // The position channel's bytes.
    vn::intrusive_ptr<const vn::Buffer<std::uint32_t>> indices;    // The index stream's bytes.
    vn::vsg::StreamUploads::VertexResult               vertex{};   // What the store answered for the channel.
    vn::vsg::StreamUploads::IndexResult                index{};    // What it answered for the index stream.
};

/**
 * @brief Uploads one position channel and one index stream through @p uploads.
 *
 * @param uploads The store the drawable's streams are shared through.
 * @param scalars Position scalars, three per vertex (the ABI's location for Position).
 * @param indices The whole index list the draw reads.
 * @return The model buffers (held by the caller) and the store's answers.
 */
inline UploadedStreams uploadTriangle(vn::vsg::StreamUploads& uploads, std::span<const float> scalars, std::span<const std::uint32_t> indices)
{
    UploadedStreams out;
    out.positions = vn::intrusive_ptr<const vn::Buffer<float>>(new vn::Buffer<float>(std::vector<float>(scalars.begin(), scalars.end())));
    out.indices   = vn::intrusive_ptr<const vn::Buffer<std::uint32_t>>(new vn::Buffer<std::uint32_t>(std::vector<std::uint32_t>(indices.begin(), indices.end())));

    vn::graphics::backend::StreamKey vertex_key;
    vertex_key.kind       = vn::graphics::backend::StreamKind::Vertex;
    vertex_key.location   = vn::graphics::attributeLocation(vn::graphics::VertexAttribute::Position);
    vertex_key.components = 3U;
    vertex_key.buffer     = out.positions.get();
    vertex_key.revision   = out.positions->revision();
    vertex_key.count      = out.positions->size();

    vn::graphics::backend::StreamKey index_key;
    index_key.kind       = vn::graphics::backend::StreamKind::Index;
    index_key.location   = 0U;
    index_key.components = 1U;
    index_key.buffer     = out.indices.get();
    index_key.revision   = out.indices->revision();
    index_key.count      = out.indices->size();

    out.vertex = uploads.acquireVertex(vertex_key, out.positions->view(), out.positions);
    out.index  = uploads.acquireIndex(index_key, out.indices->view(), out.indices);
    return out;
}

} // namespace vn::test
