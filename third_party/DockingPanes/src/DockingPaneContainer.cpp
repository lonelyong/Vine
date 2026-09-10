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

#include <QApplication>
#include <QDebug>
#include <QDomDocument>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QPainter>
#include <QSpacerItem>
#include <QVBoxLayout>
#include <math.h>

#include "DockingPaneContainer.h"
#include "DockingPaneFlyoutWidget.h"
#include "DockingPaneGeometry.h"
#include "DockingPaneGlow.h"
#include "DockingPaneManager.h"
#include "DockingPaneTheme.h"
#include "DockingPaneTitleWidget.h"
#include "DockingToolButton.h"

DockingPaneContainer::DockingPaneContainer(const QString& title, const QString& id, QWidget* parent, QWidget* clientWidget)
  : DockingPaneBase(parent)
  , m_clientWidget(clientWidget)
{

    auto* vLayout = new QVBoxLayout();

    m_isActive     = false;
    m_flyoutWidget = nullptr;

    m_floatingGlow = nullptr;

    setWindowFlags(Qt::ToolTip | Qt::FramelessWindowHint);

    m_headerWidget = new QWidget();

    m_headerWidget->setAccessibleName("headerWidget");
    m_headerWidget->setObjectName("headerWidget");

    auto* hLayout = new QHBoxLayout();

    m_titleWidget = new DockingPaneTitleWidget("Widget");

    connect(m_titleWidget, &DockingPaneTitleWidget::titleBarStartMove, this, &DockingPaneContainer::onStartDragTitle);
    connect(m_titleWidget, &DockingPaneTitleWidget::titleBarEndMove, this, &DockingPaneContainer::onEndDragTitle);
    connect(m_titleWidget, &DockingPaneTitleWidget::titleBarMoved, this, &DockingPaneContainer::onMoveDragTitle);

    m_titleWidget->setFocusProxy(clientWidget);

    hLayout->addWidget(m_titleWidget);

    m_closeButton = new DockingToolButton(DockingToolButton::closeButtonInactive);
    m_pinButton   = new DockingToolButton(DockingToolButton::pinButtonInactive);

    connect(m_closeButton, &DockingToolButton::clicked, this, &DockingPaneContainer::onCloseButtonClicked);
    connect(m_pinButton, &DockingToolButton::clicked, this, &DockingPaneContainer::onPinButtonClicked);

    m_closeButton->setMaximumWidth(16);
    m_pinButton->setMaximumWidth(16);

    hLayout->addWidget(m_pinButton);
    hLayout->addWidget(m_closeButton);
    hLayout->addSpacerItem(new QSpacerItem(2, 0, QSizePolicy::Fixed));

    hLayout->setContentsMargins(0, 0, 0, 0);
    hLayout->setSpacing(0);

    m_headerWidget->setLayout(hLayout);
    m_headerWidget->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Minimum);
    m_headerWidget->setMaximumHeight(20);

    vLayout->addWidget(m_headerWidget);

    m_clientLayout = new QGridLayout();

    m_clientLayout->setContentsMargins(0, 0, 0, 0);
    m_clientLayout->setVerticalSpacing(0);

    m_clientLayout->addWidget(clientWidget);

    vLayout->addLayout(m_clientLayout);

    vLayout->setContentsMargins(1, 1, 1, 1);
    vLayout->setSpacing(0);

    this->setLayout(vLayout);

    DockingPaneContainer::setName(title);
    DockingPaneBase::setId(id);

    connect(qApp, &QApplication::focusChanged, this, &DockingPaneContainer::onFocusChanged);
}

DockingPaneContainer::DockingPaneContainer(QWidget* parent)
  : DockingPaneBase(parent)
{}

DockingPaneContainer::~DockingPaneContainer()
{
    // Focus may change during destruction; disconnect early so the base destructor is not
    // invoked afterwards (assertObjectType assertion).
    disconnect(qApp, &QApplication::focusChanged, this, &DockingPaneContainer::onFocusChanged);
}

