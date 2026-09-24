#include <vine/vsg/core/FrameGraph.hpp>

#include <algorithm>
#include <cstdint>
#include <functional>
#include <utility>
#include <vector>

VN_VSG_NS_BEGIN

namespace core
{
namespace
{

/// @brief Marks a node the depth-first search has not reached yet.
constexpr std::uint32_t kUnvisited = 0xFFFFFFFFU;

}  // namespace

void FrameGraph::reset(std::span<const CollectedPass> passes)
{
    const std::size_t count = passes.size();

    // IN PLACE, CAPACITY KEPT (see the header's working-memory note): resizing the adjacency table only
    // allocates when the pass count GROWS, and clearing each row keeps the buffer an edge will need - which
    // is what makes a steady frame's addEdge() allocation-free instead of one allocation per edge.
    if (successors_.size() != count)
    {
        successors_.resize(count);
    }
    for (std::vector<std::uint32_t>& out : successors_)
    {
        out.clear();
    }

    if (order_key_.size() != count)
    {
        order_key_.resize(count);
    }
    if (excluded_.size() != count)
    {
        excluded_.resize(count);
    }
    std::fill(excluded_.begin(), excluded_.end(), static_cast<std::uint8_t>(0));

    // The answer of the LAST frame is cleared, not replaced: assigning a fresh FrameSchedule would free the
    // three buffers this one will fill again - and the cycle list is only touched when there really were
    // cycles (a pipeline with a cycle is a configuration error, so that path may allocate; the ordinary
    // frame must not).
    schedule_.order.clear();
    schedule_.skipped.clear();
    if (!schedule_.cycles.empty())
    {
        for (std::vector<std::uint32_t>& component : schedule_.cycles)
        {
            component.clear();
        }
        schedule_.cycles.clear();
    }

    for (std::size_t index = 0; index < count; ++index)
    {
        order_key_[index] = passes[index].order;
    }
}

void FrameGraph::addEdge(std::uint32_t before, std::uint32_t after)
{
    if (before >= successors_.size() || after >= successors_.size())
    {
        return;
    }
    std::vector<std::uint32_t>& out = successors_[before];
    if (std::find(out.begin(), out.end(), after) != out.end())
    {
        return;  // the same dependency stated twice is one dependency
    }
    out.push_back(after);
}

void FrameGraph::exclude(std::uint32_t node)
{
    if (node < excluded_.size())
    {
        excluded_[node] = 1;
    }
}

std::size_t FrameGraph::nodeCount() const noexcept
{
    return successors_.size();
}

std::size_t FrameGraph::excludedCount() const noexcept
{
    return static_cast<std::size_t>(std::count(excluded_.begin(), excluded_.end(), static_cast<std::uint8_t>(1)));
}

std::size_t FrameGraph::edgeCount() const noexcept
{
    std::size_t total = 0;
    for (std::size_t node = 0; node < successors_.size(); ++node)
    {
        if (excluded_[node] == 0)
        {
            total += successors_[node].size();
        }
    }
    return total;
}

const FrameSchedule& FrameGraph::schedule()
{
    // The bookkeeping is cleared, not rebuilt (see the header's working-memory note): a steady frame that
    // assigned fresh containers here would allocate on every frame, which is what the counting half of
    // core::AllocationGate measured (27 blocks for a one-pass pipeline).
    schedule_.order.clear();
    schedule_.skipped.clear();
    if (!schedule_.cycles.empty())
    {
        for (std::vector<std::uint32_t>& component : schedule_.cycles)
        {
            component.clear();
        }
        schedule_.cycles.clear();
    }
    findCycles();
    orderAcyclic();
    return schedule_;
}

void FrameGraph::findCycles()
{
    // Tarjan, iterative: the recursion a textbook version uses would put the pass count on the C++ call
    // stack, and the pass count is the HOST's number, not this backend's. The three tables and the two
    // stacks are MEMBERS filled in place (see the header): the textbook spelling - one `std::vector` per
    // table, constructed here - is six allocations a frame before any work happens.
    const std::size_t node_count = successors_.size();
    if (tarjan_index_.size() != node_count)
    {
        tarjan_index_.resize(node_count);
        tarjan_low_.resize(node_count);
        tarjan_cursor_.resize(node_count);
        on_stack_.resize(node_count);
    }
    std::fill(tarjan_index_.begin(), tarjan_index_.end(), kUnvisited);
    std::fill(tarjan_low_.begin(), tarjan_low_.end(), 0U);
    std::fill(tarjan_cursor_.begin(), tarjan_cursor_.end(), 0U);
    std::fill(on_stack_.begin(), on_stack_.end(), static_cast<std::uint8_t>(0));
    component_stack_.clear();
    dfs_stack_.clear();

    std::uint32_t next_index = 0;

    for (std::uint32_t root = 0; root < node_count; ++root)
    {
        if (excluded_[root] != 0 || tarjan_index_[root] != kUnvisited)
        {
            continue;
        }

        tarjan_index_[root] = next_index;
        tarjan_low_[root]   = next_index;
        ++next_index;
        component_stack_.push_back(root);
        on_stack_[root] = 1U;
        dfs_stack_.push_back(root);

        while (!dfs_stack_.empty())
        {
            const std::uint32_t node = dfs_stack_.back();
            if (tarjan_cursor_[node] < successors_[node].size())
            {
                const std::uint32_t next = successors_[node][tarjan_cursor_[node]];
                ++tarjan_cursor_[node];
                if (excluded_[next] != 0)
                {
                    continue;
                }
                if (tarjan_index_[next] == kUnvisited)
                {
                    tarjan_index_[next] = next_index;
                    tarjan_low_[next]   = next_index;
                    ++next_index;
                    component_stack_.push_back(next);
                    on_stack_[next] = 1U;
                    dfs_stack_.push_back(next);
                }
                else if (on_stack_[next] != 0)
                {
                    tarjan_low_[node] = std::min(tarjan_low_[node], tarjan_index_[next]);
                }
                continue;
            }

            // The node is finished: its component is known, and its low link reaches its parent.
            dfs_stack_.pop_back();
            if (!dfs_stack_.empty())
            {
                tarjan_low_[dfs_stack_.back()] = std::min(tarjan_low_[dfs_stack_.back()], tarjan_low_[node]);
            }

            if (tarjan_low_[node] != tarjan_index_[node])
            {
                continue;
            }

            // ONE component: collected into a stack scratch first, so the answer only allocates when the
            // component is really cyclic (the common case is a singleton, which is not copied anywhere).
            component_scratch_.clear();
            for (;;)
            {
                const std::uint32_t member = component_stack_.back();
                component_stack_.pop_back();
                on_stack_[member] = 0U;
                component_scratch_.push_back(member);
                if (member == node)
                {
                    break;
                }
            }

            const bool cyclic = component_scratch_.size() > 1U || selfLoop(node);
            if (!cyclic)
            {
                continue;
            }

            std::sort(component_scratch_.begin(), component_scratch_.end());
            schedule_.skipped.insert(schedule_.skipped.end(), component_scratch_.begin(), component_scratch_.end());
            schedule_.cycles.push_back(component_scratch_);
        }
    }

    std::sort(schedule_.skipped.begin(), schedule_.skipped.end());
}

void FrameGraph::orderAcyclic()
{
    const std::size_t node_count = successors_.size();

    // The passes of a cycle take no part in the order (they do not draw this frame); everything else does,
    // including the passes that only READ a skipped one - their input was not produced, which is the same
    // situation as an input nobody published at all, and the contract keeps drawing through it.
    //
    // Edges INTO a skipped node are dropped with the node: a consumer waiting for something that will
    // never run would wait forever and never be scheduled, which would turn one cycle into a silently
    // missing frame.
    const auto usable = [this](std::uint32_t node) noexcept -> bool {
        return excluded_[node] == 0 && !isSkipped(node);
    };

    if (in_degree_.size() != node_count)
    {
        in_degree_.resize(node_count);
    }
    std::fill(in_degree_.begin(), in_degree_.end(), 0U);

    for (std::uint32_t node = 0; node < node_count; ++node)
    {
        if (!usable(node))
        {
            continue;
        }
        for (const std::uint32_t next : successors_[node])
        {
            if (usable(next))
            {
                ++in_degree_[next];
            }
        }
    }

    // Smallest announced order first, ties by the position the pass was announced in. A HEAP OVER A MEMBER
    // VECTOR rather than a `std::priority_queue`: the queue's own container is constructed when the queue
    // is, and the push/pop algorithms are in-place, so keeping the buffer is the whole difference.
    using Ready = std::pair<int, std::uint32_t>;
    ready_.clear();
    for (std::uint32_t node = 0; node < node_count; ++node)
    {
        if (usable(node) && in_degree_[node] == 0)
        {
            ready_.emplace_back(order_key_[node], node);
        }
    }
    std::make_heap(ready_.begin(), ready_.end(), std::greater<Ready>{});

    while (!ready_.empty())
    {
        std::pop_heap(ready_.begin(), ready_.end(), std::greater<Ready>{});
        const std::uint32_t node = ready_.back().second;
        ready_.pop_back();
        schedule_.order.push_back(node);

        for (const std::uint32_t next : successors_[node])
        {
            if (!usable(next))
            {
                continue;
            }
            if (--in_degree_[next] == 0)
            {
                ready_.emplace_back(order_key_[next], next);
                std::push_heap(ready_.begin(), ready_.end(), std::greater<Ready>{});
            }
        }
    }
}

bool FrameGraph::selfLoop(std::uint32_t node) const noexcept
{
    const std::vector<std::uint32_t>& out = successors_[node];
    return std::find(out.begin(), out.end(), node) != out.end();
}

bool FrameGraph::isSkipped(std::uint32_t node) const noexcept
{
    // `skipped` is kept sorted (see findCycles), so this is the cheap half of the question.
    return std::binary_search(schedule_.skipped.begin(), schedule_.skipped.end(), node);
}

}  // namespace core

VN_VSG_NS_END
