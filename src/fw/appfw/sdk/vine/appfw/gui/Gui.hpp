#pragma once

#include <vine/appfw/appfw_global.hpp>

#include <cstdint>

#include <vine/math/Point2.hpp>
#include <vine/math/Rect2.hpp>
#include <vine/math/Vector2.hpp>

VN_APPFWGUI_NS_BEGIN

using Rect  = math::Rect2i;
using Point = math::Point2i;
using Size  = math::Vec2i;

enum class DockAreas
{
    None   = 0,
    Left   = 1,
    Top    = 2,
    Right  = 4,
    Bottom = 8
};
VN_ENABLE_ENUM_FLAGS(DockAreas);

enum class DockFeatures
{
    None     = 0,
    Closable = 1,
    All      = Closable
};
VN_ENABLE_ENUM_FLAGS(DockFeatures);

enum class StartupPosition
{
    Manual,
    CenterScreen
};

enum class WindowState
{
    Normal,
    Minimized,
    Maximized,
};

/**
 * @brief Well-known ribbon tab roles: what a tab is for, not which tab it is.
 *
 * @details
 * - A role is a host-independent anchor for plugins; the tab id stays the identity.
 * - Values are part of the plugin ABI: they are never renumbered; new roles take a new value
 *   between the existing ones or above UserRole.
 * - Custom host roles start at UserRole + n and are placed in the custom zone (before the
 *   terminal block), unless a placement says otherwise.
 * - None means "no semantic role": such a tab is only reachable by id.
 */
enum class RibbonTabRole : std::uint16_t
{
    None     = 0,
    Home     = 100,
    View     = 110,
    Tools    = 120,
    Plugins  = 200,
    Help     = 1000,
    UserRole = 0x8000,
};

/**
 * @brief Placement policy flags of a ribbon tab.
 *
 * @details
 * - Terminal keeps the tab at the end of the bar: no other tab may be placed after it.
 *   The standard catalog marks the Help tab this way.
 * - The flag is explicit on purpose: a role does not imply a policy.
 */
enum class RibbonTabPolicy
{
    None     = 0,
    Terminal = 1,
};
VN_ENABLE_ENUM_FLAGS(RibbonTabPolicy);

/**
 * @brief Where a contribution is placed relative to its siblings.
 *
 * @details
 * - One vocabulary for both hierarchies: ribbon tabs (and groups, which only use Before/After) and Backstage pages.
 * - The anchor is deliberately not part of this type: a tab anchors on a RibbonTabRole, a page on a BackstagePageRole,
 *   so each placement type carries its own anchor field and cannot be given the wrong kind of role.
 */
enum class PlacementWhere
{
    Append, ///< Add at the end of the non-terminal block.
    Before, ///< Add before the anchor.
    After,  ///< Add after the anchor.
};

/**
 * @brief Well-known Backstage page roles: what a page is for, not which page it is.
 *
 * @details
 * - A role is a host-independent anchor for plugins; the page id stays the identity.
 * - Values follow the ribbon's rule: they are never renumbered, and custom host roles start at UserRole.
 * - None means "no semantic role": such a page is reachable by id only.
 * - The page order is still decided by placement; a role only orders role-bearing pages among themselves.
 * - Plugins and Help mirror the ribbon's tab roles: the shell's own pages use them, so a plugin can anchor
 *   next to "before the help page" without knowing its id.
 */
enum class BackstagePageRole : std::uint16_t
{
    None     = 0,
    Home     = 100,
    New      = 110,
    Open     = 120,
    Info     = 200,
    Print    = 300,
    Export   = 310,
    Plugins  = 400,
    Help     = 500,
    Account  = 800,
    Options  = 900,
    About    = 950,
    Close    = 1000,
    UserRole = 0x8000,
};

/**
 * @brief Placement policy flags of a Backstage page.
 *
 * @details
 * - Terminal keeps the page at the end of the navigation: no other page may be placed after it.
 *   The standard catalog marks the Close page this way.
 * - Explicit on purpose, like RibbonTabPolicy: a role does not imply a policy.
 */
enum class BackstagePagePolicy
{
    None     = 0,
    Terminal = 1,
};
VN_ENABLE_ENUM_FLAGS(BackstagePagePolicy);

/**
 * @brief Ribbon button display style (arrangement of icon and text).
 */
enum class RibbonButtonStyle
{
    IconOnly,       // Icon only
    TextOnly,       // Text only
    TextBesideIcon, // Text to the right of the icon
    TextUnderIcon,  // Text below the icon
};

/**
 * @brief Ribbon item size (maps to SARibbon panel row proportion RowProportion).
 *
 * Used for RibbonButton sizes and for the row proportion of arbitrary controls
 * added via RibbonGroup::addControl.
 *
 * @note SARibbon has only Large/Small button types; Medium applies only as a
 * panel row proportion (two Medium items side by side occupy two rows in
 * three-row mode), while the button itself renders as a small button.
 */
enum class RibbonItemSize
{
    Large,  // Large: full row, icon on top and text below
    Medium, // Medium: two side by side occupy two rows in three-row mode
    Small,  // Small: one row (normal toolbar button)
};

/**
 * @brief Ribbon panel layout mode (maps to SARibbonPanel::PanelLayoutMode).
 */
enum class RibbonPanelLayoutMode
{
    ThreeRow,  // Three-row mode (Office style, default)
    TwoRow,    // Two-row mode (WPS compact style)
    SingleRow, // Single-row mode: all buttons act as Small, titles hidden by default
};

/**
 * @brief Ribbon global style: row count x loose/compact (maps to
 * SARibbonBar::RibbonStyles).
 */
enum class RibbonStyle
{
    ThreeRowLoose,    // Three rows, loose
    ThreeRowCompact,  // Three rows, compact
    TwoRowLoose,      // Two rows, loose
    TwoRowCompact,    // Two rows, compact (WPS style)
    SingleRowLoose,   // Single row, loose
    SingleRowCompact, // Single row, compact
};

VN_APPFWGUI_NS_END
