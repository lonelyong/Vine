#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

#include <vine/vsg/core/FrameRecorder.hpp>
#include <vine/vsg/vsg_global.hpp>

/**
 * @brief Who must execute before whom inside one frame, and the order that follows - cycles included.
 *
 * WHY A GRAPH AND NOT "JUST RUN THEM IN ORDER". The host announces its passes in registration order, but
 * what a pass READS decides when it may run: a lighting pass that samples a G-buffer must run after the
 * pass that wrote it. Nothing upstream orders them - the engine resolves a pass' declared inputs and
 * checks who announced what (validateWiring), and it has no cycle detection at all - so the backend is
 * the only place that can see a cycle.
 *
 * WHAT A CYCLE MEANS HERE, AND WHY IT IS NOT "KEEP THE CURRENT ORDER". A cycle is a declaration no
 * execution order can satisfy: A reads what B writes while B reads what A writes. The three candidate
 * answers are all wrong except one. Run them in call order anyway: the consumer samples an attachment
 * nobody wrote this frame, and the picture is wrong in a way nothing explains. Drop the whole frame: one
 * cycle between two auxiliary passes blacks out the main picture. SKIP THE CYCLE AND SAY SO: every pass
 * of a cyclic component is left out (`FrameSchedule::skipped`), every other pass still runs, and the
 * frame still presents - which is what this type implements. The host is told (FrameCompiler reports it)
 * and the counter that a phase gates on moves.
 *
 * THE SCHEDULE IS STABLE, NOT MERELY VALID. Among the passes that may run next, the one with the smallest
 * announced order goes first, and passes with equal orders keep their call order - the contract's own
 * rule ("per enabled pass, in ascending pass order", equal orders keep registration order). A topological
 * sort that ignored the announced order would silently re-stack a picture whose passes overlap.
 *
 * WORKING MEMORY, NOT PLAN STORAGE. The graph is rebuilt every frame and holds no GPU object, no host
 * pointer and no arena span - the plan the executor reads is arena-backed (see FrameCompiler) - so plain
 * vectors are the right storage here.
 */
V_VSG_NS_BEGIN

namespace core
{

/** @brief The order the frame's pass scopes execute in, and what could not be ordered. */
struct FrameSchedule
{
    std::vector<std::uint32_t>              order{};    ///< Node indices, in execution order.
    std::vector<std::vector<std::uint32_t>> cycles{};   ///< One entry per cyclic component: its members.
    std::vector<std::uint32_t>              skipped{};  ///< Every member of every cycle, flattened.

    /** @brief Gets whether the whole frame could be ordered. */
    [[nodiscard]] bool valid() const noexcept { return cycles.empty(); }

    /** @brief Gets how many cyclic components were found (two independent cycles are two problems). */
    [[nodiscard]] std::size_t cycleCount() const noexcept { return cycles.size(); }
};

/**
 * @brief The frame's pass-dependency graph (see the file note for the rule it implements).
 */
class FrameGraph
{
  public:
    /** @brief Rebuilds the graph: one node per pass, in the order the passes were announced.
     *
     * Node indices ARE the pass indices of the description, so an edge is written in the caller's own
     * terms. Priorities come from each pass' announced order and its position in the list.
     *
     * @param passes Pass scopes as the recorder collected them.
     */
    void reset(std::span<const CollectedPass> passes);

    /** @brief Adds "before executes before after", ignoring an exact duplicate.
     *
     * A self-edge is kept: it says the pass must run before itself, which only a cyclic declaration can
     * mean, and it is treated as one. The compiler never produces one - a pass that names its own target
     * as an input is the feedback pattern, which the executor judges (see FrameCompiler).
     *
     * @param before Node that produces.
     * @param after  Node that consumes.
     */
    void addEdge(std::uint32_t before, std::uint32_t after);

    /** @brief Takes a node out of the schedule (the caller could not serve its pass this frame).
     *
     * Its edges go with it: an excluded node neither orders nor blocks anything.
     *
     * @param node Node index.
     */
    void exclude(std::uint32_t node);

    /** @brief Computes (or returns) the execution order for the current graph.
     *
     * @return The schedule: what runs, in what order, and which components could not be ordered.
     */
    [[nodiscard]] const FrameSchedule& schedule();

    /** @brief Gets the number of nodes (one per pass of the last reset()). */
    [[nodiscard]] std::size_t nodeCount() const noexcept;

    /** @brief Gets how many nodes were excluded this frame. */
    [[nodiscard]] std::size_t excludedCount() const noexcept;

    /** @brief Gets how many edges were added this frame (duplicates not counted). */
    [[nodiscard]] std::size_t edgeCount() const noexcept;


  private:
    /** @brief Gets whether @p node has an edge to itself (only a cyclic declaration means that). */
    [[nodiscard]] bool selfLoop(std::uint32_t node) const noexcept;

    /** @brief Gets whether @p node is a member of a cyclic component. */
    [[nodiscard]] bool isSkipped(std::uint32_t node) const noexcept;

    /** @brief Finds the strongly connected components and records the cyclic ones. */
    void findCycles();

    /** @brief Emits the order for the nodes that are left: smallest (order, position) first. */
    void orderAcyclic();


  private:
    std::vector<std::vector<std::uint32_t>> successors_;  ///< Adjacency: node -> the nodes it must precede.
    std::vector<int>                        order_key_;   ///< Announced stacking order per node.
    std::vector<std::uint8_t>               excluded_;    ///< Non-zero = takes no part in this frame.
    FrameSchedule                           schedule_;    ///< The last computed answer.
};

}  // namespace core

V_VSG_NS_END
