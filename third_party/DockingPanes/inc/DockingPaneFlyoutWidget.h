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

#ifndef DOCKINGPANEFLYOUTWIDGET_H
#define DOCKINGPANEFLYOUTWIDGET_H

#include <QWidget>

class QGridLayout;

class DockingPaneContainer;
class DockingToolButton;
class DockingPaneTitleWidget;

/**
 * @brief Popup window (flyout) for an auto-hidden (Pinned) pane.
 *
 * Pops out from the corresponding edge when the edge auto-hide button is clicked or
 * hovered; it has a draggable title bar and a 6px wide resize edge. Focus loss
 * (onFocusChanged) or beginDrag/endDrag notifies the manager to close itself.
 *
 * @note Notes:
 *  - Relies on QCursor::pos() to compute the cursor position, which fails under
 *    Wayland (the host forces xcb to avoid this).
 *  - When opened without focus, the autoHideFlyout signal fires after 1s, but it is
 *    currently not connected (auto-hide only relies on focus loss).
 *  - Ownership belongs to DockingPaneManager/container (managed via QPointer); do not delete it yourself.
 */
class DockingPaneFlyoutWidget : public QWidget {
    Q_OBJECT

  public:
    enum FlyoutPosition
    {
        Left,
        Right,
        Top,
        Bottom
    };

  public:
    /**
     * @brief Constructs a flyout.
     * @param hasFocus Whether to take focus immediately.
     * @param container The owning container (used for the flyout size / restoring the client area).
     * @param pane      The child pane being popped out.
     * @param pos       Popup direction.
     * @param widget    The client-area widget to show.
     * @param parent
     */
    explicit DockingPaneFlyoutWidget(bool                  hasFocus,
                                     DockingPaneContainer* container,
                                     DockingPaneContainer* pane,
                                     FlyoutPosition        pos,
                                     QWidget*              widget,
                                     QWidget*              parent = nullptr);
    ~DockingPaneFlyoutWidget() override;

  public:
    /**
     * @brief Returns the client-area widget to the owning container (called when closing/dragging).
     */
    void restorePaneWidget();

    /**
     * @brief The child pane currently popped out.
     */
    DockingPaneContainer* pane();

    /**
     * @brief Begins dragging out (hides itself and returns the client area to the container).
     */
    void beginDrag();

    /**
     * @brief Ends dragging out (emits flyoutFocusLost to notify the manager to clean up).
     */
    void endDrag();

    /**
     * @brief The client-area widget.
     */
    QWidget* clientWidget();

    /**
     * @brief Visible content rectangle (the area after removing the 5~6px resize edge).
     */
    QRect paneRect();

  Q_SIGNALS:
    void unpinContainer(); // The pin button was clicked.
    void closeContainer(); // The close button was clicked.
    void startDragFlyoutTitle(QPoint pos);
    void endDragFlyoutTitle(QPoint pos);
    void moveDragFlyoutTitle(QPoint pos);
    void flyoutFocusLost(); // Focus lost / drag ended (the manager cleans up accordingly).
    void autoHideFlyout();  // Auto-hide after a no-focus timeout (currently not connected).

  public:
    bool eventFilter(QObject* obj, QEvent* event) override;

  protected:
    void paintEvent(QPaintEvent* event) override;
    void changeEvent(QEvent* event) override;
    void closeEvent(QCloseEvent* event) override;
    void resizeEvent(QResizeEvent* event) override;

    void mouseMoveEvent(QMouseEvent* event) override;
    void mouseReleaseEvent(QMouseEvent* event) override;
    void mousePressEvent(QMouseEvent* event) override;

    void enterEvent(QEnterEvent* event) override;
    void leaveEvent(QEvent* event) override;

  private Q_SLOTS:
    void onFocusChanged(QWidget* old, QWidget* now);

  private:
    void setActivePane(bool active);
    void setPositionAndSize();
    void updateCursor();
    void autoHideTimeout();

  private:
    DockingPaneContainer*   m_pane;
    DockingPaneContainer*   m_container;
    QWidget*                m_clientWidget;
    DockingPaneTitleWidget* m_titleWidget;
    QGridLayout*            m_clientLayout;

    QWidget*           m_headerWidget;
    DockingToolButton* m_closeButton;
    DockingToolButton* m_pinButton;
    QPoint             m_initialPos;
    FlyoutPosition     m_pos;
    int                m_size;
    bool               m_isActive;
    bool               m_dragMode;
    bool               m_resizeMode;
};

#endif // DOCKINGPANEFLYOUTWIDGET_H
