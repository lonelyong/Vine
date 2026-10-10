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

#ifndef DOCKINGPANEMANAGER_H
#define DOCKINGPANEMANAGER_H

#include <QWidget>

class QBoxLayout;
class QDomDocument;
class QDomNode;
class DockingPaneBase;
class DockingPaneSplitterContainer;
class DockingPaneManagerPrivate;
class DockingFrameStickers;
class DockingTargetWidget;
class DockingPaneTitleWidget;
class DockingPaneFlyoutWidget;
class DockAutoHideButton;

/**
 * @brief Core manager of the docking system.
 *
 * Owns the whole docking tree (root node m_rootPane), the central client area
 * (DockingPaneClient), the four auto-hide button strips, the drag-and-drop indicators
 * (DockingFrameStickers / DockingTargetWidget) and the current flyout. It handles pane
 * creation, docking, closing, hide/show, floating, auto-hiding, and layout save/restore.
 * The host embeds the top-level widget returned by widget() into the main window.
 *
 * @note Notes:
 *  - The four auto-hide strips are only created after widget() is called; calling
 *    saveLayout() / updateAutohideButton() / removePinnedButton() before that dereferences null pointers.
 *  - closePane() routes "committed tab children" (state()==Tabbed) to the owning
 *    DockingPaneTabbedContainer::closePane() to remove the tab; during applyLayout() restore
 *    (m_closingAll) it does not route, to avoid close callbacks and premature tab teardown.
 *  - createPane() with a duplicate id overwrites the existing entry in m_dockingPaneMap.
 *  - This class does not delete the docking tree (m_dockingWidget / m_thisWidget / m_clientPane);
 *    they belong to the host (e.g. via setCentralWidget); the destructor only cleans up the
 *    internal indicators and the flyout.
 */
class DockingPaneManager : public QObject {
    Q_OBJECT

  public:
    /**
     * @brief Dock position.
     *
     * dockTab means joining a pane's tab group (or merging into a new tab group).
     */
    enum DockPosition
    {
        dockLeft,
        dockRight,
        dockTop,
        dockBottom,
        dockFloat,
        dockTab
    };

  public:
    /**
     * @brief Constructs the manager.
     *
     * Only creates the internal structure (root/client panes and indicators);
     * the auto-hide strips are created by widget().
     */
    DockingPaneManager();

    /**
     * @brief Destructor. Releases the internal indicators and the current flyout; the docking tree belongs to the host.
     */
    ~DockingPaneManager() override;

  public:
    /**
     * @brief Returns the top-level QWidget hosting the docking system (lazily created).
     *
     * The first call creates the four auto-hide button strips and installs the event filters.
     * @return A widget that can be embedded into the main window (e.g. via QMainWindow::setCentralWidget).
     */
    QWidget* widget();

    /**
     * @brief Sets the central client-area widget.
     * @param widget The client-area content (must not be nullptr).
     * @return The client pane (DockingPaneClient).
     */
    DockingPaneBase* setClientWidget(QWidget* widget);

    /**
     * @brief Creates a pane and docks it (or floats it) at the given position.
     * @param id           Unique identifier (a duplicate overwrites the map).
     * @param title        Title.
     * @param widget       Client-area content.
     * @param initialSize  Initial size.
     * @param dockPosition Dock position; dockFloat floats and shows it immediately.
     * @param neighbourPane Neighbour pane (used for dockTab / relative docking).
     * @return The newly created DockingPaneContainer.
     */
    DockingPaneBase* createPane(const QString&                   id,
                                const QString&                   title,
                                QWidget*                         widget,
                                QSize                            initialSize,
                                DockingPaneManager::DockPosition dockPosition,
                                DockingPaneBase*                 neighbourPane = nullptr);

    /**
     * @brief Closes the pane with the given id.
     * @see closePane(DockingPaneBase*)
     */
    void closePane(const QString& id);

    /**
     * @brief Closes a pane: hides it, sets it to Hidden and detaches it from the docking tree.
     *
     * @note Tab children are routed to their tabbed container (see the class description);
     * only closes the pane — it does not delete the object nor remove it from m_dockingPaneMap.
     */
    void closePane(DockingPaneBase* dockingPane);

    /**
     * @brief Hides a pane and switches it to auto-hide (Pinned): creates an edge button.
     * @note Requires the pane to be inside a splitter; silently a no-op for floating panes.
     */
    void hidePane(DockingPaneBase* dockingPane);

    /**
     * @brief Restores the visibility of a pane according to its state.
     * @note Hidden → re-float; Pinned → open the flyout; Docked/Floating → activate.
     */
    void showPane(DockingPaneBase* dockingPane);

    /**
     * @brief Removes a pane from the manager's bookkeeping and schedules its deletion (deleteLater).
     */
    void deletePane(DockingPaneBase* pane);

    /**
     * @brief Unpins a pane: restores its visibility and removes the auto-hide button.
     */
    void unpinPane(DockingPaneBase* pane);

