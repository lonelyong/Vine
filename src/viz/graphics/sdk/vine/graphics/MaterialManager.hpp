#pragma once
#include "graphics_global.hpp"

#include <cstddef>
#include <functional>

#include <vine/intrusive_ptr.hpp>
#include <vine/Object.hpp>
#include <vine/RefCounted.hpp>
#include <vine/raw_ptr.hpp>

VN_GRAPHICS_NS_BEGIN

class Material;

/**
 * @brief Abstract material manager converting user materials to backend resources.
 *
 * @details
 * - The lifecycle contract for translating a platform-independent vn::graphics::Material (pure attributes)
 *   into backend-specific rendering resources (e.g. a uniform block the shading reads); a concrete backend
 *   implements it and owns its resource cache, so multiple drawables sharing one Material reuse ONE backend
 *   resource.
 * - The interface deliberately exposes no backend types; basic introspection (materialCount / hasMaterial /
 *   forEachMaterial) lets callers query what is registered without seeing them.
 */
class VN_GRAPHICS_API MaterialManager : public Object, public RefCounted<MaterialManager> {
    VN_OBJECT_META_DECL;

  public:
    ~MaterialManager() override = default;

    /** @brief Updates (or creates) the backend resource for a material.
     *
     * @details
     * - Called when the material's attributes change, so the backend can refresh its cached resource.
     *
     * @param material Material whose backend resource must be (re)built.
     */
    virtual void updateMaterial(raw_ptr<Material> material) = 0;

    /** @brief Releases the backend resource for a material.
     *
     * @param material Material whose backend resource is released.
     */
    virtual void releaseMaterial(raw_ptr<Material> material) = 0;

    /** @brief Releases all cached backend resources. */
    virtual void clear() = 0;

    /** @brief Gets the number of materials with a registered backend resource.
     *
     * @details
     * - A material counts as registered from the moment a resource was built for it (updateMaterial() or the
     *   backend's create path) until releaseMaterial() or clear() drops it.
     *
     * @return Number of registered materials.
     */
    virtual std::size_t materialCount() const = 0;

    /** @brief Whether a material has a registered backend resource.
     *
     * @param material Material to look up (by pointer).
     * @return true when the material is registered; false for null or unknown
     *         materials.
     */
    virtual bool hasMaterial(raw_ptr<Material> material) const = 0;

    /** @brief Invokes @p visitor for every registered material.
     *
     * @details
     * - Enumerates the registered material pointers without exposing any backend type; the visit order is the
     *   backend cache's own (unspecified to callers).
     *
     * @warning
     * - The visitor must not call updateMaterial() / releaseMaterial() / clear() while the iteration runs.
     *
     * @param visitor Callback invoked once per registered material.
     */
    virtual void forEachMaterial(const std::function<void(raw_ptr<Material>)>& visitor) const = 0;

  protected:
    MaterialManager() = default;
};

VN_GRAPHICS_NS_END
