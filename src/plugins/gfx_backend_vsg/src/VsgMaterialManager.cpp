#include <vine/vsg/VsgMaterialManager.hpp>

#include <vsg/state/material.h>

#include <unordered_map>
#include <utility>

#include <vine/graphics/Material.hpp>

#include <vine/vsg/OwnedCache.hpp>

V_VSG_NS_BEGIN

namespace
{

/**
 * @brief The Phong parameters a Vine material maps to.
 *
 * Transparency is carried by the per-vertex alpha (Geometry / Node opacity),
 * never by the shared material, so the diffuse alpha is pinned opaque here.
 * Holding the mapping as a value (instead of writing straight into the vsg
 * struct) lets one definition serve all three uses: building a value, refreshing
 * it in place, and detecting that nothing changed.
 */
struct PhongParameters
{
    ::vsg::vec4 ambient{ 0.2f, 0.2f, 0.2f, 1.0f };
    ::vsg::vec4 diffuse{ 0.8f, 0.8f, 0.8f, 1.0f };
    ::vsg::vec4 specular{ 0.2f, 0.2f, 0.2f, 1.0f };
    float       shininess = 32.0f;

    /// Field-wise equality (the scale at which a change matters).
    bool operator==(const PhongParameters& other) const noexcept
    {
        return ambient.x == other.ambient.x && ambient.y == other.ambient.y &&
               ambient.z == other.ambient.z && ambient.w == other.ambient.w &&
               diffuse.x == other.diffuse.x && diffuse.y == other.diffuse.y &&
               diffuse.z == other.diffuse.z && diffuse.w == other.diffuse.w &&
               specular.x == other.specular.x && specular.y == other.specular.y &&
               specular.z == other.specular.z && specular.w == other.specular.w &&
               shininess == other.shininess;
    }

    bool operator!=(const PhongParameters& other) const noexcept { return !(*this == other); }
};

/**
 * @brief Maps a Vine material onto Phong parameters.
 *
 * @param material Vine material (may be null -> the default grey).
 * @return The parameters to write into the cached value.
 */
PhongParameters parametersOf(vine::graphics::Material* material)
{
    PhongParameters parameters;
    if (material == nullptr) {
        return parameters;
    }
    const auto diffuse = material->diffuse();
    parameters.diffuse = ::vsg::vec4(diffuse.r, diffuse.g, diffuse.b, 1.0f);
    const auto specular = material->specular();
    parameters.specular = ::vsg::vec4(specular.r, specular.g, specular.b, specular.a);
    const auto ambient = material->ambient();
    parameters.ambient = ::vsg::vec4(ambient.r, ambient.g, ambient.b, ambient.a);
    parameters.shininess = material->shininess();
    return parameters;
}

/**
 * @brief Writes parameters into a cached Phong value.
 *
 * @param phong      Value to write into.
 * @param parameters Parameters to apply.
 */
void applyPhongParameters(::vsg::PhongMaterialValue& phong, const PhongParameters& parameters)
{
    auto& m     = phong.value();
    m.ambient   = parameters.ambient;
    m.diffuse   = parameters.diffuse;
    m.specular  = parameters.specular;
    m.shininess = parameters.shininess;
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
    applyPhongParameters(*phong, parametersOf(material));
    return phong;
}

}  // namespace

struct VsgMaterialManager::Data {
    /// One cached material resource.
    struct Entry
    {
        ::vsg::ref_ptr<::vsg::PhongMaterialValue> value;
        /// Parameters currently in @ref value, so a refresh that changes nothing
        /// writes (and dirties) nothing.
        PhongParameters parameters;
    };
    /// Keyed by the Material pointer, and each entry OWNS that Material: the
    /// pointer key is only valid while the object is alive, and the cache cannot
    /// observe destruction (D13). See OwnedCache.hpp.
    std::unordered_map<vine::graphics::Material*,
                       OwnedCacheEntry<vine::graphics::Material, Entry>>
        cache;
    /// FIFO order for capacity eviction.
    InsertionClock clock;
};

VsgMaterialManager::VsgMaterialManager()
  : d(std::make_unique<Data>())
{
}

VsgMaterialManager::~VsgMaterialManager() = default;

::vsg::ref_ptr<::vsg::PhongMaterialValue> VsgMaterialManager::getOrCreate(
    vine::raw_ptr<vine::graphics::Material> material)
{
    // The lookup key is the Material as given (a null Material is the default
    // grey entry), while the ENTRY holds an owning reference to it.
    const auto it = d->cache.find(material);
    if (it != d->cache.end()) {
        return it->second.payload().value;
    }
    Data::Entry entry;
    entry.value      = makePhongMaterial(material);
    entry.parameters = parametersOf(material);
    vine::intrusive_ptr<const vine::graphics::Material> owner(material);
    d->cache.emplace(material, OwnedCacheEntry<vine::graphics::Material, Data::Entry>(
                                   std::move(owner), std::move(entry), d->clock.tick()));
    trimToCapacity(d->cache, kMaxEntries);
    return d->cache.find(material) != d->cache.end()
               ? d->cache.find(material)->second.payload().value
               : ::vsg::ref_ptr<::vsg::PhongMaterialValue>();
}

std::size_t VsgMaterialManager::releaseAbandoned()
{
    return eraseAbandoned(d->cache);
}

void VsgMaterialManager::updateMaterial(vine::raw_ptr<vine::graphics::Material> material)
{
    if (material == nullptr) {
        return; // the default material never changes
    }
    auto it = d->cache.find(material);
    if (it == d->cache.end()) {
        getOrCreate(material);
        return;
    }
    const PhongParameters wanted = parametersOf(material);
    if (it->second.payload().parameters == wanted) {
        // Steady state: nothing to write, nothing to transfer. This is the
        // check callers used to duplicate (see the design doc, D19).
        return;
    }
    // Refresh the SAME cached object in place: descriptor sets already point at
    // this Phong value, so replacing it would orphan the live bindings. The
    // value is DYNAMIC (see makePhongMaterial); mark it dirty so the per-frame
    // TransferTask re-copies the update.
    applyPhongParameters(*it->second.payload().value, wanted);
    it->second.payload().value->dirty();
    it->second.payload().parameters = wanted;
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
    return (it != d->cache.end()) ? it->second.payload().value
                                  : ::vsg::ref_ptr<::vsg::PhongMaterialValue>();
}

V_VSG_NS_END
