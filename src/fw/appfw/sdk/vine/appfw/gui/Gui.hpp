#pragma once

#include <vine/appfw/appfw_global.hpp>

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
 * @brief Ribbon button display style (arrangement of icon and text).
 */
enum class RibbonButtonStyle
{
    IconOnly,       ///< Icon only
    TextOnly,       ///< Text only
    TextBesideIcon, ///< Text to the right of the icon
    TextUnderIcon,  ///< Text below the icon
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
    Large,  ///< Large: full row, icon on top and text below
    Medium, ///< Medium: two side by side occupy two rows in three-row mode
    Small,  ///< Small: one row (normal toolbar button)
};

/**
 * @brief Ribbon panel layout mode (maps to SARibbonPanel::PanelLayoutMode).
 */
enum class RibbonPanelLayoutMode
{
    ThreeRow,  ///< Three-row mode (Office style, default)
    TwoRow,    ///< Two-row mode (WPS compact style)
    SingleRow, ///< Single-row mode: all buttons act as Small, titles hidden by default
};

/**
 * @brief Ribbon global style: row count x loose/compact (maps to
 * SARibbonBar::RibbonStyles).
 */
enum class RibbonStyle
{
    ThreeRowLoose,    ///< Three rows, loose
    ThreeRowCompact,  ///< Three rows, compact
    TwoRowLoose,      ///< Two rows, loose
    TwoRowCompact,    ///< Two rows, compact (WPS style)
    SingleRowLoose,   ///< Single row, loose
    SingleRowCompact, ///< Single row, compact
};

VN_APPFWGUI_NS_END
