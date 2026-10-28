#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>

#include <vsg/core/ref_ptr.h>

#include <vine/graphics/Sampler.hpp>
#include <vine/intrusive_ptr.hpp>
#include <vine/raw_ptr.hpp>

#include <vine/vsg/internal/ContentImages.hpp>
#include <vine/vsg/support/VsgSceneRules.hpp>
#include <vine/vsg/vsg_global.hpp>

namespace vn::graphics
{

class Texture;

}

VN_VSG_NS_BEGIN

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
 * WHY THE REVISION IS PART OF THE KEY. `Texture::revision()` is bumped by every fill of a face, and a
 * texture that is re-filled keeps its address: without the revision the entry would keep serving the image
 * built from the old pixels - the same failure `ShaderProgram::revision` exists to prevent.
 *
 * WHAT IS NOT PART OF THE UPLOAD KEY: THE SAMPLER. Vulkan keeps the sampler out of the image, and so does
 * this cache: an entry is keyed by `(texture, revision)` and holds the view every state samples, while
 * explicit `Sampler` states are built and cached BY VALUE next to it. Two passes that read one texture with
 * different filter/wrap settings therefore share one upload and bind two samplers -
 * `acquire(texture, state, why)` is that path, and the state-less `acquire` returns the backend's own
 * default.
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
class MaterialImages {
  public:
    /** @brief Why there is NO entry bound: the host owns the memory policy.
     *
     * WHAT LIVES HERE. An entry lives exactly as long as the host holds its texture: the cache never drops
     * a texture it can still be asked to sample, because "which live content deserves GPU memory" is the
     * host's decision rather than this cache's. That matters because a bound made the cache rebuild and
     * re-upload a whole working set every time it was sampled again - a cost the picture never shows, and one
     * the cache could only trade against another arbitrary number.
     *
     * WHAT SHRINKS IT. releaseAbandoned() drops the entries whose texture the host let go (nobody can ask for
     * them again), and releaseTexture() drops one the host names explicitly. Those are the only two answers to
     * "should this leave?", and neither is a capacity rule.
     *
     * WHAT THE HOST CAN READ. count() and bytes() describe the residency so a host that wants to spend less
     * memory can decide what to release; the cache itself does not warn or evict on their values.
     */

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
    [[nodiscard]] SamplerImage acquire(vn::raw_ptr<const vn::graphics::Texture> texture, detail::TextureReject& reason);
    /**
     * @brief Gets (or builds) the images @p texture is sampled through with an EXPLICIT state.
     *
     * @details
     * - The uploaded image and its view are shared with every other state the texture is read with: only the
     *   sampler object differs, which is exactly Vulkan's separation of `VkSampler` from `VkImage`.
     * - The state is cached by value, so two passes that state the same fields share one sampler.
     * - A texture that cannot be uploaded yields the white fallback as usual, read with @p sampler.
     *
     * @param texture Texture to sample (null yields the white fallback with `TextureReject::Absent`).
     * @param sampler How the texture is read (see Sampler).
     * @param reason  Receives why the texture was not uploaded, or `TextureReject::Ok`.
     * @return The image and the sampler for @p sampler, never null.
     */
    [[nodiscard]] SamplerImage acquire(vn::raw_ptr<const vn::graphics::Texture> texture, const vn::graphics::Sampler& sampler, detail::TextureReject& reason);

    /** @brief Gets the number of EXPLICIT sampler states cached (the per-texture default samplers excluded). */
    [[nodiscard]] std::size_t samplerCount() const noexcept;
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

    /**
     * @brief Gets the staged bytes the cached images carry (the mip chains, no allocator overhead).
     *
     * @details
     * - WHAT IT IS. The sum of the bytes each entry stages for the transfer step - the same number
     *   `makeImage` sizes its storage with (every layer of every level). It is a RESIDENCY FACT, not a budget:
     *   nothing in this cache evicts, warns or grows on it.
     * - WHAT IT DOES NOT INCLUDE. The two white fallbacks, driver alignment and the sampler objects. A host that
     *   wants to decide what to release reads this beside count() and its own texture sizes.
     *
     * @return The staged bytes of every cached texture, or 0 for an empty cache.
     */
    [[nodiscard]] std::uint64_t bytes() const noexcept;

    /** @brief States whether @p texture has cached images (by address). */
    [[nodiscard]] bool has(vn::raw_ptr<const vn::graphics::Texture> texture) const noexcept;

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

    /**
     * @brief Releases the cached image of one texture the host still owns.
     *
     * @details
     * - The cache entry holds the only lookup share; erasing it is enough, because a descriptor set that already
     *   names the image holds its own reference until it is retired. The next acquire rebuilds and re-uploads the
     *   texture, which is the documented cost of releasing a texture that is used again.
     * - Samplers are keyed by their value, not by the texture, so they stay shared with the rest of the scene.
     *
     * @param texture Texture whose cached image should leave; null is refused.
     * @return true when @p texture was recognized (erased, or it had no cached image); false for null.
     */
    bool releaseTexture(vn::raw_ptr<const vn::graphics::Texture> texture);

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

    /** @brief Gets the anisotropy this cache requests from its samplers (see setMaxAnisotropy).
     *
     * 1 is "isotropic filtering", which is what a cache that was never told uses - and the reason this
     * getter exists is that a cache which is never told is indistinguishable from one that ignores the
     * device: the wiring (the session hands the device's own limit over once) is asserted through it.
     *
     * @return The anisotropy in [1, 16].
     */
    [[nodiscard]] float maxAnisotropy() const noexcept;

    ~MaterialImages();

    MaterialImages(const MaterialImages&)            = delete;
    MaterialImages& operator=(const MaterialImages&) = delete;

  private:
    /** @brief The shared body of the two acquires; @p state null means the backend's default sampler. */
    [[nodiscard]] SamplerImage acquireImpl(vn::raw_ptr<const vn::graphics::Texture> texture, const vn::graphics::Sampler* state, detail::TextureReject& reason);

    struct Data;
    // Lexically after Data so the out-of-line destructor is the only place that needs the complete
    // type.
    std::unique_ptr<Data> d;

    MaterialImages();
};

VN_VSG_NS_END
