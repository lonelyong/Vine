#pragma once

#include <QFrame>
#include <QHeaderView>
#include <QString>
#include <QTableWidget>

#include <vine/appfw/appfw_global.hpp>

V_APPFWGUI_NS_BEGIN

namespace detail
{

/**
 * @brief Blends a table into the surface it sits on.
 *
 * A table that draws its own frame next to a border the surface already has (a tab
 * pane) reads as two parallel lines with a sliver of surface in between, and its
 * opaque body cuts a rectangle out of the surrounding colour.
 * This drops the frame, makes the body transparent so the surface shows through and
 * gives the header the surface's own background, leaving only the grid, the header
 * rule and the selection drawn on top.
 * The row-number column goes as well: it is noise for a read-only list.
 *
 * Every colour is a palette lookup resolved by the style sheet while painting, so
 * the table follows a light/dark theme change instead of pinning colours.
 *
 * @param table Table to blend; the caller keeps ownership.
 */
inline void blendIntoSurface(QTableWidget* table)
{
    table->setFrameShape(QFrame::NoFrame);
    table->verticalHeader()->setVisible(false);
    table->setStyleSheet(QStringLiteral("QTableWidget {"
                                        " background-color: transparent;"
                                        " gridline-color: palette(mid);"
                                        " }"
                                        "QTableWidget::item:selected {"
                                        " background-color: palette(highlight);"
                                        " color: palette(highlighted-text);"
                                        " }"
                                        "QHeaderView::section {"
                                        " background-color: palette(window);"
                                        " border: none;"
                                        " border-bottom: 1px solid palette(mid);"
                                        " padding: 4px;"
                                        " }"));
}

} // namespace detail

V_APPFWGUI_NS_END
