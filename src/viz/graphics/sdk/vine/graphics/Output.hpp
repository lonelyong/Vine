#pragma once
#include "graphics_global.hpp"

#include <utility>

#include <vine/String.hpp>
#include <vine/intrusive_ptr.hpp>
#include <vine/raw_ptr.hpp>

#include "ImageRef.hpp"

VN_GRAPHICS_NS_BEGIN

/**
 * @brief WHAT one pass publishes for later passes: a name, and at most one promise about what that name covers.
 *
 * @details
 * - THE OUTPUT IS ONE FACT. A pass used to publish by name (`output_name`), promise a whole target
 *   (`output_target`) and promise one image (`output`) in three fields that could disagree; now the name is where
 *   the outputs appear in the frame's registry and @ref whole_target / @ref image say what the name covers.
 * - THE WHOLE-TARGET CLAIM HAS NO TARGET OF ITS OWN: it is the target the pass DRAWS INTO (see PassDesc::target),
 *   so "publishes a target it does not render into" is not a wiring mistake to report - it is unrepresentable. A
 *   pass that draws into the window has no target to promise, which the plan reports.
 * - The fine claim (@ref image) is the same `ImageRef` a consumer names: producer and consumer point at ONE object,
 *   so a hand-off cannot be mistyped (see ImageRef). Binding it to a target the pass does not draw into is reported
 *   by the wiring check.
 *
 * @note
 * - An empty @ref name publishes nothing, which is the default: a pass that only draws has no output.
 * - @ref whole_target with @ref image is two promises for one name; the plan refuses it (see isValid) rather than
 *   preferring one.
 */
struct VN_GRAPHICS_API Output {
    /// The name later passes consume the output by (empty = this pass publishes nothing).
    String name{};
    /// Whether the promise covers the WHOLE of @ref PassDesc::target (every colour attachment).
    bool whole_target{ false };
    /// The ONE image this pass promises (a target's attachment, or its depth), or null for none.
    intrusive_ptr<ImageRef> image{};

    /**
     * @brief Gets whether this output states at most one promise.
     *
     * @details
     * - The plan's check: a coarse and a fine promise for one name is two answers to "what does this name cover",
     *   and a reader cannot tell which one a consumer resolved.
     *
     * @return true unless both @ref whole_target and @ref image are stated.
     */
    [[nodiscard]] bool isValid() const noexcept
    {
        return !(whole_target && image != nullptr);
    }

    /**
     * @brief Gets whether this output publishes anything at all.
     *
     * @return true when @ref name is not empty.
     */
    [[nodiscard]] bool publishes() const noexcept
    {
        return !name.empty();
    }
};

VN_GRAPHICS_NS_END
