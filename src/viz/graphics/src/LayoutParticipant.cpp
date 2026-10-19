#include <vine/graphics/LayoutParticipant.hpp>

#include <utility>

VN_GRAPHICS_NS_BEGIN

void LayoutParticipant::resize(int width, int height, double pixel_ratio)
{
    if (width <= 0 || height <= 0) {
        return;  // a size that is not a size is not an announcement
    }
    width_  = width;
    height_ = height;
    if (pixel_ratio > 0.0) {
        pixel_ratio_ = pixel_ratio;
    }
}

void LayoutParticipant::addStep(Step step)
{
    // One list: a logical-pixel step is the ratio-aware step that ignores the ratio, so registration order is
    // the run order whatever spelling a host used.
    steps_.push_back([step = std::move(step)](int width, int height, double) { step(width, height); });
}

void LayoutParticipant::addStep(StepWithRatio step)
{
    steps_.push_back(std::move(step));
}

void LayoutParticipant::clearSteps()
{
    steps_.clear();
}

void LayoutParticipant::apply(const StepWithRatio& view_step) const
{
    applySize(width_, height_, pixel_ratio_, view_step);
}

void LayoutParticipant::applySize(int width, int height, double pixel_ratio, const StepWithRatio& view_step) const
{
    if (width <= 0 || height <= 0) {
        return;  // a size that is not a size is not a layout
    }
    if (view_step) {
        view_step(width, height, pixel_ratio);
    }
    for (const StepWithRatio& step : steps_) {
        step(width, height, pixel_ratio);
    }
}

VN_GRAPHICS_NS_END