    /**
     * @brief Closes a pinned pane and its flyout.
     */
    void closePinnedPane(DockingPaneBase* pane);

    /**
     * @brief Opens the flyout for an auto-hide button (the auto-hidden popup pane).
     * @note Returns immediately when the same flyout is already open for the same pane.
     */
    void openFlyout(DockAutoHideButton* button);

    /**
     * @brief Replaces oldPane with newPane in the docking tree (e.g. collapsing a tab group into a single pane).
     */
    void replacePane(DockingPaneBase* oldPane, DockingPaneBase* newPane);

    /**
     * @brief Updates the container/pane pointed to by auto-hide buttons after a docking operation.
     */
    void updateAutohideButton(DockingPaneBase* oldContainer, DockingPaneBase* oldPane, DockingPaneBase* newContainer, DockingPaneBase* newPane);

    /**
     * @brief Core docking logic.
     *
     * For dockTab, joins neighbourPane's tab group or creates a new one; other positions
     * create a new splitter and insert it into the docking tree.
     * @note Panes that are already tabs are first detached from their old group via closePane before joining the new one.
     * @return The container after docking completes.
     */
    DockingPaneBase* dockPane(DockingPaneBase* newPane, DockingPaneManager::DockPosition dockPosition, DockingPaneBase* neighbourPane);

    /**
     * @brief Serializes the current layout (docking tree + pinned + floating) as XML.
     * @note widget() must be called first to create the auto-hide strips, otherwise it crashes.
     */
    QString saveLayout(const QString& id);

    /**
     * @brief Restores the layout from XML.
     * @return Whether it succeeded; on failure it falls back to a client-only layout.
     */
    bool applyLayout(const QString& layout);

    /**
     * @brief Sets the main window; floating panes use it as their parent window.
     */
    void setMainWindow(QWidget* window);

    /**
     * @brief Returns the main window.
     */
    QWidget* mainWindow();

    /**
     * @brief Debugging: prints all panes and their states to qDebug.
     */
    void dumpPaneList();

    /**
     * @brief Returns the pane at the given index.
     */
    DockingPaneBase* pane(int index) const;

    /**
     * @brief Total number of panes.
     */
    int paneCount() const;

    /**
     * @brief Queries the pane's current dock position (computed dynamically).
     *
     * Docked → position relative to the client area; Tabbed → position of the owning tab group;
     * Floating/Pinned/Hidden → dockFloat.
     * @note For the host to query live (e.g. dockArea); not for deciding a docking target.
     */
    DockPosition dockPositionOf(DockingPaneBase* pane);

  public:
    /**
     * @brief Updates the docking indicators while a floating pane is dragged (called by the container's drag callbacks).
     */
    void floatingPaneMoved(DockingPaneBase* pane, QPoint cursorPos);

    /**
     * @brief Drag end: re-runs hit testing at the release position and docks.
     */
    void floatingPaneEndMove(DockingPaneBase* pane, QPoint cursorPos);

    /**
     * @brief Drag start: initializes the indicators.
     */
    void floatingPaneStartMove(DockingPaneBase* pane, QPoint cursorPos);

    /**
     * @brief Removes the auto-hide buttons of the given container (all of them when dockingPane is null).
     */
    void removePinnedButton(DockingPaneBase* dockingPaneContainer, DockingPaneBase* dockingPane = nullptr);

  private:
    friend class DockingPaneContainer;
    friend class DockingPaneTabbedContainer;

    DockPosition getClientPaneDirection(DockingPaneBase* dockingPane);
    bool         containsPane(QWidget* widget, QWidget* child);
    void         saveFloatingState(QDomNode* parentNode);

    DockingPaneBase* restoreLayout(const QDomNode& node);
    void             savePinnedState(QDomNode* parentNode, QBoxLayout* layout);
    void             restorePinnedPanes(QDomNode* node);
    void             reparentPane(DockingPaneSplitterContainer* previousParentSplitter, DockingPaneBase* dockingPane);
    void             restoreFloatingPanes(QDomNode* node);

  public:
    bool eventFilter(QObject* obj, QEvent* event) override;

  private:
    void             setWidget(QWidget* widget);
    DockingPaneBase* getDockingParent(QWidget* widget);

    void updateAllSplitters(DockingPaneSplitterContainer* parentSplitter = nullptr, bool* containsClient = nullptr);
    bool updateFloatingPane(DockingPaneBase* currentPane, QPoint cursorPos);

    /**
     * @brief Handles a docking-area edge sticker hit (no pane covers the cursor there).
     * @param cursorPos Global cursor position.
     * @return true when an edge sticker was hit and the target was recorded.
     */
    bool handleFrameEdgeHit(QPoint cursorPos);

  private Q_SLOTS:
    void onAutoDockButtonClicked();
    void onFlyoutFocusLost();

  protected:
    DockingPaneManagerPrivate* const d_ptr;

  private:
    Q_DECLARE_PRIVATE(DockingPaneManager)
};

#endif // DOCKINGPANEMANAGER_H
