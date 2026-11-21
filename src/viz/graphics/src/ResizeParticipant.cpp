#include <vine/graphics/ResizeParticipant.hpp>

#include <utility>

VN_GRAPHICS_NS_BEGIN

void ResizeParticipant::resize(int width, int height, double pixel_ratio)
{
    if (width <= 0 || height <= 0) {
        return; // a size that is not a size is not an announcement
    }
    width_  = width;
    height_ = height;
    if (pixel_ratio > 0.0) {
        pixel_ratio_ = pixel_ratio;
    }
}

void ResizeParticipant::addStep(Step step)
{
    // One list: a logical-pixel step is the ratio-aware step that ignores the ratio, so registration order is
    // the run order whatever spelling a host used.
    steps_.push_back([step = std::move(step)](int width, int height, double) { step(width, height); });
}

void ResizeParticipant::addStep(StepWithRatio step)
{
    steps_.push_back(std::move(step));
}

void ResizeParticipant::clearSteps()
{
    steps_.clear();
}

void ResizeParticipant::apply(const StepWithRatio& view_step) const
{
    applySize(width_, height_, pixel_ratio_, view_step);
}

void ResizeParticipant::applySize(int width, int height, double pixel_ratio, const StepWithRatio& view_step) const
{
    if (width <= 0 || height <= 0) {
        return; // a size that is not a size is not a resize
    }
    if (view_step) {
        view_step(width, height, pixel_ratio);
    }
    for (const StepWithRatio& step : steps_) {
        step(width, height, pixel_ratio);
    }
}

VN_GRAPHICS_NS_END
