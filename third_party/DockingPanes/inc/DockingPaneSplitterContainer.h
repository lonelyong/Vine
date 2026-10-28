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

#ifndef DOCKINGPANESPLITTERCONTAINER_H
#define DOCKINGPANESPLITTERCONTAINER_H

#include "DockingPaneBase.h"

class QSplitter;

class DockingPaneManager;

/**
 * @brief Splitter node in the docking tree (wraps a QSplitter).
 *
 * When docking, a splitter is created according to the direction and places the neighbour
 * and the docked pane side by side. With two children it is an intermediate node of the
 * binary-tree docking tree; sub-splitters without a client branch shrink first (updateAllSplitters).
 *
 * @note Split direction maps to dock position: splitVertical = stacked vertically,
 * splitHorizontal = side by side horizontally (the naming is counter-intuitive, mind the distinction).
 */
class DockingPaneSplitterContainer : public DockingPaneBase {
    Q_OBJECT

    friend class DockingPaneManager;

  public:
    enum SplitterDirection
    {
        splitVertical,  // QSplitter vertical orientation (children stacked vertically).
        splitHorizontal // QSplitter horizontal orientation (children side by side).
    };

  public:
    explicit DockingPaneSplitterContainer(QWidget* parent = nullptr, SplitterDirection direction = splitVertical);
    ~DockingPaneSplitterContainer() override;

  public:
    /**
     * @brief Current split direction.
     */
    SplitterDirection direction();

    void saveLayout(QDomNode* parentNode, bool includeGeometry = false) override;

  private:
    QSplitter* m_splitterWidget; // The actual splitter widget.
};

#endif // DOCKINGPANESPLITTERCONTAINER_H
