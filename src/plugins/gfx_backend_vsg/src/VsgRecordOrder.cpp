#include <vine/vsg/VsgRecordOrder.hpp>

#include <cstddef>
#include <map>
#include <vector>

#include <vsg/app/RenderGraph.h>
#include <vsg/core/ref_ptr.h>
#include <vsg/nodes/InstrumentationNode.h>

#include <vine/graphics/RenderTarget.hpp>

#include <vine/vsg/VsgBackendUtility.hpp>
#include <vine/vsg/VsgGpuProfile.hpp>

V_VSG_NS_BEGIN

namespace detail
{

namespace
{

/**
 * @brief One pass' graph as the command graph should record it.
 *
 * A session that measures the GPU (VINE_VSG_PROFILE, see VsgGpuProfile.hpp) records every pass through a
 * named `vsg::InstrumentationNode`: the profiler's timestamp interval for that graph then carries the GRAPH
 * as its object, which is what lets a measurement be attributed to the pass behind it -- upstream's own
 * per-graph interval passes no object at all, so its entries cannot be attributed to anything. The name is
 * only for a human reading vsg's report; the reader matches on the graph.
 *
 * @param state Session state (its profiler decides whether a wrapper is needed).
 * @param target Target the graph renders into (nullptr = the window target).
 * @param graph  The pass' render graph.
 * @return The child to record; the graph itself when the session is not measuring.
 */
::vsg::ref_ptr<::vsg::Node> recordedChild(const VsgRendererState& state, vine::graphics::RenderTarget* target,
                                         const ::vsg::ref_ptr<::vsg::RenderGraph>& graph)
{
    if (state.profiler == nullptr || graph == nullptr) {
        return graph;
    }
    std::string name = "pass";
    if (const auto found = state.targets.find(target); found != state.targets.end()) {
        const VsgGpuPassSample sample = describeGraph(target, found->second, graph.get());
        name                          = sample.pass + "@" + sample.target;
    }
    auto wrapper = ::vsg::InstrumentationNode::create(graph);
    wrapper->setName(name);
    return wrapper;
}

} // namespace

void fillRecordPlan(const VsgRendererState& state, RecordPlan& plan)
{
    const auto& children = state.command_graph->children;
    // A pass whose retained slot was RETIRED (its views detached because the pass
    // did not execute this frame) must not be recorded at all. Each pass graph is
    // its own render pass now, so leaving a retired one in the command graph would
    // execute that pass' load-ops every frame with NO content — a disabled clearing
    // pass would go on clearing a target the active passes just drew into, which is
    // the opposite of retiring it (see retireInactivePassSlots).
    const auto pass_records = [](const VsgRenderTargetEntry& owner, const SlotKey& key,
                                 const VsgRenderTargetEntry::PassObjects& objects) {
        // A pass the target was RESIZED out from under must not record. Its framebuffer names the views
        // of the images that resize replaced, and the rebuild that re-plans it has not run (an in-place
        // resize bumps the target's attachments_generation and every variant built against the old one
        // is sent back through the plan — see VsgTargetBookkeeping). Recording it would draw into images
        // the retire ring is about to release and then name destroyed views, with no validation error
        // while they are still alive. Skipping it for THIS frame keeps the images and the objects that
        // name them on one generation; the pass' next build re-plans it.
        if (objects.attachments_generation != owner.attachments_generation) {
            return false;
        }
        if (const auto it = owner.content_slots.find(key); it != owner.content_slots.end()) {
            return !it->second.detached;
        }
        if (const auto it = owner.program_slots.find(key); it != owner.program_slots.end()) {
            return !it->second.detached;
        }
        return true; // no retained slot yet: nothing to retire
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
            const auto* recorded = detail::underlyingGraph(child.get());
            if (recorded == nullptr || recorded == plan.window_graph.get()) {
                continue;
            }
            for (const auto& pass : entry.second.passes) {
                if (pass.second.graph.get() == recorded && pass_records(entry.second, pass.first, pass.second)) {
                    graphs.push_back(pass.second.graph);
                    break;
                }
            }
        }
        for (const auto& pass : entry.second.passes) {
            if (pass.second.graph != nullptr && pass_records(entry.second, pass.first, pass.second) &&
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
        const auto* recorded = detail::underlyingGraph(child.get());
        if (recorded == nullptr || recorded == plan.window_graph.get()) {
            continue;
        }
        for (const auto& entry : plan.graphs_of) {
            const bool holds = std::any_of(entry.second.begin(), entry.second.end(),
                                           [recorded](const ::vsg::ref_ptr<::vsg::RenderGraph>& graph) {
                                               return graph.get() == recorded;
                                           });
            if (!holds) {
                continue;
            }
            if (seen.insert(entry.first).second) {
                plan.recorded_now.push_back(entry.first);
            }
            break;
        }
    }
}

std::vector<GraphOrderEdge> collectOrderEdges(const VsgRendererState& state,
                                              const std::vector<vine::graphics::RenderTarget*>& recorded_now)
{
    // Sampling edges: a consumer depends on every source it samples. Self-sampling is rejected on
    // attach and mutual same-frame sampling (ping-pong inside one frame) is not a supported pattern,
    // so the edge graph is acyclic in practice; a cycle would only leave the targets in their current
    // order.
    std::map<vine::graphics::RenderTarget*, std::size_t> index_of;
    for (std::size_t i = 0; i < recorded_now.size(); ++i) {
        index_of.emplace(recorded_now[i], i);
    }
    std::vector<GraphOrderEdge> edges;
    for (auto* t : recorded_now) {
        const auto entry = state.targets.find(t);
        if (entry == state.targets.end()) {
            continue;
        }
        const auto& target     = entry->second;
        const auto  add_source = [&](const vine::graphics::RenderTarget* source) {
            if (source == nullptr || source == t) {
                return;
            }
            const auto src = index_of.find(const_cast<vine::graphics::RenderTarget*>(source));
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
        for (const auto& slot : target.program_slots) {
            if (!slot.second.detached) {
                add_source(slot.second.source_target);
            }
        }
        // A CONTENT slot samples a target too — the shadow map its shader reads, resolved from the map's
        // own statement of whose shadow it is (see resolveShadowInput) — and that is the same
        // dependency, because a shadow pass is an ordinary pass on an ordinary target. It went missing
        // here while the content path resolved its shadow at slot-setup time, which is exactly why
        // ContentSlot::sampled_target records the producer.
        for (const auto& slot : target.content_slots) {
            if (!slot.second.detached) {
                add_source(slot.second.sampled_target);
            }
        }
    }
    return edges;
}

void orderRecordPlan(const VsgRendererState& state, RecordPlan& plan)
{
    const std::vector<GraphOrderEdge> edges = collectOrderEdges(state, plan.recorded_now);
    plan.record_order.reserve(plan.recorded_now.size());
    for (const std::size_t index : stableTopologicalOrder(plan.recorded_now.size(), edges)) {
        plan.record_order.push_back(plan.recorded_now[index]);
    }
}

void applyRecordPlan(VsgRendererState& state, const RecordPlan& plan)
{
    auto& children = state.command_graph->children;
    children.clear();
    for (auto* t : plan.record_order) {
        const auto graphs = plan.graphs_of.find(t);
        if (graphs == plan.graphs_of.end()) {
            continue;
        }
        for (const auto& graph : graphs->second) {
            children.push_back(recordedChild(state, t, graph));
        }
        // After the LAST pass graph of a target whose depth another target borrows,
        // insert that borrower's depth-share barrier so its LOAD / depth test sees
        // this target's writes (both share one depth image in the attachment layout).
        //
        // A borrower whose source records NOTHING this frame (all of its passes retired) is deliberately
        // left alone: the barrier has no writes to order, and the image still holds what the source last
        // wrote, which is the only self-consistent content available — nobody wrote it this frame, so
        // there is no race, only content that stopped advancing while the producer is off. Refusing the
        // borrow here would drop a pass whose host disabled the producer on purpose.
        for (const auto& entry : state.targets) {
            const auto& other = entry.second;
            if (other.depth_source == t && other.depth_share_barrier != nullptr) {
                children.push_back(other.depth_share_barrier);
            }
        }
    }
    children.push_back(recordedChild(state, nullptr, plan.window_graph));
}

void reconcileOffscreenOrder(VsgRendererState& state)
{
    // Keeps the command graph's child render graphs in a dependency-valid RECORD
    // order: a full-screen program pass that samples another target reads that target's colour
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
