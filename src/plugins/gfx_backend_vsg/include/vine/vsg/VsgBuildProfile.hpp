#pragma once

/**
 * @brief What one frame spent on the GPU work that had to be BUILT for it, split by phase.
 *
 * A steady frame pays none of this: the first frame of a session, the first frame after a resize and
 * the frame that first draws a newly materialised pass are the ones that do. Timing the phases apart is
 * what makes a question like "can the startup build be moved earlier" answerable at all, because the
 * phases have DIFFERENT fixes and they are not interchangeable:
 *
 *   * @ref targets / @ref targets_ns — attaching an off-screen target's images, image views and
 *     framebuffers (nothing is allocated here: the attachment objects are vsg nodes, allocated when the
 *     graph that uses them is compiled);
 *   * @ref graphs / @ref graphs_ns — the FULL `Viewer::compile()` a newly materialised pass graph costs,
 *     which is where its images are allocated and, with them, the pipelines every view already in the
 *     command graph is recreated against (a VkPipeline is created while compiling its view against a
 *     render pass -- see GraphicsPipeline::compile -- so a new render pass is what forces its siblings'
 *     pipelines back through vkCreateGraphicsPipelines);
 *   * @ref slots / @ref slots_ns — building a fullscreen-program slot: its node plus the compile of its
 *     view, which is where that program's glslang translation and its VkPipeline are paid for;
 *   * @ref compile_ns — the incremental compile of the geometry views synced this frame
 *     (detail::compilePendingViews);
 *   * @ref record_ns — recording and submitting the frame's command graphs (descriptor set allocation
 *     and writes, command buffer recording);
 *   * @ref present_ns — the present itself.
 *
 * @note This is a MEASUREMENT, not a policy: nothing in the backend behaves differently because it is
 *       filled, and no field here is a bound. It exists so a caching verdict (shader modules, pipelines,
 *       attachments) is made on numbers instead of on a guess about which phase dominates -- the phases
 *       above answer that question and, read together with the frame's extent, also say whether the work
 *       was done at the size the surface kept.
 */

#include <vine/vsg/vsg_global.hpp>

#include <chrono>
#include <cstddef>
#include <cstdint>

V_VSG_NS_BEGIN

/** @brief One frame's setup cost, by phase (see VsgRendererState::build_profile). */
struct VsgBuildProfile
{
    /** @brief Off-screen targets attached this frame (a fresh attachment set, or a rebuild). */
    std::size_t targets = 0;

    /** @brief Time spent attaching them, in nanoseconds. */
    std::uint64_t targets_ns = 0;

    /** @brief Off-screen targets whose attachments were RESIZED in place this frame.
     *
     * Counted apart from @ref targets because the two are the two halves of the design (see
     * .ai/design/vsg-target-resize-in-place.md): a build tears the target's passes and slots down, an
     * in-place resize keeps them and only replaces the images, the framebuffers and the descriptor
     * bindings that named them. Reporting them apart is what keeps a resize's cost visible when it no
     * longer appears in @ref targets.
     */
    std::size_t target_resizes = 0;

    /** @brief Time spent resizing them in place, in nanoseconds. */
    std::uint64_t target_resizes_ns = 0;

    /** @brief Full pass graphs compiled this frame (a new pass' graph; see VsgContentSlot.cpp). */
    std::size_t graphs = 0;

    /** @brief Time spent in those full compiles, in nanoseconds. */
    std::uint64_t graphs_ns = 0;

    /** @brief Full compiles run to write descriptor sets that were RE-POINTED at a resized source.
     *
     * A program slot that re-pointed its sampled bindings (see ProgramSlot::source_set) holds a
     * replacement descriptor set, and a set has no Vulkan objects until a compile traversal visits it —
     * so one compile is run before the frame records. It is reported apart from @ref graphs because the
     * two say different things: a graph compile materialises a pass that did not exist, this one only
     * WRITES an object that was already there (and must stay ~1 ms).
     */
    std::size_t rebinds = 0;

    /** @brief Time spent in those compiles, in nanoseconds. */
    std::uint64_t rebinds_ns = 0;

    /** @brief Fullscreen-program slots built this frame (node + compiled view). */
    std::size_t slots = 0;

    /** @brief Time spent building them, in nanoseconds: this is where their glslang and pipeline cost is. */
    std::uint64_t slots_ns = 0;

    /** @brief Of @ref slots_ns: the time spent building the nodes themselves, in nanoseconds.
     *
     * Sub-bucket, NOT part of @ref total_ns (it would be counted twice): what it separates is our own object
     * graph -- the ShaderSet, the GraphicsPipelineConfigurator, the descriptor set and the state group -- from
     * the compile that follows it, which is what says whether a caching change has to be aimed at the node
     * assembly or at the pipeline.
     */
    std::uint64_t slots_node_ns = 0;

    /** @brief Of @ref slots_ns: the time spent compiling those nodes' views, in nanoseconds.
     *
     * Sub-bucket, NOT part of @ref total_ns. This is vsg's compile traversal on the new view: the pipeline
     * layout, the descriptor set layouts and (through vkCreateGraphicsPipelines) the pipeline itself.
     */
    std::uint64_t slots_view_ns = 0;

    /** @brief Time spent compiling the geometry views synced this frame, in nanoseconds. */
    std::uint64_t compile_ns = 0;

    /** @brief Time spent recording and submitting the frame's command graphs, in nanoseconds. */
    std::uint64_t record_ns = 0;

    /** @brief Time spent presenting the frame, in nanoseconds. */
    std::uint64_t present_ns = 0;

    /** @brief True when this frame built something (the frames worth reporting; see reportBuildProfile). */
    [[nodiscard]] bool builtAnything() const noexcept
    {
        return targets != 0 || target_resizes != 0 || graphs != 0 || slots != 0 || rebinds != 0;
    }

    /** @brief Everything the frame spent, in nanoseconds (the buckets above summed). */
    [[nodiscard]] std::uint64_t total_ns() const noexcept
    {
        return targets_ns + target_resizes_ns + graphs_ns + rebinds_ns + slots_ns + compile_ns + record_ns +
               present_ns;
    }

    /** @brief Clears every field, so the next frame's profile starts empty. */
    void reset() noexcept
    {
        *this = VsgBuildProfile{};
    }
};

/**
 * @brief Nanoseconds elapsed since @p start, read off the steady clock.
 *
 * @param start Time point taken when the phase began.
 * @return The elapsed time in nanoseconds.
 */
[[nodiscard]] inline std::uint64_t elapsedNs(const std::chrono::steady_clock::time_point& start) noexcept
{
    return static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - start).count());
}

V_VSG_NS_END
