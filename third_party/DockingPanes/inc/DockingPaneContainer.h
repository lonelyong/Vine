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

#ifndef DOCKINGPANECONTAINER_H
#define DOCKINGPANECONTAINER_H

#include "DockingPaneBase.h"
#include "DockingPaneFlyoutWidget.h"
#include <QPointer>

class QGridLayout;

class DockingPaneFlyoutWidget;
class DockingPaneGlow;
class DockingPaneManager;
class DockingPaneTitleWidget;
class DockingToolButton;

/**
 * @brief A single dockable pane: title bar + close/pin buttons + client area.
 *
 * The basic leaf node of the docking tree. It can be in one of four forms:
 * docked, floating, auto-hidden or tabbed (see DockingPaneBase::State).
 * When floating it is a Qt::ToolTip frameless topmost window with an edge
 * resize glow (DockingPaneGlow).
 *
 * @note Notes:
 *  - The default constructor DockingPaneContainer(QWidget*) is only for derived classes
 *    (DockingPaneTabbedContainer); its members are uninitialized, so calling its methods directly crashes.
 *  - As a tab child, only the clientWidget is placed into the tab group; the container
 *    object itself is not reparented. Closing/moving must go through DockingPaneManager::closePane()
 *    routing or DockingPaneTabbedContainer::closePane(); do not operate on it directly.
 *  - m_flyoutWidget is a QPointer that is automatically cleared once the manager deletes the flyout.
 *  - Dragging the title bar more than 5px turns the pane floating (floatPane), which rebuilds the
 *    window and loses the mouse grab; re-grab with reacquireGrab()/takeGrab().
 */
class DockingPaneContainer : public DockingPaneBase {
    Q_OBJECT

  public:
    /**
     * @brief Auto-hide popup direction (which edge the flyout pops out from).
     */
    enum FlyoutPosition
    {
        Left,
        Right,
        Top,
        Bottom
    };

    friend class DockingPaneManager;

  public:
    /**
     * @brief Full constructor (title, id, parent window and client area).
     */
    explicit DockingPaneContainer(const QString& title, const QString& id, QWidget* parent = nullptr, QWidget* clientWidget = nullptr);

    /**
     * @brief Default constructor; only for derived classes after initializing their own members.
     * @note This constructor does not create the title bar/buttons/client area; using it directly dereferences null pointers.
     */
    explicit DockingPaneContainer(QWidget* parent = nullptr);
    ~DockingPaneContainer() override;

  public:
    /**
     * @brief Turns the pane into a floating window (QRect overload; the argument is currently ignored and only used for the state switch).
     */
    void floatPane(QRect rect);

    /**
     * @brief Turns the pane into a floating window, offset by pos from its current position.
     *
     * Removes the pane from the docking tree via closePane() first, then floats it at the recorded global position.
     * @note The argument is a displacement, not a target: pass QPoint(0, 0) to keep the
     *       pane where it is.
     * @note Requires a valid dockingManager() and mainWindow() (otherwise it becomes a parentless top-level window).
     */
    void floatPane(QPoint offset);

    /**
     * @brief Height of the pane's title bar.
     *
     * Font dependent, so callers that need to place a floating pane relative to the
     * cursor use this instead of a constant.
     *
     * @return The title bar height in pixels, or 0 before the title bar exists.
     */
    int titleHeight() const;

    /**
     * @brief Opens the auto-hide flyout.
     * @param hasFocus Whether to take focus immediately (otherwise auto-hide timeout starts after 1s).
     * @param parent
     * @param pos
     * @param pane     The child pane to pop out (a Tab container selects it by tab).
     * @return The newly created flyout widget.
     */
    virtual DockingPaneFlyoutWidget* openFlyout(bool hasFocus, QWidget* parent, FlyoutPosition pos, DockingPaneContainer* pane);

    /**
     * @brief Sets the state; shows the pin button and releases the glow when not Floating.
     */
    void setState(State state) override;

    /**
     * @brief Number of child panes (always 1 for a single pane; a Tab container returns the tab count).
     */
    virtual int getPaneCount();

