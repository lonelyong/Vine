#include "SceneRows.hpp"

#include "ScenePublisher.hpp"

VN_GRAPHICS_NS_BEGIN

namespace detail
{

SceneSnapshotPtr SceneRows::snapshot(const Scene& scene)
{
    // THE BUILDER LIVES WITH THE ROWS (see detail::ScenePublisher): the derived side of the scene owns the baked
    // table, the published version pair and the snapshot cache, so the build is one method there - and this class
    // stays the module's one door onto it, so the public SDK names no implementation type.
    return scene.publisher_ != nullptr ? scene.publisher_->snapshot() : nullptr;
}

} // namespace detail

VN_GRAPHICS_NS_END
