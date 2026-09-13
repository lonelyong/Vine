#include <vine/vsg/VsgMaterialManager.hpp>

#include <cstring>
#include <unordered_map>
#include <utility>

#include <vine/graphics/Material.hpp>

#include <vine/vsg/OwnedCache.hpp>

V_VSG_NS_BEGIN

namespace
{

/**
 * @brief Maps a Vine material onto the engine's material block (ShaderAbi.hpp).
 *
 * The one definition of the mapping: an entry builds its block with it, a refresh compares the result
 * with what the GPU already has, and a change writes it again — so "did anything change" and "what do
 * we send" can never disagree.
 *
 * Transparency is NOT here: opacity is a per-drawable value (VineDrawBlock::params.x), because one
 * material is shared by every drawable that uses it.
 *
 * @param material Vine material (may be null -> the default grey).
 * @return The block to hand to the device.
 */
vine::graphics::VineMaterialBlock materialBlockOf(vine::graphics::Material* material)
{
    vine::graphics::VineMaterialBlock block;   // its defaults ARE the default material
    if (material == nullptr) {
        return block;
    }
    const auto diffuse = material->diffuse();
    block.diffuse      = { diffuse.r, diffuse.g, diffuse.b, diffuse.a };
    const auto specular = material->specular();
    block.specular      = { specular.r, specular.g, specular.b, specular.a };
    const auto ambient = material->ambient();
    block.ambient      = { ambient.r, ambient.g, ambient.b, ambient.a };
    block.shininess    = material->shininess();
    return block;
}

/** @brief Field-wise equality: the scale at which a material change matters. */
bool sameBlock(const vine::graphics::VineMaterialBlock& a, const vine::graphics::VineMaterialBlock& b) noexcept
{
    return a.ambient == b.ambient && a.diffuse == b.diffuse && a.specular == b.specular &&
           a.emissive == b.emissive && a.shininess == b.shininess && a.alpha_mask == b.alpha_mask &&
           a.alpha_mask_cutoff == b.alpha_mask_cutoff;
}

/**
 * @brief Writes a block into the uniform bytes the descriptor points at.
 *
 * @param data  Uniform bytes to write into.
 * @param block Block to copy in.
 */
void writeBlock(::vsg::ubyteArray& data, const vine::graphics::VineMaterialBlock& block)
{
    std::memcpy(data.data(), &block, sizeof(block));
    data.dirty();
}

/**
 * @brief Builds the uniform bytes backing one material.
 *
 * The buffer is updated IN PLACE at run time, so it is marked DYNAMIC: vsg keeps it in a transfer
 * buffer and the per-frame TransferTask re-copies it after dirty() — a static uniform would only be
 * uploaded once and later property edits would never reach the GPU (the same pitfall fixed for the
 * per-vertex opacity carrier).
 *
 * @param material Vine material (may be null).
 * @return The uniform data to bind.
 */
::vsg::ref_ptr<::vsg::ubyteArray> makeMaterialData(vine::graphics::Material* material)
{
    auto data = ::vsg::ubyteArray::create(static_cast<uint32_t>(sizeof(vine::graphics::VineMaterialBlock)));
    data->properties.dataVariance = ::vsg::DYNAMIC_DATA;
    writeBlock(*data, materialBlockOf(material));
    return data;
}

}  // namespace

struct VsgMaterialManager::Data {
    /// One cached material resource.
    struct Entry
    {
        ::vsg::ref_ptr<::vsg::ubyteArray> data;
        /// The block currently in @ref data, so a refresh that changes nothing
        /// writes (and dirties) nothing.
        vine::graphics::VineMaterialBlock block;
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

::vsg::ref_ptr<::vsg::ubyteArray> VsgMaterialManager::getOrCreate(
    vine::raw_ptr<vine::graphics::Material> material)
{
    // The lookup key is the Material as given (a null Material is the default
    // grey entry), while the ENTRY holds an owning reference to it.
    const auto it = d->cache.find(material);
    if (it != d->cache.end()) {
        return it->second.payload().data;
    }
    Data::Entry entry;
    entry.data  = makeMaterialData(material);
    entry.block = materialBlockOf(material);
    vine::intrusive_ptr<const vine::graphics::Material> owner(material);
    d->cache.emplace(material, OwnedCacheEntry<vine::graphics::Material, Data::Entry>(
                                   std::move(owner), std::move(entry), d->clock.tick()));
    trimToCapacity(d->cache, kMaxEntries);
    return d->cache.find(material) != d->cache.end()
               ? d->cache.find(material)->second.payload().data
               : ::vsg::ref_ptr<::vsg::ubyteArray>();
}

std::size_t VsgMaterialManager::releaseAbandoned()
{
    // This cache's own shares only: with several caches holding one material, only the session's
    // counts can tell "the app let go" from "another cache still holds it" (see the declaration
    // and P11). Called without them, this is the conservative answer — an entry held by a variant
    // template is kept, which is what a caller driving one manager on its own expects.
    OwnedShareCounts local;
    collectOwnedShares(local);
    return eraseAbandoned(d->cache, local);
}

std::size_t VsgMaterialManager::releaseAbandoned(const OwnedShareCounts& shares)
{
    return eraseAbandoned(d->cache, shares);
}

void VsgMaterialManager::collectOwnedShares(OwnedShareCounts& shares) const
{
    for (const auto& entry : d->cache) {
        shares.add(entry.first);
    }
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
    const vine::graphics::VineMaterialBlock wanted = materialBlockOf(material);
    if (sameBlock(it->second.payload().block, wanted)) {
        // Steady state: nothing to write, nothing to transfer. This is the
        // check callers used to duplicate (see the design doc, D19).
        return;
    }
    // Refresh the SAME cached buffer in place: descriptor sets already point at
    // it, so replacing it would orphan the live bindings. The buffer is DYNAMIC
    // (see makeMaterialData); writeBlock marks it dirty so the per-frame
    // TransferTask re-copies the update.
    writeBlock(*it->second.payload().data, wanted);
    it->second.payload().block = wanted;
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

::vsg::ref_ptr<::vsg::ubyteArray> VsgMaterialManager::find(
    vine::raw_ptr<vine::graphics::Material> material) const
{
    const auto it = d->cache.find(material);
    return (it != d->cache.end()) ? it->second.payload().data : ::vsg::ref_ptr<::vsg::ubyteArray>();
}

V_VSG_NS_END