    /**
     * @brief Returns the child pane at index (a single pane returns itself).
     */
    virtual DockingPaneContainer* getPane(int index);

    /**
     * @brief The current client-area widget.
     */
    QWidget* clientWidget();

    /**
     * @brief Sets the client-area widget.
     * @note When widget is nullptr the layout is merely cleared and the function returns (no crash).
     */
    virtual void setClientWidget(QWidget* widget);

    void saveLayout(QDomNode* parentNode, bool includeGeometry = false) override;

    /**
     * @brief Flyout size (defaults to 100x100 when unset).
     */
    QSize flyoutSize();

    /**
     * @brief Sets the flyout size (used to save the pane size when auto-hiding).
     */
    void setFlyoutSize(QSize flyoutSize);

    /**
     * @brief The edge resize glow object used when floating.
     */
    DockingPaneGlow* floatingGlow();

    /**
     * @brief Whether the pane is closable (only controls the visibility of the title-bar close button).
     * @note Does not prevent other close paths such as programmatic closePane()/removeDockPanel().
     */
    void setClosable(bool closable);
    bool isClosable() const;

    /// Close callback: returning false vetoes the close (lets the host intercept it).
    using CloseCallback = bool (*)(DockingPaneContainer*);

    /**
     * @brief Sets the close callback (for the host to install an onClosing interceptor).
     */
    void setCloseCallback(CloseCallback cb)
    {
        m_closeCallback = cb;
    }

    /**
     * @brief Invokes the close callback; treated as allowed when no callback is set.
     */
    bool invokeCloseCallback()
    {
        return m_closeCallback ? m_closeCallback(this) : true;
    }

    /**
     * @brief After a flyout drag turns floating and the flyout hides, the mouse grab is released;
     *        the target pane's title takes over so the drag can continue.
     */
    void continueDrag(QPoint pos);

  protected:
    void setName(const QString& name) override;

    /// Updates the title/button appearance when the active state changes (triggered by focus changes).
    void setActivePane(bool active);
    void paintEvent(QPaintEvent* event) override;
    void changeEvent(QEvent* event) override;

    virtual void onStartDragTitle(QPoint pos);
    virtual void onEndDragTitle(QPoint pos);
    virtual void onMoveDragTitle(QPoint pos);

    virtual void onStartDragFlyoutTitle(QPoint pos);
    virtual void onEndDragFlyoutTitle(QPoint pos);
    virtual void onMoveDragFlyoutTitle(QPoint pos);

    /// The title-bar close button was clicked (goes through the close callback, which may veto it).
    virtual void onCloseButtonClicked();
    /// The flyout's close button was clicked (closes the pinned pane).
    virtual void onCloseContainer();
    /// qApp focus changed: sets this pane's active state.
    virtual void onFocusChanged(QWidget* old, QWidget* now);
    /// The pin button was clicked: enters auto-hide via hidePane.
    virtual void onPinButtonClicked();
    /// The flyout's pin button was clicked: unpins.
    virtual void onUnpinContainer();

  protected:
    QWidget*     m_headerWidget = nullptr;
    QWidget*     m_clientWidget = nullptr;
    QGridLayout* m_clientLayout = nullptr;

    DockingToolButton* m_closeButton = nullptr;
    DockingToolButton* m_pinButton   = nullptr;

    bool   m_isActive = false;
    QPoint m_initialPos;

    DockingPaneTitleWidget* m_titleWidget            = nullptr;
    // QPointer so the container never keeps a dangling flyout pointer
    // after the manager has deleted the flyout (focus-lost path).
    QPointer<DockingPaneFlyoutWidget> m_flyoutWidget = nullptr;

    QSize m_flyoutSize;

    DockingPaneGlow* m_floatingGlow = nullptr;

    bool m_draggingFlyout = false;

    bool m_closable = true;

    CloseCallback m_closeCallback = nullptr;

  private:
    void onFClicked();
};

#endif // DOCKINGPANECONTAINER_H
