#include <vine/vsg/VsgRecordOrder.hpp>

#include <cstddef>
#include <map>
#include <vector>

#include <vsg/app/RenderGraph.h>
#include <vsg/core/ref_ptr.h>

#include <vine/graphics/RenderTarget.hpp>

#include <vine/vsg/VsgBackendUtility.hpp>

V_VSG_NS_BEGIN

namespace detail
{

void fillRecordPlan(const VsgRendererState& state, RecordPlan& plan)
{
    const auto& children = state.command_graph->children;
    // A pass whose retained slot was RETIRED (its views detached because the pass
    // did not execute this frame) must not be recorded at all. Each pass graph is
    // its own render pass now, so leaving a retired one in the command graph would
    // execute that pass' load-ops every frame with NO content — a disabled clearing
    // pass would go on clearing a target the active passes just drew into, which is
    // the opposite of retiring it (see retireInactivePassSlots).
    const auto pass_records = [](const VsgRenderTargetEntry& owner, const SlotKey& key) {
        if (const auto it = owner.content_slots.find(key); it != owner.content_slots.end()) {
            return !it->second.detached;
        }
        if (const auto it = owner.screen_slots.find(key); it != owner.screen_slots.end()) {
            return !it->second.detached;
        }
        if (const auto it = owner.program_slots.find(key); it != owner.program_slots.end()) {
            return !it->second.detached;
        }
        return true; // no retained slot (a direct-driver pass): nothing to retire
    };
    // Inside a target the graphs must record in the passes' explicit pipeline order
    // (setPassOrder) — the position each pass' content would have occupied as a View
    // of a single target-wide render pass. Taking that order from the slot map
    // (pointer order) would let a target's SECOND pass record before its first, so
    // the second pass' colour clear would wipe the first pass' draws.
    const auto graph_order = [](const VsgRenderTargetEntry& owner, const ::vsg::ref_ptr<::vsg::RenderGraph>& graph) {
        for (const auto& pass : owner.passes) {
            if (pass.second.graph == graph) {
                return pass.second.order;
            }
        }
        return std::numeric_limits<int>::max();
    };
    for (const auto& entry : state.targets) {
        if (entry.first == nullptr) {
            continue;
        }
        // Seed from the order the graphs are recorded in RIGHT NOW, so passes
        // carrying the same explicit order keep their relative position, then append
        // the ones created since the last reconcile.
        std::vector<::vsg::ref_ptr<::vsg::RenderGraph>> graphs;
        for (const auto& child : children) {
            if (child == plan.window_graph) {
                continue;
            }
            for (const auto& pass : entry.second.passes) {
                if (pass.second.graph == child && pass_records(entry.second, pass.first)) {
                    graphs.push_back(pass.second.graph);
                    break;
                }
            }
        }
        for (const auto& pass : entry.second.passes) {
            if (pass.second.graph != nullptr && pass_records(entry.second, pass.first) &&
                std::find(graphs.begin(), graphs.end(), pass.second.graph) == graphs.end()) {
                graphs.push_back(pass.second.graph);
            }
        }
        if (!graphs.empty()) {
            std::stable_sort(graphs.begin(), graphs.end(),
                             [&entry, &graph_order](const ::vsg::ref_ptr<::vsg::RenderGraph>& lhs,
                                                    const ::vsg::ref_ptr<::vsg::RenderGraph>& rhs) {
                                 return graph_order(entry.second, lhs) < graph_order(entry.second, rhs);
                             });
            plan.graphs_of.emplace(entry.first, std::move(graphs));
        }
    }
    // The state.targets recorded RIGHT NOW, in child order: the stable tie-break seed.
    std::set<vine::graphics::RenderTarget*> seen;
    for (const auto& child : children) {
        if (child == plan.window_graph) {
            continue;
        }
        for (const auto& entry : plan.graphs_of) {
            if (std::find(entry.second.begin(), entry.second.end(), child) == entry.second.end()) {
                continue;
            }
            if (seen.insert(entry.first).second) {
                plan.present.push_back(entry.first);
            }
            break;
        }
    }
}

void orderRecordPlan(const VsgRendererState& state, RecordPlan& plan)
{
    // Sampling edges: a consumer depends on every source it samples (screen slot
    // keys carry the sampled target; program slots are keyed by it). Self-sampling
    // is rejected on attach and mutual same-frame sampling (ping-pong inside one
    // frame) is not a supported pattern, so the edge graph is acyclic in practice; a
    // cycle would only leave state.targets in their current order.
    std::map<vine::graphics::RenderTarget*, std::size_t> index_of;
    for (std::size_t i = 0; i < plan.present.size(); ++i) {
        index_of.emplace(plan.present[i], i);
    }
    std::vector<GraphOrderEdge> edges;
    for (auto* t : plan.present) {
        const auto entry = state.targets.find(t);
        if (entry == state.targets.end()) {
            continue;
        }
        const auto& target     = entry->second;
        const auto  add_source = [&](vine::graphics::RenderTarget* source) {
            if (source == nullptr || source == t) {
                return;
            }
            const auto src = index_of.find(source);
            if (src == index_of.end()) {
                return; // the source is not recorded this frame: no edge
            }
            edges.push_back(GraphOrderEdge{ index_of[t], src->second });
        };
        // A DEPTH BORROW is a dependency too, and not a sampling one: the borrower's
        // pass LOADs (tests against) the depth the source's pass writes this frame.
        // Without an edge here the order came from the build order alone, so a
        // borrower whose graph happened to be created before the source's was
        // RECORDED FIRST and tested against the previous frame's depth — a silent
        // one-frame lag, which no validation layer reports (the layouts match; only
        // the write→read dependency is wrong). The barrier applied in phase 3 then
        // also sits after the source, i.e. between the two, as it must.
        if (target.depth_source != nullptr) {
            add_source(target.depth_source);
        }
        // A slot's sampled target is a slot attribute (its key is the owning pass),
        // so the dependency edges come from the attribute. A retired (detached) slot
        // is not recorded, so it contributes no edge.
        for (const auto& slot : target.screen_slots) {
            if (!slot.second.detached) {
                add_source(const_cast<vine::graphics::RenderTarget*>(slot.second.source_target));
            }
        }
        for (const auto& slot : target.program_slots) {
            if (!slot.second.detached) {
                add_source(const_cast<vine::graphics::RenderTarget*>(slot.second.source_target));
            }
        }
    }
    plan.order.reserve(plan.present.size());
    for (const std::size_t index : stableTopologicalOrder(plan.present.size(), edges)) {
        plan.order.push_back(plan.present[index]);
    }
}

void applyRecordPlan(VsgRendererState& state, const RecordPlan& plan)
{
    auto& children = state.command_graph->children;
    children.clear();
    for (auto* t : plan.order) {
        const auto graphs = plan.graphs_of.find(t);
        if (graphs == plan.graphs_of.end()) {
            continue;
        }
        for (const auto& graph : graphs->second) {
            children.push_back(graph);
        }
        // After the LAST pass graph of a target whose depth another target borrows,
        // insert that borrower's depth-share barrier so its LOAD / depth test sees
        // this target's writes (both share one depth image in the attachment layout).
        for (const auto& entry : state.targets) {
            const auto& other = entry.second;
            if (other.depth_source == t && other.depth_share_barrier != nullptr) {
                children.push_back(other.depth_share_barrier);
            }
        }
    }
    children.push_back(plan.window_graph);
}

void reconcileOffscreenOrder(VsgRendererState& state)
{
    // Keeps the command graph's child render graphs in a dependency-valid RECORD
    // order: a screen pass that samples another target reads that target's colour
    // texture, and the sample is only CURRENT when the producer's graph is recorded
    // first. The three phases (collect → order → apply) are named units; each
    // explains what it guarantees.
    if (state.command_graph == nullptr) {
        return;
    }
    const auto win = state.targets.find(nullptr);
    if (win == state.targets.end() || win->second.graph == nullptr) {
        return;
    }
    detail::RecordPlan plan;
    plan.window_graph = win->second.graph;
    fillRecordPlan(state, plan);
    orderRecordPlan(state, plan);
    applyRecordPlan(state, plan);
}

} // namespace detail

V_VSG_NS_END
