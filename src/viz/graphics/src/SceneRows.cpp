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
    return Version{ scene.published_epoch_, scene.published_boxes_, scene.published_materials_ };
}

SceneSnapshotPtr SceneRows::snapshot(const Scene& scene)
{
    const Version version_now = version(scene); // this is the call that brings the rows up to date
    SceneBake* rows = scene.content_bake_.get();
    if (rows == nullptr) {
        return makeSceneSnapshot(SceneBake{}, version_now.epoch, version_now.boxes, version_now.materials, nullptr);
    }
    const SceneSnapshotPtr& cached = rows->snapshot_cache;
    if (cached != nullptr && cached->epoch == version_now.epoch && cached->boxes == version_now.boxes &&
        cached->materials == version_now.materials) {
        return cached; // nothing moved: the previous publication is still the answer
    }

    SceneSnapshotPtr snapshot = makeSceneSnapshot(*rows,
                                                  version_now.epoch,
                                                  version_now.boxes,
                                                  version_now.materials,
                                                  cached);
    rows->snapshot_cache      = snapshot;
    return snapshot;
}

void SceneRows::publish(const Scene& scene, SceneBake& out)
{
    const SceneBake& rows = scene.ensureRows();
    out.rows              = rows.rows; // a copy: the engine side owns its table and edits it (adopt/place)
    out.seed              = rows.seed;
    out.nodes.clear();    // the node record stays on the content's thread (see the declaration)
    out.node_epochs.clear();
    out.snapshot_cache.reset();

    // THE MATERIAL VALUES ARE RE-TAKEN HERE, and this is the boundary that makes them safe to read: a material is
    // edited through a live object (`Material::setXxx`), so no row edit announces it - the publish is where the
    // content side asks each material it draws with for its current values (`MaterialBase::description`, cached by
    // revision) and hands them to the table the render side reads. That is what makes a material edit land on the
    // next frame, and a material edited INSIDE an edit batch land with the whole batch (C18): a batch publishes
    // nothing, so the values it moved are taken at the publish that closes it.
    for (Row& row : out.rows) {
        if (row.material != nullptr) {
            row.material_description = row.material->description();
        }
        // A row that OWNS its material carries the same value: the fold copies it into the resolved facts, so
        // leaving it stale would put the old values back at the next re-fold (a mirror's edit).
        if (row.own_material != nullptr) {
            row.own_material_description = row.own_material->description();
        }
        for (DrawFacts& draw : row.draws) {
            if (draw.material != nullptr) {
                draw.material_description = draw.material->description();
            }
        }
    }
}

} // namespace detail

VN_GRAPHICS_NS_END
