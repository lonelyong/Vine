#pragma once

#include <cstdint>
#include <vector>

#include <vine/vsg/core/Keys.hpp>
#include <vine/vsg/core/TargetPlan.hpp>
#include <vine/vsg/vsg_global.hpp>

/**
 * @brief What each of a pass' attachments does at the start of the pass: load, clear, or nothing at all.
 *
 * WHY THIS IS A RULE AND NOT A HABIT. Every one of these decisions has a picture on the other side of it, and
 * each was learned the hard way in this backend:
 *
 *   * A FRESH IMAGE CANNOT BE LOADED. The first pass into a bootstrap target must clear, because the image's
 *     contents are undefined (its layout is UNDEFINED); a "LOAD" there samples whatever the driver left in the
 *     memory. The existing implementation calls this the bootstrap variant, and it is the reason a target that
 *     was just built always passes through one clearing pass.
 *   * ONLY ATTACHMENT 0 GETS THE PASS' COLOUR; THE EXTRAS GET TRANSPARENT BLACK. A pass that writes several
 *     attachments has one clear colour (the engine sets one), and the others are auxiliary (normals, ids,
 *     depth-like data). Painting them the same colour is a picture that looks plausible and is wrong; leaving
 *     them uninitialised is worse. Transparent black is the value that means "nothing here".
 *   * DEPTH CLEARS TO THE REVERSE-Z FAR PLANE, which is 0.0, not 1.0. The projection maps the far plane to 0
 *     and the comparison is GREATER, so a depth buffer cleared to 1.0 rejects every fragment (the picture is
 *     empty) and one cleared to 0.0 accepts everything closer than the far plane. This has bitten this family
 *     of code before, which is why the constant has a name here.
 *   * A PRESERVED DEPTH IS NEVER CLEARED. When a later pass reads the depth an earlier pass wrote, clearing it
 *     destroys the very thing the later pass loads - and the pass would then read 0.0 (the far plane) and treat
 *     every fragment as visible. Preservation therefore overrides even the bootstrap rule: a target whose depth
 *     is borrowed or promoted from an earlier pass is exactly the case planning must protect.
 *
 * The plan is per attachment and is derived from three inputs only (the shape, what the pass asked for, and
 * whether this pass is the bootstrap one), so a phase can assert the whole table without a device.
 */
VN_VSG_NS_BEGIN

namespace core
{

/** @brief The depth value a cleared depth attachment receives under the backend's reverse-Z convention. */
inline constexpr float kReverseZFarDepth = 0.0F;

/** @brief What the pass asked for, before the rules above are applied. */
struct ClearPolicy
{
    bool  color{false};                     ///< The pass asked to clear its colour attachments.
    float color_value[4]{ 0.0F, 0.0F, 0.0F, 1.0F };  ///< The value attachment 0 receives when it clears.
    bool  depth{false};                     ///< The pass asked to clear depth (bootstrap clears it regardless).
    float depth_value{kReverseZFarDepth};   ///< Value the depth clear uses (the reverse-Z far plane by default).
};

/** @brief What one colour attachment does at the start of the pass. */
struct AttachmentClear
{
    LoadOp        load{LoadOp::Load};   ///< Keep what is there, or clear it.
    StoreOp       store{StoreOp::Store};///< Whether the result survives the pass.
    float         clear[4]{ 0.0F, 0.0F, 0.0F, 0.0F };  ///< Value for a clearing attachment (transparent black by default).

    /** @brief Compares the whole decision. */
    [[nodiscard]] bool operator==(const AttachmentClear& other) const noexcept;
};

/** @brief What one depth attachment does at the start of the pass. */
struct DepthClear
{
    LoadOp  load{LoadOp::Load};    ///< Keep what is there, or clear it.
    StoreOp store{StoreOp::Store}; ///< Whether the result survives the pass.
    float   clear{kReverseZFarDepth};  ///< Value for a clearing depth attachment.

    /** @brief Compares the whole decision. */
    [[nodiscard]] bool operator==(const DepthClear& other) const noexcept;
};

/** @brief What every attachment of one pass does, and why the pass clears at all. */
struct PassClearPlan
{
    std::vector<AttachmentClear> colors;      ///< One per colour attachment (empty for a depth-only target).
    bool                         has_depth{false};  ///< Whether the target has a depth attachment.
    DepthClear                   depth;       ///< The depth decision (read only when has_depth).
    bool                         bootstrap{false};  ///< Whether the clear was forced by the bootstrap rule.

