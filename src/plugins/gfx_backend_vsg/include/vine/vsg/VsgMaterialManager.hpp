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
    /** @brief Rebuilds the cached resource for a material. */
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
