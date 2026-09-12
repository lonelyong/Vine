#pragma once

/**
 * @brief Parks replaced GPU objects until no submitted command buffer can still reference them.
 *
 * A renderer-owned object whose Vulkan handle a SUBMITTED command buffer may still name —
 * a pass' render pass / framebuffer swapped for another load-op variant, or a fullscreen
 * program slot whose source revoked its depth promotion mid-frame — cannot be destroyed at
 * the moment it is replaced: that is undefined behaviour, and the validation layer does not
 * necessarily see it (the recording that references the object may be several frames old).
 *
 * It is PARKED here instead, and released kRetireRingDepth frame advances later. The depth
 * and the "advance after the submit" point are the ones the per-slot node ring established
 * (SceneBridge::retireNode): a command-buffer slot re-records the frame, it never reuses a
 * recording, so a ring bucket is safe once kRetireRingDepth frames have been submitted after
 * it was filled.
 *
 * There is ONE ring implementation: the session state parks the renderer-owned objects it
 * replaces (@ref VsgRendererState::retireRing) and every content slot's SceneBridge parks the
 * retained nodes it drops (SceneBridge::retireNode) — the two used to be separate arrays whose
 * depth had to be kept in step by hand.
 *
 * The alternative — holding the frame with a device-wide idle — is what this ring exists to
 * avoid, and @ref waits is the judge of that: it counts the stops that WERE taken. It must
 * stay apart from the destructive teardown paths (see waitForIdle), which cannot be made
 * wait-free this way.
 */

#include <vine/vsg/vsg_global.hpp>

#include <array>
#include <cstddef>
#include <vector>

#include <vsg/app/Viewer.h>
#include <vsg/core/Object.h>
#include <vsg/core/ref_ptr.h>

V_VSG_NS_BEGIN

struct VsgRetireRing
{
    /** @brief Number of frame advances a parked object is held for.
     *
     * One more than the viewer's command-buffer slot count, so the slot that could still
     * reference a parked object has had its fence waited (the wait happens before that slot is
     * re-recorded) before the object is released.
     */
    static constexpr std::size_t kRetireRingDepth = 4;

    /** @brief Parks @p object until every command buffer that could reference it has been
     * re-recorded.
     *
     * @param object Object to release later (null is ignored).
     */
    void park(::vsg::ref_ptr<::vsg::Object> object);

    /** @brief Releases the objects parked kRetireRingDepth frame advances ago.
     *
     * Called once per submitted frame, beside the per-slot node rings: the bucket entered
     * now was filled kRetireRingDepth submits ago, so every command buffer that could have
     * recorded one of its objects has been re-recorded since — and the recording that
     * replaced it waited on that slot's fence before starting.
     */
    void advance();

    /** @brief Stops the device and counts the stop (see @ref waits).
     *
     * Used by the DESTRUCTIVE teardown paths, which are exactly the ones that drop a
     * bridge's cache: SceneBridge::clearCache() releases the shared object registry, and a
     * pipeline / sampler in it needs no retained node to own it, so those paths cannot be
     * made wait-free by parking the slot's view (measured: vkDestroyPipeline-00765 /
     * vkDestroySampler-01082). Everything else — a variant swap, the promotion cascade, a
     * dropped program slot, an inactive pass' view, a depth-mode state rebuild — PARKS
     * instead (@ref park), and the policy-churn check asserts that the two kinds stay apart.
     *
     * @param viewer Session viewer, or null when there is no session to stop (no-op, and
     *               an unstopped device is not counted).
     */
    void waitForIdle(::vsg::ref_ptr<::vsg::Viewer> viewer);

    // One bucket per frame advance: the objects parked during one frame's assembly. The
    // ring is advanced after the frame is submitted, never during it.
    std::array<std::vector<::vsg::ref_ptr<::vsg::Object>>, kRetireRingDepth> ring;
    std::size_t head = 0;
    // Objects released by the ring so far (diagnostic; see VsgRenderer::retiredObjectCount()):
    // a ring that never released would grow without bound, so the policy-churn check asserts
    // it advances.
    std::size_t released = 0;
    // Device-wide idles taken so far (diagnostic; see VsgRenderer::deviceWaitCount()).
    // Avoiding them on the frame-assembly paths is what the ring is for, so this is the
    // judge of that change: no policy-changing frame may raise it.
    std::size_t waits = 0;
};

V_VSG_NS_END
