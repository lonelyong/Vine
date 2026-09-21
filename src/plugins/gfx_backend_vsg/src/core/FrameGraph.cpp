#include <vine/vsg/core/FrameGraph.hpp>

#include <algorithm>
#include <cstdint>
#include <queue>
#include <utility>
#include <vector>

V_VSG_NS_BEGIN

namespace core
{
namespace
{

/// @brief Marks a node the depth-first search has not reached yet.
constexpr std::uint32_t kUnvisited = 0xFFFFFFFFU;

}  // namespace

void FrameGraph::reset(std::span<const CollectedPass> passes)
{
    successors_.assign(passes.size(), std::vector<std::uint32_t>{});
    order_key_.resize(passes.size());
    excluded_.assign(passes.size(), 0);
    schedule_ = FrameSchedule{};

    for (std::size_t index = 0; index < passes.size(); ++index)
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
    schedule_ = FrameSchedule{};
    findCycles();
    orderAcyclic();
    return schedule_;
}

void FrameGraph::findCycles()
{
    // Tarjan, iterative: the recursion a textbook version uses would put the pass count on the C++ call
    // stack, and the pass count is the HOST's number, not this backend's.
    const std::size_t          node_count = successors_.size();
    std::vector<std::uint32_t> index(node_count, kUnvisited);
    std::vector<std::uint32_t> low(node_count, 0);
    std::vector<std::uint32_t> cursor(node_count, 0);
    std::vector<bool>          on_stack(node_count, false);
    std::vector<std::uint32_t> component_stack;
    std::vector<std::uint32_t> dfs_stack;
    std::uint32_t              next_index = 0;

    for (std::uint32_t root = 0; root < node_count; ++root)
    {
        if (excluded_[root] != 0 || index[root] != kUnvisited)
        {
            continue;
        }

        index[root] = next_index;
        low[root]   = next_index;
        ++next_index;
        component_stack.push_back(root);
        on_stack[root] = true;
        dfs_stack.push_back(root);

        while (!dfs_stack.empty())
        {
            const std::uint32_t node = dfs_stack.back();
            if (cursor[node] < successors_[node].size())
            {
                const std::uint32_t next = successors_[node][cursor[node]];
                ++cursor[node];
                if (excluded_[next] != 0)
                {
                    continue;
                }
                if (index[next] == kUnvisited)
                {
                    index[next] = next_index;
                    low[next]   = next_index;
                    ++next_index;
                    component_stack.push_back(next);
                    on_stack[next] = true;
                    dfs_stack.push_back(next);
                }
                else if (on_stack[next])
                {
                    low[node] = std::min(low[node], index[next]);
                }
                continue;
            }

            // The node is finished: its component is known, and its low link reaches its parent.
            dfs_stack.pop_back();
            if (!dfs_stack.empty())
            {
                low[dfs_stack.back()] = std::min(low[dfs_stack.back()], low[node]);
            }

            if (low[node] != index[node])
            {
                continue;
            }

            std::vector<std::uint32_t> component;
            for (;;)
            {
                const std::uint32_t member = component_stack.back();
                component_stack.pop_back();
                on_stack[member] = false;
                component.push_back(member);
                if (member == node)
                {
                    break;
                }
            }

            const bool cyclic = component.size() > 1 || selfLoop(node);
            if (!cyclic)
            {
                continue;
            }

            std::sort(component.begin(), component.end());
            schedule_.skipped.insert(schedule_.skipped.end(), component.begin(), component.end());
            schedule_.cycles.push_back(std::move(component));
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

    std::vector<std::uint32_t> in_degree(node_count, 0);
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
                ++in_degree[next];
            }
        }
    }

    // Smallest announced order first, ties by the position the pass was announced in.
    using Ready = std::pair<int, std::uint32_t>;
    std::priority_queue<Ready, std::vector<Ready>, std::greater<Ready>> ready;
    for (std::uint32_t node = 0; node < node_count; ++node)
    {
        if (usable(node) && in_degree[node] == 0)
        {
            ready.emplace(order_key_[node], node);
        }
    }

    while (!ready.empty())
    {
        const std::uint32_t node = ready.top().second;
        ready.pop();
        schedule_.order.push_back(node);

        for (const std::uint32_t next : successors_[node])
        {
            if (!usable(next))
            {
                continue;
            }
            if (--in_degree[next] == 0)
            {
                ready.emplace(order_key_[next], next);
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

V_VSG_NS_END
