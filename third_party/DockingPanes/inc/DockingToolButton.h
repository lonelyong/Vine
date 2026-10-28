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

#ifndef DOCKINGTOOLBUTTON_H
#define DOCKINGTOOLBUTTON_H

#include <QPushButton>

/**
 * @brief Small close/pin button on the title bar (bitmap + palette tinting).
 *
 * Loads the corresponding PNG from the resources by state (active/inactive, close/pin/unpin);
 * inactive bitmaps are re-tinted with the palette's WindowText colour to fit dark themes.
 * A semi-transparent white highlight block is drawn on hover.
 *
 * @note Derives from QPushButton but completely overrides the painting, only reusing its
 * click/hover semantics; maximum width is 16px, so the hit area is small.
 */
class DockingToolButton : public QPushButton {
    Q_OBJECT

  public:
    /// Button form (icon + active state combination).
    enum ButtonType
    {
        closeButtonActive,   // Close icon, active state (white).
        closeButtonInactive, // Close icon, inactive state (tinted).
        pinButtonActive,     // Pin icon, active state.
        pinButtonInactive,   // Pin icon, inactive state.
        unpinButtonActive,   // Unpin icon (rotated 90°).
        unpinButtonInactive  // Unpin icon, inactive state.
    };

  public:
    explicit DockingToolButton(ButtonType type, QWidget* parent = nullptr);
    ~DockingToolButton() override;

  public:
    /**
     * @brief Switches the button form (active/inactive, close/pin).
     */
    void setButton(ButtonType type);

  protected:
    void paintEvent(QPaintEvent* event) override;
    void changeEvent(QEvent* event) override;
    void enterEvent(QEnterEvent* event) override;
    void leaveEvent(QEvent* event) override;

  private:
    ButtonType m_buttonType;
    bool       m_highlight; // Hover highlight.
};

#endif // DOCKINGTOOLBUTTON_H
