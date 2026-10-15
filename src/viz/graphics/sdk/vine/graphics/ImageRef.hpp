#pragma once
#include "graphics_global.hpp"

#include <vine/intrusive_ptr.hpp>
#include <vine/Object.hpp>
#include <vine/raw_ptr.hpp>
#include <vine/RefCounted.hpp>
#include <vine/String.hpp>

#include "RenderTarget.hpp"

VN_GRAPHICS_NS_BEGIN

/**
 * @brief The identity of ONE image the pipeline hands from one pass to another: a target plus WHICH
 * of its attachments.
 *
 * @details
 * - The wiring between passes: a producer declares the image it writes, a consumer declares the images it
 *   reads, and both point at the SAME object - a hand-off that cannot be mistyped. The object is created and
 *   owned by the HOST (a pipeline builder, a demo, an editor); the engine only references it while it
 *   validates and executes the passes.
 * - ATTACHMENT OR TEXTURE - BOTH: one image, two roles. Written it is an ATTACHMENT (bound into the pass's
 *   frame buffer); sampled it is a TEXTURE. The backend switches between the two layouts (that is what the
 *   depth promotion / borrow machinery exists for), so the identity is independent of the role - a producer
 *   and its consumer name the same thing.
 * - What it carries: the TARGET + ATTACHMENT INDEX ("which image" is a target and an index, not just a
 *   target - an MRT pass hands out several; the Depth kind gives a depth sampler the same home); a STRONG
 *   reference to the target, so the hand-off describes itself and a consumer is never handed a target its
 *   owner already dropped; a LABEL, for diagnostics only (never a lookup key - that is what this object
 *   replaces).
 *
 * @note
 * - Why an object and not a name (design `graphics-render-pipeline.md` §14): a name is a stringly-typed
 *   pointer - the per-frame name registry conflated the WIRING (static), "was it produced this frame"
 *   (per frame) and "who produces it" (identity), so every structural mistake (two producers under one
 *   name, a consumer whose producer is gone, a renamed producer) could only show up at run time, silently,
 *   as "the pass drew nothing".
 * - Why this name and not `RenderPort`: "port" names a ROLE (an endpoint both sides plug into), while this
 *   object carries an IDENTITY (which image) - and the engine's rules are about identity ("one image, at
 *   most one producer"). The role is already expressed by where it is used: `RenderPass::output()` for the
 *   write side, `RenderPass::inputs()` for the read side.
 * - An image that is no pass' output at all (a material's texture, an imported image, a cube map) has no
 *   identity in this SDK yet: an ImageRef names a RenderTarget plus an attachment index, whereas a material
 *   texture is a Texture (its pixels come from imaging::Image), so the two models are not bridged yet.
 */
class VN_GRAPHICS_API ImageRef : public Object, public RefCounted<ImageRef> {
    VN_OBJECT_META_DECL;

  public:
    /** @brief Which attachment of the target the image is.
     *
     * The kind says where the image lives, not how it may be used: a colour attachment can always be
     * sampled, while a depth attachment may not be — that depends on the target's promotion state,
     * which is the backend's business.
     */
    enum class Kind {
        Color, ///< A colour attachment (written by fragment output location i).
        Depth, ///< A depth attachment (a program reconstructing positions samples this).
    };

  public:
    /** @brief Creates an image identity with a diagnostic label.
     *
     * @param label Human-readable name used in diagnostics (never used to look the image up).
     * @param kind  Which attachment of the target this image is.
     */
    explicit ImageRef(const String& label, Kind kind = Kind::Color);

  public:
    /** @brief Gets the image's diagnostic label.
     *
     * @return The label this image was created with.
     */
    const String& label() const noexcept;

    /** @brief Gets the attachment kind this image is.
     *
     * @return Color or Depth.
     */
    Kind kind() const noexcept;

    /** @brief Binds the image this object identifies: a target plus one of its attachments.
     *
     * @details
     * - Called by the host for a manually wired chain, or by whatever produces the image (a pass declaring
     *   this image as its output) - binding and producing are the same act: "this image is this target's
     *   attachment N".
     * - The attachment index is stored as given; the consumer clamps and reports an out-of-range index (it
     *   is the consumer that knows what the target offers).
     *
     * @param target     Target whose attachment this image is (null unbinds it).
     * @param attachment Attachment index inside @p target.
     */
    void bind(intrusive_ptr<RenderTarget> target, int attachment = 0);

    /** @brief Unbinds: the image this object identified is gone.
     *
     * A consumer that reads an unbound image has no input, which the engine reports (once per
     * episode) rather than handing out a stale image.
     */
    void unbind();

    /** @brief Gets the bound target.
     *
     * @return The target this image belongs to, or null when it is unbound.
     */
    raw_ptr<RenderTarget> target() const noexcept;

    /** @brief Gets the attachment index inside the bound target.
     *
     * @return The attachment index (0 while unbound).
     */
    int attachment() const noexcept;

    /** @brief States whether this image is currently bound.
     *
     * @return true when a target is bound.
     */
    bool isBound() const noexcept;

  private:
    String                      label_;
    Kind                        kind_ = Kind::Color;
    intrusive_ptr<RenderTarget> target_;
    int                         attachment_ = 0;
};

VN_GRAPHICS_NS_END
