#pragma once
#include "graphics_global.hpp"

#include <utility>

#include <vine/intrusive_ptr.hpp>
#include <vine/raw_ptr.hpp>

VN_GRAPHICS_NS_BEGIN

class RenderTarget;

/**
 * @brief The DEPTH IMAGE of a target, named as a value: what a pass draws against when the depth is not its own.
 *
 * @details
 * - WHY A VALUE AND NOT A FIELD. Sharing depth is an EDGE of the frame's resource graph ("this pass draws against
 *   that pass' depth"), and an edge is a relationship between two names rather than a property of one of them. As a
 *   value it can be handed around, stored by the pass that states it, compared and validated before a frame runs -
 *   and it reads the same whether it is given to a target (the mechanism a backend builds a framebuffer from) or
 *   declared on a pass (the edge a host states).
 * - THE VIEW KEEPS THE TARGET ALIVE. The borrower may outlive the code that created the lender (a retained target
 *   outliving a phase), so holding a bare pointer here would be the dangling-pointer shape this type exists to
 *   remove.
 * - WHAT IT NAMES is the depth image, optionally ONE LAYER of it. A layered target (a shadow map array, a CSM
 *   cascade stack) is the layered-rendering item registered in `.ai/design/graphics-scene.md`: until that lands,
 *   a target has one depth image, so a view whose layer is not 0 is a DECLARATION this SDK cannot honour yet -
 *   it is refused (and said once) rather than silently pointing at the whole image.
 * - A DEFAULT-CONSTRUCTED VIEW NAMES NOTHING, which is not the same statement as "the target's own depth": a pass
 *   declaring an invalid view draws against no depth image at all.
 */
class VN_GRAPHICS_API DepthView
{
  public:
    /** @brief Names no depth image (see the class note: absence, not "its own"). */
    DepthView() = default;

    /** @brief Names @p target's depth image, or one layer of it (null names nothing, like the default).
     *
     * @param target Target whose depth image this view names (held: the view keeps it alive).
     * @param layer  Layer index; 0 is the whole image until layered targets exist (see the class note).
     */
    DepthView(intrusive_ptr<RenderTarget> target, int layer = 0)
        : target_(std::move(target))
        , layer_(layer)
    {
    }

    /** @brief Gets whether this view names a depth image.
     *
     * @return true when a target is named.
     */
    [[nodiscard]] bool isValid() const noexcept { return target_ != nullptr; }

    /** @brief Gets the target whose depth image this is.
     *
     * @return The target, or null when this view names nothing.
     */
    [[nodiscard]] raw_ptr<RenderTarget> target() const noexcept { return target_.get(); }

    /** @brief Gets which layer of the depth image this view names (0 = the whole image). */
    [[nodiscard]] int layer() const noexcept { return layer_; }

  private:
    intrusive_ptr<RenderTarget> target_;
    int                         layer_ = 0;
};

/**
 * @brief Names @p target's depth image: the spelling a declaration reads best as.
 *
 * @param target Target whose depth image is meant (held by the returned view).
 * @param layer  Layer index (0 = the whole image; see DepthView).
 * @return The view.
 */
[[nodiscard]] inline DepthView depthViewOf(intrusive_ptr<RenderTarget> target, int layer = 0)
{
    return DepthView(std::move(target), layer);
}

VN_GRAPHICS_NS_END
