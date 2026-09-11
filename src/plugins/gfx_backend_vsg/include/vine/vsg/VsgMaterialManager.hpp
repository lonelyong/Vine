#pragma once
#include "vsg_global.hpp"

#include <cstddef>
#include <functional>
#include <memory>

#include <vsg/core/ref_ptr.h>
#include <vsg/state/material.h>

#include <vine/graphics/MaterialManager.hpp>
#include <vine/raw_ptr.hpp>

namespace vine::graphics
{
class Material;
}

V_VSG_NS_BEGIN

/**
 * @brief VSG material manager: converts vine materials to Phong resources.
 *
 * Implements vine::graphics::MaterialManager by translating a
 * vine::graphics::Material (pure attributes) into a cached
 * vsg::PhongMaterialValue. Multiple drawables sharing the same Material
 * reuse a single Phong resource, avoiding redundant GPU data and pipeline
 * variants.
 */
class V_VSG_API VsgMaterialManager : public vine::graphics::MaterialManager {
  public:
    /** @brief Upper bound on the number of cached material resources.
     *
     * The engine emits no material-lifecycle event, so a host that never calls
     * releaseMaterial() would otherwise grow this cache for the whole session
     * (D13). Past this bound the OLDEST entries are evicted: the newest are the
     * ones a live scene is using, and an evicted material simply rebuilds its
     * Phong value (and re-binds its descriptor) on its next draw.
     */
    static constexpr std::size_t kMaxEntries = 256;

  public:
    VsgMaterialManager();
    ~VsgMaterialManager() override;

  public:
    /** @brief Gets (or creates) the Phong resource for a material.
     *
     * @param material Vine material (may be null → default grey).
     * @return Cached Phong material value.
     */
    ::vsg::ref_ptr<::vsg::PhongMaterialValue> getOrCreate(vine::raw_ptr<vine::graphics::Material> material);

    /** @brief Gets the cached Phong resource for a material without creating one.
     *
     * Non-mutating lookup of the backend resource registered for @p material;
     * unlike getOrCreate() it never builds a new resource.
     *
     * @param material Vine material to look up (by pointer).
     * @return The cached Phong value, or null when the material is not
     *         registered.
     */
    ::vsg::ref_ptr<::vsg::PhongMaterialValue> find(vine::raw_ptr<vine::graphics::Material> material) const;

  public:
    /** @brief Releases the backend resources of every material the app has dropped.
     *
     * An entry OWNS the Material it is keyed by (that is what makes the pointer
     * key safe: the cache cannot observe destruction, so a recycled address
     * would otherwise be served the dead entry's Phong value and descriptor —
     * D13). Once the app itself no longer references a material, nothing can
     * look its entry up again, so this releases it — and the material with it —
     * immediately instead of letting it sit until the session ends.
     *
     * The renderer calls this once per submitted frame. Materials the app still
     * holds are kept (their entries may be reused by a hidden or culled object).
     *
     * @return Number of released entries.
     */
    std::size_t releaseAbandoned();

  public:
    /** @brief Rebuilds the cached resource for a material.
     *
     * This is the single refresh path: it re-reads the material's parameters and
     * writes them into the cached Phong value in place (descriptor sets already
     * point at it) only when they actually changed, so a steady scene transfers
     * nothing and a property edit shows up live. Callers that used to compare
     * and write the value themselves (SceneBridge) now just call this.
     */
    void updateMaterial(vine::raw_ptr<vine::graphics::Material> material) override;

    /** @brief Releases the cached resource for a material. */
    void releaseMaterial(vine::raw_ptr<vine::graphics::Material> material) override;

    /** @brief Releases all cached resources. */
    void clear() override;

    /** @brief Gets the number of materials with a registered Phong resource. */
    std::size_t materialCount() const override;

    /** @brief Whether a material has a registered Phong resource. */
    bool hasMaterial(vine::raw_ptr<vine::graphics::Material> material) const override;

    /** @brief Invokes @p visitor for every registered material. */
    void forEachMaterial(const std::function<void(vine::raw_ptr<vine::graphics::Material>)>& visitor) const override;

  private:
    struct Data;
    // Owns the cache through RAII (see the repo's "avoid raw owning pointers"
    // rule); declared after Data so the out-of-line destructor is the only
    // place that needs the complete type.
    std::unique_ptr<Data> d;
};

V_VSG_NS_END
