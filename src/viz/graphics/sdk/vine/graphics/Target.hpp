#pragma once
#include "graphics_global.hpp"

#include <cstdint>
#include <optional>
#include <utility>
#include <vector>

#include <vine/String.hpp>
#include <vine/intrusive_ptr.hpp>
#include <vine/raw_ptr.hpp>

#include "RenderTarget.hpp"

VN_GRAPHICS_NS_BEGIN

/**
 * @brief A device-pixel extent: the two numbers a transient target is sized to when the frame's area is not the answer.
 *
 * @details
 * - A VALUE, so "the size the declaration states" and "the size the frame lays out on" are different by TYPE rather
 *   than by the magic 0 the declaration used to carry (see Target::extent): unset means the frame's area, and a
 *   stated extent is the number itself.
 * - No mechanism: the only consumer is a transient target's build today; a second consumer (a partial readback) gets
 *   the same two ints, not a new rectangle type.
 */
struct VN_GRAPHICS_API Extent {
    /// Width in DEVICE pixels.
    int width{ 0 };
    /// Height in DEVICE pixels.
    int height{ 0 };
};

/**
 * @brief The shape a transient target is built at: its colour attachments, depth and sampleability (see Target).
 *
 * @details
 * - A transient is declared rather than created: the pass says what IMAGE it needs, and the frame's compile allocates
 *   the attachment, keys it by (identity, shape), reuses it while the declaration holds, and lets it go when the
 *   declaration goes away (see RenderEngine::transientTargetCount).
 * - The extent is NOT here: it belongs to the declaration beside the shape (see Target::extent), because "unset =
 *   the frame's area" is a statement about the frame, not about the image's format.
 */
struct VN_GRAPHICS_API TransientShape {
    /// Colour attachments, in attachment order (empty means depth-only).
    std::vector<RenderTarget::ColorFormat> colors{ RenderTarget::ColorFormat::RGBA8 };
    /// Depth attachment, or unset for none.
    std::optional<RenderTarget::DepthFormat> depth{ RenderTarget::DepthFormat::D24 };
    /// Whether the depth attachment ends up sampleable (see RenderTarget::setDepthPromotion).
    bool depth_promotion{ true };
};

/**
 * @brief WHERE one pass draws: the window, an explicit target the host owns, or a transient it declares by identity.
 *
 * @details
 * - ONE FIELD, THREE SPELLINGS THAT CANNOT BE CONFUSED: the kind says which of @ref target / @ref name / @ref extent
 *   and @ref shape is meaningful, so a reader no longer has to decide whether `writes`, `writes_image` or "null"
 *   wins. A value that states two forms at once is invalid (see isValid) and the plan reports it instead of picking
 *   one silently.
 * - `Window` is the default: the pass draws into the frame's presenting surface (no target object exists for it).
 *   `Explicit` is a target the host created and keeps (see RenderTarget); `Transient` is the pass saying WHAT it
 *   needs - the compile allocates it, shares it with every pass declaring the same identity and shape, and is the
 *   one that hands the resolved target back (see RenderPass::resolvedTarget).
 * - The compile no longer rewrites this declaration with the target it resolved: "I asked for a transient" and "the
 *   target that answered it" are two facts, and only the first belongs to the declaration (see RenderPass).
 *
 * @note
 * - One target one value, not a `RenderTarget*` beside a name: the old `writes` / `writes_image` / `writes_shape`
 *   triple could state two of them at once, which is why every reader needed a precedence rule.
 */
struct VN_GRAPHICS_API Target {
    /// Which form this target takes; it says which fields below are meaningful.
    enum class Kind : std::uint8_t
    {
        Window,    // The frame's presenting surface (no target object).
        Explicit,  // A target the host owns (see target).
        Transient, // A target the compile allocates from name / extent / shape.
    };

    /// The form this target takes.
    Kind kind{ Kind::Window };
    /// Explicit: the target the host owns (null for the other kinds).
    intrusive_ptr<RenderTarget> target{};
    /// Transient: the stable identity the compile keys the target by (empty for the other kinds).
    String name{};
    /// Transient: the DEVICE-pixel extent to build at, or unset for the frame's area (ignored otherwise).
    std::optional<Extent> extent{};
    /// Transient: the attachment shape to build, or unset for the default (ignored otherwise).
    std::optional<TransientShape> shape{};

    /**
     * @brief Builds an explicit target (the form a host that owns its target states).
     *
     * @param target The target the pass draws into; must not be null (see isValid).
     * @return The explicit form naming @p target.
     */
    static Target explicitTarget(intrusive_ptr<RenderTarget> target)
    {
        Target value;
        value.kind   = Kind::Explicit;
        value.target = std::move(target);
        return value;
    }

    /**
     * @brief Builds a transient target declared by identity, with an optional shape and extent.
     *
     * @param identity The stable identity the compile keys the target by (must not be empty).
     * @param shape    The attachment shape, or unset for the default (RGBA8 + D24, sampleable).
     * @param extent   The device-pixel extent, or unset for the frame's area size.
     * @return The transient form naming @p identity.
     */
    static Target transient(String identity, std::optional<TransientShape> shape = {}, std::optional<Extent> extent = {})
    {
        Target value;
        value.kind   = Kind::Transient;
        value.name   = std::move(identity);
        value.shape  = std::move(shape);
        value.extent = extent;
        return value;
    }

    /// @brief Gets whether this states the window (draws into the frame's presenting surface).
    [[nodiscard]] bool isWindow() const noexcept
    {
        return kind == Kind::Window;
    }

    /// @brief Gets whether this states an explicit target.
    [[nodiscard]] bool isExplicit() const noexcept
    {
        return kind == Kind::Explicit;
    }

    /// @brief Gets whether this declares a transient target by identity.
    [[nodiscard]] bool isTransient() const noexcept
    {
        return kind == Kind::Transient;
    }

    /**
     * @brief Gets whether the fields agree with @ref kind.
     *
     * @details
     * - The one check a plan makes before it uses a target: a value that states two forms at once (an explicit
     *   target beside a transient name, or an extent on a window) is refused rather than resolved by precedence.
     *
     * @return true when only the fields @ref kind gives meaning to are set.
     */
    [[nodiscard]] bool isValid() const noexcept
    {
        switch (kind) {
            case Kind::Window:
                return target == nullptr && name.empty() && !extent.has_value() && !shape.has_value();
            case Kind::Explicit:
                return target != nullptr && name.empty() && !extent.has_value() && !shape.has_value();
            case Kind::Transient:
                return target == nullptr && !name.empty();
        }
        return false;
    }
};

VN_GRAPHICS_NS_END
