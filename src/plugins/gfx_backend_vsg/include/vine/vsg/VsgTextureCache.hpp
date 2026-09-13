#pragma once
#include "vsg_global.hpp"

#include <cstddef>
#include <memory>

#include <vsg/core/ref_ptr.h>
#include <vsg/state/ImageInfo.h>

#include <vine/raw_ptr.hpp>

#include <vine/vsg/VsgSceneRules.hpp>

namespace vine::graphics
{
class Texture;
}

V_VSG_NS_BEGIN

/**
 * @brief The vsg resources one vine texture is sampled through, cached per texture.
 *
 * WHY A CACHE AT ALL. Uploading a texture is per-texture work — allocate an image, stage the whole mip
 * chain, build a view and a sampler — while a draw is per-geometry: many drawables share one texture, and
 * a material's texture can change between frames. Building the resources once per texture and handing the
 * same vsg::ImageInfo to every draw is what keeps that work out of the frame loop.
 *
 * WHY THE ENTRY OWNS ITS KEY. The cache is keyed by the texture's ADDRESS and cannot observe its
 * destruction, so an entry that merely referenced it could outlive it — and a new texture allocated at the
 * recycled address would then be served the dead texture's GPU image (wrong pixels, silently). Each entry
 * therefore holds an owning reference (see OwnedCacheEntry, and the same rule in VsgMaterialManager).
 *
 * WHY IT DOES NOT REPORT. The cache answers "can this be uploaded, and if not why" as a TextureReject and
 * says nothing about it: the caller owns the diagnostic stream and the per-episode reporting discipline
 * (see SceneBridge::report). That also keeps the decision testable without a device — it is
 * classifyTexture() in VsgSceneRules.
 *
 * The cache is NOT thread-safe: it is used from the frame's own thread, like the rest of the bridge.
 */
class V_VSG_API VsgTextureCache
{
  public:
    /** @brief Upper bound on cached textures.
     *
     * Mirrors VsgMaterialManager's bound and exists for the same reason: nothing tells the backend that a
     * texture is gone, so a session that keeps creating them would otherwise grow this cache forever. Past
     * the bound the OLDEST entries are evicted, because the newest are the ones a live scene is using; an
     * evicted texture simply rebuilds (and re-uploads) on its next draw.
     */
    static constexpr std::size_t kMaxEntries = 256;

  public:
    VsgTextureCache();
    ~VsgTextureCache();

    VsgTextureCache(const VsgTextureCache&) = delete;
    VsgTextureCache& operator=(const VsgTextureCache&) = delete;

  public:
    /**
     * @brief Gets (or creates) the resources a texture is sampled through.
     *
     * A texture that cannot be uploaded yields the white fallback rather than a null resource: a
     * descriptor the shader samples must always be bound to SOMETHING, and a drawable that renders
     * untextured is better than one that cannot render at all. The caller learns what happened from
     * @p reason and reports it.
     *
     * @param texture Texture to sample (null yields the white fallback with TextureReject::Absent).
     * @param reason  Receives why the texture was not uploaded, or TextureReject::Ok.
     * @return The resources to sample, never null.
     */
    ::vsg::ref_ptr<::vsg::ImageInfo> getOrCreate(vine::raw_ptr<const vine::graphics::Texture> texture,
                                                detail::TextureReject& reason);

    /**
     * @brief Gets the 1x1 opaque white texture.
     *
     * The single texture every untextured draw samples, so the shader has ONE path — it always multiplies
     * by a texture — instead of a branch that would have to be kept in step with the sampler binding.
     *
     * @return The white texture's resources, never null.
     */
    ::vsg::ref_ptr<::vsg::ImageInfo> whiteFallback();

    /**
     * @brief Gets the 1x1 opaque white CUBE texture.
     *
     * The cube slot's counterpart of whiteFallback(): one white texel per face, in a cube view. A cube
     * sampler cannot reuse the 2-D fallback — binding a 2-D view where the shader declares samplerCube is
     * an invalid descriptor, not an untextured draw — so a cube drawable with no cube texture samples
     * this instead, and the shader keeps ONE path per kind.
     *
     * @return The white cube's resources, never null.
     */
    ::vsg::ref_ptr<::vsg::ImageInfo> whiteCubeFallback();

    /**
     * @brief Releases the resources of every texture the app has dropped.
     *
     * An entry is abandoned when the cache is the only owner left (the app released the texture): nothing
     * can look it up again, so it is released now rather than pinned until the session ends.
     *
     * @return Number of released entries.
     */
    std::size_t releaseAbandoned();

    /** @brief Releases every cached resource, including the white fallback. */
    void clear();

    /** @brief Gets the number of textures with cached resources. */
    std::size_t count() const;

    /**
     * @brief States whether a texture has cached resources.
     *
     * @param texture Texture to look up (by address).
     * @return true when the cache holds an entry for it.
     */
    bool has(vine::raw_ptr<const vine::graphics::Texture> texture) const;

    /**
     * @brief States the anisotropy a device's samplers may be created with.
     *
     * A device limit (and a device feature) rather than something this cache can derive, so the caller that
     * knows the device announces it. Values below 1 are answered with 1 by the sampler's own clamp; the
     * default of 1 keeps a cache that was never told usable rather than illegal.
     *
     * @param device_limit The device's reported maxSamplerAnisotropy.
     */
    void setMaxAnisotropy(float device_limit) noexcept;

  private:
    struct Data;
    // Owns the cache through RAII (see the repo's "avoid raw owning pointers" rule); declared after Data
    // so the out-of-line destructor is the only place that needs the complete type.
    std::unique_ptr<Data> d;
};

V_VSG_NS_END
