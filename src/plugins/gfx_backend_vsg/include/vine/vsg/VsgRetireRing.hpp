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
 * It is PARKED instead, and released kRetireRingDepth frame advances later — the clock itself is
 * VsgDeferredRelease, and this ring is one of its three users (the per-draw slot pool parks slots on
 * it, this parks the renderer-owned objects, and every content slot's SceneBridge parks the retained
 * nodes it drops: they used to be separate countdowns whose depths had to be kept in step by hand).
 *
 * The depth and the "advance after the submit" point are the ones the per-slot node ring established
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

#include <cstddef>

#include <vsg/core/Object.h>
#include <vsg/core/ref_ptr.h>

#include <vine/vsg/VsgDeferredRelease.hpp>
#include <vine/vsg/VsgFwd.hpp>

V_VSG_NS_BEGIN

struct VsgRetireRing
{
    /** @brief Number of frame advances a parked object is held for.
     *
     * The deferral clock's depth, under the name this codebase has used since the per-slot node
     * ring: one more than the viewer's command-buffer slot count, so the slot that could still
     * reference a parked object has had its fence waited before the object is released.
     */
    static constexpr std::size_t kRetireRingDepth = kDeferredReleaseFrames;

    /** @brief Parks @p object until every command buffer that could reference it has been
     * re-recorded.
     *
     * @param object Object to release later (null is ignored).
     */
    void park(::vsg::ref_ptr<::vsg::Object> object);

    /** @brief Releases the objects parked kRetireRingDepth frame advances ago.
     *
     * Called once per committed frame, beside the per-slot node rings: the bucket entered now was
     * filled kRetireRingDepth submits ago, so every command buffer that could have recorded one of its
     * objects has been re-recorded since — and the recording that replaced it waited on that slot's
     * fence before starting.
     *
     * IT IS THE FRAME'S LAST STEP. An object parked after this call enters the bucket entered now and
     * is released one frame early — which is how this backend once destroyed render passes /
     * framebuffers / pipelines a submitted command buffer still named (see VsgDeferredRelease: the
     * depth is only worth its full count if the park lands before the advance).
     *
     * @param commit Evidence that the frame this advance accounts for was committed (see
     *               @ref FrameCommit: the advance is only legal after the submit).
     */
    void advance(FrameCommit commit);

    /** @brief Gets how many objects are parked right now.
     *
     * Diagnostic: a ring that never released would grow without bound, so tests and the
     * policy-churn check watch this count (see @ref releasedCount for the other end of it).
     *
     * @return Number of parked objects.
     */
    [[nodiscard]] std::size_t parkedCount() const noexcept { return parked.parkedCount(); }

    /** @brief Gets how many objects the ring has released so far.
     *
     * Diagnostic (see parkedCount): the count only ever grows, so a parked count that stops
     * falling while this one stops rising is a ring that is stuck.
     *
     * @return Number of objects released.
     */
    [[nodiscard]] std::size_t releasedCount() const noexcept { return released_; }

    /** @brief Gets how many device-wide idles have been taken.
     *
     * Diagnostic: avoiding them on the frame-assembly paths is what this ring is for, so this is
     * the judge of that — no policy-changing frame may raise it.
     *
     * @return Number of deviceWaitIdle() calls this ring performed.
     */
    [[nodiscard]] std::size_t waitCount() const noexcept { return waits_; }

    /** @brief Stops the device and counts the stop (see @ref waitCount).
     * Used by the DESTRUCTIVE teardown paths, which are exactly the ones that drop a
     * bridge's cache: SceneBridge::clearCache() releases the shared object registry, and a
     * pipeline / sampler in it needs no retained node to own it, so those paths cannot be
     * made wait-free by parking the slot's view (measured: vkDestroyPipeline-00765 /
     * vkDestroySampler-01082). Everything else — a variant swap, the promotion cascade, a
     * dropped program slot, an inactive pass' view, a depth-mode state rebuild — PARKS
     * instead (@ref park), and the policy-churn check asserts that the two kinds stay apart.
     *
     * @param viewer Session viewer, or null when there is no session to stop (no-op, and
     *               an unstopped device is not counted). Borrowed for the call: the wait does not
     *               need its own reference to the session's viewer.
     */
    void waitForIdle(const ::vsg::ref_ptr<::vsg::Viewer>& viewer);

  private:
    // The parked objects, on the shared deferral clock (see VsgDeferredRelease).
    VsgDeferredRelease<::vsg::ref_ptr<::vsg::Object>> parked;
    // Objects released by the ring so far (diagnostic; see releasedCount()).
    std::size_t released_ = 0;
    // Device-wide idles taken so far (diagnostic; see waitCount()).
    std::size_t waits_ = 0;
};

V_VSG_NS_END
