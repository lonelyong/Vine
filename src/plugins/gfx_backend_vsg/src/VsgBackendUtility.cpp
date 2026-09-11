#include "VsgBackendUtility.hpp"

#include <algorithm>
#include <cstdlib>

V_VSG_NS_BEGIN

namespace detail
{

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
