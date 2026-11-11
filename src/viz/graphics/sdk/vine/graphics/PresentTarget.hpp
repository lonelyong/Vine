#pragma once
#include "graphics_global.hpp"

VN_GRAPHICS_NS_BEGIN

/**
 * @brief What became of a frame's picture (see PresentTarget::present).
 *
 * @details
 * - THE THREE ANSWERS ARE DIFFERENT FACTS, not degrees of failure:
 *   - #Presented: the platform accepted it - the frame reached a window (or, for a presenter that is not a window, the
 *     place it sends to). It does NOT mean "on the glass" (that needs a present-timing extension this tree does not
 *     require), only that the presentation path took it.
 *   - #Skipped: there was nothing to present into - a device that presents nothing, an engine with no area, a frame a
 *     host named no target for. A first-class state (an offline bake, a display-less run), not an error.
 *   - #Lost: there WAS a surface, and the picture did not reach it - the submission or the present was refused, the
 *     surface is gone (the window was destroyed under it), or the swapchain could no longer serve it. The picture is
 *     lost, the frame is over, and the host's move is to re-attach (see `.ai/design/graphics-threading.md` F9).
 * - Why a VALUE and not a diagnostic: a lost frame is the one thing a host must ACT on (re-attach) while frames keep
 *   running, so it travels with the frame it belongs to (RenderEngine::presentRecord) instead of becoming a message a
 *   reader has to correlate with a frame number.
 */
enum class PresentOutcome : std::uint8_t
{
    Presented, // The platform accepted the frame's picture.
    Skipped,   // There was nothing to present into (no target, or a device that presents nothing).
    Lost,      // There was a surface and the picture did not reach it (refused, gone, or out of date).
};

/**
 * @brief A target a frame is PRESENTED through: the one contract every presenter implements.
 *
 * @details
 * - WHAT IT IS FOR. A frame's execution (see RenderFrameExecutor::executeFrame) says what a frame IS; a presenter
 *   says where the picture goes once there is somewhere to put it: it takes the announced surface size (resize) and
 *   it is what a finished frame is handed to (present). Those two verbs are what a frame's tail needs, and they are
 *   ALL this contract has - which is what lets a presenter that is not a window exist at all.
 * - A WINDOW IS ONE IMPLEMENTATION (WindowPresentTarget), and the window's own questions - its native handle, the
 *   re-announcement after a platform recreation - live THERE, because a host that owns a window is the only party
 *   that asks them. A presenter that encodes and sends a frame, or that captures it to a file, implements this
 *   contract and nothing else: the frame, the area and the engine never learn which kind they were handed.
 * - WHO DRIVES ONE. The EXECUTOR calls present() once per frame it ran (RenderFrameExecutor::executeFrame): the
 *   frame is whole when that call returns, so a host that drove a frame never presents it itself - not the engine
 *   (RenderEngine::frame()), not a host driving one pass (RenderPass::execute()). A device that presents nothing
 *   hands out no presenter at all (RenderDevice::createSurfaceTarget answers "no presentation" - see
 *   Capabilities::presentation), and a frame that names none presents nowhere: a first-class state (an offline bake,
 *   a display-less run), not an empty implementation of the two calls below waiting to be written.
 * - THREADING and lifetime follow the DEVICE's own rules (see RenderDevice): one thread, serialised calls, and a
 *   presenter outlives neither the surface it presents through nor the device that made it.
 */
class VN_GRAPHICS_API PresentTarget {
  public:
    virtual ~PresentTarget() = default;

    /** @brief Announces a change of the surface size.
     *
     * @details
     * - The surface itself owns its size, so the authority order is surface > announcement > default: a presenter
     *   whose surface belongs to a window system (an embedded host window, a swapchain) FOLLOWS THAT SURFACE and uses
     *   this call to re-derive what hangs off the size, whereas one that owns its surface (headless, or it created
     *   the window itself) applies the announcement.
     * - WHO CALLS IT: the FRAME, at its head, with the size that frame was laid out on (see design 2.12) - once per
     *   frame at most, and only when the number changed. A host announces a size to the area (RenderSurface::resize),
     *   the area records it, and the next frame hands it here; nothing on the host's side waits for the device.
     * - The engine records the announced numbers either way: its frame context is what the passes that lay themselves
     *   out on the surface size read (see RenderEngine::resize, which is the announcement's way in).
     *
     * @param width  Announced surface width in pixels.
     * @param height Announced surface height in pixels.
     */
    virtual void resize(int width, int height) = 0;

    /** @brief Finishes the frame the executor last ran: it becomes real work, and its image is presented.
     *
     * @details
     * - The ONE call that presents, made once per frame the executor ran (RenderFrameExecutor::executeFrame) at the
     *   frame's tail. A host that drove the frame does not call it: a second present would find no frame of its own to
     *   hand back.
     * - What it answers is @ref PresentOutcome: whether the picture reached this target, there was nothing to present
     *   into, or the surface could not take it. A presenter that cannot tell the last two apart answers #Lost, because
     *   "the picture did not arrive" is the fact a host must act on.
     *
     * @return What became of the frame's picture.
     */
    virtual PresentOutcome present() = 0;
};

VN_GRAPHICS_NS_END
