/*
 * This file is part of DockingPanes. (https://github.com/KestrelRadarSensors/dockingpanes)
 *
 * (C) 2020 Kestrel Radar Sensors (https://www.kestrelradarsensors.com)
 *
 * DockingPanes is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * DockingPanes is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.

 * You should have received a copy of the GNU General Public License
 * along with DockingPanes.  If not, see <https://www.gnu.org/licenses/>.
 */

#ifndef DOCKINGPANETITLEWIDGET_H
#define DOCKINGPANETITLEWIDGET_H

#include <QWidget>

class QPoint;

/**
 * @brief Pane title bar: draws the title text + dotted pattern on the right, and handles dragging.
 *
 * On press it calls grabMouse() to keep receiving events for the whole drag (including when the
 * cursor moves over the top-level docking indicators). The drag events are forwarded to the
 * container through three signals.
 *
 * @note Drag mechanism notes:
 *  - After a docked pane drags past the threshold, floatPane() rebuilds the native window and the
 *    grab is lost; call reacquireGrab() after the rebuild to re-grab.
 *  - When a flyout drag turns floating, hiding the flyout releases the grab; use takeGrab() to take it over.
 *  - Mouse release is only handled for the left button, and m_grabbing records the grab state to avoid a duplicate releaseMouse().
 *  - A click (press + release without a single move) is not a drag: it reports the start but not the
 *    end, so the container's docking-target scan does not run for it.
 */
class DockingPaneTitleWidget : public QWidget {
    Q_OBJECT

  public:
    /**
     * @brief Constructs a title bar.
     * @param text Initial title text.
     * @param parent
     */
    explicit DockingPaneTitleWidget(const QString& text = QString(), QWidget* parent = nullptr);
    ~DockingPaneTitleWidget() override;

  public:
    /**
     * @brief Sets the title text.
     */
    void setText(const QString& text);

    /**
     * @brief Sets the active state (affects the foreground/pattern colours).
     */
    void setActive(bool active);

    /**
     * @brief Re-acquires the mouse grab (called after a docked pane is rebuilt as floating).
     * @note Only re-grabs with grabMouse() while still in the grabbing state (m_grabbing).
     */
    void reacquireGrab();

    /**
     * @brief Takes over the drag: grabs the mouse and enters the grabbing state.
     *        Used when the flyout is hidden mid-drag and this title continues the drag.
     */
    void takeGrab();

  Q_SIGNALS:
    void titleBarStartMove(QPoint pos); // Title pressed (global coordinates).
    void titleBarEndMove(QPoint pos);   // Title released (global coordinates).
    void titleBarMoved(QPoint pos);     // Drag move (global coordinates).

  protected:
    void paintEvent(QPaintEvent* event) override;
    void changeEvent(QEvent* event) override;
    void resizeEvent(QResizeEvent* event) override;
    void mousePressEvent(QMouseEvent* event) override;
    void mouseMoveEvent(QMouseEvent* event) override;
    void mouseReleaseEvent(QMouseEvent* event) override;

  private:
    void drawPattern(QPainter* p, int x, int y, int w, int h);
    void onFocusChanged(QWidget* old, QWidget* now);

  private:
    QString m_text;
    bool    m_active;
    bool    m_grabbing = false; // Whether the mouse grab is currently held.
    bool    m_moved    = false; // Whether the pointer moved since the press (a click is not a drag).
};

#endif // DOCKINGPANETITLEWIDGET_H
