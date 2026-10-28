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

#ifndef DOCKINGPANEBASE_H
#define DOCKINGPANEBASE_H

#include <QWidget>

class QDomNode;

class DockingPaneManager;

/**
 * @brief Abstract base class for all nodes of the docking system.
 *
 * Every element of the docking tree (single pane DockingPaneContainer, tab group
 * DockingPaneTabbedContainer, splitter DockingPaneSplitterContainer, central client
 * DockingPaneClient) derives from this class. It provides a uniform identity
 * (id/name), state (State) and host data (userData).
 *
 * @note Known design flaw: setState() only assigns m_state without any side effects;
 * the real state transitions (hide/show/pin/float, etc.) must be performed by the
 * concrete subclasses or the corresponding DockingPaneManager operations; callers
 * must not rely on setState to produce UI changes.
 */
class DockingPaneBase : public QWidget {
    Q_OBJECT

  public:
    /**
     * @brief Pane lifecycle state.
     *
     * Hidden   Closed/hidden (may still remain in the tree);
     * Docked   Docked in the tree;
     * Floating Floating independent window;
     * Pinned   Auto-hidden (has an edge button);
     * Tabbed   A tab page of a DockingPaneTabbedContainer.
     */
    enum State
    {
        Hidden,
        Docked,
        Floating,
        Pinned,
        Tabbed
    };

    friend class DockingPaneManager;

  public:
    explicit DockingPaneBase(QWidget* parent = nullptr);
    ~DockingPaneBase() override;

  public:
    /**
     * @brief Display name (pane title / source of the Tab text).
     */
    const QString& name();

    /**
     * @brief Unique identifier (used for layout restore and lookups).
     */
    const QString& id();

    /**
     * @brief Returns the owning DockingPaneManager.
     * @note nullptr when not added to a manager.
     */
    DockingPaneManager* dockingManager();

    /**
     * @brief Sets host custom data (e.g. a pointer to the host's UIElement wrapper object).
     * @note Raw void*; the caller owns its lifetime and should clear it before deleting the object.
     */
    void setUserData(void* data)
    {
        m_userData = data;
    }

    void* userData() const
    {
        return m_userData;
    }

    /**
     * @brief Current state. @see State
     */
    virtual State state();

    /**
     * @brief Sets the state; for internal use only.
     * @note Merely assigns the value without UI side effects (see the class description).
     * @note For derived classes and DockingPaneManager (friend) only; hosts should change state via
     * the manager's high-level operations (dockPane/closePane/hidePane, etc.).
     */
    virtual void setState(State state);

    /**
     * @brief Writes this node and its children into the parent XML node (layout save).
     * @param parentNode
     * @param includeGeometry Whether to include geometry information (used for floating panes).
     */
    virtual void saveLayout(QDomNode* parentNode, bool includeGeometry = false);

  protected:
    virtual void setName(const QString& name);
    virtual void setId(const QString& id);

  protected:
    bool                m_isClient;           // Whether this is the central client area.
    DockingPaneManager* m_dockingManager;     // The owning manager.
    State               m_state;              // Current state.
    QString             m_name;               // Display name.
    QString             m_id;                 // Unique identifier.
    void*               m_userData = nullptr; // Host custom data.
};

#endif // DOCKINGPANEBASE_H
