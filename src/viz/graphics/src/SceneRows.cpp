#include "SceneRows.hpp"

#include <utility>

VN_GRAPHICS_NS_BEGIN

namespace detail
{

SceneRows::Version SceneRows::version(const Scene& scene)
{
    // Ensuring the rows is what makes the answer CURRENT: the build is keyed by the epoch, and the refresh is what
    // notices a box that moved under it.
    (void)scene.ensureRows();
    // THE PUBLISHED pair, not the scene's own: inside an edit batch these stay where they were, so a frame that lands
    // mid-batch publishes nothing (see Scene::beginEdit / ensureRows).
    return Version{ scene.published_epoch_, scene.published_boxes_ };
}

void SceneRows::publish(const Scene& scene, SceneBake& out)
{
    const SceneBake& rows = scene.ensureRows();
    out.rows              = rows.rows; // a copy: the engine side owns its table and edits it (adopt/place)
    out.seed              = rows.seed;
    out.nodes.clear(); // the node record stays on the content's thread (see the declaration)
}

} // namespace detail

VN_GRAPHICS_NS_END
