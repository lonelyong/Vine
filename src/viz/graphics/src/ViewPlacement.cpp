#include <vine/graphics/ViewPlacement.hpp>

VN_GRAPHICS_NS_BEGIN

ViewPlacement::~ViewPlacement() = default;

ViewDependence localToWorldForView(const ViewPlacement& placement, const Mat4d& parent_world, const CollectView& view, Mat4d& world)
{
    const ViewDependence dependence = placement.dependence(view);
    if (dependence == ViewDependence::None) {
        return ViewDependence::None; // this view needs nothing beyond the node's own local matrix
    }
    // The placement answers the node's WHOLE local matrix, so it composes with the PARENT - not with a world matrix
    // that already contains this node's own view-independent placement.
    world = parent_world * placement.localMatrix(parent_world, view);
    return dependence;
}

VN_GRAPHICS_NS_END
