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

#ifndef DOCKINGFRAMECENTRESTICKER_H
#define DOCKINGFRAMECENTRESTICKER_H

#include <QMap>
#include <QWidget>

class QPaintEvent;

class DockingFrameFrameSticker;

/**
 * @brief Group of docking indicators (centre + four edge icons + corner frame stickers).
 *
 * Shown at the centre of the current candidate pane while a floating pane is being
 * dragged: the centre/edge icons indicate which side of the pane to dock to, while
 * the corner frame stickers indicate docking to the whole docking-area edge.
 * Hit testing is done by getHit(), which drives DockingPaneManager's docking decision.
 *
 * @note Implementation details:
 *  - It is a Qt::ToolTip topmost frameless window with WA_TranslucentBackground.
 *  - m_rcCentre is legacy dead code: the centre area is actually hit via m_rcTab (Tab icon),
 *    so the centre cannot be docked when the tab is not visible.
 *  - m_rcBottom / the frame corners are positioned with the Top/Left sizes, a copy-paste
 *    leftover; harmless because all icons currently share the same size.
 */
class DockingFrameStickers : public QWidget {
    Q_OBJECT

  public:
    /// Hit result: dock to a pane side / docking-area edge / Tab.
    enum DockingPosition
    {
        paneLeft,    // Dock to the left side of the pane.
        paneRight,   // Dock to the right side of the pane.
        paneTop,     // Dock to the top of the pane.
        paneBottom,  // Dock to the bottom of the pane.
        frameLeft,   // Dock to the left edge of the docking area.
        frameRight,  // Dock to the right edge of the docking area.
        frameTop,    // Dock to the top edge of the docking area.
        frameBottom, // Dock to the bottom edge of the docking area.
        tab          // Join the tab group.
    };

  public:
    explicit DockingFrameStickers(QWidget* parent = nullptr);
    ~DockingFrameStickers() override;

  public:
    /**
     * @brief Sets the docking-area (main window central area) rectangle used to position the corner frame stickers.
     */
    void setFrameRect(QRect rect);

    /**
     * @brief Updates each indicator's active state while dragging (the one under the cursor is highlighted).
     */
    void updateCursorPos(QPoint pos);

    /**
     * @brief Whether to show the centre Tab icon (shown when the candidate pane is a normal pane).
     */
    void setTabVisible(bool state);

    /**
     * @brief Hit testing.
     * @param pos     Global cursor position.
     * @param dockPos Output: the hit dock position.
     * @return true if any indicator is hit.
     */
    bool getHit(QPoint pos, DockingPosition* dockPos);

    /**
     * @brief Hit testing limited to the four docking-area edge stickers.
     *
     * They sit on the docking-area border, where no pane covers the cursor, so the manager
     * has to test them independently of the per-pane hit test.
     *
     * @param pos     Global cursor position.
     * @param dockPos Output: frameLeft / frameRight / frameTop / frameBottom.
     * @return true if an edge sticker is hit.
     */
    bool getFrameHit(QPoint pos, DockingPosition* dockPos);

  protected:
    void paintEvent(QPaintEvent* event) override;
    void hideEvent(QHideEvent* e) override;
    void showEvent(QShowEvent* e) override;

  private:
    enum StickerPosition
    {
        Centre,
        Left,
        Right,
        Top,
        Bottom,
        Tab,
    };

    void initializeStickersImages();

  private:
    bool m_isActive;
    bool m_tabVisible;

    QRect m_rcCentre;
    QRect m_rcLeft;
    QRect m_rcRight;
    QRect m_rcTop;
    QRect m_rcBottom;
    QRect m_rcTab;

    DockingFrameFrameSticker* m_frameLeftSticker;
    DockingFrameFrameSticker* m_frameRightSticker;
    DockingFrameFrameSticker* m_frameTopSticker;
    DockingFrameFrameSticker* m_frameBottomSticker;

    QMap<StickerPosition, QImage> m_activeStickers;
    QMap<StickerPosition, QImage> m_inactiveStickers;
};

#endif // DOCKINGFRAMECENTRESTICKER_H
