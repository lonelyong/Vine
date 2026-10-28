#pragma once

#include <vine/String.hpp>

#include "Gui.hpp"

VN_APPFWGUI_NS_BEGIN

/**
 * @brief Where a ribbon contribution is placed relative to its siblings.
 *
 * @details
 * - Append: after the last non-terminal sibling; a role-bearing tab is appended in role order
 *   among the role-bearing tabs (roles below UserRole sort by value).
 * - Before / After: next to the sibling named by anchor_id, or - for tabs only - by anchor_role.
 * - Exactly one anchor is used. Before/After with neither anchor, or with both, is rejected
 *   with RibbonPlaceStatus::InvalidAnchor.
 *
 * @note Group-level placements anchor by group id only: a role anchor there is invalid.
 */
struct RibbonPlacement
{
    /// @brief The kind of placement.
    enum class Where
    {
        Append, ///< Add at the end of the non-terminal block.
        Before, ///< Add before the anchor.
        After,  ///< Add after the anchor.
    };

    Where         where       = Where::Append;      ///< Placement kind.
    String        anchor_id;                        ///< Sibling id to anchor on, or empty.
    RibbonTabRole anchor_role = RibbonTabRole::None; ///< Tab role to anchor on, or None.

    /**
     * @brief Creates an "append" placement.
     *
     * @return The placement value.
     */
    static RibbonPlacement append()
    {
        return {};
    }

    /**
     * @brief Creates a "before the sibling with this id" placement.
     *
     * @param id Stable id of the sibling.
     * @return The placement value.
     */
    static RibbonPlacement before(const String& id)
    {
        RibbonPlacement placement;
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
    static RibbonPlacement after(const String& id)
    {
        RibbonPlacement placement;
        placement.where     = Where::After;
        placement.anchor_id = id;
        return placement;
    }

    /**
     * @brief Creates a "before the first tab with this role" placement.
     *
     * @param role Tab role to anchor on.
     * @return The placement value.
     */
    static RibbonPlacement before(RibbonTabRole role)
    {
        RibbonPlacement placement;
        placement.where       = Where::Before;
        placement.anchor_role = role;
        return placement;
    }

    /**
     * @brief Creates an "after the first tab with this role" placement.
     *
     * @param role Tab role to anchor on.
     * @return The placement value.
     */
    static RibbonPlacement after(RibbonTabRole role)
    {
        RibbonPlacement placement;
        placement.where       = Where::After;
        placement.anchor_role = role;
        return placement;
    }
};

/**
 * @brief Outcome of a placement request.
 *
 * @details
 * - The contribution is still added unless the status says otherwise: a fallback or a clamp
 *   keeps the caller's UI visible and says what happened.
 * - DuplicateId, AlreadyAdded and InvalidAnchor mean the contribution was not added.
 */
enum class RibbonPlaceStatus
{
    Placed,                ///< Placed exactly where the request asked.
    PlacedByFallback,      ///< The anchor was missing; placed at the fallback position.
    ClampedBeforeTerminal, ///< The request pointed after a Terminal tab; moved before it.
    DuplicateId,           ///< A sibling with the same non-empty id already exists.
    AlreadyAdded,          ///< The same object is already registered.
    InvalidAnchor,         ///< Before/After without a usable anchor (or both anchors set).
};

/**
 * @brief Result of a placement request.
 */
struct RibbonPlaceResult
{
    RibbonPlaceStatus status = RibbonPlaceStatus::Placed; ///< What happened.
    int               index  = -1;                        ///< Final logical index, or -1 when not added.
};

VN_APPFWGUI_NS_END
