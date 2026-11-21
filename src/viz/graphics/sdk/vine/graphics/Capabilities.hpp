#pragma once
#include "graphics_global.hpp"

#include <cstdint>

VN_GRAPHICS_NS_BEGIN

/**
 * @brief The YES/NO facts a backend reports about itself (see Capabilities).
 *
 * @details
 * - A FEATURE IS A PROPERTY OF THE INITIALIZED BACKEND, not a tuning knob and not a requirement: the numbers a plan
 *   may also need (samples, anisotropy) are LIMITS and live beside this type (see Limits), while anything the call
 *   itself answers better (whether a readback worked, say) stays with that call's own machine-readable result
 *   instead of becoming a second spelling here.
 * - The set exists so a host branches on ONE value before it draws: a HUD asks whether GPU times can be shown, a
 *   viewer's menu asks whether a wireframe mode means anything, a texture pipeline asks whether anisotropic sampling
 *   is honoured rather than clamped.
 * - A backend that cannot answer yet (no device up) reports NO for every feature, which is the honest answer:
 *   nothing renders without a device, so nothing is known either.
 * - WHAT EACH BACKEND ANSWERS TODAY: `Wireframe` and `AnisotropicFiltering` are required by both backends' device
 *   floor, so a live session reports them true; `GpuTimings` is the device's own answer (a device that cannot write
 *   timestamps reports false, and the backend's frame report then leaves its GPU half disengaged) - it is not a
 *   backend difference.
 */
struct VN_GRAPHICS_API Features {
    /**
     * @brief One YES/NO fact a host has to know BEFORE it plans a frame (see supports).
     *
     * @details
     * - The set exists so a host branches on ONE value before it draws: a HUD asks whether GPU times can be
     *   shown, a viewer's menu asks whether a wireframe mode means anything, a texture pipeline asks whether
     *   anisotropic sampling is honoured rather than clamped.
     */
    enum class Feature : std::uint8_t
    {
        /// The device can write timestamps, so per-pass GPU times can be measured and shown.
        GpuTimings,
        /// Triangle rasterisation can be line/point, so a wireframe (or point) view mode draws something.
        Wireframe,
        /// Samplers may request anisotropic filtering above 1x, so the request is honoured rather than clamped.
        AnisotropicFiltering,
    };

    /// The features this backend answers TRUE, one bit per Feature (see supports / bit).
    std::uint32_t bits{ 0 };

    /**
     * @brief Answers one feature (see Feature).
     *
     * @details
     * - Reads the bit set, so it says exactly what the backend filled in and nothing it inferred.
     *
     * @param feature Feature to ask about.
     * @return true when this backend reports it.
     */
    [[nodiscard]] bool supports(Feature feature) const noexcept
    {
        return (bits & bit(feature)) != 0U;
    }

    /**
     * @brief The bit @p feature occupies in @ref bits.
     *
     * @param feature Feature to spell.
     * @return The bit a backend sets to answer true.
     */
    [[nodiscard]] static constexpr std::uint32_t bit(Feature feature) noexcept
    {
        return std::uint32_t{ 1 } << static_cast<std::uint32_t>(feature);
    }
};

/**
 * @brief The numbers a backend reports about itself (see Capabilities).
 *
 * @details
 * - A LIMIT ANSWERS "how far", a feature answers "at all" (see Features): "anisotropy exists" and "up to which
 *   factor" are different facts, and a host showing or clamping a quality setting needs both.
 * - Nothing here is enforced by the engine on the host's behalf: a request above a limit is REFUSED by the backend
 *   that would have to honour it (reported once), never silently lowered. A host reads these before it authors a
 *   pipeline, because only the host can decide what to do when the device cannot serve the shape it wants.
 * - WHICH BACKEND ANSWERS WHAT: all four are device answers in both backends (vk from `VkPhysicalDeviceProperties`,
 *   vsg from the same physical-device limits); a device without a feature reports the neutral value (1 sample,
 *   1 attachment, 1.0 width/anisotropy), which is a true answer rather than a missing one.
 */
struct VN_GRAPHICS_API Limits {
    /// The largest anisotropic filtering factor the device honours (1 = no anisotropy).
    ///
    /// A LIMIT, not a feature: the feature bit says "anisotropy is available at all", this says how far a sample
    /// request is honoured before the backend clamps it - which is what a host showing or clamping a quality
    /// setting needs.
    float max_anisotropy{ 1.0F };

    /// The largest sample count a render target may ask for (1 = multisampling is not available).
    ///
    /// The legal counts are POWERS OF TWO (1, 2, 4, 8, ...), so this number and that rule together name every
    /// count the device offers. The count reported is the one BOTH the colour and the depth attachment support,
    /// because one target has one sample count.
    std::uint32_t max_samples{ 1 };

    /// The widest LINE primitive the device rasterises, in framebuffer pixels (1 = line width is fixed).
    ///
    /// `RenderState::lineWidth` is clamped to it, so a host that offers a width slider reads this, and a host that
    /// needs thick lines regardless of the device should generate geometry instead (a rasterised line cannot get
    /// joins, caps or antialiasing anyway).
    float max_line_width{ 1.0F };

    /// How many colour attachments one render target may carry.
    ///
    /// A CEILING, not a promise the engine enforces: a declaration asking for more is refused by the backend that
    /// would have to build the attachment, with a diagnostic naming the count.
    std::uint32_t max_color_attachments{ 1 };
};

/**
 * @brief What a backend can do, as ONE value (see RenderDevice::capabilities).
 *
 * @details
 * - WHY A VALUE. Every "can you do X" question used to be its own bool on the contract, with its own default and its
 *   own sentence per backend that does not do it - `supportsRenderTargets()` was the last one left. A capability is a
 *   DESCRIPTION of the backend, and descriptions are values: asked once, answered whole, and extended by adding a
 *   field (which every answer keeps compiling, because the fields are defaulted).
 * - TWO QUESTIONS IN ONE AGGREGATE: `features` says WHETHER something is available, `limits` says HOW FAR it goes.
 *   Keeping them apart is what lets a new limit be added without touching the feature set (and the other way
 *   round); the two booleans below stay here because they describe the session's shape, not a number.
 * - WHAT DOES NOT BELONG HERE is anything the call itself answers better: whether a readback works is answered by
 *   that call's own machine-readable result (see ReadbackResult - `Unsupported` is the documented answer for "this
 *   backend cannot do it", and a host gates the feature on it), so a flag here would be a second spelling of a fact
 *   that already has one.
 * - It lives in its own header because it is a LEAF of the SDK's vocabulary: the engine answers it
 *   (RenderEngine::capabilities) without knowing the backend contract, so this must not drag that in.
 */
struct VN_GRAPHICS_API Capabilities {
    /// Off-screen render targets are built, drawn into and sampled (see RenderDevice::setRenderTarget); false
    /// means a non-null target is ignored and the pass draws into the default framebuffer.
    bool render_targets{ false };

    /// Frames can be presented somewhere: this session has a window to show a picture through (see
    /// RenderDevice::createSurfaceTarget, and RenderArea::bindTarget for the window an area is bound to).
    ///
    /// False is a first-class state, not an error: a session brought up to draw only into render targets runs
    /// whole frames and presents nothing (an offline bake, a display-less run). It answers "was this session
    /// opened with a presentation intent at all" - which is why it is asked before a host plans a frame, rather
    /// than discovered from a present that did nothing.
    bool presentation{ false };

    /// The YES/NO facts this backend answers (see Features).
    Features features{};

    /// The numbers this backend reports (see Limits).
    Limits limits{};
};

VN_GRAPHICS_NS_END
