#pragma once
#include "vsg_global.hpp"

#include <cstddef>
#include <functional>
#include <memory>

#include <vsg/core/ref_ptr.h>
#include <vsg/state/material.h>

#include <vine/graphics/MaterialManager.hpp>

#include <vine/vsg/OwnedCache.hpp>
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
     * A material is ALSO held by the variant template of every slot that draws
     * it, so this only counts this cache's own shares: "the app let go" needs the
     * session's counts (see releaseAbandoned(const OwnedShareCounts&) and P11).
     *
     * The renderer calls this once per submitted frame.
     *
     * @return Number of released entries.
     */
    std::size_t releaseAbandoned();

    /** @brief Releases every entry whose material the session's counts say the app has dropped.
     *
     * The exact form of the rule above: a material is released when the only
     * references left to it are the retained entries that hold it, counted across
     * every cache that sweeps with this one (OwnedShareCounts). Without that count
     * the two caches that hold one material wait for each other for ever: each sees
     * the other's share and judges by "nothing but me references it" (P11).
     *
     * @param shares Retained shares counted for the materials this sweep judges.
     * @return Number of released entries.
     */
    std::size_t releaseAbandoned(const OwnedShareCounts& shares);

    /** @brief Counts one retained share per material this manager holds.
     *
     * Feeds the session's OwnedShareCounts (see releaseAbandoned).
     *
     * @param shares Counts to add to.
     */
    void collectOwnedShares(OwnedShareCounts& shares) const;

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
