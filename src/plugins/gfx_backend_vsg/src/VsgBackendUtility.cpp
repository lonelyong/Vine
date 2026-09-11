#include "VsgBackendUtility.hpp"

#include <algorithm>
#include <cstdlib>

V_VSG_NS_BEGIN

namespace detail
{

std::vector<std::size_t> stableTopologicalOrder(std::size_t node_count,
                                                const std::vector<GraphOrderEdge>& edges)
{
    std::vector<std::vector<std::size_t>> consumers(node_count);
    std::vector<std::size_t>              indegree(node_count, 0);
    for (const auto& edge : edges) {
        if (edge.consumer >= node_count || edge.source >= node_count || edge.consumer == edge.source) {
            continue; // defensive: out-of-range / self edge
        }
        consumers[edge.source].push_back(edge.consumer);
        ++indegree[edge.consumer];
    }

    std::vector<std::size_t> order;
    order.reserve(node_count);
    std::vector<std::size_t> ready;
    ready.reserve(node_count);
    for (std::size_t i = 0; i < node_count; ++i) {
        if (indegree[i] == 0) {
            ready.push_back(i); // seeded in index order -> stable
        }
    }
    for (std::size_t head = 0; head < ready.size(); ++head) {
        const std::size_t node = ready[head];
        order.push_back(node);
        for (const std::size_t consumer : consumers[node]) {
            if (--indegree[consumer] == 0) {
                ready.push_back(consumer);
            }
        }
    }
    for (std::size_t i = 0; i < node_count; ++i) {
        if (indegree[i] > 0) {
            order.push_back(i); // cycle remainder (unsupported pattern)
        }
    }
    return order;
}

bool forceOwnWindow()
{
    const char* value = std::getenv("VINE_VSG_OWN_WINDOW");
    return value != nullptr && value[0] != '\0';
}

void removeGraphChild(::vsg::Group* graph, const ::vsg::ref_ptr<::vsg::Node>& node)
{
    if (graph == nullptr) {
        return;
    }
    auto& children = graph->children;
    children.erase(
        std::remove_if(children.begin(),
                       children.end(),
                       [&node](const ::vsg::ref_ptr<::vsg::Node>& child) { return child.get() == node.get(); }),
        children.end());
}

void waitForIdle(::vsg::Viewer* viewer)
{
    if (viewer != nullptr) {
        viewer->deviceWaitIdle();
    }
}

} // namespace detail

V_VSG_NS_END