void DockingPaneContainer::floatPane(QRect)
{
    this->setParent(dockingManager()->mainWindow());

    setWindowFlags(Qt::ToolTip | Qt::FramelessWindowHint);

    setState(DockingPaneBase::Floating);

    // A floating pane is resized by the user, so it needs a floor: without one it can
    // be dragged down to a few pixels and cannot be grabbed again. Dropped again when
    // the pane docks back (DockingPaneManager::dockPane), so docked layouts are not
    // constrained by it.
    DockingPaneGeometry::applyFloatingMinimum(this);

    if (m_floatingGlow) {
        delete m_floatingGlow;
    }

    m_floatingGlow = new DockingPaneGlow(this, dockingManager()->mainWindow());

    DockingPaneGeometry::keepTitleBarReachable(this, titleHeight());
}

void DockingPaneContainer::floatPane(QPoint offset)
{
    QRect paneRect;

    paneRect.setTopLeft(mapToGlobal(QPoint(0, 0)));
    paneRect.setBottomRight(mapToGlobal(QPoint(width() - 1, height() - 1)));

    m_dockingManager->closePane(this);

    setWindowFlags(Qt::ToolTip | Qt::FramelessWindowHint);

    // The argument is an offset from the position the pane had while docked, not a
    // target position: the pane keeps following the drag that detached it.
    paneRect.translate(offset);

    this->move(paneRect.topLeft());

    floatPane(paneRect);

    this->setParent(dockingManager()->mainWindow());

    this->show();
    this->activateWindow();
}

int DockingPaneContainer::titleHeight() const
{
    return m_titleWidget != nullptr ? m_titleWidget->height() : 0;
}

DockingPaneFlyoutWidget* DockingPaneContainer::openFlyout(bool hasFocus, QWidget* parent, FlyoutPosition pos, DockingPaneContainer* pane)
{
    m_flyoutWidget = new DockingPaneFlyoutWidget(hasFocus, pane, pane, static_cast<DockingPaneFlyoutWidget::FlyoutPosition>(pos), m_clientWidget, parent);

    connect(m_flyoutWidget, &DockingPaneFlyoutWidget::unpinContainer, this, &DockingPaneContainer::onUnpinContainer);
    connect(m_flyoutWidget, &DockingPaneFlyoutWidget::closeContainer, this, &DockingPaneContainer::onCloseContainer);
    connect(m_flyoutWidget, &DockingPaneFlyoutWidget::startDragFlyoutTitle, this, &DockingPaneContainer::onStartDragFlyoutTitle);
    connect(m_flyoutWidget, &DockingPaneFlyoutWidget::endDragFlyoutTitle, this, &DockingPaneContainer::onEndDragFlyoutTitle);
    connect(m_flyoutWidget, &DockingPaneFlyoutWidget::moveDragFlyoutTitle, this, &DockingPaneContainer::onMoveDragFlyoutTitle);

    m_flyoutWidget->show();

    // The flyout is a top-level tool window; raise it so it stays above the
    // whole main window (including any native render surface).
    m_flyoutWidget->raise();

    return (m_flyoutWidget);
}

void DockingPaneContainer::setState(DockingPaneBase::State state)
{
    if (state != DockingPaneBase::Floating) {
        m_pinButton->show();

        if (m_floatingGlow) {
            delete m_floatingGlow;

            m_floatingGlow = nullptr;
        }
    }
    else {
        m_pinButton->hide();
    }

    DockingPaneBase::setState(state);
}

int DockingPaneContainer::getPaneCount()
{
    return (1);
}

DockingPaneContainer* DockingPaneContainer::getPane(int)
{
    return (this);
}

QWidget* DockingPaneContainer::clientWidget()
{
    return (m_clientWidget);
}

void DockingPaneContainer::setClientWidget(QWidget* widget)
{
    while (m_clientLayout->count()) {
        m_clientLayout->takeAt(0);
    }

    // Keep the member in sync: m_clientWidget must always be the widget that is
    // actually shown in the layout, not the widget passed to the constructor
    // (a host may swap the content after creation, e.g. replace the temporary
    // placeholder; leaving the stale pointer behind made clientWidget() return
    // a dangling widget once the old one was deleted).
    m_clientWidget = widget;

    // The title bar forwards focus to the client; keep the proxy in sync with
    // the current client for the same reason as m_clientWidget above (a stale
    // proxy pointing at a replaced/deleted content widget crashes on focus).
    if (m_titleWidget) {
        m_titleWidget->setFocusProxy(widget);
    }

    // A layout cannot hold a null widget (QGridLayout::addWidget(nullptr)
    // asserts in debug and crashes in release). A null client means "clear",
    // which is represented by leaving the layout empty.
    if (!widget) {
        return;
    }

    m_clientLayout->addWidget(widget);

    widget->setVisible(true);
}

