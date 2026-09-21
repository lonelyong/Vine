#include <vine/vsg/core/FrameArena.hpp>

#include <algorithm>

V_VSG_NS_BEGIN

namespace core
{

FrameArena::FrameArena(std::size_t chunk_size)
  : chunk_size_(chunk_size == 0 ? 4096 : chunk_size)
{
    // The first chunk is the reservation, not a growth: it must not show up in allocations(), or every
    // steady frame would look like it allocated.
    chunks_.push_back(Chunk{std::make_unique<std::byte[]>(chunk_size_), chunk_size_, 0});
}

FrameArena::~FrameArena() = default;

void FrameArena::reset() noexcept
{
    for (Chunk& chunk : chunks_)
    {
        chunk.used = 0;
    }
    cursor_ = 0;
}

void* FrameArena::allocate(std::size_t bytes, std::size_t alignment)
{
    if (bytes == 0)
    {
        return nullptr;
    }
    if (alignment == 0 || (alignment & (alignment - 1)) != 0 || alignment > alignof(std::max_align_t))
    {
        // Documented limit: a chunk comes from `new std::byte[]`, so an alignment stricter than the
        // platform's default new alignment cannot be served - refused rather than served misaligned.
        return nullptr;
    }

    for (;;)
    {
        Chunk&            chunk   = chunks_[cursor_];
        const std::size_t aligned = (chunk.used + (alignment - 1)) & ~(alignment - 1);
        if (aligned + bytes <= chunk.capacity)
        {
            chunk.used = aligned + bytes;
            return chunk.storage.get() + aligned;
        }

        if (cursor_ + 1 < chunks_.size())
        {
            // A chunk from an earlier frame still has room: reuse it instead of adding storage. This is
            // what keeps a steady frame at zero new chunks.
            ++cursor_;
            continue;
        }

        // A new chunk, big enough for this request AND for the alignment slack in front of it. The
        // reference above must not be touched afterwards: push_back may move the chunk objects (their
        // storage does not move - that is the point of the chunk list).
        addChunk(std::max(chunk_size_, bytes + alignment));
        ++cursor_;
    }
}

std::size_t FrameArena::bytesUsed() const noexcept
{
    std::size_t used = 0;
    for (const Chunk& chunk : chunks_)
    {
        used += chunk.used;
    }
    return used;
}

std::size_t FrameArena::bytesReserved() const noexcept
{
    std::size_t reserved = 0;
    for (const Chunk& chunk : chunks_)
    {
        reserved += chunk.capacity;
    }
    return reserved;
}

std::size_t FrameArena::allocations() const noexcept
{
    return allocations_;
}

void FrameArena::addChunk(std::size_t bytes)
{
    chunks_.push_back(Chunk{std::make_unique<std::byte[]>(bytes), bytes, 0});
    ++allocations_;
}

}  // namespace core

V_VSG_NS_END
