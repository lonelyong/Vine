#include <vine/vsg/VsgMaterialManager.hpp>

#include <vsg/state/material.h>

#include <vine/graphics/Material.hpp>

V_VSG_NS_BEGIN

namespace
{

/**
 * @brief Builds a PhongMaterialValue from a Vine material.
 *
 * Maps the Vine material's diffuse/specular/ambient/shininess/opacity onto
 * VSG's PhongMaterial struct. A default grey Phong material is produced when
 * no Vine material is bound.
 *
 * @param material Vine material (may be null).
 * @return VSG Phong material value.
 */
/**
 * @brief Writes a Vine material's parameters into a Phong value.
 *
 * Shared by creation and by in-place refresh so both paths produce identical
 * state. Transparency is carried by the per-vertex alpha (Geometry / Node
 * opacity), never by the shared material: the diffuse alpha stays opaque.
 *
 * @param phong    Value to write into (must already be DYNAMIC when reused).
 * @param material Vine material (may be null -> default grey).
 */
void applyPhongMaterial(::vsg::PhongMaterialValue& phong, vine::graphics::Material* material)
{
    auto& m = phong.value();
    m.ambient = ::vsg::vec4(0.2f, 0.2f, 0.2f, 1.0f);
    m.diffuse = ::vsg::vec4(0.8f, 0.8f, 0.8f, 1.0f);
    m.specular = ::vsg::vec4(0.2f, 0.2f, 0.2f, 1.0f);
    m.shininess = 32.0f;
    if (material != nullptr) {
        const auto diffuse = material->diffuse();
        m.diffuse = ::vsg::vec4(diffuse.r, diffuse.g, diffuse.b, diffuse.a);
        const auto specular = material->specular();
        m.specular = ::vsg::vec4(specular.r, specular.g, specular.b, specular.a);
        const auto ambient = material->ambient();
        m.ambient = ::vsg::vec4(ambient.r, ambient.g, ambient.b, ambient.a);
        m.shininess = material->shininess();
        m.diffuse.a = 1.0f;
    }
}

/**
 * @brief Builds a PhongMaterialValue from a Vine material.
 *
 * The uniform backing the value is updated IN PLACE at run time, so it is
 * marked DYNAMIC: vsg keeps it in a transfer buffer and the per-frame
 * TransferTask re-copies it after dirty() — a static uniform would only be
 * uploaded once at compile and later property edits would never reach the GPU
 * (the same pitfall fixed for the per-vertex opacity carrier).
 *
 * @param material Vine material (may be null).
 * @return VSG Phong material value.
 */
::vsg::ref_ptr<::vsg::PhongMaterialValue> makePhongMaterial(vine::graphics::Material* material)
{
    auto phong = ::vsg::PhongMaterialValue::create();
    phong->properties.dataVariance = ::vsg::DYNAMIC_DATA;
    applyPhongMaterial(*phong, material);
    return phong;
}

}  // namespace

struct VsgMaterialManager::Data {
    std::map<vine::graphics::Material*, ::vsg::ref_ptr<::vsg::PhongMaterialValue>> cache;
};

VsgMaterialManager::VsgMaterialManager()
  : d(std::make_unique<Data>())
{
}

VsgMaterialManager::~VsgMaterialManager() = default;

::vsg::ref_ptr<::vsg::PhongMaterialValue> VsgMaterialManager::getOrCreate(
    vine::raw_ptr<vine::graphics::Material> material)
{
    auto it = d->cache.find(material);
    if (it != d->cache.end()) {
        return it->second;
    }
    auto phong = makePhongMaterial(material);
    d->cache[material] = phong;
    return phong;
}

void VsgMaterialManager::updateMaterial(vine::raw_ptr<vine::graphics::Material> material)
{
    if (material == nullptr) {
        return;
    }
    auto it = d->cache.find(material);
    if (it != d->cache.end()) {
        // Refresh the SAME cached object in place: descriptor sets already
        // point at this Phong value, so replacing it would orphan the live
        // bindings. The value is DYNAMIC (see makePhongMaterial); mark it
        // dirty so the per-frame TransferTask re-copies the update.
        applyPhongMaterial(*it->second, material);
        it->second->dirty();
    }
    else {
        d->cache[material] = makePhongMaterial(material);
    }
}

void VsgMaterialManager::releaseMaterial(vine::raw_ptr<vine::graphics::Material> material)
{
    d->cache.erase(material);
}

void VsgMaterialManager::clear()
{
    d->cache.clear();
}

std::size_t VsgMaterialManager::materialCount() const
{
    return d->cache.size();
}

bool VsgMaterialManager::hasMaterial(vine::raw_ptr<vine::graphics::Material> material) const
{
    return d->cache.find(material) != d->cache.end();
}

void VsgMaterialManager::forEachMaterial(
    const std::function<void(vine::raw_ptr<vine::graphics::Material>)>& visitor) const
{
    for (const auto& entry : d->cache) {
        visitor(entry.first);
    }
}

::vsg::ref_ptr<::vsg::PhongMaterialValue> VsgMaterialManager::find(
    vine::raw_ptr<vine::graphics::Material> material) const
{
    const auto it = d->cache.find(material);
    return (it != d->cache.end()) ? it->second
                                  : ::vsg::ref_ptr<::vsg::PhongMaterialValue>();
}

V_VSG_NS_END
