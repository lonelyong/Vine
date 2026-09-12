#pragma once

/**
 * @brief The per-frame decisions a target's passes are driven by, as plain VALUES.
 *
 * The plans a PASS is driven by: the attachment set it attaches (PassAttachments) and what
 * it has to record this frame (PassPlan). They live here rather than in the state because
 * each is a decision about a target, taken from that target plus a handful of session facts
 * — the deciding code and its contracts are documented where it is defined
 * (VsgPassMaterialiser.cpp). The command graph's own order is a separate decision and lives
 * in VsgRecordOrder.hpp.
 */

#include <vine/vsg/vsg_global.hpp>

#include <vector>

#include <vsg/core/ref_ptr.h>

#include <vine/graphics/RenderTarget.hpp>

#include <vine/vsg/VsgPipelineFactory.hpp>
#include <vine/vsg/VsgRenderTargetEntry.hpp>

V_VSG_NS_BEGIN

namespace detail {

/** @brief The Vulkan-side description of one target's attachment set.
 *
 * Every pass of a target attaches exactly the same images and only differs in
 * its load-ops, so this is computed once per pass build and then passed around:
 * the device the objects are created on, the attachment formats the render
 * pass must declare, and which attachments exist (owned or BORROWED — see
 * RenderTarget::shareDepth).
 */
struct PassAttachments {
    ::vsg::ref_ptr<::vsg::Device> device;
    std::vector<VkFormat>         color_formats;
    VkFormat                      depth_format = VK_FORMAT_UNDEFINED;
    bool                          has_color    = false;
    bool                          has_depth    = false;
    bool                          borrowed     = false;
};

/** @brief What one pass of a target needs THIS frame (§28, decided in one place).
 *
 * Values only: everything the apply side needs, read at plan time — because applying
 * the plan (revoking a depth promotion, publishing the new objects) is exactly what
 * changes them. @ref current in particular points INTO the target's pass table, so it
 * has to be taken before the pass is published under its key.
 */
struct PassPlan {
    /// The target's attachment set (owned by buildOffscreenTarget).
    PassAttachments att;
    /// The variant this pass has to record (load-ops, promotion, transient bootstrap).
    detail::PassVariant variant;
    /// The pass' recorded objects, or null when this is a NEW pass.
    const VsgRenderTargetEntry::PassObjects* current = nullptr;
    /// Whether the target has a colour / a depth attachment.
    bool has_color = false;
    /// Whether the target has a depth attachment (owned or borrowed).
    bool has_depth = false;
    /// This pass' own clear requests (what the host asked). The variant's materialised
    /// load-ops come from these — never from a materialised comparison (see planPass).
    bool want_color_clear = false;
    bool want_depth_clear = false;
    /// The variant's colour load-op is CLEAR (the pass clears rather than loads).
    bool color_clear = false;
    /// The variant's depth load-op is LOAD (the pass preserves the depth it finds).
    bool depth_load = false;
};

} // namespace detail

V_VSG_NS_END
