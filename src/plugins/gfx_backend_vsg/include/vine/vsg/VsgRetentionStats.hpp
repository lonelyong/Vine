#pragma once

/**
 * @brief What a session is holding back, in one value (see VsgRenderer::retentionStats).
 *
 * A backend retains things past the frame that built them: per-draw slots, replaced GPU objects on
 * their way out, and — for as long as the app holds the content — a compiled subtree per drawable.
 * Those pieces are only meaningful read TOGETHER: a pool whose capacity climbs while its reserved
 * count does not is a leak, and a compile-context count that grows while the slot count does not is
 * retention vsg offers no way to release. Reading them as one value is also what makes a check
 * possible at all: the same numbers were being reassembled by hand whenever someone asked whether
 * the backend was holding more than the scene needs.
 *
 * @note These are counters, not a bound: nothing here says the numbers are wrong, only what they
 *       are. The doc for each field says which direction is the suspicious one.
 */

#include <vine/vsg/vsg_global.hpp>

#include <cstddef>

#include <vine/vsg/VsgDrawBlockPool.hpp>

V_VSG_NS_BEGIN

/** @brief A session's retention picture (see VsgRenderer::retentionStats). */
struct VsgRetentionStats
{
    /** @brief Content slots that exist right now, across every target. */
    std::size_t content_slots = 0;

    /** @brief The per-draw slot pool's picture (chunks / capacity / reserved / retired). */
    VsgDrawBlockPool::Stats slots;

    /** @brief Objects parked on the session's retire ring, waiting out the frames in flight.
     *
     * Climbs and falls with churn; a value that only climbs means nothing is being released.
     */
    std::size_t parked_nodes = 0;

    /** @brief Objects the retire ring has released so far (only ever climbs). */
    std::size_t released_nodes = 0;

    /** @brief Device-wide idles the retire ring took (only ever climbs).
     *
     * A policy-changing frame must not raise this: the ring exists so that replaced objects can
     * wait instead of stopping the device.
     */
    std::size_t device_waits = 0;

    /** @brief Compile contexts this session's manager holds: one per live content slot.
     *
     * Incremental compile registers a slot's (render pass + view) context with the viewer's
     * CompileManager, and vsg 1.1.16 offers no way to remove one -- each Context owns a
     * VkCommandPool and holds the render pass it was registered against. This session's manager is
     * therefore the backend's own subclass, which can drop a registration where the slot it belongs
     * to dies (see docs/backend.md 5.3.1), so the count follows the slots ALIVE and not the slots a
     * session has ever created: at the self-test's churn phase the two numbers were 60 and 4 before
     * that release existed, and 4 and 4 with it.
     */
    std::size_t compile_contexts = 0;

    /** @brief Streams the session's mesh cache holds (a bind plus the bytes behind it).
     *
     * Bounded by the cache's capacity, so this is what a host can budget against: the count follows how
     * much geometry the session has drawn, not how long it has run.
     */
    std::size_t mesh_streams = 0;

    /** @brief Textures the session's texture cache holds (one image each).
     *
     * Bounded by the cache's capacity. Unlike the draw-block slots this is LIVE (a texture the app
     * releases is swept), so a value pinned at the capacity means the app hands the cache more distinct
     * textures than it lets go of.
     */
    std::size_t textures = 0;

    /** @brief Device bytes the session's per-draw slots occupy (see VsgDrawBlockPool::Stats::bytes).
     *
     * The one figure here that is DEVICE MEMORY rather than a count: the pool's chunks are never given
     * back, so a value that keeps climbing is the shape of a leak (a slot that is never returned), and a
     * host sizing a budget has a figure to compare against. A byte budget for the mesh and texture caches
     * is not here yet: those caches bound entries, and the honest way to add bytes is with the measured
     * numbers the perf backlog asks for (see .ai/memory/graphics-perf-backlog.md).
     */
    std::size_t slot_bytes = 0;
};

V_VSG_NS_END
