#include <vine/backend_vk/internal/PassTarget.hpp>

#include <vine/backend_vk/internal/TargetObjects.hpp>
#include <vine/backend_vk/internal/WindowTarget.hpp>

VN_BACKEND_VK_NS_BEGIN

PassTarget PassTarget::forTarget(TargetObjects& images) noexcept
{
    PassTarget target;
    target.images_        = &images;
    target.extent_        = VkExtent2D{ static_cast<std::uint32_t>(images.width()), static_cast<std::uint32_t>(images.height()) };
    target.compatibility_ = images.compatibility();
    return target;
}

PassTarget PassTarget::forWindow(WindowTarget& window) noexcept
{
    PassTarget target;
    target.window_        = &window;
    target.extent_        = VkExtent2D{ window.width(), window.height() };
    target.compatibility_ = window.compatibility();
    return target;
}

bool PassTarget::holdsPicture() const noexcept
{
    if (images_ == nullptr) {
        // THE ACQUIRED IMAGE HOLDS THE PRESENTATION ENGINE'S CONTENT, never a picture the host asked to keep: the first
        // pass into it clears, and a later pass of the same frame uses `FrameRecording::window_written` instead.
        return false;
    }
    return !images_->fresh.load(std::memory_order_relaxed);
}

void PassTarget::begin(const VolkDeviceTable& table, const VkCommandBuffer commands, const PassAttachmentOps& load, RecordingTally* const tally) const
{
    if (images_ != nullptr) {
        images_->beginPass(table, commands, load, tally);
        return;
    }
    if (window_ != nullptr) {
        window_->beginPass(commands, load, tally);
    }
}

void PassTarget::end(const VolkDeviceTable& table, const VkCommandBuffer commands) const noexcept
{
    if (images_ != nullptr) {
        images_->endPass(table, commands);
        return;
    }
    if (window_ != nullptr) {
        window_->endPass(commands);
    }
}

void PassTarget::rewind() const noexcept
{
    if (images_ != nullptr) {
        // THE WINDOW HALF OWES NOTHING (see the header): its image is given back, and the next acquire states its own
        // starting layout.
        images_->resetLayouts();
    }
}

VN_BACKEND_VK_NS_END
