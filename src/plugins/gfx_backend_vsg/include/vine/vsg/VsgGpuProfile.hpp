#pragma once

/**
 * @brief What the DEVICE spent per pass, on the newest frame that has results.
 *
 * WHY THIS EXISTS. The backend could already time itself on the CPU (see VsgBuildProfile: attach /
 * compile / record / present), which answers "what did building this frame cost". It could not answer
 * "what did the GPU do with it" — the other half of every pipeline / pass / state question, and the half
 * that a caching verdict cannot be made without. Upstream vsg has the machinery (vsg::Profiler is an
 * Instrumentation that writes timestamp queries per command buffer), so this is a thin, opt-in layer on
 * top of it rather than a second query pool of our own:
 *
 *   * `VINE_VSG_PROFILE=1` before a session comes up installs a `vsg::Profiler` on the session's viewer.
 *     `VINE_VSG_PROFILE_CPU` / `VINE_VSG_PROFILE_GPU` set its two instrumentation levels (defaults 0 and
 *     1: the per-pass level, see below — a higher GPU level timestamps EVERY recorded node and the
 *     profiler's 1024-query pool silently drops what does not fit, which is why the default is the level
 *     that measures passes).
 *   * With it on, each pass' render graph is wrapped in a named `vsg::InstrumentationNode` when the
 *     command graph's record order is applied (see detail::applyRecordPlan). That wrapper is what gives a
 *     timestamp pair an IDENTITY: upstream's own per-graph timestamp is written by `RenderGraph::record`
 *     with a null object, so its entries cannot be attributed to a pass — the wrapper passes the graph
 *     itself as the object, and the pass it belongs to is then looked up in the session's targets here.
 *   * Read through VsgRenderer::gpuProfile(). Nothing is measured when the switch is off: no wrapper node,
 *     no query pool, no read.
 *
 * WHAT IS MEASURED, EXACTLY. One sample per render pass — every off-screen pass, plus the WINDOW render
 * graph, which is one graph carrying every window pass' views (a window pass is a view of the shared
 * swapchain graph, not a graph of its own; its sample is therefore the present path as a whole, not one
 * window pass). The interval is the time the DEVICE spends between the timestamp before that graph is
 * recorded and the one after it, i.e. the render pass itself: its load/store of every attachment and the
 * draws inside it, but not the submit, and not the CPU time it took to record. `frame_gpu_ms` is the
 * command buffer's own interval, which covers every graph recorded into it.
 *
 * WHY THE NUMBERS LAG, AND WHY THAT IS THE POINT. vsg reads the results back WITHOUT
 * `VK_QUERY_RESULT_WAIT_BIT` (a query that is not ready comes back VK_NOT_READY and is retried on a later
 * frame), so a frame's timestamps become readable only once the submission that wrote them has completed —
 * in practice a few frames later. Reading them is therefore free of device stalls, and the price is that
 * this value describes the newest frame that HAS results rather than the frame just recorded; @ref
 * age_frames says how many frames behind the session it is. A host that sees age_frames of 0 in a live
 * session is looking at a blocking read, which this backend must not do (its device waits are counted and
 * a policy change must not raise them).
 */

#include <cstdint>
#include <string>
#include <vector>

#include <vine/vsg/vsg_global.hpp>

// The pass a measurement belongs to is answered from the session's target table, and this header only names
// those types behind a reference -- so they are declared here rather than included (see VsgFwd.hpp for the
// library's side of the same rule).
namespace vine::graphics
{
class RenderTarget;
}

#include <vine/vsg/VsgFwd.hpp>

V_VSG_NS_BEGIN

class VsgRendererState;
struct VsgRenderTargetEntry;

/** @brief One pass' GPU time in one frame (see VsgGpuProfile). */
struct VsgGpuPassSample
{
    /** @brief Name of the pass the measured graph was built for ("window" for the window render graph). */
    std::string pass;

    /** @brief Name of the target it rendered into ("window" for the window, "(unnamed)" without a name). */
    std::string target;

    /** @brief The pass' explicit order (setPassOrder); 0 for the window render graph. */
    int order = 0;

    /** @brief GPU time between the pass' begin and its end, in milliseconds. */
    double gpu_ms = 0.0;
};

/** @brief A session's GPU profile: per-pass device time of the newest frame that has results. */
struct VsgGpuProfile
{
    /** @brief Whether profiling was asked for when the session came up (VINE_VSG_PROFILE). */
    bool enabled = false;

    /** @brief Whether the device reports timestamps at all (limits.timestampComputeAndGraphics).
     *
     * False makes every other field meaningless: an enabled profile on such a device has nothing to
     * report, and a host must not read "no samples" as "nothing was on the GPU".
     */
    bool timestamps_available = false;

    /** @brief How many frames behind the session the samples are (see the note on the read).
     *
     * 1 or more in a waiting-free session: the results of a frame are only readable once its submission
     * completed. 0 would mean the read blocked the device.
     */
    std::uint64_t age_frames = 0;

    /** @brief GPU time of the whole command buffer the frame was recorded into, in milliseconds. */
    double frame_gpu_ms = 0.0;

    /** @brief One sample per render pass recorded in that frame, in record order. */
    std::vector<VsgGpuPassSample> passes;
};

namespace detail
{

/**
 * @brief Names the pass a measured render graph belongs to.
 *
 * The profiler's log carries the graph as a raw pointer, so the name behind it is answered from the
 * session's own target table rather than from the entry -- one place that knows how a pass is named, used
 * both when a graph is wrapped (see detail::applyRecordPlan) and when the measurements are read back.
 *
 * @param target Target the graph renders into (nullptr = the window target).
 * @param entry  That target's session entry.
 * @param graph  The measured render graph.
 * @return The pass' identity; a pass name of "(rebuilt)" when the target no longer holds the graph.
 */
[[nodiscard]] VsgGpuPassSample describeGraph(vine::graphics::RenderTarget* target, const VsgRenderTargetEntry& entry,
                                           const ::vsg::RenderGraph* graph);

/**
 * @brief Reads the session's GPU profile out of the profiler the session installed.
 *
 * @param state Session state (the profiler, its log and the targets that name a measured graph).
 * @return The profile; @ref VsgGpuProfile::enabled is false when no profiler is installed, and
 *         @ref VsgGpuProfile::passes stays empty while no frame with timestamps has completed yet.
 */
[[nodiscard]] VsgGpuProfile readGpuProfile(const VsgRendererState& state);

} // namespace detail

V_VSG_NS_END
