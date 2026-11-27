#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>

#include <vine/intrusive_ptr.hpp>
#include <vine/raw_ptr.hpp>

#include <vine/graphics/PassChain.hpp>

VN_GRAPHICS_NS_BEGIN

class Camera;
class Light;
class Material;
class RenderTarget;
class Scene;
class ShaderProgram;

namespace detail
{

/**
 * @brief The implementation behind PassChain::build(): recipes, not a public concept.
 *
 * @details
 * - It exists because the recipes need somewhere to keep their per-build counters and the engine they register on;
 *   the host-facing entry is the static PassChain::build(engine, desc), so no builder object has to be stored.
 * - Everything here is behaviour, not state a host can observe: the request lives in ChainDesc, the result in
 *   PassChain, and the engine's registry is the only registration.
 */
class ChainBuilder {
  public:
    /** @brief Constructs a builder targeting @p engine (borrowed; may be null, then nothing builds). */
    explicit ChainBuilder(raw_ptr<RenderEngine> engine, const void* owner = nullptr);

    /** @brief Assembles @p desc into a registered chain (see PassChain::build). */
    intrusive_ptr<PassChain> build(const ChainDesc& desc);

  private:
    /** @brief Builds the forward path's window pass into @p chain. */
    bool buildForwardPath(PassChain& chain);

    /** @brief Builds the deferred path (G-buffer + lighting) into @p chain. */
    bool buildDeferredPath(PassChain& chain, const ChainDesc& desc);

    /** @brief Builds the depth-only shadow pass a casting light asks for, and returns its map. */
    intrusive_ptr<RenderTarget> buildShadowPass(PassChain& chain, const intrusive_ptr<const Light>& light);

    /**
     * @brief Declares the background's pass: the window on the forward path, the composite (against the G-buffer's
     *        depth) on the deferred one.
     *
     * @details
     * - ONE place states the placement the whole feature is about: after the shading, before every other
     *   forward-only pass, with the depth test on and no depth write, and drawn by skyboxProgram() with
     *   skyboxRenderState() (both stated by the scene it draws, see makeBackgroundScene).
     * - A background that has no environment yet (`BackgroundOptions::map` null) declares the SAME row with
     *   `enabled = false`: the picture behaves as if there were none (what a pixel holds is the path's own answer)
     *   and PassChain::setBackgroundMap turns it on without a rebuild.
     *
     * @param target      The target it draws into; null draws into the window.
     * @param depth_owner The target whose depth it tests against when @p target is a composite (the G-buffer).
     * @return The declaration (see PassDesc).
     */
    PassDesc backgroundPassDesc(const intrusive_ptr<RenderTarget>& target, const intrusive_ptr<RenderTarget>& depth_owner) const;

    /** @brief Adds the optional HUD overlays to @p chain. */
    bool applyOverlays(PassChain& chain, const ChainDesc& desc);

    /**
     * @brief Reports the shadow-casting lights this build did NOT honour.
     *
     * @details
     * - Three reasons, counted apart and each named in its own message: a SECOND caster (the chain builds one map),
     *   a POINT caster (six faces; backlog S6) and a path that has no lit shading to shadow.
     * - Silent when everything asked for was built - a report that fires on a request the chain honoured tells the
     *   host its picture is unshadowed while it is shadowed.
     */
    void reportRequestedShadows() const;

  private:
    raw_ptr<RenderEngine> engine_;
    /// The scope the chain's rows belong to (see PassChain::build); null means "the chain is its own scope".
    const void* owner_{ nullptr };
    /// This build's request, copied once at the start of build() (the public request is the ChainDesc value).
    raw_ptr<Camera>      camera_;
    intrusive_ptr<Scene> content_;
    intrusive_ptr<Scene> transparent_;
    /// This build's background declaration, when the request has one (see ChainDesc::background).
    std::optional<BackgroundOptions> background_;
    /// The scene the background pass draws (owned by the chain from the moment it is built).
    intrusive_ptr<Scene> background_scene_;
    /// Shadow passes THIS build created: a requested shadow that WAS built is not a gap to report.
    std::size_t shadows_built_    = 0;
    /// This build's content selection, taken from ChainDesc::layer_mask.
    std::uint32_t content_layers_ = kAllLayers;
};

} // namespace detail

VN_GRAPHICS_NS_END
