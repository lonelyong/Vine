#pragma once
#include <vine/vsg/vsg_global.hpp>

// Internal header: the small free-function helpers every renderer translation
// unit needs — render-graph surgery, device synchronization and session policy.
// Split out of VsgRenderer.cpp together with the object factories (see
// VsgPipelineFactory.hpp for why). Not installed.

#include <vsg/app/Viewer.h>
#include <vsg/core/ref_ptr.h>
#include <vsg/nodes/Group.h>
#include <vsg/nodes/Node.h>

#include <cstddef>
#include <vector>

V_VSG_NS_BEGIN

namespace detail
{

/** @brief One record-order dependency: @ref consumer must be recorded after @ref source. */
struct GraphOrderEdge
{
    std::size_t consumer = 0; ///< Node index that must come later.
    std::size_t source   = 0; ///< Node index that must come first.
};

/**
 * @brief Stable topological order of @p node_count nodes under @p edges.
 *
 * The renderer's command graph records each target's render graph (and, with
 * the per-pass model, each PASS' render graph) in a dependency-valid order: a
 * consumer that samples another target's colour, or LOADs its depth, must be
 * recorded after that source. This is Kahn's algorithm seeded in index order,
 * so unrelated nodes keep the caller's input order (the caller seeds the
 * indices in the current record order), which keeps unrelated targets stable
 * frame to frame. A cycle is not a supported pattern (feedback loops are
 * rejected when a slot is attached), so the remaining nodes are appended in
 * index order rather than dropped — a cycle can never make the caller lose a
 * graph.
 *
 * @param node_count Number of nodes to order.
 * @param edges      Dependency edges (consumer after source); out-of-range and
 *                   self edges are ignored.
 * @return Node indices in dependency-valid order (size @p node_count).
 */
std::vector<std::size_t> stableTopologicalOrder(std::size_t node_count,
                                                const std::vector<GraphOrderEdge>& edges);

/**
 * @brief Detaches a child node from a vsg group (command graph / render
 * graph).
 *
 * The renderer retires whole views (PiP / fullscreen-program slots), rebuilt
 * off-screen graphs and released targets by detaching them from the owning
 * graph. vsg groups store plain child lists, so removal is a remove-and-erase
 * sweep; a null graph or a null node is a safe no-op.
 *
 * @param graph Graph whose children are swept (may be null).
 * @param node  Child to detach (may be null).
 */
void removeGraphChild(::vsg::Group* graph, const ::vsg::ref_ptr<::vsg::Node>& node);

/**
 * @brief Waits for all in-flight GPU work on the viewer's device.
 *
 * Every teardown path (releasing a slot / target / rebuilding an off-screen
 * graph) must wait before dropping Vulkan objects that a still-in-flight
 * command buffer may reference. A null viewer is a safe no-op.
 *
 * @param viewer Viewer whose device to wait on (may be null).
 */
void waitForIdle(::vsg::Viewer* viewer);

/**
 * @brief Temporary test escape hatch: when VINE_VSG_OWN_WINDOW is set, the
 * backend creates its own independent vsg window instead of binding to the
 * Qt-hosted surface.
 *
 * Used to verify rendering end-to-end independent of the Qt child-window
 * compositing path (see design notes). Remove once the on-screen path is
 * decided.
 */
bool forceOwnWindow();

} // namespace detail

V_VSG_NS_END
