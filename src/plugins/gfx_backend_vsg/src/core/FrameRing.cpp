#include <vine/vsg/core/FrameRing.hpp>

#include <algorithm>

VN_VSG_NS_BEGIN

namespace core
{

namespace
{

/** @brief Rounds @p value up to a multiple of @p alignment (which must not be zero). */
std::uint64_t alignUp(std::uint64_t value, std::uint64_t alignment) noexcept
{
    const std::uint64_t remainder = value % alignment;
    return remainder == 0 ? value : value + (alignment - remainder);
}

}  // namespace

FrameRing::FrameRing(const Layout& layout)
{
    stride_           = std::max<std::uint64_t>(layout.stride, 1U);
    alignment_        = std::max<std::uint64_t>(layout.alignment, 1U);
    slabs_            = std::max<std::uint32_t>(layout.slabs, 1U);
    blocks_per_frame_ = std::max<std::uint32_t>(layout.blocks_per_frame, 1U);

    // The aligned stride is the EFFECTIVE stride: every block starts aligned, so a reservation's offset can be
    // handed to a dynamic offset directly, with no per-caller rounding that could disagree with the layout.
    stride_     = alignUp(stride_, alignment_);
    slab_bytes_ = stride_ * blocks_per_frame_;
}

void FrameRing::beginFrame() noexcept
{
    if (started_) {
        ++frames_;
    }
    else {
        started_ = true;
    }
    reserved_ = 0;
}

FrameRing::Reservation FrameRing::reserve() noexcept
{
    if (reserved_ >= blocks_per_frame_) {
        // The budget is a promise to the GPU memory, not a hint: refusing is the only answer that keeps a
        // submitted command buffer's block valid. The caller reports it and rebuilds the ring deliberately.
        ++overflows_;
        return {};
    }

    const std::uint64_t offset = static_cast<std::uint64_t>(slot()) * slab_bytes_ +
                                 static_cast<std::uint64_t>(reserved_) * stride_;
    ++reserved_;
    high_water_ = std::max(high_water_, reserved_);
    return {true, offset};
}

std::uint64_t FrameRing::stride() const noexcept
{
    return stride_;
}

std::uint64_t FrameRing::alignment() const noexcept
{
    return alignment_;
}

std::uint64_t FrameRing::slabBytes() const noexcept
{
    return slab_bytes_;
}

std::uint64_t FrameRing::capacityBytes() const noexcept
{
    return slab_bytes_ * slabs_;
}

std::uint64_t FrameRing::frames() const noexcept
{
    return frames_;
}

std::uint32_t FrameRing::slot() const noexcept
{
    return static_cast<std::uint32_t>(frames_ % slabs_);
}

std::uint32_t FrameRing::reserved() const noexcept
{
    return reserved_;
}

std::uint32_t FrameRing::highWater() const noexcept
{
    return high_water_;
}

std::uint64_t FrameRing::overflows() const noexcept
{
    return overflows_;
}

}  // namespace core

VN_VSG_NS_END
