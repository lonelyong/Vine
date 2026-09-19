#pragma once

/**
 * @brief What a session has DONE, in one value (see VsgRenderer::counters).
 *
 * These are the backend's build and behaviour counters: how many off-screen graphs, windows and
 * fullscreen-program slots it built, how many data edits it served in place, and how the deferral
 * behaved (detached views, device waits, released objects). They are the other half of
 * VsgRetentionStats: that one says what the session is HOLDING, this one says what it DID to get
 * there, and the two are read as a series -- a build count that climbs while the frame count stays
 * stable is the signature of a rebuild loop, which no single number can show.
 *
 * @note These are counters, not a bound: nothing here says a number is wrong, only what it is. The
 *       doc for each field says which direction is the suspicious one, and two of them are
 *       invariants -- device_waits must stay at 0, and detached_slots is 0 while every registered
 *       pass draws.
 */

#include <vine/vsg/vsg_global.hpp>

#include <cstddef>

V_VSG_NS_BEGIN

/** @brief A session's build and behaviour counters (see VsgRenderer::counters). */
struct VsgRendererCounters
{
    /** @brief Off-screen target graphs built: a fresh attachment set, or a rebuild.
     *
     * Diagnostic: counts every successful off-screen build. A target built once and then driven
     * with a stable size and clear policy must keep this flat -- a count that grows with the frame
     * count is the signature of a rebuild loop (a size or depth-policy mismatch re-entering the
     * build path every frame, which tears the graph down, waits for the device and recompiles the
     * whole graph).
     */
    std::size_t offscreen_builds = 0;

    /** @brief Off-screen targets RESIZED in place: the same attachments, at a new size.
     *
     * Diagnostic, and the sibling of offscreen_builds: a resize that only changes the target's size
     * keeps its passes, its slots and their pipelines and replaces the images / framebuffers and the
     * descriptor bindings that named them (see .ai/design/vsg-target-resize-in-place.md), so it is
     * deliberately not counted as a build. A host that animates a target's size shows this count
     * climbing while offscreen_builds and program_slot_builds stay flat -- which is exactly the
     * difference the in-place path exists to make.
     */
    std::size_t offscreen_resizes = 0;

    /** @brief Windows built.
     *
     * Diagnostic: a session owns one window, and that window owns the VkInstance, the physical
     * device and the VkDevice -- so "no new window" is what "the device, and every pipeline
     * compiled against it, were kept" looks like from outside. A host that announces a new native
     * window is asking the session to MOVE to it (see VsgRenderer::moveSessionToHostSurface), which
     * leaves this flat; a session that had to be rebuilt instead -- no host window, the same
     * handle, or a new window that cannot present this session's render pass -- counts one more.
     */
    std::size_t window_builds = 0;

    /** @brief Retained fullscreen-program slots built (VsgRenderer::drawScreenProgram).
     *
     * Diagnostic: counts every successful build / rebuild of a retained fullscreen-program slot. A
     * slot is rebuilt when its sampled source, destination size or program changes -- and, since
     * the slot's identity includes the program's CONTENT revision, also when the program object is
     * edited in place (ShaderProgram::replaceStages / setStage). This makes a hot-reload
     * observable, which a pointer-only identity could not: it kept drawing the old SPIR-V. A
     * steady scene must keep this flat.
     */
    std::size_t program_slot_builds = 0;

    /** @brief Geometry data edits served by re-pointing the changed stream IN PLACE.
     *
     * Diagnostic, and the pair to data_nodes_built: an incremental update and a full data rebuild
     * draw the same picture, so pixels cannot tell them apart, and "only the changed stream was
     * re-uploaded" is the property the incremental path exists for. Summed over the session's
     * content slots (see SceneBridge::DataEditStats).
     */
    std::size_t streams_refreshed = 0;

    /** @brief Geometry data nodes materialised: a first build, or a rebuild.
     *
     * A phase that asserts "the edit did not rebuild" compares this against the same value before
     * the edit (see streams_refreshed for the other half).
     */
    std::size_t data_nodes_built = 0;

    /** @brief Retained slot views currently detached.
     *
     * A slot whose pass did not execute in the last submitted frame has its view detached from the
     * render graph (its data and pipelines are kept, so re-enabling the pass only re-attaches).
     * This counts those detached views: it is 0 while every registered pass draws, and rises as
     * passes are disabled / unregistered. Diagnostic for "why is nothing drawing?" and the
     * regression check of the retirement path.
     */
    std::size_t detached_slots = 0;

    /** @brief Device-wide idles taken: an invariant, and it is 0.
     *
     * Diagnostic with an invariant behind it: NO frame-assembly path may stop the device any more
     * -- a replaced render pass / framebuffer, a dropped fullscreen-program node, a slot being torn
     * down, a target being rebuilt and a bridge dropping its state wrappers all PARK their objects
     * in the retire ring instead. So this stays 0, and a check that changes a pass' clear and depth
     * policy, a pass' activity, its depth MODE and a target's attachment shape every frame asserts
     * that it does (VsgRetireRing::waitForIdle is the counted entry point a future teardown that
     * cannot park would have to use).
     */
    std::size_t device_waits = 0;

    /** @brief Objects the retire ring has released (only ever climbs).
     *
     * Diagnostic: a replaced render pass / framebuffer, a dropped fullscreen-program node and a
     * dropped content-slot node are PARKED for a few frame advances and then released (see
     * VsgRetireRing -- park() is the only way in, and its three users are named on the type). A
     * ring that never released would grow without bound, so a policy-changing check asserts this
     * advances -- and, together with the validation-clean run, is what shows the deferral is live
     * rather than merely silent.
     */
    std::size_t released_objects = 0;
};

V_VSG_NS_END
