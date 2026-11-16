#pragma once

#include <vine/intrusive_ptr.hpp>
#include <vine/raw_ptr.hpp>

#include <vine/graphics/Diagnostics.hpp>
#include <vine/graphics/ReportOnce.hpp>
#include <vine/graphics/graphics_global.hpp>

VN_GRAPHICS_NS_BEGIN

class PresentTarget;
class RenderArea;
class RenderBackend;
class RenderEngine;

namespace detail
{

/**
 * @brief THE SURFACE LEDGER: the one area an engine drives, its announced size, and the target the frame presents
 *        through.
 *
 * @details
 * - WHY IT IS NOT IN `RenderEngine` (`G5` / `G12`): "which surface does this frame land on, how big was it announced,
 *   and what has the swapchain already been told" is MECHANISM with its own lifetime - and the composition root grew
 *   four fields and three verbs for it. The ledger owns exactly that state; the engine composes it.
 * - ONE ENGINE, ONE AREA, ONE OWNER: the ledger owns the area the engine created (`createArea`), so the 1:1 relation
 *   has an owner instead of a paragraph, and a second area is refused once per episode - a second area would draw over
 *   the same frame and its size would silently drive the passes.
 * - TWO SIZES, deliberately: the size the host ANNOUNCED (what the passes lay themselves out on, recorded even before
 *   a device exists) and the size already handed to the TARGET (the window half, applied at a frame's head and only
 *   when it changed). The frame takes the announced size when it starts, so an announcement that lands while a frame
 *   is being answered belongs to the next one (design 2.12).
 * - The target itself is NOT owned: it is one of the device's objects (RenderDevice::createSurfaceTarget), so the
 *   ledger only borrows it and hands it back when the engine detaches (see `detach`).
 *
 * @note The present table (which pass is the picture) lives on `RenderArea`, where a declaration belongs; the ledger
 *       answers "which target does the frame resolve to", never "who presents".
 */
class SurfaceLedger {
  public:
    /**
     * @brief Creates an empty ledger (no area, no announced size, no target).
     *
     * @param diagnostics The engine's one route (borrowed; the second-area refusal is reported through it).
     */
    explicit SurfaceLedger(Diagnostics& diagnostics) noexcept;

    /** @brief Releases the area (a host that kept a handle keeps a dead-lease surface, never a dangling one). */
    ~SurfaceLedger();

    SurfaceLedger(const SurfaceLedger&)            = delete;
    SurfaceLedger& operator=(const SurfaceLedger&) = delete;

  public:
    /**
     * @brief Creates and adopts the ONE area this engine drives.
     *
     * @details
     * - A SECOND ask is answered null and said once per episode: two areas on one engine would both declare the
     *   picture for one frame, and only one of them is the frame's default framebuffer.
     *
     * @param engine The engine the area borrows (it stays alive through a lease).
     * @return The area (also owned here), or null when one already exists.
     */
    [[nodiscard]] intrusive_ptr<RenderArea> createArea(RenderEngine& engine);

    /**
     * @brief Records the size the host announced for this engine's own area.
     *
     * @details
     * - ONLY the ledger's own area is heard: a frame laid out on another window's size would be a silent disagreement.
     * - Recorded even before a device exists, because the passes and their targets are laid out on it.
     *
     * @param from   Area announcing.
     * @param width  Surface width in logical pixels (> 0).
     * @param height Surface height in logical pixels (> 0).
     */
    void announce(raw_ptr<const RenderArea> from, int width, int height);

    /**
     * @brief Asks @p device for the window target the frame presents through (once, at attach).
     *
     * @details
     * - An AREA that holds a target keeps it (a host that bound one is never overridden); an area with none is asked
     *   for the device's own window, or told there is no presentation - which is a first-class answer.
     * - An engine with NO area keeps the device's own target here, because its frames still have to name one when the
     *   device presents through a window of its own.
     *
     * @param device The device this engine borrows (must be up; the caller checked).
     */
    void attach(RenderBackend& device);

    /** @brief Unbinds the area's target and forgets what the swapchain was told (the device may destroy it next). */
    void detach();

    /** @brief Gets the area this engine drives, or null. */
    [[nodiscard]] raw_ptr<RenderArea> area() const noexcept;

    /** @brief Gets whether an area exists. */
    [[nodiscard]] bool hasArea() const noexcept;

    /** @brief Gets the target this frame resolves to: the area's window, else the device's own, else null. */
    [[nodiscard]] raw_ptr<PresentTarget> frameTarget() const noexcept;

    /**
     * @brief Hands the size a frame was LAID OUT on to the window half, when it changed.
     *
     * @details
     * - One place a frame waits on the device: the swapchain follows the frame, so a host resizing twice between two
     *   frames costs one resize instead of two.
     *
     * @param target Target of the frame being answered (null: nothing to tell).
     * @param width  Width the frame was laid out on.
     * @param height Height the frame was laid out on.
     */
    void applyFrameSize(raw_ptr<PresentTarget> target, int width, int height);

    /**
     * @brief Forgets what the target was told, so the next frame hands it the size again.
     *
     * @details
     * - THE LOST-PICTURE REPAIR: a swapchain is rebuilt by re-announcing its extent, so a `PresentOutcome::Lost`
     *   frame forgets the applied size and the next frame re-applies it - one repair, on the side that owns the size.
     */
    void forgetFrameSize() noexcept;

    /** @brief Gets the width the host announced (logical pixels; 0 until the first resize). */
    [[nodiscard]] int width() const noexcept;

    /** @brief Gets the height the host announced (logical pixels; 0 until the first resize). */
    [[nodiscard]] int height() const noexcept;

  private:
    /// The engine's one diagnostic route (borrowed; outlives every ledger).
    Diagnostics& diagnostics_;
    /// The one area this engine drives (owned; the 1:1 relation needs an owner).
    intrusive_ptr<RenderArea> area_;
    /// The target the device made for itself when this engine has no area (borrowed; null when it presents nothing).
    raw_ptr<PresentTarget> own_target_{ nullptr };
    /// What the host announced: the number the passes and their targets are laid out on.
    int width_{ 0 };
    /// What the host announced: the number the passes and their targets are laid out on.
    int height_{ 0 };
    /// What the target was LAST told: the swapchain half, applied at a frame's head when the number moved.
    int applied_width_{ 0 };
    /// What the target was LAST told: the swapchain half, applied at a frame's head when the number moved.
    int applied_height_{ 0 };
    /// "A second area was asked for" is one condition, not a per-call event (see createArea).
    ReportOnce area_refused_{};
};

} // namespace detail

VN_GRAPHICS_NS_END
