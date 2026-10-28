#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <utility>
#include <vector>

#include <vine/graphics/graphics_global.hpp>

VN_GRAPHICS_NS_BEGIN

namespace detail
{

/** @brief The order a frame's passes execute in, and the components that could not be ordered. */
struct FrameSchedule {
    std::vector<std::uint32_t>              order{};   // Indices, in execution order.
    std::vector<std::vector<std::uint32_t>> cycles{};  // One entry per cyclic component: its members.
    std::vector<std::uint32_t>              skipped{}; // Every member of every cycle, flattened.

    /** @brief Gets whether the whole frame could be ordered. */
    [[nodiscard]] bool valid() const noexcept
    {
        return cycles.empty();
    }

    /** @brief Gets how many cyclic components were found (two independent cycles are two problems). */
    [[nodiscard]] std::size_t cycleCount() const noexcept
    {
        return cycles.size();
    }
};

/**
 * @brief The engine's one pass scheduler: dependencies first, announced order as the tie-break.
 *
 * @details
 * - WHY THE ENGINE OWNS IT. The declared inputs decide when a pass MAY run; the frame is the union of
 *   every registered pass, so the compile is the only place that sees all producers and consumers. A backend
 *   that re-derived the order could disagree with the frame the engine announced, and two orders mean two
 *   pictures.
 * - STABLE, NOT MERELY VALID. Among the passes that may run next, the smallest announced order goes first,
 *   and equal orders keep registration order - the contract's own stacking rule.
 * - A CYCLE IS SKIPPED AND SAID. Running a cyclic component samples an attachment nobody wrote; dropping the
 *   frame blacks out the picture for a bug in two auxiliary passes. Every member of a cyclic component is
 *   left out, every other pass still runs, and the frame still presents.
 * - WORKING MEMORY, NOT PLAN STORAGE. Rebuilt every frame; every table below is a MEMBER filled in place, so
 *   a steady frame asks the allocator for nothing (the finding that put this note here is ~27 allocations for
 *   a one-pass pipeline when the tables were locals; see AllocationGate's counting half).
 */
class FrameScheduler {
  public:
    /** @brief Rebuilds the graph: one node per pass, in announcement order.
     *
     * @param orders The announced sort key of each pass (its stage-derived or explicit order).
     */
    void reset(std::span<const int> orders);

    /** @brief Adds "before executes before after", ignoring an exact duplicate.
     *
     * @param before Node that produces.
     * @param after  Node that consumes.
     */
    void addEdge(std::uint32_t before, std::uint32_t after);

    /** @brief Takes a node out of the schedule (the caller could not serve its pass this frame).
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
    std::vector<std::vector<std::uint32_t>> successors_; // Adjacency: node -> the nodes it must precede.
    std::vector<int>                        order_key_;  // Announced stacking order per node.
    std::vector<std::uint8_t>               excluded_;   // Non-zero = takes no part in this frame.
    FrameSchedule                           schedule_;   // The last computed answer.

    // The per-frame working memory (see the class note): the Tarjan tables, the component stack and the
    // ready-queue backing store. Members, because a solver that builds them per frame allocates per frame.
    std::vector<std::uint32_t>                 tarjan_index_;      // Discovery index per node (kUnvisited = unseen).
    std::vector<std::uint32_t>                 tarjan_low_;        // Lowest reachable index per node.
    std::vector<std::uint32_t>                 tarjan_cursor_;     // Next successor to visit per node.
    std::vector<std::uint8_t>                  on_stack_;          // Whether a node is on the component stack.
    std::vector<std::uint32_t>                 component_stack_;   // Tarjan's component stack.
    std::vector<std::uint32_t>                 dfs_stack_;         // The iterative DFS' own stack.
    std::vector<std::uint32_t>                 in_degree_;         // Unordered predecessors per node.
    std::vector<std::pair<int, std::uint32_t>> ready_;             // The ready set, kept as a heap.
    std::vector<std::uint32_t>                 component_scratch_; // One component, before it is known cyclic.
};

} // namespace detail

VN_GRAPHICS_NS_END
