#pragma once

#include <vine/String.hpp>

#include "Gui.hpp"

VN_APPFWGUI_NS_BEGIN

/**
 * @brief Where a Backstage page is placed relative to its siblings.
 *
 * @details
 * - Append: after the last non-terminal page; a role-bearing page is appended in role order among the
 *   role-bearing pages (roles below UserRole sort by value), so page order does not depend on who loaded first.
 * - Before / After: next to the sibling named by anchor_id, or by anchor_role.
 * - Exactly one anchor is used. Before/After with neither anchor, or with both, is rejected with
 *   BackstagePlaceStatus::InvalidAnchor.
 * - The same rules the ribbon resolves tabs with: a Terminal page (BackstagePagePolicy::Terminal) keeps the
 *   suffix, and a request that points past it is clamped before it.
 *
 * @note A page anchors on a page role only: a caller that needs a ribbon slot uses RibbonPlacement instead.
 */
struct BackstagePlacement
{
    /// @brief The kind of placement (shared with the ribbon's RibbonPlacement).
    using Where = PlacementWhere;

    Where             where       = Where::Append;               ///< Placement kind.
    String            anchor_id;                                 ///< Sibling id to anchor on, or empty.
    BackstagePageRole anchor_role = BackstagePageRole::None;     ///< Page role to anchor on, or None.

    /**
     * @brief Creates an "append" placement.
     *
     * @return The placement value.
     */
    static BackstagePlacement append()
    {
        return {};
    }

    /**
     * @brief Creates a "before the sibling with this id" placement.
     *
     * @param id Stable id of the sibling.
     * @return The placement value.
     */
    static BackstagePlacement before(const String& id)
    {
        BackstagePlacement placement;
        placement.where     = Where::Before;
        placement.anchor_id = id;
        return placement;
    }

    /**
     * @brief Creates an "after the sibling with this id" placement.
     *
     * @param id Stable id of the sibling.
     * @return The placement value.
     */
    static BackstagePlacement after(const String& id)
    {
        BackstagePlacement placement;
        placement.where     = Where::After;
        placement.anchor_id = id;
        return placement;
    }

    /**
     * @brief Creates a "before the first page with this role" placement.
     *
     * @param role Page role to anchor on.
     * @return The placement value.
     */
    static BackstagePlacement before(BackstagePageRole role)
    {
        BackstagePlacement placement;
        placement.where       = Where::Before;
        placement.anchor_role = role;
        return placement;
    }

    /**
     * @brief Creates an "after the first page with this role" placement.
     *
     * @param role Page role to anchor on.
     * @return The placement value.
     */
    static BackstagePlacement after(BackstagePageRole role)
    {
        BackstagePlacement placement;
        placement.where       = Where::After;
        placement.anchor_role = role;
        return placement;
    }
};

/**
 * @brief Outcome of a page placement request.
 *
 * @details
 * - The page is still added unless the status says otherwise: a fallback or a clamp keeps the caller's page
 *   visible and says what happened.
 * - DuplicateId, AlreadyAdded, InvalidAnchor and Unavailable mean the page was not added.
 */
enum class BackstagePlaceStatus
{
    Placed,                ///< Placed exactly where the request asked.
    PlacedByFallback,      ///< The anchor was missing; placed at the fallback position.
    ClampedBeforeTerminal, ///< The request pointed after a Terminal page; moved before it.
    DuplicateId,           ///< A sibling with the same non-empty id already exists.
    AlreadyAdded,          ///< The same object is already registered.
    InvalidAnchor,         ///< Before/After without a usable anchor (or both anchors set).
    Unavailable,           ///< The Backstage has no window any more (teardown); nothing was added.
};

/**
 * @brief Result of a page placement request.
 */
struct BackstagePlaceResult
{
    BackstagePlaceStatus status = BackstagePlaceStatus::Placed; ///< What happened.
    int                  index  = -1;                           ///< Final logical index, or -1 when not added.
};

VN_APPFWGUI_NS_END
