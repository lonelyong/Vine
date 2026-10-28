#pragma once
#include "graphics_global.hpp"

#include <functional>
#include <vector>

VN_GRAPHICS_NS_BEGIN

/**
 * @brief A view's participation in a surface's layout: the size it draws at, and the steps that follow it.
 *
 * @details
 * - WHAT IT OWNS is the layout half of a view and nothing else: the last size the host announced (logical pixels plus
 *   the device pixel ratio), and the creator-managed steps registered against it. It knows no camera, no pipeline and
 *   no surface - the consumer supplies ONE step (its own: camera aspect, manipulator resize, pipeline resize) and the
 *   participant runs it before the registered steps, in the registered order.
 * - WHY IT IS SEPARATE. Resizing has three readers with different owners: the CAMERA (whose picture is built for one
 *   shape), the ADOPTED PIPELINE (whose off-screen targets are sized for the surface), and whoever registered a step
 *   (a preview re-anchoring itself in a corner). The first two are the view's; the third is the host's. Keeping the
 *   announcement and the step list in one small object is what lets a host participate in layout without reaching
 *   into a view's internals - and what keeps `SceneView` a facade rather than the place every layout rule accumulates.
 * - THE SIZE HERE IS THE VIEW'S OWN, used when it presents nothing. A view that presents a surface reads the
 *   SURFACE's size instead (see RenderSurface::resize): the size of the thing the picture ends up in has one owner,
 *   and it is not the view.
 *
 * @note
 * - A step is a plain `std::function`: the participant does not own what a step touches, exactly like the callbacks it
 *   replaces. Registering the same step twice runs it twice (the list is a sequence, not a set).
 */
class VN_GRAPHICS_API LayoutParticipant {
  public:
    /** @brief A layout step in logical pixels (see SceneView::addSurfaceLayout). */
    using Step = std::function<void(int width, int height)>;

    /** @brief A layout step that also wants the device pixel ratio. */
    using StepWithRatio = std::function<void(int width, int height, double pixel_ratio)>;

  public:
    /** @brief Records an announced size (a non-positive width/height is ignored, as is a non-positive ratio).
     *
     * @details
     * - Recording is all this does: running the steps is the caller's apply(), because a view must apply its own
     *   step (camera and pipeline) in the same pass and in a defined order.
     *
     * @param width       Surface width in logical pixels.
     * @param height      Surface height in logical pixels.
     * @param pixel_ratio Device pixels per logical pixel; <= 0 keeps the recorded one.
     */
    void resize(int width, int height, double pixel_ratio);

    /** @brief Gets the recorded width in logical pixels (0 before anything was announced). */
    [[nodiscard]] int width() const noexcept
    {
        return width_;
    }

    /** @brief Gets the recorded height in logical pixels (0 before anything was announced). */
    [[nodiscard]] int height() const noexcept
    {
        return height_;
    }

    /** @brief Gets the recorded device pixel ratio (1.0 before anything was announced). */
    [[nodiscard]] double pixelRatio() const noexcept
    {
        return pixel_ratio_;
    }

    /** @brief Gets whether a usable size has been announced. */
    [[nodiscard]] bool hasSize() const noexcept
    {
        return width_ > 0 && height_ > 0;
    }

    /** @brief Registers a layout step in logical pixels.
     *
     * @param step Step to run on every change (appended: the list keeps registration order).
     */
    void addStep(Step step);

    /** @brief Registers a layout step that also wants the device pixel ratio.
     *
     * @param step Step to run on every change (appended: the list keeps registration order).
     */
    void addStep(StepWithRatio step);

    /** @brief Removes every registered step (the recorded size is untouched). */
    void clearSteps();

    /**
     * @brief Runs @p view_step and then every registered step, for the recorded size.
     *
     * @details
     * - Nothing runs before a usable size was announced, and the whole call is skipped then - a step told a zero
     *   width would have to defend itself against a size that is not a size.
     * - Order: @p view_step (the consumer's own: camera, manipulator, pipeline), then every registered step in
     *   REGISTRATION order (one list, so a host that interleaves the two spellings gets the order it wrote).
     *   That is the order the view has always applied them in, kept here so a second consumer cannot reverse it.
     *
     * @param view_step The consumer's own step, or empty.
     */
    void apply(const StepWithRatio& view_step) const;

    /**
     * @brief Runs @p view_step and every registered step for an EXPLICIT size.
     *
     * @details
     * - The form a consumer uses when the size is not this participant's to own: a view that PRESENTS a surface reads
     *   that surface's size (the thing the picture ends up in owns it - see RenderSurface::resize) and still wants
     *   its host-registered steps run. The recorded size is not consulted or changed.
     * - Same order and same skip rule as apply().
     *
     * @param width       Surface width in logical pixels.
     * @param height      Surface height in logical pixels.
     * @param pixel_ratio Device pixels per logical pixel.
     * @param view_step   The consumer's own step, or empty.
     */
    void applySize(int width, int height, double pixel_ratio, const StepWithRatio& view_step) const;

  private:
    int    width_       = 0;
    int    height_      = 0;
    double pixel_ratio_ = 1.0;

    std::vector<StepWithRatio> steps_;
};

VN_GRAPHICS_NS_END