    /** @brief Compares the whole plan. */
    [[nodiscard]] bool operator==(const PassClearPlan& other) const noexcept;
};

/** @brief Decides what each attachment of a pass does at the start of the pass.
 *
 * The rules, in the order they are applied (see the file note for why each one exists):
 *
 *   1. a bootstrap pass clears EVERY attachment: colour 0 with the policy's value, the extras with transparent
 *      black, and depth with the policy's value (the reverse-Z far plane by default);
 *   2. a pass that is not a bootstrap keeps the attachments it does not ask to clear;
 *   3. a BORROWED depth (`depth_borrowed`: the image belongs to the target this one shares it with) is NEVER
 *      cleared - not by the pass' request and not by the bootstrap rule. Clearing it would erase the depth the
 *      LENDER's pass wrote for every reader after it, and the lender is who decides what happens to that
 *      image. A lender's OWN depth is cleared like any other attachment (a fresh one must be, and a pass that
 *      asks for a clear gets it) - its borrowers read the depth the lender's pass writes in THIS frame, so a
 *      clear at the start of that pass is exactly what makes their depth test meaningful;
 *   4. a clear request clears all colour attachments, and only attachment 0 receives the pass' colour.
 *
 * @param shape          The target's shape (how many colour attachments, whether there is depth).
 * @param policy         What the pass asked for.
 * @param bootstrap      Whether this is the first pass into a freshly built target.
 * @param depth_borrowed Whether the depth attachment is ANOTHER target's image (see rule 3).
 * @return The per-attachment plan.
 */
[[nodiscard]] PassClearPlan planClearValues(const TargetShape& shape, const ClearPolicy& policy, bool bootstrap,
                                            bool depth_borrowed) noexcept;

/** @brief Gets the layout a target leaves its depth attachment in.
 *
 * A DEPTH-ONLY target that the host asked to be sampleable is a SHADOW MAP, and that is what such a target is
 * for: its depth ends - and therefore starts the next pass - in the layout a shader reads it in, so the pass
 * that samples it needs no barrier and no transition of its own. Every other shape keeps its depth in the
 * attachment layout: it is an attachment for the next pass (which depth-tests against it), and "make it a
 * texture" is a promotion that a later slice plans per pass.
 *
 * The rule is a function of the shape and the host's request only, so a phase can pin it without a device -
 * and it is the SAME value for the end of one pass and the start of the next, because that is what "leaves it
 * in" means.
 *
 * @param shape            The target's shape (colour attachments + optional depth format).
 * @param depth_sampleable Whether the host asked for the depth to be usable as a texture.
 * @return The depth's steady layout for this target.
 */
[[nodiscard]] ImageLayout depthFinalLayout(const TargetShape& shape, bool depth_sampleable) noexcept;

/** @brief Names the render pass variant a pass' clear plan asks for (see LoadOpVariantKey).
 *
 * The two layouts a target leaves its attachments in are INPUTS rather than facts of the plan: the plan says
 * what a pass does at the START, and the target is the one that decides what its attachments hold BETWEEN
 * passes (this backend leaves colour sampleable and the depth an attachment - see makeOffscreenRenderPass).
 *
 * A CLEAR starts from UNDEFINED whatever the attachment held: the contents are about to be discarded, and
 * UNDEFINED is the cheapest transition the driver can make. A LOAD has to name the layout the last pass left,
 * which is exactly `color_final` / `depth_final`.
 *
 * @param plan         The pass' per-attachment decisions (from @ref planClearValues).
 * @param color_final  The layout the colour attachments are left in.
 * @param depth_steady The layout the depth is in when this pass does NOT clear it (the target's own
 *                     @ref depthFinalLayout, or its lender's when the depth is borrowed).
 * @param depth_final  The layout this pass leaves the depth in.
 * @return The variant key: two passes that may share one render pass object compare equal on it.
 */
[[nodiscard]] LoadOpVariantKey loadOpVariantOf(const PassClearPlan& plan, ImageLayout color_final,
                                               ImageLayout depth_steady, ImageLayout depth_final) noexcept;

}  // namespace core

VN_VSG_NS_END
