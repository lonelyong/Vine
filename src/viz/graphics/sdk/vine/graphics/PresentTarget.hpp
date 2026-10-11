#pragma once
#include "graphics_global.hpp"

VN_GRAPHICS_NS_BEGIN

/**
 * @brief A target a frame is PRESENTED through: the one contract every presenter implements.
 *
 * WHAT IT IS FOR. A frame's execution (see RenderFrameExecutor::executeFrame) says what a frame IS; a presenter says
 * where the picture goes once there is somewhere to put it: it takes the announced surface size (resize) and it is
 * what a finished frame is handed to (present). Those two verbs are what a frame's tail needs, and they are ALL this
 * contract has - which is what lets a presenter that is not a window exist at all.
 *
 * A WINDOW IS ONE IMPLEMENTATION (WindowPresentTarget), and the window's own questions - its native handle, the
 * re-announcement after a platform recreation - live THERE, because a host that owns a window is the only party that
 * asks them. A presenter that encodes and sends a frame, or that captures it to a file, implements this contract and
 * nothing else: the frame, the area and the engine never learn which kind they were handed.
 *
 * WHO DRIVES ONE. The frame's driver calls present() once per frame it ran (see RenderEngine::render(), which is the
 * engine's own tail, and RenderPass::execute(), which is a host driving one pass). A device that presents nothing
 * hands out no presenter at all (RenderDevice::createSurfaceTarget answers "no presentation" - see
 * Capabilities::presentation), and a frame that names none presents nowhere: a first-class state (an offline bake, a
 * display-less run), not an empty implementation of the two calls below waiting to be written.
 *
 * THREADING and lifetime follow the DEVICE's own rules (see RenderDevice): one thread, serialised calls, and a
 * presenter outlives neither the surface it presents through nor the device that made it.
 */
class VN_GRAPHICS_API PresentTarget
{
  public:
    virtual ~PresentTarget() = default;

    /** @brief Announces a change of the surface size.
     *
     * The surface itself owns its size, so the authority order is surface > announcement > default: a presenter
     * whose surface belongs to a window system (an embedded host window, a swapchain) FOLLOWS THAT SURFACE and uses
     * this call to re-derive what hangs off the size, whereas one that owns its surface (headless, or it created the
     * window itself) applies the announcement. The engine records the announced numbers either way: its frame
     * context is what the passes that lay themselves out on the surface size read (see RenderEngine::resize, which
     * is the announcement's way in).
     *
     * @param width  Announced surface width in pixels.
     * @param height Announced surface height in pixels.
     */
    virtual void resize(int width, int height) = 0;

    /** @brief Finishes the frame the executor last ran: it becomes real work, and its image is presented.
     *
     * The frame's own body - acquiring the next presentable image, collecting, compiling - happens inside
     * RenderFrameExecutor::executeFrame; this is the half that records what was collected into the frame's graph,
     * submits it, and hands the acquired image back to the display. It is the ONE call that presents, it is
     * called once per frame the executor ran, and the executor itself must not present from executeFrame.
     */
    virtual void present() = 0;
};

VN_GRAPHICS_NS_END
