#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>

#include <vsg/core/ref_ptr.h>

#include <vine/intrusive_ptr.hpp>
#include <vine/raw_ptr.hpp>

#include <vine/vsg/VsgSceneRules.hpp>
#include <vine/vsg/api/ContentImages.hpp>
#include <vine/vsg/vsg_global.hpp>

namespace vine::graphics
{
class Texture;
}

V_VSG_NS_BEGIN

/**
 * @brief The images one engine texture is SAMPLED through: the GPU side of a mat
erial's map.
 *
 * WHY A CACHE. Uploading a texture is per-texture work - size a staged chain, bu
ild the image, the view and
 * the sampler - while a draw is per-geometry: many drawables share one material,
 and a material's map can
 * change between frames. Building the image once per texture and handing the sam
e view and sampler to every
 * declared set that binds it is what keeps that work out of the frame loop.
 *
 * WHAT IT DOES NOT DO. It uploads nothing itself. A texture's pixels reach the d
evice through the image the
 * cache describes: the image carries the staged bytes as its `vsg::Data`, and th
e viewer's transfer step
 * copies them into the image before the frame that samples it records - the mech
anism the backend has always
 * relied on, and the reason a re-filled texture is re-uploaded (see the revision
 note below) rather than
 * patched in place.
 *
 * WHY THE ENTRY OWNS ITS KEY. The cache is keyed by the texture's ADDRESS and ca
nnot observe its
 * destruction, so an entry that merely referenced it could outlive it - and a ne
w texture allocated at the
 * recycled address would then be served the dead texture's image (wrong pixels,
silently). Each entry holds
 * an owning reference, and @ref releaseAbandoned drops the entries whose only re
maining owner is the cache
 * itself.
 *
 * WHY THE REVISION IS PART OF THE KEY. `Texture::revision()` is bumped by every
 fill of a face, and a
 * texture that is re-filled keeps its address: without the revision the entry wo
uld keep serving the image
 * built from the old pixels - the same failure `ShaderProgram::revision` exists
to prevent.
 *
 * WHY IT DOES NOT REPORT. The cache answers "what can this texture be sampled th
rough, and if nothing, why"
 * as a `detail::TextureReject` and says nothing about it: the caller owns the di
agnostic stream and the
 * per-episode reporting discipline (see api/ContentImages for the same rule). Th
at also keeps the decision
 * testable without a device.
 *
 * NO DEVICE IS NEEDED to build it: the image, the view and the sampler are crea
te-infos until a
 * `vsg::Context` compiles them (the same reason api/WhiteImage needs none). The
 device's anisotropy limit
 * is the one device fact that reaches the samplers, and it is announced rather t
han queried (see
 * @ref setMaxAnisotropy).
 *
 * NOT thread-safe: it is used from the frame's own thread, like the rest of the
backend.
 */
class MaterialImages
{
  public:
    /** @brief Upper bound on cached textures.
     *
     * Nothing tells the backend that a texture is gone, so a session that keeps
 creating them would
     * otherwise grow this cache for ever. Past the bound the OLDEST entries are
 evicted, because the newest
     * are the ones a live scene is using; an evicted texture simply rebuilds (an
d re-uploads) on its next
     * acquire.
     */
    static constexpr std::size_t kMaxEntries = 256;

  public:
    /** @brief Builds the cache, including the two white fallbacks (no device; s
ee the class note).
     *
     * @return The cache, or null when its objects could not be created.
     */
    [[nodiscard]] static std::shared_ptr<MaterialImages> create();

    /**
     * @brief Gets (or builds) the images @p texture is sampled through.
     *
     * A texture that cannot be uploaded yields the WHITE fallback rather than a
 null image: a descriptor
     * a shader samples must always be bound to something, and a drawable that re
nders untextured is better
     * than one that cannot render at all. The caller learns what happened from @
p reason and reports it.
     *
     * @param texture Texture to sample (null yields the white fallback with `Te
xtureReject::Absent`).
     * @param reason  Receives why the texture was not uploaded, or `TextureReje
ct::Ok`.
     * @return The images to bind, never null (its members are never null either
).
     */
    [[nodiscard]] SamplerImage acquire(vine::raw_ptr<const vine::graphics::Texture> texture,
                                       detail::TextureReject& reason);

    /** @brief Gets the 1x1 white image a 2D map falls back to (never null; see a
pi/WhiteImage). */
    [[nodiscard]] SamplerImage white() const noexcept;

    /**
     * @brief Gets the 1x1 white CUBE image a cube map falls back to (never null
).
     *
     * A cube sampler cannot bind the 2D fallback: a 2D view where the text decl
ares `samplerCube` is an
     * invalid descriptor, not an untextured draw, so the cube slot has a fallba
ck of its own.
     */
    [[nodiscard]] SamplerImage whiteCube() const noexcept;

    /** @brief Gets the number of textures with cached images (the fallbacks exc
luded). */
    [[nodiscard]] std::size_t count() const noexcept;

    /** @brief States whether @p texture has cached images (by address). */
    [[nodiscard]] bool has(vine::raw_ptr<const vine::graphics::Texture> texture) const noexcept;

    /**
     * @brief Releases the images of every texture the app has dropped.
     *
     * An entry is abandoned when the cache is the only owner left: nothing can
look it up again, so its
     * image and view are released now rather than pinned until the session ends
.
     *
     * @return Number of released entries.
     */
    std::size_t releaseAbandoned();

    /** @brief Releases every cached image, including the two fallbacks. */
    void clear();

    /**
     * @brief States the anisotropy a device's samplers may be created with.
     *
     * A device limit and a device feature rather than something this cache can
derive, so the caller that
     * knows the device announces it. Values below 1 are answered with 1, and th
e default of 1 keeps a cache
     * that was never told usable rather than illegal.
     *
     * @param device_limit The device's reported maxSamplerAnisotropy.
     */
    void setMaxAnisotropy(float device_limit) noexcept;

    ~MaterialImages();

    MaterialImages(const MaterialImages&) = delete;
    MaterialImages& operator=(const MaterialImages&) = delete;

  private:
    struct Data;
    // Lexically after Data so the out-of-line destructor is the only place that needs the complete
    // type.
    std::unique_ptr<Data> d;

    MaterialImages();
};

V_VSG_NS_END