void DockingPaneContainer::saveLayout(QDomNode* parentNode, bool includeGeometry)
{
    QDomDocument doc = parentNode->ownerDocument();

    QDomElement domElement = doc.createElement(this->metaObject()->className());

    domElement.setAttribute("id", this->id());

    if (includeGeometry) {
        domElement.setAttribute("geometry", static_cast<QString>(this->saveGeometry().toBase64()));
    }

    parentNode->appendChild(domElement);
}

QSize DockingPaneContainer::flyoutSize()
{
    if (m_flyoutSize.isValid()) {
        return (m_flyoutSize);
    }

    return (QSize(100, 100));
}

void DockingPaneContainer::setFlyoutSize(QSize flyoutSize)
{
    m_flyoutSize = flyoutSize;
}

DockingPaneGlow* DockingPaneContainer::floatingGlow()
{
    return (m_floatingGlow);
}

void DockingPaneContainer::setClosable(bool closable)
{
    m_closable = closable;
    m_closeButton->setVisible(closable);
}

bool DockingPaneContainer::isClosable() const
{
    return m_closable;
}

void DockingPaneContainer::continueDrag(QPoint pos)
{
    m_initialPos = pos;

    if (m_titleWidget) {
        m_titleWidget->takeGrab();
    }
}

void DockingPaneContainer::setName(const QString& name)
{
    m_titleWidget->setText(name);

    DockingPaneBase::setName(name);
}

void DockingPaneContainer::setActivePane(bool active)
{
    m_isActive = active;

    m_titleWidget->setActive(m_isActive);

    if (m_isActive) {
        this->m_pinButton->setButton(DockingToolButton::pinButtonActive);
        this->m_closeButton->setButton(DockingToolButton::closeButtonActive);

        if (m_floatingGlow) {
            m_floatingGlow->raise();
        }
    }
    else {
        this->m_pinButton->setButton(DockingToolButton::pinButtonInactive);
        this->m_closeButton->setButton(DockingToolButton::closeButtonInactive);
    }

    update();
}

void DockingPaneContainer::paintEvent(QPaintEvent*)
{
    QPainter p(this);

    // Pane header background. Painted here (instead of a style sheet) so the
    // colours always follow the application palette.
    p.fillRect(m_headerWidget->geometry(), m_isActive ? DockingPaneTheme::activeHeaderColor() : DockingPaneTheme::inactiveHeaderColor());

    QPen pen(DockingPaneTheme::borderColor());

    pen.setWidth(1);

    p.setPen(pen);

    QRect clientRect = this->rect();

    if (state() == DockingPaneBase::Floating) {
        p.setPen(DockingPaneTheme::floatingBorderColor());

        p.drawLine(clientRect.topLeft(), clientRect.topRight());
        p.drawLine(clientRect.topLeft(), clientRect.bottomLeft());
        p.drawLine(clientRect.topRight(), clientRect.bottomRight());
        p.drawLine(clientRect.bottomLeft(), clientRect.bottomRight());

        clientRect.adjust(1, 1, -1, -1);
    }

    p.drawLine(clientRect.topLeft(), clientRect.topRight());
    p.drawLine(clientRect.topLeft(), clientRect.bottomLeft());
    p.drawLine(clientRect.topRight(), clientRect.bottomRight());
    p.drawLine(clientRect.bottomLeft(), clientRect.bottomRight());
}

void DockingPaneContainer::changeEvent(QEvent* event)
{
    if (event->type() == QEvent::PaletteChange || event->type() == QEvent::ApplicationPaletteChange) {
        update();
    }

    DockingPaneBase::changeEvent(event);
}

void DockingPaneContainer::onStartDragTitle(QPoint pos)
{
    m_dockingManager->floatingPaneStartMove(this, pos);

    m_initialPos = pos;
}

