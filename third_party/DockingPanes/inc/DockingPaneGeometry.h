#pragma once

#include <QGuiApplication>
#include <QRect>
#include <QScreen>
#include <QWidget>

/**
 * @brief Geometry rules shared by every floating-pane code path.
 *
 * A floating pane can be created from three drag sources (title bar, tab strip,
 * flyout) plus the public API, so the rules - a floor for the size and keeping the
 * title bar reachable - live here instead of being repeated per call site.
 */
namespace DockingPaneGeometry
{

/// Floor for a floating pane: below this it is hard to grab again.
constexpr int minimumWidth = 120;
/// Floor for a floating pane: below this it is hard to grab again.
constexpr int minimumHeight = 80;
/// How much of the title bar must stay inside the screen for the pane to be grabbable.
constexpr int titleBarMargin = 24;

/**
 * @brief Floors the size of a floating pane.
 *
 * @param pane Pane to constrain.
 */
inline void applyFloatingMinimum(QWidget* pane)
{
    pane->setMinimumSize(qMax(pane->minimumWidth(), minimumWidth), qMax(pane->minimumHeight(), minimumHeight));
}

/**
 * @brief Drops the floating floor, so a docked pane follows its splitter again.
 *
 * @param pane Pane to release.
 */
inline void clearFloatingMinimum(QWidget* pane)
{
    pane->setMinimumSize(0, 0);
}

/**
 * @brief Moves a floating pane so its title bar stays reachable.
 *
 * A fast drag can drop the pane outside the screen, where its title bar cannot be
 * grabbed any more (and the pane is then unrecoverable). This pulls it back so at
 * least titleBarMargin pixels of the title bar remain inside the available area.
 *
 * @param pane        Pane to constrain.
 * @param titleHeight Height of the pane's title bar.
 * @return true when the pane had to be moved.
 */
inline bool keepTitleBarReachable(QWidget* pane, int titleHeight)
{
    QScreen* screen = pane->screen();
    if (screen == nullptr) {
        return false;
    }

    const QRect  available = screen->availableGeometry();
    const QRect  frame     = pane->frameGeometry();
    const QPoint before    = frame.topLeft();

    // Horizontally the pane may hang off the screen as long as the margin is visible;
    // vertically the title bar has to stay inside.
    const int min_x = available.left() - frame.width() + titleBarMargin;
    const int max_x = available.right() - titleBarMargin;
    const int max_y = available.bottom() - qMax(titleBarMargin, titleHeight);

    QPoint pos(before);
    pos.setX(qBound(qMin(min_x, max_x), pos.x(), qMax(min_x, max_x)));
    pos.setY(qBound(available.top(), pos.y(), qMax(available.top(), max_y)));

    if (pos == before) {
        return false;
    }

    pane->move(pos);
    return true;
}

} // namespace DockingPaneGeometry
