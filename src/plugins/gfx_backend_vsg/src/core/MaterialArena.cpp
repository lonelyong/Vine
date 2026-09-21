#include <vine/vsg/core/MaterialArena.hpp>

#include <algorithm>

V_VSG_NS_BEGIN

namespace core
{

MaterialArena::MaterialArena(const Layout& layout)
{
    layout_.block_bytes = std::max<std::uint32_t>(layout.block_bytes, 1U);
    layout_.copies      = std::max<std::uint32_t>(layout.copies, 1U);
    layout_.capacity    = std::max<std::uint32_t>(layout.capacity, 1U);
}

std::uint32_t MaterialArena::currentCopy() const noexcept
{
    return static_cast<std::uint32_t>(frame_ % layout_.copies);
}

void MaterialArena::beginFrame() noexcept
{
    // The first call begins frame 0: a caller that calls this at the top of every frame (the only correct
    // way to use it) then sees the frames numbered from zero, and the copy rotation starts at copy zero.
    if (started_) {
        ++frame_;
    }
    else {
        started_ = true;
    }
}

std::uint64_t MaterialArena::offsetOf(std::uint32_t slot, std::uint32_t copy) const noexcept
{
    // Slot-major: all copies of one material sit together. The API layer binds with a dynamic offset, so the
    // only requirement is that this function and its buffer allocation agree - and they do, because the size
    // comes from capacityBytes().
    const std::uint64_t copies = layout_.copies;
    return (static_cast<std::uint64_t>(slot) * copies + copy) * layout_.block_bytes;
}

MaterialArena::Write MaterialArena::note(const void* material, std::uint64_t revision)
{
    const std::uint32_t copy = currentCopy();

    const auto found = index_.find(material);
    if (found != index_.end()) {
        Entry& entry = *found->second;
        if (entry.revision == revision) {
            // The bytes the storage holds are the bytes the material has: this is the steady frame, and the
            // whole reason materials have their own arena instead of a per-frame write.
            ++hits_;
            return {WriteKind::Unchanged, entry.slot, entry.last_copy, 0};
        }
        if (entry.last_write_frame != frame_) {
            entry.revision        = revision;
            entry.last_copy       = copy;
            entry.last_write_frame = frame_;
            ++writes_;
            bytes_written_ += layout_.block_bytes;
            return {WriteKind::Rewritten, entry.slot, copy, layout_.block_bytes};
        }
        // Already written this frame: the second edit of a value the GPU can only ever see one version of.
        entry.revision = revision;
        ++hits_;
        return {WriteKind::Unchanged, entry.slot, entry.last_copy, 0};
    }

    // A slot is allocated for a material the arena has not seen. Past the capacity the OLDEST entry leaves -
    // the same FIFO bound the retained caches use, and the same meaning: the newest are the ones a live scene
    // is using, and an evicted material simply allocates again on its next note.
    if (index_.size() >= layout_.capacity) {
        Entry& oldest = order_.front();
        index_.erase(oldest.material);
        free_slots_.push_back(oldest.slot);
        order_.pop_front();
        ++evictions_;
    }

    std::uint32_t slot = 0;
    if (!free_slots_.empty()) {
        slot = free_slots_.back();
        free_slots_.pop_back();
    }
    else {
        slot = next_slot_++;
    }

    order_.push_back(Entry{material, revision, slot, frame_, copy});
    index_[material] = std::prev(order_.end());
    ++allocations_;
    ++writes_;
    bytes_written_ += layout_.block_bytes;
    return {WriteKind::Allocated, slot, copy, layout_.block_bytes};
}

bool MaterialArena::release(const void* material) noexcept
{
    const auto found = index_.find(material);
    if (found == index_.end()) {
        return false;
    }
    free_slots_.push_back(found->second->slot);
    order_.erase(found->second);
    index_.erase(found);
    return true;
}

std::uint64_t MaterialArena::capacityBytes() const noexcept
{
    return static_cast<std::uint64_t>(layout_.capacity) * layout_.copies * layout_.block_bytes;
}

std::uint32_t MaterialArena::blockBytes() const noexcept
{
    return layout_.block_bytes;
}

std::uint32_t MaterialArena::copies() const noexcept
{
    return layout_.copies;
}

std::uint32_t MaterialArena::capacity() const noexcept
{
    return layout_.capacity;
}

std::uint64_t MaterialArena::frame() const noexcept
{
    return frame_;
}

std::size_t MaterialArena::live() const noexcept
{
    return index_.size();
}

std::uint64_t MaterialArena::writes() const noexcept
{
    return writes_;
}

std::uint64_t MaterialArena::allocations() const noexcept
{
    return allocations_;
}

std::uint64_t MaterialArena::hits() const noexcept
{
    return hits_;
}

std::uint64_t MaterialArena::evictions() const noexcept
{
    return evictions_;
}

std::uint64_t MaterialArena::bytesWritten() const noexcept
{
    return bytes_written_;
}

void MaterialArena::clear() noexcept
{
    order_.clear();
    index_.clear();
    free_slots_.clear();
    next_slot_ = 0;
}

}  // namespace core

V_VSG_NS_END