void DockingPaneContainer::onEndDragTitle(QPoint pos)
{
    m_dockingManager->floatingPaneEndMove(this, pos);
}

void DockingPaneContainer::onMoveDragTitle(QPoint pos)
{
    QPoint deltaPos = pos - m_initialPos;

    if (state() == DockingPaneBase::Floating) {
        move(this->pos() + deltaPos);

        if (m_floatingGlow) {
            m_floatingGlow->update();
        }

        m_initialPos = pos;

        m_dockingManager->floatingPaneMoved(this, pos);
    }
    else {
        double trueLength = sqrt(pow(deltaPos.x(), 2) + pow(deltaPos.y(), 2));

        if (trueLength > 5) {
            floatPane(deltaPos);

            m_initialPos = pos;

            m_dockingManager->floatingPaneStartMove(this, pos);

            // floatPane() recreated the native window, which released the
            // mouse grab taken on press; re-grab so the drag keeps receiving
            // events while moving over the main window / indicators.
            m_titleWidget->reacquireGrab();
        }
    }
}

void DockingPaneContainer::onStartDragFlyoutTitle(QPoint pos)
{
    dockingManager()->floatingPaneStartMove(this, pos);

    m_initialPos     = pos;
    m_draggingFlyout = false;
}

void DockingPaneContainer::onEndDragFlyoutTitle(QPoint pos)
{
    dockingManager()->floatingPaneEndMove(this, pos);

    if (m_flyoutWidget) {
        m_flyoutWidget->endDrag();

        m_flyoutWidget = nullptr;
    }
}

void DockingPaneContainer::onMoveDragFlyoutTitle(QPoint pos)
{
    QPoint deltaPos = pos - m_initialPos;

    if (m_draggingFlyout) {
        move(this->pos() + deltaPos);

        if (m_floatingGlow) {
            m_floatingGlow->update();
        }

        m_initialPos = pos;

        dockingManager()->floatingPaneMoved(this, pos);
    }
    else {
        const double trueLength = sqrt(pow(deltaPos.x(), 2) + pow(deltaPos.y(), 2));

        if (trueLength > 5) {
            QWidget*    widget = m_flyoutWidget->clientWidget();
            const QSize size   = m_flyoutWidget->paneRect().size();
            m_initialPos       = pos;
            deltaPos           = m_flyoutWidget->mapFromGlobal(m_initialPos);

            const QPoint global_pos = m_flyoutWidget->mapToGlobal(m_flyoutWidget->paneRect().topLeft());

            m_flyoutWidget->beginDrag();

            setClientWidget(widget);

            dockingManager()->removePinnedButton(this);

            resize(size);

            floatPane(deltaPos);

            move(global_pos);
            dockingManager()->floatingPaneStartMove(this, global_pos);

            m_draggingFlyout = true;

            // Flyout is already hidden and the mouse grab was released with it, so
            // endDragFlyoutTitle will not fire again; clean up the flyout immediately,
            // otherwise it would linger until exit (assertion on focus change).
            m_flyoutWidget->endDrag();
            m_flyoutWidget = nullptr;

            // Let the container title take over the mouse grab so the drag continues after floating.
            m_titleWidget->takeGrab();
        }
    }
}

void DockingPaneContainer::onCloseButtonClicked()
{
    if (m_closeCallback && !m_closeCallback(this))
        return; // callback vetoed
    m_dockingManager->closePane(this);
}

void DockingPaneContainer::onCloseContainer()
{
    m_flyoutWidget->restorePaneWidget();

    dockingManager()->closePinnedPane(m_flyoutWidget->pane());

    m_flyoutWidget = nullptr;
}

void DockingPaneContainer::onFocusChanged(QWidget*, QWidget* now)
{
    this->setActivePane(this->isAncestorOf(now));
}

void DockingPaneContainer::onPinButtonClicked()
{
    m_dockingManager->hidePane(this);
}

void DockingPaneContainer::onUnpinContainer()
{
    m_flyoutWidget->restorePaneWidget();

    dockingManager()->unpinPane(m_flyoutWidget->pane());

    m_flyoutWidget = nullptr;
}

void DockingPaneContainer::onFClicked()
{
    dockingManager()->dumpPaneList();
}
